/* connhold: lightweight held-connection load generator for C1M runs.
 *
 * h2load spends ~60 KB of client memory per connection, which caps a
 * same-host run near 700k connections before the server is even loaded.
 * connhold keeps one small slot per connection and an epoll loop per
 * process: it opens connections at a fixed rate across several loopback
 * VIPs (one source-port range per VIP), optionally completes a TLS
 * handshake (ALPN http/1.1), sends one GET, reads the response, then keeps
 * the socket open and re-sends the GET every --ping seconds so the server
 * keep-alive timer never fires. Everything is counted, nothing retried:
 * a connection the server closes stays closed and shows up in "closed".
 *
 *   connhold --conns 1000000 --procs 8 --rate 20000 --vips 24 \
 *            --port 8888 --tls --path /robots.txt --ping 60 --hold 120
 *
 * The parent prints one aggregate line every 2 s. It stops opening new
 * connections when MemAvailable drops below --mem-guard MiB.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum { ST_FREE, ST_CONNECT, ST_HANDSHAKE, ST_SEND, ST_READ, ST_IDLE };

typedef struct {
    int fd;
    SSL *ssl;
    uint8_t state;
    uint8_t mask; /* current epoll interest (EPOLLIN/EPOLLOUT bits) */
    uint8_t served; /* got at least one full response */
    uint16_t hdr_len;
    char *hdr; /* header bytes of the response in flight only */
    int64_t body_left;
    uint32_t sent;
    uint32_t deadline; /* seconds since start: connect/handshake/response */
    uint32_t next_ping;
} slot_t;

typedef struct {
    _Atomic uint64_t opened, connecting, handshaking, live, served_now;
    _Atomic uint64_t responses, connect_fail, tls_fail, closed, timeouts;
} stats_t;

static struct {
    long conns;
    int procs, vips, port, ping, hold, mem_guard_mib;
    long rate;
    bool tls;
    const char *path;
    const char *base;
} cfg = {1000, 1, 20, 8888, 60, 60, 2048, 1000, false, "/robots.txt", "127.0.0"};

static stats_t *g_stats; /* one per process, MAP_SHARED */
static _Atomic int *g_stop_ramp;
static _Atomic int *g_quit;
static time_t g_t0;
static char g_req[512];
static size_t g_req_len;
static SSL_CTX *g_ctx;

static uint32_t now_s(void) {
    return (uint32_t)(time(NULL) - g_t0);
}

static long mem_available_kib(void) {
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return -1;
    char line[256];
    long v = -1;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "MemAvailable: %ld kB", &v) == 1) break;
    }
    fclose(f);
    return v;
}

static void set_mask(int ep, slot_t *s, long idx, uint8_t mask) {
    if (s->mask == mask) return;
    struct epoll_event ev = {.events = 0, .data.u64 = (uint64_t)idx};
    if (mask & 1) ev.events |= EPOLLIN;
    if (mask & 2) ev.events |= EPOLLOUT;
    epoll_ctl(ep, EPOLL_CTL_MOD, s->fd, &ev);
    s->mask = mask;
}

static void close_slot(int ep, stats_t *st, slot_t *s) {
    if (s->state == ST_CONNECT) atomic_fetch_sub(&st->connecting, 1);
    else if (s->state == ST_HANDSHAKE) atomic_fetch_sub(&st->handshaking, 1);
    else if (s->state != ST_FREE) atomic_fetch_sub(&st->live, 1);
    if (s->served) atomic_fetch_sub(&st->served_now, 1);
    epoll_ctl(ep, EPOLL_CTL_DEL, s->fd, NULL);
    if (s->ssl) SSL_free(s->ssl);
    close(s->fd);
    if (s->hdr != (char *)1) free(s->hdr);
    memset(s, 0, sizeof(*s));
    s->fd = -1;
}

static int io_read(slot_t *s, char *buf, size_t len) {
    if (!s->ssl) {
        ssize_t n = recv(s->fd, buf, len, 0);
        if (n > 0) return (int)n;
        if (n == 0) return -2;
        return (errno == EAGAIN || errno == EWOULDBLOCK) ? -1 : -2;
    }
    int n = SSL_read(s->ssl, buf, (int)len);
    if (n > 0) return n;
    int e = SSL_get_error(s->ssl, n);
    return (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) ? -1 : -2;
}

static int io_write(slot_t *s, const char *buf, size_t len) {
    if (!s->ssl) {
        ssize_t n = send(s->fd, buf, len, MSG_NOSIGNAL);
        if (n >= 0) return (int)n;
        return (errno == EAGAIN || errno == EWOULDBLOCK) ? -1 : -2;
    }
    int n = SSL_write(s->ssl, buf, (int)len);
    if (n > 0) return n;
    int e = SSL_get_error(s->ssl, n);
    return (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) ? -1 : -2;
}

static void start_send(int ep, slot_t *s, long idx) {
    s->state = ST_SEND;
    s->sent = 0;
    s->deadline = now_s() + 60;
    set_mask(ep, s, idx, 2);
}

/* Consume response bytes; returns 1 when the response completed, 0 when more
 * is needed, -1 on a malformed response. */
static int consume(slot_t *s, const char *data, size_t n) {
    while (n > 0) {
        if (!s->hdr || s->body_left < 0) {
            if (!s->hdr) {
                s->hdr = malloc(16384);
                if (!s->hdr) return -1;
                s->hdr_len = 0;
                s->body_left = -1;
            }
            size_t room = 16383 - s->hdr_len;
            if (room == 0) return -1;
            size_t take = n < room ? n : room;
            memcpy(s->hdr + s->hdr_len, data, take);
            s->hdr_len += (uint16_t)take;
            s->hdr[s->hdr_len] = '\0';
            char *end = strstr(s->hdr, "\r\n\r\n");
            if (!end) {
                data += take;
                n -= take;
                continue;
            }
            size_t head = (size_t)(end - s->hdr) + 4;
            long cl = -1;
            for (char *p = s->hdr; p && p < end; p = strstr(p, "\r\n")) {
                if (*p == '\r') p += 2;
                if (strncasecmp(p, "Content-Length:", 15) == 0) {
                    cl = strtol(p + 15, NULL, 10);
                    break;
                }
            }
            if (cl < 0) return -1;
            size_t extra = s->hdr_len - head;
            /* Bytes of this read that were past the header go to the body. */
            size_t consumed_here = take - extra;
            data += consumed_here;
            n -= consumed_here;
            s->body_left = cl;
            free(s->hdr);
            s->hdr = (char *)1; /* marker: headers done */
        }
        size_t eat = (size_t)s->body_left < n ? (size_t)s->body_left : n;
        s->body_left -= (int64_t)eat;
        data += eat;
        n -= eat;
        if (s->body_left == 0) {
            s->hdr = NULL;
            return n == 0 ? 1 : -1; /* no pipelining: extra bytes are a protocol error */
        }
    }
    return 0;
}

static void on_event(int ep, stats_t *st, slot_t *s, long idx, uint32_t ev) {
    static char buf[65536];
    if (s->state == ST_CONNECT) {
        int err = 0;
        socklen_t len = sizeof(err);
        getsockopt(s->fd, SOL_SOCKET, SO_ERROR, &err, &len);
        if (err) {
            atomic_fetch_add(&st->connect_fail, 1);
            close_slot(ep, st, s);
            return;
        }
        atomic_fetch_sub(&st->connecting, 1);
        if (cfg.tls) {
            s->ssl = SSL_new(g_ctx);
            SSL_set_fd(s->ssl, s->fd);
            SSL_set_connect_state(s->ssl);
            s->state = ST_HANDSHAKE;
            atomic_fetch_add(&st->handshaking, 1);
            s->deadline = now_s() + 60;
        } else {
            atomic_fetch_add(&st->live, 1);
            start_send(ep, s, idx);
            return;
        }
    }
    if (s->state == ST_HANDSHAKE) {
        int r = SSL_do_handshake(s->ssl);
        if (r == 1) {
            atomic_fetch_sub(&st->handshaking, 1);
            atomic_fetch_add(&st->live, 1);
            start_send(ep, s, idx);
        } else {
            int e = SSL_get_error(s->ssl, r);
            if (e == SSL_ERROR_WANT_READ) set_mask(ep, s, idx, 1);
            else if (e == SSL_ERROR_WANT_WRITE) set_mask(ep, s, idx, 2);
            else {
                atomic_fetch_add(&st->tls_fail, 1);
                close_slot(ep, st, s);
            }
            return;
        }
    }
    if (s->state == ST_SEND) {
        while (s->sent < g_req_len) {
            int w = io_write(s, g_req + s->sent, g_req_len - s->sent);
            if (w == -1) {
                set_mask(ep, s, idx, 2);
                return;
            }
            if (w < 0) {
                atomic_fetch_add(&st->closed, 1);
                close_slot(ep, st, s);
                return;
            }
            s->sent += (uint32_t)w;
        }
        s->state = ST_READ;
        set_mask(ep, s, idx, 1);
        ev = EPOLLIN; /* TLS may already hold decrypted bytes */
    }
    if (s->state == ST_READ || s->state == ST_IDLE) {
        if (!(ev & (EPOLLIN | EPOLLHUP | EPOLLERR))) return;
        for (;;) {
            int r = io_read(s, buf, sizeof(buf));
            if (r == -1) return;
            if (r < 0 || s->state == ST_IDLE) {
                /* Idle sockets only become readable on close (or a stray
                 * TLS record such as a session ticket, which SSL_read eats
                 * and reports as WANT_READ). */
                if (r >= 0) continue;
                atomic_fetch_add(&st->closed, 1);
                close_slot(ep, st, s);
                return;
            }
            int c = consume(s, buf, (size_t)r);
            if (c < 0) {
                atomic_fetch_add(&st->closed, 1);
                close_slot(ep, st, s);
                return;
            }
            if (c == 1) {
                atomic_fetch_add(&st->responses, 1);
                if (!s->served) {
                    s->served = 1;
                    atomic_fetch_add(&st->served_now, 1);
                }
                s->state = ST_IDLE;
                s->next_ping = now_s() + (uint32_t)cfg.ping + (uint32_t)(idx % cfg.ping);
                return;
            }
        }
    }
}

static void run_child(int id, long count) {
    stats_t *st = &g_stats[id];
    slot_t *slots = calloc((size_t)count, sizeof(slot_t));
    if (!slots) _exit(1);
    for (long i = 0; i < count; i++) slots[i].fd = -1;
    int ep = epoll_create1(0);
    struct epoll_event evs[1024];
    long next = 0;
    double per_ms = (double)cfg.rate / cfg.procs / 1000.0;
    double budget = 0;
    struct timespec last;
    clock_gettime(CLOCK_MONOTONIC, &last);
    uint32_t last_scan = 0;
    uint32_t ramp_done_at = 0;

    while (!atomic_load(g_quit)) {
        struct timespec nowts;
        clock_gettime(CLOCK_MONOTONIC, &nowts);
        double ms = (nowts.tv_sec - last.tv_sec) * 1e3 + (nowts.tv_nsec - last.tv_nsec) / 1e6;
        last = nowts;
        budget += ms * per_ms;
        if (budget > 5000) budget = 5000;
        while (next < count && budget >= 1 && !atomic_load(g_stop_ramp)) {
            budget -= 1;
            slot_t *s = &slots[next];
            int vip = (int)((next * cfg.procs + id) % cfg.vips) + 1;
            int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
            if (fd < 0) {
                atomic_fetch_add(&st->connect_fail, 1);
                next++;
                continue;
            }
            struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons((uint16_t)cfg.port)};
            char ip[32];
            snprintf(ip, sizeof(ip), "%s.%d", cfg.base, vip);
            inet_pton(AF_INET, ip, &sa.sin_addr);
            int one = 1;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
            int r = connect(fd, (struct sockaddr *)&sa, sizeof(sa));
            if (r < 0 && errno != EINPROGRESS) {
                close(fd);
                atomic_fetch_add(&st->connect_fail, 1);
                next++;
                continue;
            }
            s->fd = fd;
            s->state = ST_CONNECT;
            s->deadline = now_s() + 60;
            s->mask = 2;
            struct epoll_event ev = {.events = EPOLLOUT, .data.u64 = (uint64_t)next};
            epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev);
            atomic_fetch_add(&st->opened, 1);
            atomic_fetch_add(&st->connecting, 1);
            next++;
        }
        if ((next >= count || atomic_load(g_stop_ramp)) && !ramp_done_at) ramp_done_at = now_s();

        int n = epoll_wait(ep, evs, 1024, 5);
        for (int i = 0; i < n; i++) {
            long idx = (long)evs[i].data.u64;
            slot_t *s = &slots[idx];
            if (s->state == ST_FREE) continue;
            on_event(ep, st, s, idx, evs[i].events);
        }

        uint32_t t = now_s();
        if (t != last_scan) {
            last_scan = t;
            for (long i = 0; i < next; i++) {
                slot_t *s = &slots[i];
                if (s->state == ST_FREE) continue;
                if (s->state == ST_IDLE) {
                    if (t >= s->next_ping) start_send(ep, s, i), on_event(ep, st, s, i, EPOLLOUT);
                } else if (t >= s->deadline) {
                    atomic_fetch_add(&st->timeouts, 1);
                    close_slot(ep, st, s);
                }
            }
        }
    }
    _exit(0);
}

static void on_signal(int sig) {
    (void)sig;
    if (g_quit) atomic_store(g_quit, 1);
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : "";
        if (!strcmp(a, "--conns")) cfg.conns = atol(v), i++;
        else if (!strcmp(a, "--procs")) cfg.procs = atoi(v), i++;
        else if (!strcmp(a, "--rate")) cfg.rate = atol(v), i++;
        else if (!strcmp(a, "--vips")) cfg.vips = atoi(v), i++;
        else if (!strcmp(a, "--port")) cfg.port = atoi(v), i++;
        else if (!strcmp(a, "--ping")) cfg.ping = atoi(v), i++;
        else if (!strcmp(a, "--hold")) cfg.hold = atoi(v), i++;
        else if (!strcmp(a, "--mem-guard")) cfg.mem_guard_mib = atoi(v), i++;
        else if (!strcmp(a, "--path")) cfg.path = v, i++;
        else if (!strcmp(a, "--base")) cfg.base = v, i++;
        else if (!strcmp(a, "--tls")) cfg.tls = true;
        else {
            fprintf(stderr, "unknown option %s\n", a);
            return 2;
        }
    }
    if (cfg.procs < 1 || cfg.vips < 1 || cfg.ping < 1 || cfg.conns < 1) return 2;

    struct rlimit rl = {1050000, 1050000};
    setrlimit(RLIMIT_NOFILE, &rl);
    signal(SIGPIPE, SIG_IGN);
    g_req_len = (size_t)snprintf(g_req, sizeof(g_req),
                                 "GET %s HTTP/1.1\r\nHost: localhost\r\nUser-Agent: connhold\r\n"
                                 "Connection: keep-alive\r\n\r\n",
                                 cfg.path);
    if (cfg.tls) {
        g_ctx = SSL_CTX_new(TLS_client_method());
        SSL_CTX_set_verify(g_ctx, SSL_VERIFY_NONE, NULL);
        SSL_CTX_set_session_cache_mode(g_ctx, SSL_SESS_CACHE_OFF);
        static const unsigned char alpn[] = "\x08http/1.1";
        SSL_CTX_set_alpn_protos(g_ctx, alpn, sizeof(alpn) - 1);
    }

    g_stats = mmap(NULL, sizeof(stats_t) * (size_t)cfg.procs, PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    int *flags = mmap(NULL, 2 * sizeof(int), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS,
                      -1, 0);
    if (g_stats == MAP_FAILED || flags == MAP_FAILED) return 1;
    memset(g_stats, 0, sizeof(stats_t) * (size_t)cfg.procs);
    g_stop_ramp = (_Atomic int *)&flags[0];
    g_quit = (_Atomic int *)&flags[1];
    atomic_store(g_stop_ramp, 0);
    atomic_store(g_quit, 0);
    g_t0 = time(NULL);

    pid_t *kids = calloc((size_t)cfg.procs, sizeof(pid_t));
    for (int p = 0; p < cfg.procs; p++) {
        long share = cfg.conns / cfg.procs + (p < cfg.conns % cfg.procs ? 1 : 0);
        pid_t pid = fork();
        if (pid == 0) run_child(p, share);
        kids[p] = pid;
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    long peak_live = 0, peak_served = 0;
    uint32_t reached_at = 0;
    bool guard_hit = false;
    for (;;) {
        sleep(2);
        stats_t sum = {0};
        for (int p = 0; p < cfg.procs; p++) {
#define ADD(f) sum.f += atomic_load(&g_stats[p].f)
            ADD(opened); ADD(connecting); ADD(handshaking); ADD(live); ADD(served_now);
            ADD(responses); ADD(connect_fail); ADD(tls_fail); ADD(closed); ADD(timeouts);
#undef ADD
        }
        long avail = mem_available_kib();
        if (!guard_hit && avail >= 0 && avail / 1024 < cfg.mem_guard_mib) {
            atomic_store(g_stop_ramp, 1);
            guard_hit = true;
            printf("mem-guard: MemAvailable %ld MiB < %d MiB, ramp stopped\n", avail / 1024,
                   cfg.mem_guard_mib);
        }
        if ((long)sum.live > peak_live) peak_live = (long)sum.live;
        if ((long)sum.served_now > peak_served) peak_served = (long)sum.served_now;
        uint32_t t = now_s();
        printf("t=%u opened=%lu connecting=%lu handshaking=%lu live=%lu served=%lu responses=%lu "
               "connect_fail=%lu tls_fail=%lu closed=%lu timeouts=%lu memavail=%ldMiB\n",
               t, sum.opened, sum.connecting, sum.handshaking, sum.live, sum.served_now,
               sum.responses, sum.connect_fail, sum.tls_fail, sum.closed, sum.timeouts,
               avail / 1024);
        fflush(stdout);
        bool ramp_over = guard_hit || (long)sum.opened + (long)sum.connect_fail >= cfg.conns;
        if (ramp_over && sum.connecting == 0 && sum.handshaking == 0 && !reached_at) reached_at = t;
        if ((reached_at && t - reached_at >= (uint32_t)cfg.hold) || atomic_load(g_quit)) break;
    }
    atomic_store(g_quit, 1);
    for (int p = 0; p < cfg.procs; p++) waitpid(kids[p], NULL, 0);
    printf("peak_live=%ld peak_served=%ld\n", peak_live, peak_served);
    return 0;
}

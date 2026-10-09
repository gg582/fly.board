#define _POSIX_C_SOURCE 200809L
#include "ipban.h"
#include <arpa/inet.h>
#include <errno.h>
#include <cwist/core/log.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <time.h>

#define IPBAN_SLOTS 4096
#define IPBAN_PROBE 32

/* Binary address key: '4' + 4 bytes, or '6' + 16 bytes. */
typedef struct {
    uint64_t key;           /* FNV-1a hash of the address bytes, 0 = empty */
    uint8_t addr[17];
    uint8_t addr_len;
    uint8_t fails;          /* consecutive failures in the current window */
    time_t window_start;    /* time of the first failure of the current run */
    time_t banned_until;    /* 0 = not banned */
} ipban_slot_t;

/* Shared by every worker process (MAP_SHARED before fork), same pattern as
 * the spam guard table. */
typedef struct {
    pthread_mutex_t lock;
    ipban_slot_t slots[IPBAN_SLOTS];
} ipban_table_t;

static ipban_table_t *g_ipban = NULL;
static long g_threshold = 5;
static long g_window_sec = 600;
static long g_duration_sec = 3600;
static size_t g_bans_created = 0;
static time_t g_last_stats_log = 0;

static long env_long(const char *name, long fallback) {
    const char *v = getenv(name);
    if (!v || !v[0]) return fallback;
    long n = strtol(v, NULL, 10);
    return n > 0 ? n : fallback;
}

bool ipban_init(void) {
    /* Shared by every worker process (MAP_SHARED before fork), same pattern
     * as the spam guard table. */
    void *mem = mmap(NULL, sizeof(ipban_table_t), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) {
        CWIST_LOG_ERROR("IP ban: shared memory unavailable; IP banning disabled");
        return false;
    }
    ipban_table_t *t = (ipban_table_t *)mem;
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    /* A worker killed while holding the lock must not wedge the others. */
    pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);
    bool ok = pthread_mutex_init(&t->lock, &attr) == 0;
    pthread_mutexattr_destroy(&attr);
    if (!ok) {
        munmap(mem, sizeof(ipban_table_t));
        CWIST_LOG_ERROR("IP ban: init failed; IP banning disabled");
        return false;
    }
    g_threshold = env_long("FLY_BAN_THRESHOLD", 5);
    g_window_sec = env_long("FLY_BAN_WINDOW_SEC", 600);
    g_duration_sec = env_long("FLY_BAN_DURATION_SEC", 3600);
    g_ipban = t;
    CWIST_LOG_INFO("IP ban: threshold=%ld window=%lds duration=%lds",
                   g_threshold, g_window_sec, g_duration_sec);
    return true;
}

/* Peer address as bytes ('4'+addr / '6'+addr); v4-mapped v6 is normalized
 * to plain v4. Like the spam guard, HTTP/2/3 streams without a connected
 * socket have no usable address. */
static bool peer_key(cwist_http_request *req, uint8_t *addr, uint8_t *addr_len, uint64_t *hash) {
    if (!req || req->client_fd < 0) return false;
    struct sockaddr_storage ss;
    socklen_t len = sizeof(ss);
    if (getpeername(req->client_fd, (struct sockaddr *)&ss, &len) != 0) return false;
    if (ss.ss_family == AF_INET) {
        struct sockaddr_in *a = (struct sockaddr_in *)&ss;
        addr[0] = '4';
        memcpy(addr + 1, &a->sin_addr, 4);
        *addr_len = 5;
    } else if (ss.ss_family == AF_INET6) {
        struct sockaddr_in6 *a = (struct sockaddr_in6 *)&ss;
        if (IN6_IS_ADDR_V4MAPPED(&a->sin6_addr)) {
            addr[0] = '4';
            memcpy(addr + 1, a->sin6_addr.s6_addr + 12, 4);
            *addr_len = 5;
        } else {
            addr[0] = '6';
            memcpy(addr + 1, a->sin6_addr.s6_addr, 16);
            *addr_len = 17;
        }
    } else {
        return false;
    }
    /* FNV-1a 64. */
    uint64_t h = 1469598103934665603ULL;
    for (uint8_t i = 0; i < *addr_len; i++) {
        h ^= addr[i];
        h *= 1099511628211ULL;
    }
    if (h == 0) h = 1;
    *hash = h;
    return true;
}

static bool addr_matches(const ipban_slot_t *s, const uint8_t *addr, uint8_t addr_len) {
    return s->addr_len == addr_len && memcmp(s->addr, addr, addr_len) == 0;
}

/* Find the slot for addr, inserting an empty one if requested. Returns NULL
 * when the probe sequence finds neither the key nor a free slot (table
 * saturated; fail open). Caller must hold the lock. */
static ipban_slot_t *slot_for(ipban_table_t *t, uint64_t key,
                              const uint8_t *addr, uint8_t addr_len, bool insert) {
    size_t start = (size_t)(key % IPBAN_SLOTS);
    for (size_t i = 0; i < IPBAN_PROBE; i++) {
        ipban_slot_t *s = &t->slots[(start + i) % IPBAN_SLOTS];
        if (s->key == key) {
            return addr_matches(s, addr, addr_len) ? s : NULL; /* hash collision, different addr */
        }
        if (s->key == 0) {
            if (!insert) return NULL;
            memset(s, 0, sizeof(*s));
            s->key = key;
            s->addr_len = addr_len;
            memcpy(s->addr, addr, addr_len);
            return s;
        }
    }
    return NULL;
}

static void lock_table(ipban_table_t *t) {
    if (pthread_mutex_lock(&t->lock) == EOWNERDEAD) pthread_mutex_consistent(&t->lock);
}

/* Log the active-ban count at most once per minute (called when a ban is
 * created, so a password-guessing burst does not spam the log). */
static void stats_log(void) {
    time_t now = time(NULL);
    if (now - g_last_stats_log < 60) return;
    g_last_stats_log = now;
    size_t banned = 0;
    lock_table(g_ipban);
    for (size_t i = 0; i < IPBAN_SLOTS; i++)
        if (g_ipban->slots[i].key != 0 && g_ipban->slots[i].banned_until > now) banned++;
    pthread_mutex_unlock(&g_ipban->lock);
    CWIST_LOG_WARN("IP ban: %zu address(es) currently banned", banned);
}

void ipban_note_failure(cwist_http_request *req) {
    if (!g_ipban) return;
    uint8_t addr[17];
    uint8_t addr_len = 0;
    uint64_t key = 0;
    if (!peer_key(req, addr, &addr_len, &key)) return;
    time_t now = time(NULL);
    lock_table(g_ipban);
    ipban_slot_t *s = slot_for(g_ipban, key, addr, addr_len, true);
    if (s) {
        if (s->banned_until > now) {
            /* Already banned: no counter churn, just extend nothing. */
        } else {
            if (s->fails == 0 || now - s->window_start > g_window_sec) {
                s->fails = 0;
                s->window_start = now;
            }
            s->fails++;
            if (s->fails >= g_threshold) {
                s->banned_until = now + g_duration_sec;
                g_bans_created++;
            }
        }
    }
    pthread_mutex_unlock(&g_ipban->lock);
    if (s) stats_log();
}

void ipban_note_success(cwist_http_request *req) {
    if (!g_ipban) return;
    uint8_t addr[17];
    uint8_t addr_len = 0;
    uint64_t key = 0;
    if (!peer_key(req, addr, &addr_len, &key)) return;
    lock_table(g_ipban);
    ipban_slot_t *s = slot_for(g_ipban, key, addr, addr_len, false);
    if (s) {
        s->fails = 0;
        s->window_start = 0;
        s->banned_until = 0;
    }
    pthread_mutex_unlock(&g_ipban->lock);
}

void ipban_middleware(cwist_http_request *req, cwist_http_response *res, cwist_handler_func next) {
    bool banned = false;
    if (g_ipban) {
        uint8_t addr[17];
        uint8_t addr_len = 0;
        uint64_t key = 0;
        if (peer_key(req, addr, &addr_len, &key)) {
            time_t now = time(NULL);
            lock_table(g_ipban);
            ipban_slot_t *s = slot_for(g_ipban, key, addr, addr_len, false);
            if (s && s->banned_until > 0) {
                if (s->banned_until <= now) {
                    /* Lazily evict the expired ban. */
                    s->key = 0;
                    s->fails = 0;
                    s->window_start = 0;
                    s->banned_until = 0;
                } else {
                    banned = true;
                }
            }
            pthread_mutex_unlock(&g_ipban->lock);
        }
    }
    if (banned) {
        res->status_code = CWIST_HTTP_FORBIDDEN;
        cwist_sstring_assign(res->body, "Access temporarily blocked: too many failed login attempts from this address.");
        cwist_http_header_add(&res->headers, "Content-Type", "text/plain; charset=utf-8");
        cwist_http_header_add(&res->headers, "Cache-Control", "no-store");
        return;
    }
    if (next) next(req, res);
}

size_t ipban_banned_count(void) {
    if (!g_ipban) return 0;
    time_t now = time(NULL);
    size_t banned = 0;
    lock_table(g_ipban);
    for (size_t i = 0; i < IPBAN_SLOTS; i++)
        if (g_ipban->slots[i].key != 0 && g_ipban->slots[i].banned_until > now) banned++;
    pthread_mutex_unlock(&g_ipban->lock);
    return banned;
}

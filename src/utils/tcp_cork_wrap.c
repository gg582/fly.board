/*
 * TCP_CORK / TCP_NOPUSH / TCP_NODELAY wrapper for CWIST response senders.
 *
 * fly.board does not own the listening socket; CWIST accepts connections
 * internally.  Instead of patching CWIST, we use the linker's --wrap feature
 * to intercept cwist_http_send_response() and cwist_https_send_response(),
 * enable cork around the real send, and disable it immediately afterwards.
 * This coalesces header+body (and TLS records) into fewer TCP segments on
 * high-RTT paths without keeping the socket corked between requests.
 *
 * We also enable TCP_NODELAY on every send.  Nagle's algorithm otherwise
 * delays small writes until an ACK arrives, which amplifies latency on
 * high-RTT links.  TCP_CORK still controls when data is actually flushed,
 * so the two options complement each other rather than conflicting.
 *
 * NOTE: Inline assets are intentionally split into smaller chunks (critical
 * shell, small fonts, optional CDN libs, images) and oversized assets are kept
 * as separate cached requests.  This keeps each corked burst bounded and
 * prevents a single giant HTML payload from monopolising the send buffer.
 */
#define _GNU_SOURCE
#include <cwist/net/http/http.h>
#include <cwist/net/http/https.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <stddef.h>
#include <stdlib.h>

static void fly_cork_enable(int fd)
{
    if (fd < 0) return;
#if defined(__linux__)
    int on = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_CORK, &on, sizeof(on));
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
    int on = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NOPUSH, &on, sizeof(on));
#else
    (void)fd;
#endif
}

static void fly_cork_disable(int fd)
{
    if (fd < 0) return;
#if defined(__linux__)
    int off = 0;
    setsockopt(fd, IPPROTO_TCP, TCP_CORK, &off, sizeof(off));
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
    int off = 0;
    setsockopt(fd, IPPROTO_TCP, TCP_NOPUSH, &off, sizeof(off));
#else
    (void)fd;
#endif
}

static void fly_nodelay_enable(int fd)
{
    if (fd < 0) return;
    int on = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
}

static size_t fly_response_body_len(const cwist_http_response *res)
{
    if (!res) return 0;
    if (res->use_file_stream) return res->file_stream_len;
    if (res->is_ptr_body) return res->ptr_body_len;
    if (res->body) return res->body->size;
    return 0;
}

static size_t fly_response_header_len(const cwist_http_response *res)
{
    if (!res) return 0;
    size_t len = 0;
    for (const cwist_http_header_node *h = res->headers; h; h = h->next) {
        if (h->key && h->key->data) len += h->key->size;
        if (h->value && h->value->data) len += h->value->size;
        len += 4; /* ": " + "\r\n" */
    }
    len += 32; /* status line + blank line + slop */
    return len;
}

static size_t fly_cork_threshold(void)
{
    const char *env = getenv("FLYBOARD_CORK_THRESHOLD");
    if (env) {
        long val = strtol(env, NULL, 10);
        if (val > 0) return (size_t)val;
    }
    return 1460; /* one IPv4 MSS; safe default for IPv6/PPPoE when tuned */
}

static bool fly_use_cork(const cwist_http_response *res)
{
    size_t total = fly_response_body_len(res) + fly_response_header_len(res);
    return total > fly_cork_threshold();
}

/* Real implementations come from libcwist.a via --wrap linker magic. */
extern cwist_error_t __real_cwist_http_send_response(int client_fd, cwist_http_response *res);
extern cwist_error_t __real_cwist_https_send_response(cwist_https_connection *conn, cwist_http_response *res);

cwist_error_t __wrap_cwist_http_send_response(int client_fd, cwist_http_response *res)
{
    fly_nodelay_enable(client_fd);
    bool cork = fly_use_cork(res);
    if (cork) fly_cork_enable(client_fd);
    cwist_error_t err = __real_cwist_http_send_response(client_fd, res);
    if (cork) fly_cork_disable(client_fd);
    return err;
}

cwist_error_t __wrap_cwist_https_send_response(cwist_https_connection *conn, cwist_http_response *res)
{
    int fd = conn ? conn->fd : -1;
    fly_nodelay_enable(fd);
    bool cork = fly_use_cork(res);
    if (cork) fly_cork_enable(fd);
    cwist_error_t err = __real_cwist_https_send_response(conn, res);
    if (cork) fly_cork_disable(fd);
    return err;
}

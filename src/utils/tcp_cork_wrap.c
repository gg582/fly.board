/*
 * TCP_CORK / TCP_NOPUSH wrapper for CWIST response senders.
 *
 * fly.board does not own the listening socket; CWIST accepts connections
 * internally.  Instead of patching CWIST, we use the linker's --wrap feature
 * to intercept cwist_http_send_response() and cwist_https_send_response(),
 * enable cork around the real send, and disable it immediately afterwards.
 * This coalesces header+body (and TLS records) into fewer TCP segments on
 * high-RTT paths without keeping the socket corked between requests.
 */
#define _GNU_SOURCE
#include <cwist/net/http/http.h>
#include <cwist/net/http/https.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>

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

/* Real implementations come from libcwist.a via --wrap linker magic. */
extern cwist_error_t __real_cwist_http_send_response(int client_fd, cwist_http_response *res);
extern cwist_error_t __real_cwist_https_send_response(cwist_https_connection *conn, cwist_http_response *res);

cwist_error_t __wrap_cwist_http_send_response(int client_fd, cwist_http_response *res)
{
    fly_cork_enable(client_fd);
    cwist_error_t err = __real_cwist_http_send_response(client_fd, res);
    fly_cork_disable(client_fd);
    return err;
}

cwist_error_t __wrap_cwist_https_send_response(cwist_https_connection *conn, cwist_http_response *res)
{
    int fd = conn ? conn->fd : -1;
    fly_cork_enable(fd);
    cwist_error_t err = __real_cwist_https_send_response(conn, res);
    fly_cork_disable(fd);
    return err;
}

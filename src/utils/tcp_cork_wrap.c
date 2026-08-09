/*
 * Response-boundary TCP_CORK wrapper for CWIST.
 *
 * CWIST owns accepted sockets, so linker wrapping is used to bracket each
 * HTTP response.  CORK is enabled before serialization and cleared as soon
 * as the response sender returns; clearing it forces the final partial
 * segment out.  This combines headers and body without leaving a keep-alive
 * connection corked between requests.
 *
 * Do not gate CORK by response size.  A high-RTT client pays for every
 * separate header/body write, including small redirects, API replies, and
 * page fragments.  The immediate uncork makes small single-write responses
 * flush promptly while still protecting multi-write responses.
 */
#include <cwist/net/http/http.h>
#include <cwist/net/http/https.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

static void fly_set_response_cork(int fd, int enabled)
{
    if (fd < 0) return;
#if defined(__linux__)
    (void)setsockopt(fd, IPPROTO_TCP, TCP_CORK, &enabled, sizeof(enabled));
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NOPUSH, &enabled, sizeof(enabled));
#else
    (void)enabled;
#endif
}

static void fly_enable_nodelay(int fd)
{
    if (fd < 0) return;
    const int enabled = 1;
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled));
}

extern cwist_error_t __real_cwist_http_send_response(int client_fd,
                                                     cwist_http_response *res);
extern cwist_error_t __real_cwist_https_send_response(cwist_https_connection *conn,
                                                      cwist_http_response *res);

cwist_error_t __wrap_cwist_http_send_response(int client_fd,
                                              cwist_http_response *res)
{
    fly_enable_nodelay(client_fd);
    fly_set_response_cork(client_fd, 1);
    cwist_error_t err = __real_cwist_http_send_response(client_fd, res);
    fly_set_response_cork(client_fd, 0);
    return err;
}

cwist_error_t __wrap_cwist_https_send_response(cwist_https_connection *conn,
                                               cwist_http_response *res)
{
    const int fd = conn ? conn->fd : -1;
    fly_enable_nodelay(fd);
    fly_set_response_cork(fd, 1);
    cwist_error_t err = __real_cwist_https_send_response(conn, res);
    fly_set_response_cork(fd, 0);
    return err;
}

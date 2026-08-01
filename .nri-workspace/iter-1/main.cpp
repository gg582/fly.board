// fly.board.c
/* Targeted fix for the routing drift on persistent browser connections.
 *
 * Root cause: per-request state (path, resolved route) was stored on the
 * connection object and not reset between HTTP requests on a keep-alive
 * connection.  After the first request to '/' the stale path/route shadowed
 * every later slug, so Firefox/Chrome (which reuse connections) got the root
 * response for every URL.  curl usually starts a new TCP connection per
 * invocation and therefore saw the correct slug output.
 *
 * Fix: reset the per-request structure and any connection-level route cache
 * at the start of every request, and resolve routes strictly from the current
 * request URI without falling back to '/'.
 */

#include <stddef.h>
#include <string.h>

struct route;
struct request {
    char    path[256];
    int     keep_alive;
};

struct connection {
    int                 fd;
    struct request      req;
    const struct route *cached_route;
};

static void request_reset(struct request *req)
{
    memset(req, 0, sizeof(*req));
}

static const struct route *route_resolve(const char *path)
{
    extern const struct route *route_lookup(const char *path);

    if (!path || path[0] != '/')
        return NULL;
    return route_lookup(path);
}

static void connection_serve(struct connection *conn)
{
    for (;;) {
        /* CRITICAL: wipe previous request so the path cannot leak. */
        request_reset(&conn->req);
        conn->cached_route = NULL;

        if (http_read_request(conn, &conn->req) <= 0)
            break;

        conn->cached_route = route_resolve(conn->req.path);
        if (!conn->cached_route) {
            http_send_status(conn, 404);
        } else {
            serve_route(conn, conn->cached_route);
        }

        if (!conn->req.keep_alive)
            break;
    }
}
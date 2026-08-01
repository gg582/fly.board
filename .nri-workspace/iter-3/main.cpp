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

/* External HTTP helpers used by this unit. */
extern int  http_read_request(struct connection *conn, struct request *req);
extern void http_send_status(struct connection *conn, int status);
extern void serve_route(struct connection *conn, const struct route *route);
extern const struct route *route_lookup(const char *path);

static void request_reset(struct request *req)
{
    memset(req, 0, sizeof(*req));
}

static const struct route *route_resolve(const char *path)
{
    if (!path || path[0] != '/')
        return NULL;
    return route_lookup(path);
}

static void connection_serve(struct connection *conn)
{
    for (;;) {
        /* CRITICAL: wipe previous request and route cache so neither leaks. */
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

// test_fly_board.c
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Include the unit under test so we can exercise its static helpers. */
#include "fly.board.c"

struct route {
    const char *path;
};

static const char *expected_paths[] = { "/", "/slug", NULL };
static int feed_index = 0;

static const struct route routes[] = {
    { "/" },
    { "/slug" },
};

const struct route *route_lookup(const char *path)
{
    size_t i;
    for (i = 0; i < sizeof(routes) / sizeof(routes[0]); ++i) {
        if (strcmp(path, routes[i].path) == 0)
            return &routes[i];
    }
    return NULL;
}

static const char *served[4];
static int served_count = 0;

int http_read_request(struct connection *conn, struct request *req)
{
    const char *next = expected_paths[feed_index++];
    if (!next)
        return 0;
    strncpy(req->path, next, sizeof(req->path) - 1);
    req->path[sizeof(req->path) - 1] = '\0';
    req->keep_alive = (expected_paths[feed_index] != NULL);
    (void)conn;
    return 1;
}

void http_send_status(struct connection *conn, int status)
{
    static char buf[32];
    snprintf(buf, sizeof(buf), "status:%d", status);
    served[served_count++] = buf;
    (void)conn;
}

void serve_route(struct connection *conn, const struct route *route)
{
    served[served_count++] = route->path;
    (void)conn;
}

int main(void)
{
    struct connection conn = { .fd = -1 };
    connection_serve(&conn);

    if (served_count != 2 || strcmp(served[0], "/") != 0 || strcmp(served[1], "/slug") != 0) {
        fprintf(stderr, "FAILED: served_count=%d routes={%s,%s,...}\n",
                served_count, served_count > 0 ? served[0] : "(none)",
                served_count > 1 ? served[1] : "(none)");
        return 1;
    }
    printf("ok: root and slug served distinctly\n");
    return 0;
}
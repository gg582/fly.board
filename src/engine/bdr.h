#ifndef ENGINE_BDR_H
#define ENGINE_BDR_H

#include <cwist/sys/app/app.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Big Dumb Reply for fly.board's own routes.
 *
 * cwist only consults its BDR on the cleartext HTTP/1.1 path, and fly.board
 * pages carry Vary: Cookie, so cwist never learns them. This layer keeps
 * finished responses in the app's cwist BDR store under a key built from
 * every request input the page depends on, and serves them straight from
 * the gate without running (or deferring) the route.
 *
 * Content changes bump a counter shared by all worker processes. The first
 * request after a bump misses, runs the normal route once, and its response
 * replaces the stale entry; later requests are BDR hits again. */

#define ENGINE_BDR_KEY_MAX 1024

/* Call before cwist_app_listen() forks, after cwist_app_configure_bdr(). */
void engine_bdr_init(cwist_app *app);

/* Content changed somewhere: stale every cached reply in every process. */
void engine_bdr_bump(void);

/* Build the cache key for @p req. Returns false when the request must not
 * touch the cache (logged in, conditional/range request, non-GET, ...). */
bool engine_bdr_key(cwist_http_request *req, char *out, size_t out_len);

/* Fill @p res from the cache. Returns false on a miss or stale entry. */
bool engine_bdr_serve(cwist_http_request *req, cwist_http_response *res, const char *key);

/* Content version to pass to engine_bdr_store(). Read it before the route
 * runs: a bump during rendering then leaves the stored reply already stale
 * instead of labelling old content with the new version. */
uint64_t engine_bdr_version(void);

/* Store a finished response (status, final headers, encoded body). */
void engine_bdr_store(cwist_http_request *req, cwist_http_response *res, const char *key,
                      uint64_t version);

#endif

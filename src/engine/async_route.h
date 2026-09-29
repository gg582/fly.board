#ifndef ENGINE_ASYNC_ROUTE_H
#define ENGINE_ASYNC_ROUTE_H

#include <cwist/sys/app/app.h>

/* Post-handler hook run on the request worker for a deferred response, in
 * place of the middleware code that would have run after next() returned. */
typedef void (*engine_async_finish_func)(cwist_http_request *req, cwist_http_response *res);

/* Register the async gate. Must be called before any other cwist_app_use()
 * so the gate is the outermost middleware: deferred jobs are only handed to
 * the request workers once the whole middleware chain has unwound.
 * @p compress is applied to deferred responses on the worker (it cannot run
 * in the chain, the body does not exist yet); @p finish replays the
 * post-next() part of the application middleware. */
void engine_async_init(cwist_app *app, cwist_middleware_func compress,
                       engine_async_finish_func finish);

/* Route registration: the handler runs on a request worker thread and the
 * connection's reactor/pool thread is released while it does. Falls back to
 * inline execution where cwist cannot complete a deferred exchange (HTTP/3,
 * in-memory dispatch) or when the worker queue is full. */
void engine_async_get(cwist_app *app, const char *path, cwist_handler_func handler);
/* Like engine_async_get(), and anonymous replies are kept in the route BDR
 * (engine/bdr.h) until content changes. Only for pages whose bytes depend on
 * nothing but the inputs engine_bdr_key() folds into the key. */
void engine_async_get_cached(cwist_app *app, const char *path, cwist_handler_func handler);
void engine_async_post(cwist_app *app, const char *path, cwist_handler_func handler);

/* Side effects a route records on every GET (view counters) still have to
 * happen when the route BDR answers instead. Runs on a request worker with
 * the request path. */
typedef void (*engine_async_hit_func)(cwist_db *db, const char *path);
void engine_async_set_hit_hook(engine_async_hit_func hook);

#endif

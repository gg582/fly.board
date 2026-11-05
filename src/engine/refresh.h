#ifndef ENGINE_REFRESH_H
#define ENGINE_REFRESH_H

#include <stdbool.h>

typedef void (*engine_refresh_func)(void *arg);
typedef void (*engine_refresh_free_func)(void *arg);

/* Elect one leader for key and enqueue its independent refresh job. Returns
 * false only when the job could not be queued. An existing leader is a
 * successful no-op. arg is always released with free_arg. */
bool engine_refresh_schedule(const char *key, engine_refresh_func refresh,
                             engine_refresh_free_func free_arg, void *arg);

#endif

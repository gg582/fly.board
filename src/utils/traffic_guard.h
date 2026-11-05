#ifndef FLYBOARD_TRAFFIC_GUARD_H
#define FLYBOARD_TRAFFIC_GUARD_H

#include <cwist/net/http/http.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    TRAFFIC_READ = 0,
    TRAFFIC_WRITE = 1
} traffic_class_t;

typedef enum {
    TRAFFIC_ACTION_429 = 0,
    TRAFFIC_ACTION_CACHED = 1,
    TRAFFIC_ACTION_TARPIT = 2
} traffic_action_t;

/* Read FLY_TRAFFIC_* settings and allocate the process-shared fixed table.
 * Call before cwist forks. Both rates default to zero (disabled). */
bool traffic_guard_init(void);

/* Consume one token for the request's peer address, or its session cookie
 * when no connected peer is available. Unknown identities fail open. */
bool traffic_guard_check(cwist_http_request *req, unsigned *retry_after);

traffic_action_t traffic_guard_action(void);
unsigned traffic_guard_tarpit_ms(void);
void traffic_guard_reject(cwist_http_response *res, unsigned retry_after);

/* Fixed-size table observability; used by tests and available to metrics. */
void traffic_guard_stats(uint64_t *allowed, uint64_t *limited, size_t *used);

/* Keyed entry point kept separate from socket/cookie identity extraction. */
bool traffic_guard_check_key(const void *id, size_t id_len, traffic_class_t cls,
                             uint64_t now_ns, unsigned *retry_after);

#endif

#include "engine/nats.h"
#include "nats/fly_nats.h"
#include <cwist/core/log.h>
#include <stdlib.h>
#include <stdbool.h>

bool engine_nats_init(void) {
    const char *nats_url = getenv("NATS_URL");
    if (!nats_url) return true;

    if (fly_nats_init(nats_url)) {
        /* cnats dispatches asynchronous subscriptions on its own blocking
         * dispatcher threads.  Running fly_nats_dispatch() here would only
         * issue natsConnection_Flush() in a tight loop when idle. */
        FLY_LOG_DEBUG("NATS subscriptions registered without polling worker");
    } else {
        FLY_LOG_ERROR("NATS init failed, continuing without messaging");
    }
    return true;
}

void engine_nats_stop(void) {
    fly_nats_close();
}

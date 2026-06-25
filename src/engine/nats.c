#include "engine/nats.h"
#include "engine/pool.h"
#include "nats/fly_nats.h"
#include <cwist/core/log.h>
#include <stdlib.h>
#include <stdbool.h>

static volatile bool g_nats_running = false;

static void *nats_worker(void *arg) {
    (void)arg;
    while (g_nats_running) {
        fly_nats_dispatch();
    }
    return NULL;
}

bool engine_nats_init(void) {
    const char *nats_url = getenv("NATS_URL");
    if (!nats_url) return true;

    if (fly_nats_init(nats_url)) {
        g_nats_running = true;
        if (!engine_pool_schedule(nats_worker, NULL, 0x4e415453ULL, TTAK_TASK_DOMAIN_NET, 80)) {
            g_nats_running = false;
            fly_nats_close();
            FLY_LOG_ERROR("Failed to schedule NATS worker");
            return false;
        }
    } else {
        FLY_LOG_ERROR("NATS init failed, continuing without messaging");
    }
    return true;
}

void engine_nats_stop(void) {
    g_nats_running = false;
    fly_nats_close();
}

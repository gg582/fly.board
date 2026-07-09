#include "engine/nats.h"
#include "engine/pool.h"
#include "nats/fly_nats.h"
#include <cwist/core/log.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdatomic.h>

static _Atomic bool g_nats_running = false;

static void *nats_worker(void *arg) {
    (void)arg;
    while (atomic_load_explicit(&g_nats_running, memory_order_acquire)) {
        fly_nats_dispatch();
    }
    return NULL;
}

bool engine_nats_init(void) {
    const char *nats_url = getenv("NATS_URL");
    if (!nats_url) return true;

    if (fly_nats_init(nats_url)) {
        atomic_store_explicit(&g_nats_running, true, memory_order_release);
        if (!engine_pool_schedule(nats_worker, NULL, 0x4e415453ULL, TTAK_TASK_DOMAIN_NET, 80)) {
            atomic_store_explicit(&g_nats_running, false, memory_order_release);
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
    atomic_store_explicit(&g_nats_running, false, memory_order_release);
    fly_nats_close();
}

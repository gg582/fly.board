#include "engine/pool.h"
#include <ttak/timing/timing.h>
#include <cwist/core/log.h>
#include <unistd.h>

static ttak_thread_pool_t *g_background_pool = NULL;

static size_t background_pool_size(void) {
    long cores = sysconf(_SC_NPROCESSORS_ONLN);
    if (cores < 2) return 2;
    if (cores > 4) return 4;
    return (size_t)cores;
}

bool engine_pool_init(void) {
    g_background_pool = ttak_thread_pool_create(background_pool_size(), 0, ttak_get_tick_count());
    if (!g_background_pool) {
        FLY_LOG_ERROR("Failed to initialize libttak background thread pool");
        return false;
    }
    return true;
}

void engine_pool_shutdown(void) {
    if (g_background_pool) {
        ttak_thread_pool_destroy(g_background_pool);
        g_background_pool = NULL;
    }
}

bool engine_pool_schedule(ttak_task_func_t func,
                          void *arg,
                          uint64_t hash,
                          ttak_task_domain_t domain,
                          uint8_t urgency) {
    if (!g_background_pool || !func) return false;
    uint64_t now = ttak_get_tick_count();
    ttak_task_t *task = ttak_task_create(func, arg, NULL, now);
    if (!task) return false;
    ttak_task_set_hash(task, hash);
    ttak_task_set_domain(task, domain);
    ttak_task_set_urgency(task, urgency);
    if (!ttak_thread_pool_schedule_task(g_background_pool, task, 0, now)) {
        ttak_task_destroy(task, now);
        return false;
    }
    return true;
}

#include "engine/pool.h"
#include <ttak/timing/timing.h>
#include <cwist/core/log.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <unistd.h>

static ttak_thread_pool_t *g_work_pool = NULL;
static ttak_thread_pool_t *g_service_pool = NULL;
static pthread_rwlock_t g_pool_lock = PTHREAD_RWLOCK_INITIALIZER;
static bool g_accepting_work = false;

static size_t pool_size(const char *env_name, size_t fallback, size_t min, size_t max) {
    const char *value = getenv(env_name);
    if (value && value[0]) {
        char *end = NULL;
        unsigned long parsed = strtoul(value, &end, 10);
        if (end && *end == '\0' && parsed >= min && parsed <= max) return (size_t)parsed;
        FLY_LOG_ERROR("Ignoring invalid %s value; expected %zu..%zu", env_name, min, max);
    }
    return fallback;
}

static size_t work_pool_size(void) {
    long cores = sysconf(_SC_NPROCESSORS_ONLN);
    size_t fallback = cores < 2 ? 2 : (cores > 8 ? 8 : (size_t)cores);
    return pool_size("FLY_ENGINE_WORKERS", fallback, 2, 32);
}

static size_t service_pool_size(void) {
    /* NATS and scheduled maintenance are independent, long-lived loops. */
    return pool_size("FLY_ENGINE_SERVICE_WORKERS", 2, 1, 8);
}

static bool schedule_on_pool(ttak_thread_pool_t *pool,
                             ttak_task_func_t func,
                             void *arg,
                             uint64_t hash,
                             ttak_task_domain_t domain,
                             uint8_t urgency) {
    if (!pool || !func) return false;
    uint64_t now = ttak_get_tick_count();
    ttak_task_t *task = ttak_task_create(func, arg, NULL, now);
    if (!task) return false;
    ttak_task_set_hash(task, hash);
    ttak_task_set_domain(task, domain);
    ttak_task_set_urgency(task, urgency);
    if (!ttak_thread_pool_schedule_task(pool, task, 0, now)) {
        ttak_task_destroy(task, now);
        return false;
    }
    return true;
}

bool engine_pool_init(void) {
    pthread_rwlock_wrlock(&g_pool_lock);
    if (g_accepting_work) {
        pthread_rwlock_unlock(&g_pool_lock);
        return true;
    }
    g_work_pool = ttak_thread_pool_create(work_pool_size(), 0, ttak_get_tick_count());
    g_service_pool = ttak_thread_pool_create(service_pool_size(), 0, ttak_get_tick_count());
    if (!g_work_pool || !g_service_pool) {
        ttak_thread_pool_t *work_pool = g_work_pool;
        ttak_thread_pool_t *service_pool = g_service_pool;
        g_work_pool = NULL;
        g_service_pool = NULL;
        pthread_rwlock_unlock(&g_pool_lock);
        if (work_pool) ttak_thread_pool_destroy(work_pool);
        if (service_pool) ttak_thread_pool_destroy(service_pool);
        FLY_LOG_ERROR("Failed to initialize engine worker pools");
        return false;
    }
    g_accepting_work = true;
    pthread_rwlock_unlock(&g_pool_lock);
    return true;
}

void engine_pool_shutdown(void) {
    pthread_rwlock_wrlock(&g_pool_lock);
    g_accepting_work = false;
    ttak_thread_pool_t *work_pool = g_work_pool;
    ttak_thread_pool_t *service_pool = g_service_pool;
    g_work_pool = NULL;
    g_service_pool = NULL;
    pthread_rwlock_unlock(&g_pool_lock);
    if (service_pool) ttak_thread_pool_destroy(service_pool);
    if (work_pool) ttak_thread_pool_destroy(work_pool);
}

bool engine_pool_schedule(ttak_task_func_t func,
                          void *arg,
                          uint64_t hash,
                          ttak_task_domain_t domain,
                          uint8_t urgency) {
    pthread_rwlock_rdlock(&g_pool_lock);
    bool ok = g_accepting_work && schedule_on_pool(g_work_pool, func, arg, hash, domain, urgency);
    pthread_rwlock_unlock(&g_pool_lock);
    return ok;
}

bool engine_pool_schedule_service(ttak_task_func_t func,
                                  void *arg,
                                  uint64_t hash,
                                  ttak_task_domain_t domain,
                                  uint8_t urgency) {
    pthread_rwlock_rdlock(&g_pool_lock);
    bool ok = g_accepting_work && schedule_on_pool(g_service_pool, func, arg, hash, domain, urgency);
    pthread_rwlock_unlock(&g_pool_lock);
    return ok;
}

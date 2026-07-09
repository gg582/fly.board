#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "tasfa_internal.h"

_Atomic(ttak_thread_pool_t *) g_tasfa_pool = NULL;
pthread_once_t g_scheduler_once = PTHREAD_ONCE_INIT;
_Atomic unsigned int g_round_robin_idx = 0;

static unsigned int hash_upload_id(const char *id) {
    unsigned int h = 5381;
    int c;
    while ((c = *id++)) h = ((h << 5) + h) + c;
    return h;
}

static void *tasfa_pool_job_run(void *arg) {
    tasfa_job_t *job = (tasfa_job_t *)arg;
    if (job && job->func) {
        job->func(job->arg);
        if (job->free_after_done) {
            free(job);
        }
    }
    return NULL;
}

static void tasfa_scheduler_init_impl(void) {
    int n = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 2) n = 2;
    if (n > TASFA_MAX_WORKERS) n = TASFA_MAX_WORKERS;
    ttak_thread_pool_t *pool = ttak_thread_pool_create((size_t)n, 0, ttak_get_tick_count());
    if (!pool) {
        fprintf(stderr, "[TASFA] Failed to initialize libttak scheduler pool.\n");
    } else {
        atomic_store_explicit(&g_tasfa_pool, pool, memory_order_release);
    }
}

static void tasfa_scheduler_ensure_init(void) {
    pthread_once(&g_scheduler_once, tasfa_scheduler_init_impl);
}

void tasfa_scheduler_submit(const char *upload_id, void (*func)(void *), void *arg, tasfa_job_t *job) {
    tasfa_scheduler_ensure_init();
    uint64_t hash;
    if (upload_id) {
        hash = (uint64_t)hash_upload_id(upload_id);
    } else {
        hash = (uint64_t)atomic_fetch_add_explicit(&g_round_robin_idx, 1, memory_order_relaxed);
    }

    job->func = func;
    job->arg = arg;

    uint64_t now = ttak_get_tick_count();
    ttak_task_t *task = ttak_task_create(tasfa_pool_job_run, job, NULL, now);
    if (task) {
        ttak_task_set_hash(task, hash);
        ttak_task_set_domain(task, TTAK_TASK_DOMAIN_IO);
        ttak_task_set_urgency(task, 70);
        ttak_thread_pool_t *pool = atomic_load_explicit(&g_tasfa_pool, memory_order_acquire);
        if (pool && ttak_thread_pool_schedule_task(pool, task, 0, now)) {
            return;
        }
        ttak_task_destroy(task, now);
    }

    tasfa_pool_job_run(job);
}

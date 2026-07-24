#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "tasfa_internal.h"
#include "engine/pool.h"

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

void tasfa_scheduler_submit(const char *upload_id, void (*func)(void *), void *arg, tasfa_job_t *job) {
    if (!job || !func) return;
    job->func = func;
    job->arg = arg;
    uint64_t hash = upload_id && upload_id[0] ? (uint64_t)hash_upload_id(upload_id) : 0;
    if (engine_pool_schedule(tasfa_pool_job_run, job, hash, TTAK_TASK_DOMAIN_IO, 70)) {
        return;
    }
    /* Do not run expensive media work on an HTTP worker when the executor is
     * stopping.  The caller retains non-owned jobs; owned jobs are released. */
    FLY_LOG_ERROR("TASFA scheduler rejected background job");
    if (job->free_after_done) free(job);
}

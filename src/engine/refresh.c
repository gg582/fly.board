#define _POSIX_C_SOURCE 200809L
#include "engine/refresh.h"
#include "utils/reqshare.h"
#include <cwist/core/log.h>
#include <cwist/core/mem/gc.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct refresh_job {
    char *key;
    engine_refresh_func refresh;
    engine_refresh_free_func free_arg;
    void *arg;
    struct refresh_job *next;
} refresh_job;

static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;
static refresh_job *g_head;
static refresh_job *g_tail;
static size_t g_len;
static pid_t g_owner;
static bool g_workers_ok;

static size_t env_size(const char *name, size_t fallback, size_t min, size_t max) {
    const char *value = getenv(name);
    if (!value || !value[0]) return fallback;
    char *end = NULL;
    unsigned long parsed = strtoul(value, &end, 10);
    if (end && *end == '\0' && parsed >= min && parsed <= max) return (size_t)parsed;
    return fallback;
}

static void job_destroy(refresh_job *job) {
    if (!job) return;
    if (job->free_arg) job->free_arg(job->arg);
    free(job->key);
    free(job);
}

static void *refresh_worker(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&g_mtx);
        while (!g_head) pthread_cond_wait(&g_cond, &g_mtx);
        refresh_job *job = g_head;
        g_head = job->next;
        if (!g_head) g_tail = NULL;
        g_len--;
        pthread_mutex_unlock(&g_mtx);

        job->refresh(job->arg);
        reqshare_finish(job->key, NULL);
        job_destroy(job);
        cwist_gc_scope_flush();
    }
    return NULL;
}

static bool ensure_workers_locked(void) {
    pid_t pid = getpid();
    if (g_owner == pid) return g_workers_ok;
    g_head = g_tail = NULL;
    g_len = 0;
    g_owner = pid;
    g_workers_ok = false;
    size_t count = env_size("FLY_PAGE_REFRESH_WORKERS", 2, 1, 16);
    size_t started = 0;
    for (size_t i = 0; i < count; i++) {
        pthread_t thread;
        if (pthread_create(&thread, NULL, refresh_worker, NULL) != 0) break;
        pthread_detach(thread);
        started++;
    }
    g_workers_ok = started > 0;
    return g_workers_ok;
}

bool engine_refresh_schedule(const char *key, engine_refresh_func refresh,
                             engine_refresh_free_func free_arg, void *arg) {
    if (!key || !refresh) {
        if (free_arg) free_arg(arg);
        return false;
    }
    if (!reqshare_try_start(key)) {
        if (free_arg) free_arg(arg);
        return true;
    }

    refresh_job *job = calloc(1, sizeof(*job));
    if (job) job->key = strdup(key);
    if (!job || !job->key) {
        if (job) free(job);
        if (free_arg) free_arg(arg);
        reqshare_finish(key, NULL);
        return false;
    }
    job->refresh = refresh;
    job->free_arg = free_arg;
    job->arg = arg;

    pthread_mutex_lock(&g_mtx);
    size_t max = env_size("FLY_PAGE_REFRESH_QUEUE_MAX", 64, 1, 4096);
    bool ok = ensure_workers_locked() && g_len < max;
    if (ok) {
        if (g_tail) g_tail->next = job;
        else g_head = job;
        g_tail = job;
        g_len++;
        pthread_cond_signal(&g_cond);
    }
    pthread_mutex_unlock(&g_mtx);
    if (!ok) {
        reqshare_finish(key, NULL);
        job_destroy(job);
    }
    return ok;
}

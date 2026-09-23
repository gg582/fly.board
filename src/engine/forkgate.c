#define _POSIX_C_SOURCE 200809L
#include "engine/forkgate.h"
#include <cwist/core/log.h>
#include <pthread.h>
#include <stdbool.h>
#include <errno.h>
#include <time.h>

static pthread_mutex_t g_forkgate_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_forkgate_cond = PTHREAD_COND_INITIALIZER;
/* Number of threads currently inside a fork-unsafe section. */
static unsigned g_forkgate_active = 0;
/* Set by the atfork prepare handler while a fork is being serialized. */
static bool g_fork_pending = false;
/* give up waiting after this long so a leaked enter() can never wedge
 * the accept loop's fork; the child then inherits whatever state exists,
 * which is the pre-gate behavior. */
#define FORKGATE_PREPARE_MAX_SECS 30
#define FORKGATE_PREPARE_SLICE_MS 50

void fly_forkgate_enter(void) {
    pthread_mutex_lock(&g_forkgate_mtx);
    while (g_fork_pending) {
        pthread_cond_wait(&g_forkgate_cond, &g_forkgate_mtx);
    }
    g_forkgate_active++;
    pthread_mutex_unlock(&g_forkgate_mtx);
}

void fly_forkgate_leave(void) {
    pthread_mutex_lock(&g_forkgate_mtx);
    if (g_forkgate_active > 0) g_forkgate_active--;
    if (g_forkgate_active == 0) {
        pthread_cond_broadcast(&g_forkgate_cond);
    }
    pthread_mutex_unlock(&g_forkgate_mtx);
}

static void fly_forkgate_prepare(void) {
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += FORKGATE_PREPARE_MAX_SECS;

    pthread_mutex_lock(&g_forkgate_mtx);
    g_fork_pending = true;
    /* Wake any thread parked in enter() so it re-checks the flag and
     * stays out until the fork completes. */
    pthread_cond_broadcast(&g_forkgate_cond);
    while (g_forkgate_active > 0) {
        struct timespec now, wake_at;
        clock_gettime(CLOCK_REALTIME, &now);
        wake_at = now;
        wake_at.tv_nsec += FORKGATE_PREPARE_SLICE_MS * 1000000L;
        if (wake_at.tv_nsec >= 1000000000L) {
            wake_at.tv_sec++;
            wake_at.tv_nsec -= 1000000000L;
        }
        int rc = pthread_cond_timedwait(&g_forkgate_cond, &g_forkgate_mtx, &wake_at);
        if (rc == ETIMEDOUT && now.tv_sec >= deadline.tv_sec) {
            CWIST_LOG_ERROR("forkgate: fork proceeding with %u active fork-unsafe section(s) after %ds",
                            g_forkgate_active, FORKGATE_PREPARE_MAX_SECS);
            break;
        }
    }
    pthread_mutex_unlock(&g_forkgate_mtx);
}

static void fly_forkgate_parent(void) {
    pthread_mutex_lock(&g_forkgate_mtx);
    g_fork_pending = false;
    pthread_cond_broadcast(&g_forkgate_cond);
    pthread_mutex_unlock(&g_forkgate_mtx);
}

static void fly_forkgate_child(void) {
    /* The child is single-threaded at this point; reset the counters so
     * inherited state can never wedge a later enter(). */
    pthread_mutex_lock(&g_forkgate_mtx);
    g_fork_pending = false;
    g_forkgate_active = 0;
    pthread_cond_broadcast(&g_forkgate_cond);
    pthread_mutex_unlock(&g_forkgate_mtx);
}

void fly_forkgate_install(void) {
    pthread_atfork(fly_forkgate_prepare, fly_forkgate_parent, fly_forkgate_child);
}

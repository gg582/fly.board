/**
 * @file self_probe.c
 * @brief Loopback liveness probe that logs when the server stops answering.
 *
 * Replicas have seen the primary stop answering for seconds to hours while
 * its own log stayed silent: a stall before any handler runs (accept, TLS,
 * reactor, the defer queue) never reaches the slow-request log.  This thread
 * issues GET /metrics against our own port, which walks that whole path plus
 * a DB read, and logs each slow or failed probe with per-phase timings, then
 * how long the stall lasted once probes succeed again.
 *
 * Runs in the supervisor only (started before the worker fork); the kernel
 * hands each probe connection to whichever worker accepts it.
 */

#define _POSIX_C_SOURCE 200809L
#include "utils/self_probe.h"
#include <cwist/core/log.h>
#include <cwist/core/mem/gc.h>
#include <cwist/sys/app/shutdown.h>
#include <curl/curl.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

#define SELF_PROBE_DEFAULT_SEC 10
#define SELF_PROBE_TIMEOUT_SEC 15L
#define SELF_PROBE_SLOW_MS 3000
/* While a stall lasts, repeat the warning at most this often. */
#define SELF_PROBE_REPEAT_SEC 60

static _Atomic bool g_probe_running = false;
static int g_probe_wake_fd = -1;
static pthread_t g_probe_thread;
static bool g_probe_thread_started = false;
static pid_t g_probe_thread_owner = 0;
static char g_probe_url[128];
static int g_probe_interval = SELF_PROBE_DEFAULT_SEC;

static size_t probe_discard(char *ptr, size_t size, size_t nmemb, void *userdata) {
    (void)ptr;
    (void)userdata;
    return size * nmemb;
}

static int64_t mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void *self_probe_worker(void *arg) {
    (void)arg;
    int64_t stall_since = 0;     /* first bad probe of the current stall */
    int64_t last_warn = 0;
    int bad_count = 0;

    while (atomic_load_explicit(&g_probe_running, memory_order_acquire)) {
        struct pollfd pfd = { .fd = g_probe_wake_fd, .events = POLLIN };
        int ready = poll(&pfd, 1, g_probe_interval * 1000);
        if (ready < 0 && errno != EINTR) break;
        if (ready > 0) break; /* woken by self_probe_stop() */
        if (!atomic_load_explicit(&g_probe_running, memory_order_acquire)) break;

        CURL *curl = curl_easy_init();
        if (!curl) continue;
        curl_easy_setopt(curl, CURLOPT_URL, g_probe_url);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, probe_discard);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, SELF_PROBE_TIMEOUT_SEC);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        /* Loopback to our own listener: the certificate names the public
         * host, not 127.0.0.1, and nothing secret travels on this request. */
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
        CURLcode rc = curl_easy_perform(curl);
        long status = 0;
        double t_conn = 0, t_tls = 0, t_first = 0, t_total = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        curl_easy_getinfo(curl, CURLINFO_CONNECT_TIME, &t_conn);
        curl_easy_getinfo(curl, CURLINFO_APPCONNECT_TIME, &t_tls);
        curl_easy_getinfo(curl, CURLINFO_STARTTRANSFER_TIME, &t_first);
        curl_easy_getinfo(curl, CURLINFO_TOTAL_TIME, &t_total);
        curl_easy_cleanup(curl);

        /* The listener closes as soon as shutdown starts, before this thread
         * is stopped; that refusal is not a stall. */
        if (!atomic_load(&g_cwist_running)) break;

        int64_t now = mono_ms();
        bool bad = rc != CURLE_OK || status != 200 || t_total * 1000.0 >= SELF_PROBE_SLOW_MS;
        if (bad) {
            if (stall_since == 0) stall_since = now;
            bad_count++;
            if (last_warn == 0 || now - last_warn >= SELF_PROBE_REPEAT_SEC * 1000) {
                CWIST_LOG_WARN("self-probe: GET /metrics %s (curl=%d %s http=%ld connect=%.2fs tls=%.2fs "
                               "first_byte=%.2fs total=%.2fs; %d bad probe(s) over %llds)",
                               rc == CURLE_OK && status == 200 ? "slow" : "failed",
                               (int)rc, curl_easy_strerror(rc), status, t_conn, t_tls, t_first, t_total,
                               bad_count, (long long)((now - stall_since) / 1000));
                last_warn = now;
            }
        } else if (stall_since != 0) {
            CWIST_LOG_WARN("self-probe: recovered after %llds (%d bad probe(s))",
                           (long long)((now - stall_since) / 1000), bad_count);
            stall_since = 0;
            last_warn = 0;
            bad_count = 0;
        }
        /* Long-lived thread: sweep per wakeup like the other watchdogs. */
        cwist_gc_scope_flush();
    }
    return NULL;
}

void self_probe_start(int port, bool use_tls) {
    const char *v = getenv("FLY_SELF_PROBE_SEC");
    if (v && v[0]) g_probe_interval = atoi(v);
    if (g_probe_interval <= 0 || port <= 0) {
        CWIST_LOG_INFO("Self-probe disabled");
        return;
    }
    snprintf(g_probe_url, sizeof(g_probe_url), "%s://127.0.0.1:%d/metrics", use_tls ? "https" : "http", port);
    g_probe_wake_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (g_probe_wake_fd < 0) {
        CWIST_LOG_ERROR("Self-probe: failed to create wake event");
        return;
    }
    atomic_store_explicit(&g_probe_running, true, memory_order_release);
    if (pthread_create(&g_probe_thread, NULL, self_probe_worker, NULL) != 0) {
        atomic_store_explicit(&g_probe_running, false, memory_order_release);
        close(g_probe_wake_fd);
        g_probe_wake_fd = -1;
        CWIST_LOG_ERROR("Self-probe: failed to start worker");
        return;
    }
    g_probe_thread_started = true;
    g_probe_thread_owner = getpid();
    CWIST_LOG_INFO("Self-probe started (%s every %ds)", g_probe_url, g_probe_interval);
}

void self_probe_stop(void) {
    /* Workers inherit the eventfd across fork; a worker's teardown writing
     * to it would stop the supervisor's probe thread. */
    if (g_probe_wake_fd < 0 || g_probe_thread_owner != getpid()) return;
    atomic_store_explicit(&g_probe_running, false, memory_order_release);
    uint64_t one = 1;
    (void)write(g_probe_wake_fd, &one, sizeof(one));
}

void self_probe_join(void) {
    if (g_probe_thread_started && g_probe_thread_owner == getpid()) {
        pthread_join(g_probe_thread, NULL);
    }
    g_probe_thread_started = false;
    if (g_probe_wake_fd >= 0) {
        close(g_probe_wake_fd);
        g_probe_wake_fd = -1;
    }
}

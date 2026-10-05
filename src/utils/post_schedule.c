#include "utils/post_schedule.h"
#include "utils/cache.h"
#include "db/db.h"
#include "engine/forkgate.h"
#include "nats/fly_nats.h"
#include <cwist/core/log.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

/* Catch-up sweep for announcements a crashed or racing save left behind. */
#define SWEEP_INTERVAL_SEC 60

/* MAP_SHARED before fork (same pattern as the write policy). */
typedef struct {
    _Atomic long long next_due; /* UTC epoch, 0 = nothing scheduled */
    _Atomic unsigned generation;
    _Atomic bool busy;
} shared_schedule_t;

static shared_schedule_t g_fallback;
static shared_schedule_t *g_sched = &g_fallback;

static pthread_t g_thread;
static bool g_thread_started = false;
static pid_t g_thread_owner = 0;
static _Atomic bool g_thread_running = false;
static cwist_db *g_thread_db = NULL;

void post_schedule_init(cwist_db *db) {
    void *mem = mmap(NULL, sizeof(shared_schedule_t), PROT_READ | PROT_WRITE,
                     MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) {
        CWIST_LOG_ERROR("Post schedule: shared memory unavailable; due posts surface per process");
    } else {
        g_sched = mem;
    }
    atomic_store(&g_sched->next_due, db ? db_post_next_scheduled(db) : 0);
}

void post_schedule_note(const char *publish_at) {
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    if (!publish_at || !strptime(publish_at, "%Y-%m-%d %H:%M:%S", &tm)) return;
    long long due = (long long)timegm(&tm);
    if (due <= (long long)time(NULL)) return;
    long long cur = atomic_load(&g_sched->next_due);
    while ((cur == 0 || due < cur) &&
           !atomic_compare_exchange_weak(&g_sched->next_due, &cur, due)) {
    }
}

void post_schedule_announce(cwist_db *db) {
    if (!db) return;
    cJSON *claimed = db_post_claim_unannounced(db);
    cJSON *row = NULL;
    cJSON_ArrayForEach(row, claimed) {
        cJSON *title = cJSON_GetObjectItem(row, "title");
        cJSON *slug = cJSON_GetObjectItem(row, "slug");
        cJSON *summary = cJSON_GetObjectItem(row, "summary");
        if (!cJSON_IsString(title) || !cJSON_IsString(slug)) continue;
        fly_nats_publish_post(title->valuestring, slug->valuestring,
                              cJSON_IsString(summary) ? summary->valuestring : "");
        CWIST_LOG_INFO("Post published: slug='%s'", slug->valuestring);
    }
    if (claimed) cJSON_Delete(claimed);
}

/* Runs the due work at most once across processes. */
static void run_due(cwist_db *db) {
    long long due = atomic_load_explicit(&g_sched->next_due, memory_order_relaxed);
    if (due == 0 || (long long)time(NULL) < due || !db) return;
    bool idle = false;
    if (!atomic_compare_exchange_strong(&g_sched->busy, &idle, true)) return;
    due = atomic_load(&g_sched->next_due);
    if (due != 0 && (long long)time(NULL) >= due) {
        long long next = db_post_next_scheduled(db);
        /* A failed exchange means a save just queued an earlier time; keep it. */
        atomic_compare_exchange_strong(&g_sched->next_due, &due, next);
        post_schedule_bump();
        /* Shared route BDR; worker page caches miss through the generation. */
        page_cache_clear();
        post_schedule_announce(db);
    }
    atomic_store(&g_sched->busy, false);
}

/* Lives in the master process, which never serves requests, so publishing
 * does not wait for traffic and costs the request path nothing. */
static void *schedule_worker(void *arg) {
    (void)arg;
    time_t last_sweep = time(NULL);
    struct timespec tick = { .tv_sec = 1, .tv_nsec = 0 };
    while (atomic_load_explicit(&g_thread_running, memory_order_acquire)) {
        nanosleep(&tick, NULL);
        time_t now = time(NULL);
        long long due = atomic_load_explicit(&g_sched->next_due, memory_order_relaxed);
        bool is_due = due != 0 && (long long)now >= due;
        bool sweep = now - last_sweep >= SWEEP_INTERVAL_SEC;
        if (!is_due && !sweep) continue;
        /* sqlite is fork-unsafe; cwist forks workers from this process. */
        fly_forkgate_enter();
        if (is_due) run_due(g_thread_db);
        if (sweep) {
            post_schedule_announce(g_thread_db);
            last_sweep = now;
        }
        fly_forkgate_leave();
    }
    return NULL;
}

void post_schedule_start(cwist_db *db) {
    if (g_thread_started || !db) return;
    g_thread_db = db;
    atomic_store(&g_thread_running, true);
    if (pthread_create(&g_thread, NULL, schedule_worker, NULL) != 0) {
        atomic_store(&g_thread_running, false);
        CWIST_LOG_ERROR("Failed to start post schedule worker; scheduled posts will not go public");
        return;
    }
    g_thread_started = true;
    g_thread_owner = getpid();
}

void post_schedule_stop(void) {
    /* Forked workers share this code path but not the thread. */
    if (!g_thread_started || getpid() != g_thread_owner) return;
    atomic_store(&g_thread_running, false);
    pthread_join(g_thread, NULL);
    g_thread_started = false;
}

void post_schedule_bump(void) {
    atomic_fetch_add(&g_sched->generation, 1);
}

unsigned post_schedule_generation(void) {
    return atomic_load_explicit(&g_sched->generation, memory_order_relaxed);
}

#include "utils/post_schedule.h"
#include "utils/cache.h"
#include "db/db.h"
#include "db/db_internal.h"
#include "db/subscriber.h"
#include "engine/forkgate.h"
#include "nats/fly_nats.h"
#include "config/config.h"
#include "utils/email.h"
#include <cwist/core/log.h>
#include <cwist/core/mem/gc.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

/* Defined in src/handlers/handlers.c next to json_int(); declared in
 * handlers_internal.h, which utils must not depend on. */
const char *json_string_or_empty(cJSON *obj, const char *key);

/* Catch-up sweep for announcements a crashed or racing save left behind. */
#define SWEEP_INTERVAL_SEC 60
/* Batched view-counter flush (src/db/post.c): eventual consistency within
 * this window for post view counts. */
#define VIEW_FLUSH_INTERVAL_SEC 30
/* WAL truncate checkpoint: autocheckpoint is PASSIVE-only and never shrinks
 * the WAL while readers hold snapshots, so a periodic TRUNCATE keeps the WAL
 * from growing toward journal_size_limit over uptime. */
#define WAL_TRUNCATE_INTERVAL_SEC 600
/* sync_journal retention purge (rows older than 7 days, see db_sync.c). */
#define JOURNAL_PURGE_INTERVAL_SEC 3600

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

/* ---- Newsletter: plain-text new-post email to active subscribers ----
 *
 * Gated twice: g_config.newsletter_enabled (blog.settings, replicated) and
 * FLY_NEWSLETTER_SEND (env, primary node only) - so replica nodes that also
 * claim announced posts never double-send. Each claimed post gets a detached
 * thread: it reads the subscriber list on its own sqlite connection under the
 * fork gate, then does the blocking SMTP sends off the scheduler tick. */

typedef struct {
    char *title;
    char *slug;
    char *summary;
    char db_path[512];
} newsletter_job_t;

static void *newsletter_worker(void *arg) {
    newsletter_job_t *j = (newsletter_job_t *)arg;
    if (!j) return NULL;
    cJSON *subs = NULL;
    /* sqlite is fork-unsafe; the fork gate keeps cwist from forking a worker
     * while this thread holds a connection. */
    fly_forkgate_enter();
    sqlite3 *conn = NULL;
    if (sqlite3_open(j->db_path, &conn) == SQLITE_OK && db_configure_connection(conn)) {
        cwist_db local = { .conn = conn };
        subs = db_subscriber_list_active(&local);
    }
    if (conn) sqlite3_close(conn);
    fly_forkgate_leave();
    if (subs) {
        const char *site = g_config.title[0] ? g_config.title : "New post";
        char subject[256];
        snprintf(subject, sizeof(subject), "%s: %s", site, j->title ? j->title : "");
        cJSON *s = NULL;
        int sent = 0, failed = 0;
        cJSON_ArrayForEach(s, subs) {
            const char *email = json_string_or_empty(s, "email");
            const char *token = json_string_or_empty(s, "token");
            if (!email[0]) continue;
            char body[2048];
            snprintf(body, sizeof(body),
                     "%s\n\n%s\n\nRead: %spost/%s\n\nUnsubscribe: %sunsubscribe?token=%s\n",
                     j->title ? j->title : "",
                     j->summary ? j->summary : "",
                     g_config.root_url, j->slug ? j->slug : "",
                     g_config.root_url, token);
            if (email_send(email, subject, body)) sent++;
            else failed++;
            /* Friendly-pace the relay so a burst of subscribers does not trip
             * its rate limits; the worker runs off the request path. */
            usleep(200 * 1000);
        }
        CWIST_LOG_INFO("Newsletter: post '%s' emailed to %d subscriber(s), %d failed",
                       j->slug ? j->slug : "", sent, failed);
        cJSON_Delete(subs);
    }
    free(j->title);
    free(j->slug);
    free(j->summary);
    free(j);
    return NULL;
}

static void newsletter_send_async(cwist_db *db, const char *title, const char *slug, const char *summary) {
    if (!g_config.newsletter_enabled) return;
    const char *gate = getenv("FLY_NEWSLETTER_SEND");
    if (!gate || gate[0] != '1') return;
    newsletter_job_t *j = calloc(1, sizeof(*j));
    if (!j) return;
    j->title = strdup(title ? title : "");
    j->slug = strdup(slug ? slug : "");
    j->summary = strdup(summary ? summary : "");
    const char *p = sqlite3_db_filename(fly_db_conn(db), "main");
    snprintf(j->db_path, sizeof(j->db_path), "%s", p ? p : "data/blog.db");
    pthread_t t;
    if (pthread_create(&t, NULL, newsletter_worker, j) == 0) {
        pthread_detach(t);
    } else {
        /* Last resort: run inline so the announcement is not lost. */
        newsletter_worker(j);
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
        newsletter_send_async(db, title->valuestring, slug->valuestring,
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
 * does not wait for traffic and costs the request path nothing.  This thread
 * is also the process-wide periodic maintenance timer: batched view-counter
 * flush (30 s), WAL truncate checkpoint (10 min) and sync_journal retention
 * purge (1 h). */
static void *schedule_worker(void *arg) {
    (void)arg;
    time_t last_sweep = time(NULL);
    time_t last_view_flush = time(NULL);
    time_t last_wal_truncate = time(NULL);
    time_t last_journal_purge = time(NULL);
    struct timespec tick = { .tv_sec = 1, .tv_nsec = 0 };
    while (atomic_load_explicit(&g_thread_running, memory_order_acquire)) {
        nanosleep(&tick, NULL);
        time_t now = time(NULL);
        long long due = atomic_load_explicit(&g_sched->next_due, memory_order_relaxed);
        bool is_due = due != 0 && (long long)now >= due;
        bool sweep = now - last_sweep >= SWEEP_INTERVAL_SEC;
        bool view_flush = now - last_view_flush >= VIEW_FLUSH_INTERVAL_SEC;
        bool wal_truncate = now - last_wal_truncate >= WAL_TRUNCATE_INTERVAL_SEC;
        bool journal_purge = now - last_journal_purge >= JOURNAL_PURGE_INTERVAL_SEC;
        if (!is_due && !sweep && !view_flush && !wal_truncate && !journal_purge) continue;
        /* sqlite is fork-unsafe; cwist forks workers from this process. */
        fly_forkgate_enter();
        if (is_due) run_due(g_thread_db);
        if (sweep) {
            post_schedule_announce(g_thread_db);
            last_sweep = now;
        }
        if (view_flush) {
            db_view_counters_flush();
            last_view_flush = now;
        }
        if (wal_truncate) {
            db_wal_checkpoint_truncate();
            last_wal_truncate = now;
        }
        if (journal_purge) {
            db_sync_journal_purge(g_thread_db);
            /* Stale pending subscriptions (expired confirm links). */
            db_subscriber_purge_pending(g_thread_db);
            last_journal_purge = now;
        }
        fly_forkgate_leave();
        /* CWIST full GC is enabled process-wide; a long-lived thread like
         * this one would otherwise hold tracked allocations until exit. */
        cwist_gc_scope_flush();
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
    /* Clean shutdown hook: no workers or forks are running anymore, so one
     * last flush pushes any remaining batched view counters to SQLite. */
    db_view_counters_flush();
}

void post_schedule_bump(void) {
    atomic_fetch_add(&g_sched->generation, 1);
}

unsigned post_schedule_generation(void) {
    return atomic_load_explicit(&g_sched->generation, memory_order_relaxed);
}

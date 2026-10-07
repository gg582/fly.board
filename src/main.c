#define _POSIX_C_SOURCE 200809L
#include "handlers/handlers.h"
#include "auth/auth.h"
#include "crypto/fly_crypto.h"
#include "db/db.h"
#include "utils/spam_guard.h"
#include "tools/backup.h"
#include <cwist/core/mem/alloc.h>
#include <cwist/core/mem/gc.h>
#include "db/db_internal.h"
#include "utils/media_preview.h"
#include "cwist/board_tree.h"
#include "nats/fly_nats.h"
#include "config/config.h"
#include "config/write_policy.h"
#include "engine/pool.h"
#include "engine/forkgate.h"
#include "engine/nats.h"
#include "engine/db.h"
#include "engine/settings.h"
#include "engine/routes.h"
#include "engine/async_route.h"
#include "engine/bdr.h"
#include "utils/post_schedule.h"
#include "engine/warmup.h"
#include "engine/flywire.h"
#include "utils/cache.h"
#include "utils/reqshare.h"
#include "utils/image_inline.h"
#include "utils/image_invert.h"
#include "utils/cert_renewal.h"
#include <cwist/net/http/http3.h>
#include <openssl/ssl.h>
#include <cwist/sys/app/app.h>
#include <cwist/sys/app/compress.h>
#include <ttak/async/task.h>
#include <ttak/timing/timing.h>
#include <curl/curl.h>
#include <signal.h>
#if defined __has_include
#  if __has_include (<cwist/security/tls/ech.h>)
#    define HAVE_ECH 1
#    include <cwist/security/tls/ech.h>
#  endif
#endif
#include <cwist/core/log.h>
#include <sqlite3.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <strings.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <limits.h>

#define BLOG_CERT "server.crt"
#define BLOG_KEY  "server.key"

static _Atomic bool g_cleanup_running = false;
static int g_cleanup_wake_fd = -1;
static pthread_t g_cleanup_thread;
static bool g_cleanup_thread_started = false;
/* cwist_app_listen() forks worker processes; the thread exists only in the
 * process that created it, so children must not join the inherited handle. */
static pid_t g_cleanup_thread_owner = 0;

/* Wake the cleanup worker immediately during shutdown instead of making it
 * periodically poll just to observe g_cleanup_running. */
static void cleanup_stop(void) {
    atomic_store_explicit(&g_cleanup_running, false, memory_order_release);
    if (g_cleanup_wake_fd >= 0) {
        uint64_t one = 1;
        (void)write(g_cleanup_wake_fd, &one, sizeof(one));
    }
}

static void cleanup_close_wake_fd(void) {
    if (g_cleanup_wake_fd >= 0) {
        close(g_cleanup_wake_fd);
        g_cleanup_wake_fd = -1;
    }
}

static void cleanup_join(void) {
    if (g_cleanup_thread_started && g_cleanup_thread_owner == getpid()) {
        pthread_join(g_cleanup_thread, NULL);
    }
    g_cleanup_thread_started = false;
}

/* Full preview regeneration can launch many ffmpeg processes.  New uploads
 * and TASFA assets already create their derivatives on demand, so reserve a
 * complete legacy backfill for an explicit maintenance run. */
static bool startup_media_backfill_enabled(void) {
    const char *value = getenv("FLYBOARD_MEDIA_BACKFILL_ON_START");
    return value && (strcmp(value, "1") == 0 ||
                     strcasecmp(value, "true") == 0 ||
                     strcasecmp(value, "on") == 0);
}

static bool dir_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool ensure_workdir_with_public(const char *root) {
    if (!root || !root[0]) return false;
    char public_path[PATH_MAX];
    int result = snprintf(public_path, sizeof(public_path), "%s/public", root);
    if (result < 0 || result >= (int)sizeof(public_path)) return false;
    if (!dir_exists(public_path)) return false;
    return chdir(root) == 0;
}

static bool ensure_asset_workdir(void) {
    if (dir_exists("public")) return true;
    const char *env_root = getenv("BLOG_ROOT");
    if (ensure_workdir_with_public(env_root)) return true;
#if defined(__linux__)
    char exe_path[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len <= 0 || len >= (ssize_t)(sizeof(exe_path) - 1)) return dir_exists("public");
    exe_path[len] = '\0';
    char *slash = strrchr(exe_path, '/');
    if (!slash) return dir_exists("public");
    *slash = '\0';
    if (ensure_workdir_with_public(exe_path)) return true;
#endif
    return dir_exists("public");
}

static int create_daily_3am_timer(void) {
    int fd = timerfd_create(CLOCK_REALTIME, 0);
    if (fd < 0) return -1;

    time_t now = time(NULL);
    struct tm *tm_now = localtime(&now);
    struct tm target = *tm_now;
    target.tm_hour = 3;
    target.tm_min = 0;
    target.tm_sec = 0;
    time_t target_time = mktime(&target);
    if (target_time <= now) target_time += 24 * 3600;

    struct itimerspec its;
    its.it_value.tv_sec = target_time - now;
    its.it_value.tv_nsec = 0;
    its.it_interval.tv_sec = 24 * 3600;
    its.it_interval.tv_nsec = 0;

    if (timerfd_settime(fd, 0, &its, NULL) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void *cleanup_worker(void *arg) {
    (void)arg;
    /* The connection is opened lazily per daily job and closed right after,
     * never held across cwist_app_listen()'s worker fork(): a connection
     * left open by a parent thread at fork time is inherited by every child
     * as a dead copy of the parent's sqlite state (the child must never
     * close it, so LeakSanitizer reports the whole connection in every
     * child).  The fork gate brackets every sqlite section so a fork can
     * never land inside a call either (src/engine/forkgate.h). */
    /* localtime() inside the timer setup is a fork hazard while cwist forks
     * workers; tzset() in main() makes it a cached fast path, and the gate
     * covers the residual window. */
    fly_forkgate_enter();
    int tfd = create_daily_3am_timer();
    fly_forkgate_leave();
    if (tfd < 0) {
        CWIST_LOG_ERROR("Failed to create cleanup timerfd");
        return NULL;
    }
    int epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0) {
        FLY_LOG_ERROR("Failed to create cleanup epoll instance");
        close(tfd);
        return NULL;
    }
    struct epoll_event event = { .events = EPOLLIN };
    event.data.fd = tfd;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, tfd, &event) < 0) {
        FLY_LOG_ERROR("Failed to register cleanup timer");
        close(epfd);
        close(tfd);
        return NULL;
    }
    event.data.fd = g_cleanup_wake_fd;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, g_cleanup_wake_fd, &event) < 0) {
        FLY_LOG_ERROR("Failed to register cleanup wake event");
        close(epfd);
        close(tfd);
        return NULL;
    }

    uint64_t exp;
    while (atomic_load_explicit(&g_cleanup_running, memory_order_acquire)) {
        struct epoll_event ready_event;
        int ready = epoll_wait(epfd, &ready_event, 1, -1);
        if (ready < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (ready_event.data.fd == g_cleanup_wake_fd) break;
        if (ready_event.data.fd != tfd) continue;
        ssize_t s = read(tfd, &exp, sizeof(exp));
        if (s != sizeof(exp)) continue;
        /* Never share the request-serving connection with this worker.
         * A dedicated WAL connection keeps its statements and busy state
         * isolated from HTTP handlers. */
        fly_forkgate_enter();
        sqlite3 *conn = NULL;
        if (sqlite3_open_v2("data/blog.db", &conn, SQLITE_OPEN_READWRITE, NULL) == SQLITE_OK &&
            db_configure_connection(conn)) {
            /* The open -> sweep -> close sequence below is ONE continuous
             * fork-gate section: a fork can never land mid-write on this
             * connection (audit finding), and the child inherits no live
             * sqlite state. */
            cwist_db db = { .conn = conn };
            db_cleanup_orphaned_files(&db);
            int expired_users = db_user_delete_unverified_expired(&db);
            if (expired_users > 0) {
                CWIST_LOG_INFO("Removed %d unverified account%s past the 24h verification window",
                               expired_users, expired_users == 1 ? "" : "s");
            }
            unsigned long long freed = 0;
            int swept = tasfa_sweep_stale_sessions(false, &freed);
            if (swept > 0) CWIST_LOG_INFO("Removed %d abandoned transfer sessions (%.1f MB)", swept, freed / 1048576.0);
            sqlite3_close(conn);
            fly_forkgate_leave();
            /* Scheduled backup runs as its own process. It is deliberately
             * OUTSIDE the gate above so the forked child never inherits a
             * held fork-gate lock; it needs no sqlite handle. */
            backup_settings_t bs;
            if (backup_settings_load(&bs) && backup_settings_ready(&bs)) {
                if (backup_spawn()) CWIST_LOG_INFO("Scheduled backup started");
                else FLY_LOG_ERROR("Scheduled backup could not be started");
            }
            memset(&bs, 0, sizeof(bs));
        } else {
            FLY_LOG_ERROR("Failed to open cleanup database connection");
            fly_forkgate_leave();
            if (conn) sqlite3_close(conn);
        }
    }
    close(epfd);
    close(tfd);
    return NULL;
}

/* --sign-posts: sign every post whose stored signature does not verify
 * under the current key (unsigned posts, or ones signed by a lost key),
 * then exit. Kept out of normal startup on purpose: re-signing silently on
 * every boot would also bless content altered directly in the database. */
static int sign_posts_backfill(cwist_db *db) {
    cJSON *posts = db_post_list_for_signing(db);
    if (!posts) {
        FLY_LOG_ERROR("sign-posts: could not list posts");
        return 1;
    }
    int total = 0, valid = 0, signed_now = 0, failed = 0;
    cJSON *p = NULL;
    cJSON_ArrayForEach(p, posts) {
        total++;
        cJSON *id = cJSON_GetObjectItem(p, "id");
        cJSON *title = cJSON_GetObjectItem(p, "title");
        cJSON *content = cJSON_GetObjectItem(p, "content");
        cJSON *sig = cJSON_GetObjectItem(p, "pqc_signature");
        const char *t = cJSON_IsString(title) ? title->valuestring : "";
        const char *c = cJSON_IsString(content) ? content->valuestring : "";
        /* Same message the post page verifies: title + "\n" + content. */
        size_t mlen = strlen(t) + 1 + strlen(c);
        char *msg = (char *)malloc(mlen + 1);
        if (!msg) { failed++; continue; }
        snprintf(msg, mlen + 1, "%s\n%s", t, c);
        if (cJSON_IsString(sig) && fly_crypto_verify((const uint8_t *)msg, mlen, sig->valuestring)) {
            valid++;
        } else {
            char *new_sig = NULL;
            if (fly_crypto_sign((const uint8_t *)msg, mlen, &new_sig) &&
                db_post_set_signature(db, cJSON_IsNumber(id) ? id->valueint : 0, new_sig)) {
                signed_now++;
            } else {
                failed++;
            }
            if (new_sig) cwist_free(new_sig);
        }
        free(msg);
    }
    cJSON_Delete(posts);
    CWIST_LOG_INFO("sign-posts: %d posts, %d already valid, %d signed, %d failed", total, valid, signed_now, failed);
    fprintf(stderr, "sign-posts: %d posts, %d already valid, %d signed, %d failed\n", total, valid, signed_now, failed);
    return failed ? 1 : 0;
}

int main(int argc, char **argv) {
    /* Enable CWIST full GC before ANY allocation, thread creation, or CWIST
     * call: cwist_full_gc() latches on the first call, and from then on every
     * cwist_alloc()/cJSON block is tracked on the allocating thread's
     * pending-sweep list. Plain free() on such a pointer would double-free at
     * the next scope flush, so all tracked frees go through cwist_free(). */
    cwist_full_gc(true);
    bool sign_posts_mode = argc > 1 && strcmp(argv[1], "--sign-posts") == 0;
    /* SQLite must be configured before any connection is opened.  Use the
     * serialized threading mode so a single connection can be safely shared
     * across multiple worker threads. */
    sqlite3_config(SQLITE_CONFIG_SERIALIZED);

    /* Serve through cwist's event-driven C1M reactor path; HTTPS handshakes
     * are shepherded non-blocking there, so churn cannot park the accept
     * loop. The environment can still force the legacy pool path. */
    setenv("CWIST_C1M_MODE", "1", 0);
    signal(SIGPIPE, SIG_IGN);
    /* libcurl requires curl_global_init() to run once, before any thread
     * calls curl_easy_init(), and explicitly forbids relying on the
     * implicit lazy init inside curl_easy_init() in a multi-threaded
     * program (src/handlers/api.c and src/utils/s3_client.c both call
     * libcurl from worker-pool threads). Without this, concurrent
     * first-use races reinitialize global state (OpenSSL engines, NSS,
     * etc.) repeatedly instead of once, which is UB and leaks - and the
     * more concurrent curl traffic (e.g. translation requests), the
     * worse it gets. Do this before engine_pool_init() spawns workers. */
    curl_global_init(CURL_GLOBAL_DEFAULT);
    fly_log_init();
    /* Initialize the process timezone before any thread is created. The
     * first localtime()/gmtime() call reads /etc/localtime while holding
     * glibc's tzset_lock, and that slow path is a fork hazard: a worker
     * forked while another thread sits inside it inherits the locked
     * tzset_lock and every later date-header render in the child deadlocks
     * (observed as a worker wedged on g_date_lock/tzset_lock, hanging the
     * supervisor's shutdown waitpid). After this call the per-call lock
     * hold is a cached microsecond path. */
    tzset();
    if (!ensure_asset_workdir()) {
        FLY_LOG_ERROR("Public assets not found; set BLOG_ROOT or run from project root");
        return 1;
    }
    CWIST_LOG_INFO("Workdir verified");
    /* Backup tooling runs before the signing key is loaded: a restore must
     * be able to bring the old seed back instead of minting a new one. */
    int backup_rc = fly_backup_cli(argc, argv);
    if (backup_rc >= 0) return backup_rc;

    if (argc > 1 && strcmp(argv[1], "--sweep-uploads") == 0) {
        bool dry = argc > 2 && strcmp(argv[2], "--dry-run") == 0;
        unsigned long long freed = 0;
        int n = tasfa_sweep_stale_sessions(dry, &freed);
        fprintf(stderr, "%s %d abandoned transfer sessions, %.1f MB\n", dry ? "would remove" : "removed", n,
                freed / 1048576.0);
        return 0;
    }

    if (!fly_crypto_init("data/.pqc_mldsa65_seed")) {
        FLY_LOG_ERROR("PQC crypto init failed");
        return 1;
    }
    CWIST_LOG_INFO("Crypto initialized");

    if (!auth_jwt_init("data/.jwt_secret")) {
        FLY_LOG_ERROR("JWT secret initialization failed");
        fly_crypto_cleanup();
        return 1;
    }
    CWIST_LOG_INFO("JWT secret initialized");

    if (!engine_settings_load()) {
        fly_crypto_cleanup();
        return 1;
    }
    image_inline_cache_build();
    CWIST_LOG_INFO("Inline image cache built");
    image_invert_cache_build();

    if (!engine_pool_init()) {
        fly_crypto_cleanup();
        return 1;
    }

    if (!engine_nats_init()) {
        engine_pool_shutdown();
        fly_crypto_cleanup();
        return 1;
    }

    cwist_app *app = cwist_app_create();
    if (!app) {
        FLY_LOG_ERROR("Failed to create app");
        engine_nats_stop();
        engine_pool_shutdown();
        fly_crypto_cleanup();
        return 1;
    }
    CWIST_LOG_INFO("CWIST app created");

    cwist_db *db = NULL;
    if (!engine_db_init(app, &db)) {
        engine_nats_stop();
        engine_pool_shutdown();
        cwist_app_destroy(app);
        fly_crypto_cleanup();
        return 1;
    }

    int site_admin_uid = db_user_ensure_site_admin(db, auth_admin_username());
    if (site_admin_uid <= 0) {
        FLY_LOG_ERROR("Failed to set up the admin.settings account in the users table");
        engine_nats_stop();
        engine_pool_shutdown();
        cwist_app_destroy(app);
        fly_crypto_cleanup();
        return 1;
    }
    if (!db_pqc_keys_sync(db)) {
        FLY_LOG_ERROR("Failed to register the post signing key");
        engine_nats_stop();
        engine_pool_shutdown();
        cwist_app_destroy(app);
        fly_crypto_cleanup();
        return 1;
    }
    {
        unsigned long long freed = 0;
        int swept = tasfa_sweep_stale_sessions(false, &freed);
        if (swept > 0) CWIST_LOG_INFO("Removed %d abandoned transfer sessions (%.1f MB)", swept, freed / 1048576.0);
    }
    if (sign_posts_mode) {
        int rc = sign_posts_backfill(db);
        engine_nats_stop();
        engine_pool_shutdown();
        cwist_app_destroy(app);
        fly_crypto_cleanup();
        return rc;
    }
    auth_site_admin_set_uid(site_admin_uid);
    CWIST_LOG_INFO("Site admin account: users.id=%d", site_admin_uid);
    write_policy_init(db);
    spam_guard_init();
    post_schedule_init(db);
    db_file_cleanup_duplicates(db);
    if (startup_media_backfill_enabled()) {
        media_preview_backfill(db);
        media_preview_backfill_static_assets();
    } else {
        CWIST_LOG_INFO("Startup media backfill skipped; set FLYBOARD_MEDIA_BACKFILL_ON_START=1 for maintenance");
    }
    db_cleanup_orphaned_files(db);
    CWIST_LOG_INFO("Orphaned files cleanup completed");

    page_cache_init();
    reqshare_init();
    page_cache_warmup(db);

    g_cleanup_wake_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (g_cleanup_wake_fd < 0) {
        FLY_LOG_ERROR("Failed to create cleanup wake event");
    } else {
        atomic_store_explicit(&g_cleanup_running, true, memory_order_release);
    }
    if (g_cleanup_wake_fd >= 0 &&
        pthread_create(&g_cleanup_thread, NULL, cleanup_worker, NULL) != 0) {
        cleanup_stop();
        FLY_LOG_ERROR("Failed to start cleanup worker");
    } else if (g_cleanup_wake_fd >= 0) {
        g_cleanup_thread_started = true;
        g_cleanup_thread_owner = getpid();
    }

    post_schedule_start(db);

    /* FlyWire replica apply loop: like the cleanup worker above, this
     * thread is created before cwist_app_listen() and therefore exists only
     * in the supervisor process, never in forked workers. */
    flywire_start();

    cwist_app_set_max_memspace(app, CWIST_MIB(512));
    cwist_app_configure_bdr(app, CWIST_MIB(256), 600, 250000);
    engine_bdr_init(app);

    if (g_config.use_http2) {
        cwist_app_use_https2(app, true);
        CWIST_LOG_INFO("HTTP/2 enabled");
    }
    if (g_config.use_http3) {
        cwist_app_use_https3(app, true);
        CWIST_LOG_INFO("HTTP/3 enabled");
        if (app->h3_ctx && app->h3_ctx->ssl_ctx) {
#if defined(SSL_CTX_set_early_data_enabled)
            SSL_CTX_set_early_data_enabled(app->h3_ctx->ssl_ctx, 1);
#elif defined(SSL_CTX_set_max_early_data)
            SSL_CTX_set_max_early_data(app->h3_ctx->ssl_ctx, 0xFFFFFFFF);
#endif
            SSL_CTX_set_session_cache_mode(app->h3_ctx->ssl_ctx, SSL_SESS_CACHE_SERVER);
            SSL_CTX_set_num_tickets(app->h3_ctx->ssl_ctx, 2);
            CWIST_LOG_INFO("HTTP/3 0-RTT early data and session resumption enabled");
        }
    }
    if (g_config.use_tls) {
        cwist_error_t tls = cwist_app_use_https(app, BLOG_CERT, BLOG_KEY);
        if (tls.errtype != CWIST_ERR_INT16 || tls.error.err_i16 != 0) {
            FLY_LOG_ERROR("HTTPS init failed; run ./keygen.sh first");
            cleanup_stop();
            cleanup_join();
            engine_nats_stop();
            engine_pool_shutdown();
            cleanup_close_wake_fd();
            cwist_app_destroy(app);
            return 1;
        }
        CWIST_LOG_INFO("HTTPS initialized");
    } else {
        CWIST_LOG_INFO("TLS disabled, running plain HTTP");
    }

    const char *ech_key = getenv("BLOG_ECH_KEY");
    const char *ech_dir = getenv("BLOG_ECH_DIR");
    if (ech_key || ech_dir) {
        cwist_error_t ech = cwist_app_use_ech(app, ech_key, ech_dir);
        if (ech.errtype != CWIST_ERR_INT16 || ech.error.err_i16 != 0) {
            FLY_LOG_ERROR("ECH init failed");
            cleanup_stop();
            cleanup_join();
            engine_nats_stop();
            engine_pool_shutdown();
            cleanup_close_wake_fd();
            cwist_app_destroy(app);
            return 1;
        }
        CWIST_LOG_INFO("ECH initialized");
    }

    /* Daily ACME renewal watchdog; no-op unless FLY_CERT_RENEWAL=true. */
    if (g_config.use_tls) {
        cert_renewal_start(app);
    }

    /* Register payload compression backends in preference order:
     * brotli (best ratio) → zstd (balanced speed + ratio) → gzip (widest compat).
     * cwist_mw_compress picks the first backend the client's Accept-Encoding supports. */
    cwist_compress_unregister_all();
    cwist_compress_register_backend(cwist_compress_backend_brotli());
    cwist_compress_register_backend(cwist_compress_backend_zstd());
    cwist_compress_register_backend(cwist_compress_backend_gzip());
    /* The async gate must be the outermost middleware (see
     * engine/async_route.c); deferred responses are compressed on the
     * request worker instead of in the chain. */
    cwist_middleware_func compress_mw = cwist_mw_compress(1024);
    engine_async_init(app, compress_mw, global_middleware_finish);
    cwist_app_use(app, compress_mw);
    CWIST_LOG_INFO("Compression middleware registered (brotli > zstd > gzip, min 1 KiB)");

    engine_routes_register(app);

    /* cwist_app_listen() forks worker children from this thread, and pthread
     * TLS values survive fork for the forking thread. Close this thread's
     * per-thread DB connections so children inherit no sqlite state copies
     * (see fly_db_close_thread_conns). */
    fly_db_close_thread_conns();



    if (g_config.use_tls) {
        if (g_config.use_http3) {
            CWIST_LOG_INFO("Starting server on port %d (HTTP/3 on UDP %d)", g_config.port, g_config.port);
            printf("Docker Blog: https://localhost:%d (HTTP/3 on UDP %d)\n", g_config.port, g_config.port);
        } else if (g_config.use_http2) {
            CWIST_LOG_INFO("Starting server on port %d (HTTP/2)", g_config.port);
            printf("Docker Blog: https://localhost:%d (HTTP/2)\n", g_config.port);
        } else {
            CWIST_LOG_INFO("Starting server on port %d (HTTPS)", g_config.port);
            printf("Docker Blog: https://localhost:%d\n", g_config.port);
        }
    } else {
        if (g_config.use_http2) {
            CWIST_LOG_INFO("Starting server on port %d (HTTP/2, plain)", g_config.port);
            printf("Docker Blog: http://localhost:%d (HTTP/2)\n", g_config.port);
        } else {
            CWIST_LOG_INFO("Starting server on port %d (HTTP/1.1, plain)", g_config.port);
            printf("Docker Blog: http://localhost:%d\n", g_config.port);
        }
    }
    int rc = cwist_app_listen(app, g_config.port);
    flywire_stop();
    cert_renewal_stop();
    cert_renewal_join();
    cleanup_stop();
    cleanup_join();
    post_schedule_stop();
    engine_nats_stop();
    engine_pool_shutdown();
    cleanup_close_wake_fd();
    db_comment_close();
    db_board_tree_close();
    cwist_app_destroy(app);
    fly_crypto_cleanup();
    curl_global_cleanup();
    return rc;
}

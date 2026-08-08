#define _POSIX_C_SOURCE 200809L
#include "handlers/handlers.h"
#include "auth/auth.h"
#include "crypto/fly_crypto.h"
#include "db/db.h"
#include "db/db_internal.h"
#include "utils/media_preview.h"
#include "cwist/board_tree.h"
#include "nats/fly_nats.h"
#include "config/config.h"
#include "engine/pool.h"
#include "engine/nats.h"
#include "engine/db.h"
#include "engine/settings.h"
#include "engine/routes.h"
#include "engine/warmup.h"
#include "utils/cache.h"
#include "utils/reqshare.h"
#include "utils/image_inline.h"
#include <cwist/net/http/http3.h>
#include <openssl/ssl.h>
#include <cwist/sys/app/app.h>
#include <cwist/sys/app/compress.h>
#include <ttak/async/task.h>
#include <ttak/timing/timing.h>
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
    if (g_cleanup_thread_started) {
        pthread_join(g_cleanup_thread, NULL);
        g_cleanup_thread_started = false;
    }
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
    /* Never share the request-serving connection with this long-lived worker.
     * A dedicated WAL connection keeps its statements and busy state isolated
     * from HTTP handlers. */
    sqlite3 *conn = NULL;
    if (sqlite3_open_v2("data/blog.db", &conn, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK ||
        !db_configure_connection(conn)) {
        FLY_LOG_ERROR("Failed to open cleanup database connection");
        if (conn) sqlite3_close(conn);
        return NULL;
    }
    cwist_db db = { .conn = conn };
    int tfd = create_daily_3am_timer();
    if (tfd < 0) {
        CWIST_LOG_ERROR("Failed to create cleanup timerfd");
        sqlite3_close(conn);
        return NULL;
    }
    int epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0) {
        FLY_LOG_ERROR("Failed to create cleanup epoll instance");
        close(tfd);
        sqlite3_close(conn);
        return NULL;
    }
    struct epoll_event event = { .events = EPOLLIN };
    event.data.fd = tfd;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, tfd, &event) < 0) {
        FLY_LOG_ERROR("Failed to register cleanup timer");
        close(epfd);
        close(tfd);
        sqlite3_close(conn);
        return NULL;
    }
    event.data.fd = g_cleanup_wake_fd;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, g_cleanup_wake_fd, &event) < 0) {
        FLY_LOG_ERROR("Failed to register cleanup wake event");
        close(epfd);
        close(tfd);
        sqlite3_close(conn);
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
        db_cleanup_orphaned_files(&db);
    }
    close(epfd);
    close(tfd);
    sqlite3_close(conn);
    return NULL;
}

int main(void) {
    /* SQLite must be configured before any connection is opened.  Use the
     * serialized threading mode so a single connection can be safely shared
     * across multiple worker threads. */
    sqlite3_config(SQLITE_CONFIG_SERIALIZED);

    setenv("CWIST_C1M_MODE", "0", 1);
    signal(SIGPIPE, SIG_IGN);
    fly_log_init();
    if (!ensure_asset_workdir()) {
        FLY_LOG_ERROR("Public assets not found; set BLOG_ROOT or run from project root");
        return 1;
    }
    CWIST_LOG_INFO("Workdir verified");
    if (!fly_crypto_init()) {
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
    }

    cwist_app_set_max_memspace(app, CWIST_MIB(512));
    cwist_app_configure_bdr(app, CWIST_MIB(256), 600, 250000);

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

    /* Register payload compression backends in preference order:
     * brotli (best ratio for text) → zstd (fastest) → gzip (widest compat).
     * cwist_mw_compress picks the first backend the client's Accept-Encoding supports. */
    cwist_compress_unregister_all();
    cwist_compress_register_backend(cwist_compress_backend_brotli());
    cwist_compress_register_backend(cwist_compress_backend_zstd());
    cwist_compress_register_backend(cwist_compress_backend_gzip());
    cwist_app_use(app, cwist_mw_compress(1024));
    CWIST_LOG_INFO("Compression middleware registered (brotli > zstd > gzip, min 1 KiB)");

    engine_routes_register(app);



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
    cleanup_stop();
    cleanup_join();
    engine_nats_stop();
    engine_pool_shutdown();
    cleanup_close_wake_fd();
    db_comment_close();
    db_board_tree_close();
    cwist_app_destroy(app);
    fly_crypto_cleanup();
    return rc;
}

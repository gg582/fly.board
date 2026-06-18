#define _POSIX_C_SOURCE 200809L
#include "handlers/handlers.h"
#include "auth/auth.h"
#include "crypto/fly_crypto.h"
#include "db/db.h"
#include "utils/media_preview.h"
#include "cwist/board_tree.h"
#include "nats/fly_nats.h"
#include "config/config.h"
#include <cwist/sys/app/app.h>
#include <ttak/async/task.h>
#include <ttak/thread/pool.h>
#include <ttak/timing/timing.h>
#include <signal.h>
#if defined __has_include
#  if __has_include (<cwist/security/tls/ech.h>)
#    define HAVE_ECH 1
#    include <cwist/security/tls/ech.h>
#  endif
#endif
#include <cwist/core/log.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <limits.h>

#define BLOG_CERT "server.crt"
#define BLOG_KEY  "server.key"
#define DB_PATH   "data/blog.db"

static volatile bool g_nats_running = false;
static volatile bool g_cleanup_running = false;
static ttak_thread_pool_t *g_background_pool = NULL;

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

static void *nats_worker(void *arg) {
    (void)arg;
    while (g_nats_running) {
        fly_nats_dispatch();
    }
    return NULL;
}

static size_t background_pool_size(void) {
    long cores = sysconf(_SC_NPROCESSORS_ONLN);
    if (cores < 2) return 2;
    if (cores > 4) return 4;
    return (size_t)cores;
}

static bool schedule_background_task(ttak_task_func_t func,
                                     void *arg,
                                     uint64_t hash,
                                     ttak_task_domain_t domain,
                                     uint8_t urgency) {
    if (!g_background_pool || !func) return false;
    uint64_t now = ttak_get_tick_count();
    ttak_task_t *task = ttak_task_create(func, arg, NULL, now);
    if (!task) return false;
    ttak_task_set_hash(task, hash);
    ttak_task_set_domain(task, domain);
    ttak_task_set_urgency(task, urgency);
    if (!ttak_thread_pool_schedule_task(g_background_pool, task, 0, now)) {
        ttak_task_destroy(task, now);
        return false;
    }
    return true;
}

static void shutdown_background_pool(void) {
    g_nats_running = false;
    g_cleanup_running = false;
    fly_nats_close();
    if (g_background_pool) {
        ttak_thread_pool_destroy(g_background_pool);
        g_background_pool = NULL;
    }
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
    cwist_db *db = (cwist_db *)arg;
    int tfd = create_daily_3am_timer();
    if (tfd < 0) {
        CWIST_LOG_ERROR("Failed to create cleanup timerfd");
        return NULL;
    }
    uint64_t exp;
    struct pollfd pfd = { .fd = tfd, .events = POLLIN };
    while (g_cleanup_running) {
        int ready = poll(&pfd, 1, 1000);
        if (ready < 0) break;
        if (ready == 0) continue;
        ssize_t s = read(tfd, &exp, sizeof(exp));
        if (s != sizeof(exp)) continue;
        db_cleanup_orphaned_files(db);
    }
    close(tfd);
    return NULL;
}

int main(void) {
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
    if (!auth_admin_load("admin.settings")) {
        FLY_LOG_ERROR("Failed to load admin.settings");
        return 1;
    }
    CWIST_LOG_INFO("Admin settings loaded");
    blog_config_load("blog.settings");
    CWIST_LOG_INFO("Blog config loaded");
    font_settings_load("fonts.settings");
    CWIST_LOG_INFO("Font settings loaded");

    g_background_pool = ttak_thread_pool_create(background_pool_size(), 0, ttak_get_tick_count());
    if (!g_background_pool) {
        FLY_LOG_ERROR("Failed to initialize libttak background thread pool");
        fly_crypto_cleanup();
        return 1;
    }

    const char *nats_url = getenv("NATS_URL");
    if (nats_url) {
        if (fly_nats_init(nats_url)) {
            g_nats_running = true;
            if (!schedule_background_task(nats_worker, NULL, 0x4e415453ULL, TTAK_TASK_DOMAIN_NET, 80)) {
                g_nats_running = false;
                fly_nats_close();
                FLY_LOG_ERROR("Failed to schedule NATS worker");
            }
        } else {
            FLY_LOG_ERROR("NATS init failed, continuing without messaging");
        }
    }

    cwist_app *app = cwist_app_create();
    if (!app) {
        FLY_LOG_ERROR("Failed to create app");
        shutdown_background_pool();
        fly_crypto_cleanup();
        return 1;
    }
    CWIST_LOG_INFO("CWIST app created");

    cwist_error_t dberr = cwist_app_use_db(app, DB_PATH);
    if (dberr.errtype != CWIST_ERR_INT16 || dberr.error.err_i16 != 0) {
        FLY_LOG_ERROR("Failed to open database");
        shutdown_background_pool();
        cwist_app_destroy(app);
        return 1;
    }
    CWIST_LOG_INFO("Database opened: %s", DB_PATH);

    cwist_db *db = cwist_app_get_db(app);
    if (!db_init(db)) {
        FLY_LOG_ERROR("Failed to initialize database schema");
        shutdown_background_pool();
        cwist_app_destroy(app);
        return 1;
    }
    CWIST_LOG_INFO("Database schema initialized");
    if (!db_comment_init("data/comments.db")) {
        FLY_LOG_ERROR("Failed to initialize comments database");
        shutdown_background_pool();
        cwist_app_destroy(app);
        return 1;
    }
    CWIST_LOG_INFO("Comments database initialized");
    if (!db_board_tree_init("data/board_tree.db")) {
        FLY_LOG_ERROR("Failed to initialize board tree database");
        shutdown_background_pool();
        cwist_app_destroy(app);
        return 1;
    }
    CWIST_LOG_INFO("Board tree database initialized");

    db_file_cleanup_duplicates(db);
    media_preview_backfill(db);
    db_cleanup_orphaned_files(db);
    CWIST_LOG_INFO("Orphaned files cleanup completed");

    g_cleanup_running = true;
    if (!schedule_background_task(cleanup_worker, db, 0x434c45414e5550ULL, TTAK_TASK_DOMAIN_IO, 10)) {
        g_cleanup_running = false;
        FLY_LOG_ERROR("Failed to schedule cleanup worker");
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
    }
    if (g_config.use_tls) {
        cwist_error_t tls = cwist_app_use_https(app, BLOG_CERT, BLOG_KEY);
        if (tls.errtype != CWIST_ERR_INT16 || tls.error.err_i16 != 0) {
            FLY_LOG_ERROR("HTTPS init failed; run ./keygen.sh first");
            shutdown_background_pool();
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
            shutdown_background_pool();
            cwist_app_destroy(app);
            return 1;
        }
        CWIST_LOG_INFO("ECH initialized");
    }

    cwist_app_use(app, global_middleware);
    cwist_app_register_error_handler(app, CWIST_HTTP_NOT_FOUND, handler_not_found);

    cwist_app_get(app, "/assets/img/:filename", handler_asset_img);
    cwist_app_get(app, "/assets/uploads/:filename", handler_asset_upload);
    cwist_app_get(app, "/assets/profile/:filename", handler_asset_profile_upload);
    cwist_app_get(app, "/assets/tasfa/:scope/:filename/handshake", handler_asset_tasfa_handshake);
    cwist_app_get(app, "/assets/tasfa/:scope/:filename/chunk/:chunk_index", handler_asset_tasfa_chunk);
    cwist_app_static(app, "/assets/images", "public/images");
    cwist_app_static(app, "/assets/js", "public/js");
    cwist_app_static(app, "/js", "public/js");
    cwist_app_static(app, "/assets/media", "public/media");
    cwist_app_get(app, "/sw.js", handler_sw_js);
    cwist_app_get(app, "/__tasfa_stream__/:stream_id", handler_tasfa_stream_placeholder);

    /* Routes */
    cwist_app_get(app, "/", handler_home);
    cwist_app_get(app, "/theme.json", handler_theme_json);
    cwist_app_get(app, "/themes.json", handler_themes_json);
    cwist_app_get(app, "/rss.xml", handler_rss_xml);

    cwist_app_get(app, "/login", handler_login_get);
    cwist_app_post(app, "/login", handler_login_post);
    cwist_app_get(app, "/logout", handler_logout);
    cwist_app_get(app, "/register", handler_register_get);
    cwist_app_post(app, "/register", handler_register_post);
    cwist_app_post(app, "/unregister", handler_unregister_post);

    cwist_app_get(app, "/profile", handler_profile_get);
    cwist_app_post(app, "/profile", handler_profile_post);
    cwist_app_get(app, "/account/settings", handler_account_settings_get);
    cwist_app_post(app, "/account/settings", handler_account_settings_post);
    cwist_app_get(app, "/account/password", handler_password_change_get);
    cwist_app_post(app, "/account/password", handler_password_change_post);
    cwist_app_get(app, "/user/:id", handler_user_profile_get);

    cwist_app_get(app, "/boards", handler_board_list);
    cwist_app_get(app, "/board/new", handler_board_new_get);
    cwist_app_post(app, "/board/new", handler_board_new_post);
    cwist_app_get(app, "/board/:id/edit", handler_board_edit_get);
    cwist_app_post(app, "/board/edit", handler_board_edit_post);
    cwist_app_get(app, "/board/:id/delete", handler_board_delete);
    cwist_app_get(app, "/board/:id/perms", handler_board_perms_get);
    cwist_app_post(app, "/board/perms", handler_board_perms_post);
    cwist_app_post(app, "/board/perms/revoke", handler_board_perms_revoke_post);

    cwist_app_get(app, "/search", handler_post_list);
    cwist_app_get(app, "/board/:slug", handler_post_list);
    cwist_app_get(app, "/post/:slug", handler_post_get);
    cwist_app_get(app, "/post/new", handler_post_new_get);
    cwist_app_post(app, "/post/new", handler_post_new_post);
    cwist_app_get(app, "/post/:id/edit", handler_post_edit_get);
    cwist_app_post(app, "/post/:id/edit", handler_post_edit_post);
    cwist_app_get(app, "/post/delete/:id", handler_post_delete);

    cwist_app_get(app, "/files", handler_file_repo);
    cwist_app_get(app, "/file/preview/:id", handler_file_preview);
    cwist_app_get(app, "/file/:id", handler_file_detail_get);
    cwist_app_get(app, "/file/download/:id", handler_file_download);
    cwist_app_get(app, "/file/download/:id/handshake", handler_file_download_handshake);
    cwist_app_get(app, "/file/download/:id/chunk/:chunk_index", handler_file_download_chunk);
    cwist_app_post(app, "/file/download/complete", handler_file_download_complete);
    cwist_app_post(app, "/file/upload/init", handler_file_upload_init);
    cwist_app_post(app, "/file/upload/status", handler_file_upload_status);
    cwist_app_post(app, "/file/upload/renegotiate", handler_file_upload_renegotiate);
    cwist_app_post(app, "/file/upload", handler_file_upload);
    cwist_app_post(app, "/file/upload/complete", handler_file_upload_complete);
    cwist_app_post(app, "/file/upload/cancel", handler_file_upload_cancel);
    cwist_app_post(app, "/file/delete", handler_file_delete);

    cwist_app_post(app, "/comment/new", handler_comment_new_post);
    cwist_app_post(app, "/comment/edit", handler_comment_edit_post);
    cwist_app_get(app, "/comment/:id/delete", handler_comment_delete_get);

    cwist_app_get(app, "/admin", handler_admin_dashboard);
    cwist_app_get(app, "/admin/users", handler_admin_users);
    cwist_app_post(app, "/admin/user/role", handler_admin_user_role);
    cwist_app_post(app, "/admin/files/drop", handler_admin_files_drop);
    cwist_app_get(app, "/admin/boards", handler_admin_boards_get);

    cwist_app_post(app, "/api/preview", handler_api_preview);
    cwist_app_post(app, "/api/upload", handler_api_upload);
    cwist_app_get(app, "/api/boards", handler_api_boards_json);
    cwist_app_get(app, "/api/my-files", handler_api_my_files);
    cwist_app_post(app, "/post/vote", handler_post_vote);

    /* Enable io_uring-based async reactor for C1M scale mode.
     * cwist_reactor will use Linux io_uring direct syscalls on Linux,
     * falling back to epoll/kqueue on other platforms. */
    setenv("CWIST_C1M_MODE", "1", 1);

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
    shutdown_background_pool();
    db_comment_close();
    db_board_tree_close();
    cwist_app_destroy(app);
    fly_crypto_cleanup();
    return rc;
}

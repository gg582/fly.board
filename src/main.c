#define _POSIX_C_SOURCE 200809L
#include "handlers/handlers.h"
#include "auth/auth.h"
#include "crypto/fly_crypto.h"
#include "db/db.h"
#include "nats/fly_nats.h"
#include "config/config.h"
#include <cwist/sys/app/app.h>
#include <cwist/security/tls/ech.h>
#include <cwist/core/log.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdbool.h>

#define BLOG_CERT "server.crt"
#define BLOG_KEY  "server.key"
#define DB_PATH   "data/blog.db"

static volatile bool g_nats_running = false;

static void *nats_worker(void *arg) {
    (void)arg;
    while (g_nats_running) {
        fly_nats_dispatch();
    }
    return NULL;
}

int main(void) {
    fly_log_init();
    if (!fly_crypto_init()) {
        FLY_LOG_ERROR("PQC crypto init failed");
        return 1;
    }
    if (!auth_admin_load("admin.settings")) {
        FLY_LOG_ERROR("Failed to load admin.settings");
        return 1;
    }
    blog_config_load("blog.settings");

    const char *nats_url = getenv("NATS_URL");
    if (nats_url) {
        if (fly_nats_init(nats_url)) {
            g_nats_running = true;
            pthread_t ntid;
            pthread_create(&ntid, NULL, nats_worker, NULL);
        } else {
            FLY_LOG_ERROR("NATS init failed, continuing without messaging");
        }
    }

    cwist_app *app = cwist_app_create();
    if (!app) {
        FLY_LOG_ERROR("Failed to create app");
        fly_crypto_cleanup();
        return 1;
    }

    cwist_error_t dberr = cwist_app_use_db(app, DB_PATH);
    if (dberr.errtype != CWIST_ERR_INT16 || dberr.error.err_i16 != 0) {
        FLY_LOG_ERROR("Failed to open database");
        cwist_app_destroy(app);
        return 1;
    }

    cwist_db *db = cwist_app_get_db(app);
    if (!db_init(db)) {
        FLY_LOG_ERROR("Failed to initialize database schema");
        cwist_app_destroy(app);
        return 1;
    }

    cwist_app_use_https3(app, true);
    cwist_error_t tls = cwist_app_use_https(app, BLOG_CERT, BLOG_KEY);
    if (tls.errtype != CWIST_ERR_INT16 || tls.error.err_i16 != 0) {
        FLY_LOG_ERROR("HTTPS init failed; run ./keygen.sh first");
        cwist_app_destroy(app);
        return 1;
    }

    const char *ech_key = getenv("BLOG_ECH_KEY");
    const char *ech_dir = getenv("BLOG_ECH_DIR");
    if (ech_key || ech_dir) {
        cwist_ech_config ech = {.ech_key_file = ech_key, .ech_dir = ech_dir, .enforce_ech = false};
        cwist_error_t ech_err = cwist_app_use_ech(app, &ech);
        if (ech_err.errtype != CWIST_ERR_INT16 || ech_err.error.err_i16 != 0) {
            FLY_LOG_ERROR("ECH not available in this build");
            cwist_app_use_ech(app, NULL);
            cwist_app_use_https(app, BLOG_CERT, BLOG_KEY);
        }
    }

    cwist_app_static(app, "/assets", "public");
    cwist_app_static(app, "/img", "img");
    cwist_app_get(app, "/uploads/:filename", handler_uploads_static);

    /* Routes */
    cwist_app_get(app, "/", handler_home);
    cwist_app_get(app, "/theme.css", handler_theme_css);

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

    cwist_app_get(app, "/board/:slug", handler_post_list);
    cwist_app_get(app, "/post/:slug", handler_post_get);
    cwist_app_get(app, "/post/new", handler_post_new_get);
    cwist_app_post(app, "/post/new", handler_post_new_post);
    cwist_app_get(app, "/post/:id/edit", handler_post_edit_get);
    cwist_app_post(app, "/post/edit", handler_post_edit_post);
    cwist_app_get(app, "/post/:id/delete", handler_post_delete);

    cwist_app_get(app, "/file/:id", handler_file_detail_get);

    cwist_app_post(app, "/comment/new", handler_comment_new_post);
    cwist_app_post(app, "/comment/edit", handler_comment_edit_post);
    cwist_app_get(app, "/comment/:id/delete", handler_comment_delete_get);

    cwist_app_get(app, "/admin/users", handler_admin_users);
    cwist_app_post(app, "/admin/user/role", handler_admin_user_role);

    cwist_app_post(app, "/api/preview", handler_api_preview);
    cwist_app_post(app, "/api/upload", handler_api_upload);

    printf("Docker Blog: https://localhost:%d (HTTP/3 on UDP %d)\n", g_config.port, g_config.port);
    int rc = cwist_app_listen(app, g_config.port);
    g_nats_running = false;
    fly_nats_close();
    cwist_app_destroy(app);
    fly_crypto_cleanup();
    return rc;
}

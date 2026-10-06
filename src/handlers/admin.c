#define _POSIX_C_SOURCE 200809L
#include "handlers_internal.h"
#include "cwist/board_tree.h"
#include "config/write_policy.h"
#include "tools/backup.h"
#include <openssl/mem.h>

void handler_admin_dashboard(cwist_http_request *req, cwist_http_response *res) {
    if (!auth_require_admin(req, res)) return;
    int uid = 0; char role[32] = {0};
    auth_is_logged_in(req, &uid, role, sizeof(role));
    char *pp = get_profile_pic(req->db, uid, role);
    const char *msg = cwist_query_map_get(req->query_params, "msg");
    /* Backup target and last run, for the Scheduled Backups section. The
     * secret key and passphrase never leave the server: only whether they
     * are set. */
    backup_settings_t bs;
    bool has_target = backup_settings_load(&bs);
    cJSON *backup = cJSON_CreateObject();
    cJSON_AddStringToObject(backup, "target", has_target ? bs.target : "");
    cJSON_AddBoolToObject(backup, "enabled", has_target && bs.enabled);
    cJSON_AddBoolToObject(backup, "ready", has_target && backup_settings_ready(&bs));
    cJSON_AddStringToObject(backup, "endpoint", bs.endpoint);
    cJSON_AddStringToObject(backup, "region", bs.region);
    cJSON_AddStringToObject(backup, "bucket", bs.bucket);
    cJSON_AddStringToObject(backup, "access_key", bs.access_key);
    cJSON_AddBoolToObject(backup, "has_secret_key", bs.secret_key[0] != '\0');
    cJSON_AddStringToObject(backup, "prefix", bs.prefix);
    cJSON_AddBoolToObject(backup, "use_path_style", bs.use_path_style);
    cJSON_AddStringToObject(backup, "path", bs.path);
    cJSON_AddNumberToObject(backup, "keep", bs.keep);
    cJSON_AddBoolToObject(backup, "has_passphrase", bs.passphrase[0] != '\0');
    OPENSSL_cleanse(&bs, sizeof(bs));
    static const char *const status_keys[] = {"backup_last_at", "backup_last_result", "backup_last_error",
                                              "backup_last_name", "backup_last_size"};
    for (size_t i = 0; i < sizeof(status_keys) / sizeof(status_keys[0]); i++) {
        char value[512] = {0};
        db_site_setting_get(req->db, status_keys[i], value, sizeof(value));
        cJSON_AddStringToObject(backup, status_keys[i] + 7, value); /* drop "backup_" */
    }
    cwist_sstring *page = render_admin_dashboard(is_dark(req), pp, is_mobile_request(req), msg,
                                                 db_report_count_open(req->db), backup);
    cJSON_Delete(backup);
    send_html_res(res, page);
    free(pp);
}

static void form_copy(char *dst, size_t size, cwist_query_map *kv, const char *key) {
    const char *v = cwist_query_map_get(kv, key);
    snprintf(dst, size, "%s", v ? v : "");
    /* trim surrounding whitespace */
    size_t n = strlen(dst);
    while (n > 0 && (dst[n - 1] == ' ' || dst[n - 1] == '\t')) dst[--n] = '\0';
    size_t lead = strspn(dst, " \t");
    if (lead) memmove(dst, dst + lead, strlen(dst + lead) + 1);
}

/* Register, update or remove the scheduled-backup target. Blank secret
 * fields keep the stored values, so the form never has to echo them. */
void handler_admin_backup_post(cwist_http_request *req, cwist_http_response *res) {
    if (!auth_require_admin(req, res)) return;
    cwist_query_map *kv = cwist_query_map_create();
    if (req->body && req->body->data) cwist_query_map_parse(kv, req->body->data);
    const char *action = cwist_query_map_get(kv, "action");
    const char *msg = "backup_error";
    if (action && !strcmp(action, "remove")) {
        if (backup_settings_remove()) msg = "backup_removed";
    } else if (action && !strcmp(action, "run")) {
        backup_settings_t cur;
        bool ready = backup_settings_load(&cur) && backup_settings_ready(&cur);
        OPENSSL_cleanse(&cur, sizeof(cur));
        msg = !ready ? "backup_not_ready" : backup_spawn() ? "backup_started" : "backup_error";
    } else {
        backup_settings_t old, s;
        backup_settings_load(&old);
        memset(&s, 0, sizeof(s));
        form_copy(s.target, sizeof(s.target), kv, "target");
        s.enabled = cwist_query_map_get(kv, "enabled") != NULL;
        form_copy(s.endpoint, sizeof(s.endpoint), kv, "endpoint");
        form_copy(s.region, sizeof(s.region), kv, "region");
        form_copy(s.bucket, sizeof(s.bucket), kv, "bucket");
        form_copy(s.access_key, sizeof(s.access_key), kv, "access_key");
        form_copy(s.secret_key, sizeof(s.secret_key), kv, "secret_key");
        if (!s.secret_key[0]) memcpy(s.secret_key, old.secret_key, sizeof(s.secret_key));
        form_copy(s.prefix, sizeof(s.prefix), kv, "prefix");
        s.use_path_style = cwist_query_map_get(kv, "use_path_style") != NULL;
        form_copy(s.path, sizeof(s.path), kv, "path");
        const char *keep = cwist_query_map_get(kv, "keep");
        s.keep = keep ? atoi(keep) : 14;
        form_copy(s.passphrase, sizeof(s.passphrase), kv, "passphrase");
        if (cwist_query_map_get(kv, "clear_passphrase")) s.passphrase[0] = '\0';
        else if (!s.passphrase[0]) memcpy(s.passphrase, old.passphrase, sizeof(s.passphrase));
        bool valid = (!strcmp(s.target, "s3") || !strcmp(s.target, "dir")) && s.keep >= 1 && s.keep <= 1000 &&
                     (!s.passphrase[0] || strlen(s.passphrase) >= 12) &&
                     (strcmp(s.target, "dir") || s.path[0] == '/') &&
                     (strcmp(s.target, "s3") || !strncmp(s.endpoint, "https://", 8) || !strncmp(s.endpoint, "http://", 7));
        if (valid && backup_settings_save(&s)) msg = backup_settings_ready(&s) ? "backup_saved" : "backup_incomplete";
        else if (!valid) msg = "backup_invalid";
        OPENSSL_cleanse(&old, sizeof(old));
        OPENSSL_cleanse(&s, sizeof(s));
    }
    cwist_query_map_destroy(kv);
    CWIST_LOG_INFO("Backup settings action=%s -> %s", action ? action : "save", msg);
    char url[96];
    snprintf(url, sizeof(url), "/admin/dashboard?msg=%s#backups", msg);
    redirect(res, url);
}

void handler_dashboard(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0;
    char role[32] = {0};
    if (!auth_require_login(req, res, &uid, role, sizeof(role))) return;
    if (strcmp(role, "admin") == 0) {
        handler_admin_dashboard(req, res);
    } else {
        redirect(res, "/profile");
    }
}

void handler_admin_write_policy_post(cwist_http_request *req, cwist_http_response *res) {
    if (!auth_require_admin(req, res)) return;
    cwist_query_map *kv = cwist_query_map_create();
    if (req->body && req->body->data) cwist_query_map_parse(kv, req->body->data);
    write_policy_t policy = write_policy_get();
    const char *post_scope = cwist_query_map_get(kv, "post_scope");
    const char *comment_scope = cwist_query_map_get(kv, "comment_scope");
    bool ok = write_scope_parse(post_scope, &policy.post) &&
              write_scope_parse(comment_scope, &policy.comment);
    /* An unchecked checkbox is simply absent from the form body. */
    policy.require_board = cwist_query_map_get(kv, "require_board") != NULL;
    policy.spam_honeypot = cwist_query_map_get(kv, "spam_honeypot") != NULL;
    const char *limit = cwist_query_map_get(kv, "spam_rate_limit");
    int limit_val = limit && limit[0] ? atoi(limit) : 0;
    if (limit_val < 0 || limit_val > SPAM_RATE_LIMIT_MAX) ok = false;
    else policy.spam_rate_limit = limit_val;
    cwist_query_map_destroy(kv);
    if (ok) ok = write_policy_set(req->db, &policy);
    if (ok) {
        CWIST_LOG_INFO("Write policy updated: posts=%s comments=%s require_board=%s spam_honeypot=%s spam_rate_limit=%d",
                       write_scope_name(policy.post), write_scope_name(policy.comment),
                       policy.require_board ? "yes" : "no", policy.spam_honeypot ? "yes" : "no",
                       policy.spam_rate_limit);
        /* Cached pages carry the New Post button and comment forms. This
         * also bumps the shared route BDR; the other workers' page caches
         * miss through the policy generation in their keys. */
        page_cache_invalidate_all();
    } else {
        CWIST_LOG_ERROR("Write policy update failed");
    }
    redirect(res, ok ? "/admin/dashboard?msg=saved" : "/admin/dashboard?msg=error");
}

void handler_admin_users(cwist_http_request *req, cwist_http_response *res) {
    if (!auth_require_admin(req, res)) return;
    int uid = 0; char role[32] = {0};
    auth_is_logged_in(req, &uid, role, sizeof(role));
    cJSON *users = db_user_list(req->db);
    char *pp = get_profile_pic(req->db, uid, role);
    cwist_sstring *page = render_user_admin(users, is_dark(req), pp, is_mobile_request(req));
    if (users) cJSON_Delete(users);
    send_html_res(res, page);
    free(pp);
}

void handler_admin_user_role(cwist_http_request *req, cwist_http_response *res) {
    if (!auth_require_admin(req, res)) return;
    cwist_query_map *kv = cwist_query_map_create(); cwist_query_map_parse(kv, req->body->data);
    const char *id_str = cwist_query_map_get(kv, "id");
    const char *role = cwist_query_map_get(kv, "role");
    if (id_str && role) {
        int target_uid = atoi(id_str);
        if (auth_is_site_admin(target_uid)) {
            CWIST_LOG_WARN("User role update refused: uid=%d is the admin.settings account", target_uid);
        } else if (target_uid > 0 && db_user_update_role(req->db, target_uid, role)) {
            CWIST_LOG_INFO("User role updated: uid=%d role='%s'", target_uid, role);
            page_cache_invalidate_all();
        } else {
            CWIST_LOG_ERROR("User role update failed: uid=%d role='%s'", target_uid, role);
        }
    } else {
        CWIST_LOG_WARN("User role update failed: missing fields");
    }
    cwist_query_map_destroy(kv);
    const char *referer = cwist_http_header_get(req->headers, "Referer");
    redirect_referer_safe(res, referer, "/admin/users");
}

void handler_admin_files_drop(cwist_http_request *req, cwist_http_response *res) {
    if (!auth_require_admin(req, res)) return;
    int count = db_file_drop_all(req->db);
    CWIST_LOG_INFO("Admin dropped all files: count=%d", count);
    page_cache_invalidate_all();
    const char *referer = cwist_http_header_get(req->headers, "Referer");
    redirect_referer_safe(res, referer, "/files");
}

void handler_admin_boards_get(cwist_http_request *req, cwist_http_response *res) {
    if (!auth_require_admin(req, res)) return;
    int uid = 0; char role[32] = {0};
    auth_is_logged_in(req, &uid, role, sizeof(role));
    char *pp = get_profile_pic(req->db, uid, role);
    cJSON *boards = db_board_list(req->db);
    cJSON *tree = db_board_tree_get_all();
    cJSON *ordered = cJSON_CreateArray();
    append_boards_flat(ordered, boards, tree, 0, 4);
    cwist_sstring *page = render_admin_boards(ordered, tree, is_dark(req), pp, is_mobile_request(req));
    if (ordered) cJSON_Delete(ordered);
    if (boards) cJSON_Delete(boards);
    if (tree) cJSON_Delete(tree);
    send_html_res(res, page);
    free(pp);
}

/* ---- API ---- */

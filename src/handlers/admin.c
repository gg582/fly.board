#define _POSIX_C_SOURCE 200809L
#include "handlers_internal.h"

void handler_admin_users(cwist_http_request *req, cwist_http_response *res) {
    if (!auth_require_admin(req, res)) return;
    int uid = 0; char role[32] = {0};
    auth_is_logged_in(req, &uid, role, sizeof(role));
    cJSON *users = db_user_list(req->db);
    char *pp = get_profile_pic(req->db, uid, role);
    cwist_sstring *page = render_user_admin(users, is_dark(req), pp);
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
        if (db_user_update_role(req->db, target_uid, role)) {
            CWIST_LOG_INFO("User role updated: uid=%d role='%s'", target_uid, role);
        } else {
            CWIST_LOG_ERROR("User role update failed: uid=%d role='%s'", target_uid, role);
        }
    } else {
        CWIST_LOG_WARN("User role update failed: missing fields");
    }
    cwist_query_map_destroy(kv);
    redirect(res, "/admin/users");
}

void handler_admin_files_drop(cwist_http_request *req, cwist_http_response *res) {
    if (!auth_require_admin(req, res)) return;
    int count = db_file_drop_all(req->db);
    CWIST_LOG_INFO("Admin dropped all files: count=%d", count);
    redirect(res, "/files");
}

/* ---- API ---- */

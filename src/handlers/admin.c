#define _POSIX_C_SOURCE 200809L
#include "handlers_internal.h"
#include "cwist/board_tree.h"
#include "config/write_policy.h"

void handler_admin_dashboard(cwist_http_request *req, cwist_http_response *res) {
    if (!auth_require_admin(req, res)) return;
    int uid = 0; char role[32] = {0};
    auth_is_logged_in(req, &uid, role, sizeof(role));
    char *pp = get_profile_pic(req->db, uid, role);
    const char *msg = cwist_query_map_get(req->query_params, "msg");
    cwist_sstring *page = render_admin_dashboard(is_dark(req), pp, is_mobile_request(req), msg);
    send_html_res(res, page);
    free(pp);
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
    cwist_query_map_destroy(kv);
    if (ok) ok = write_policy_set(req->db, &policy);
    if (ok) {
        CWIST_LOG_INFO("Write policy updated: posts=%s comments=%s require_board=%s",
                       write_scope_name(policy.post), write_scope_name(policy.comment),
                       policy.require_board ? "yes" : "no");
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

#define _POSIX_C_SOURCE 200809L
#include "handlers_internal.h"

void handler_comment_new_post(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0;
    char role[32] = {0};
    if (!auth_require_login(req, res, &uid, role, sizeof(role))) return;
    form_kv_t *kv = parse_urlencoded(req->body->data);
    const char *target_type = form_kv_get(kv, "target_type");
    const char *target_id_str = form_kv_get(kv, "target_id");
    const char *parent_id_str = form_kv_get(kv, "parent_id");
    const char *content = form_kv_get(kv, "content");
    const char *referer = cwist_http_header_get(req->headers, "Referer");
    if (target_type && target_id_str && content && content[0]) {
        int target_id = atoi(target_id_str);
        int parent_id = parent_id_str ? atoi(parent_id_str) : 0;
        db_comment_create(req->db, target_type, target_id, uid, parent_id, content);
    }
    form_kv_free(kv);
    redirect(res, referer && referer[0] ? referer : "/");
}

void handler_comment_edit_post(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0;
    char role[32] = {0};
    if (!auth_require_login(req, res, &uid, role, sizeof(role))) return;
    form_kv_t *kv = parse_urlencoded(req->body->data);
    const char *id_str = form_kv_get(kv, "id");
    const char *content = form_kv_get(kv, "content");
    const char *referer = cwist_http_header_get(req->headers, "Referer");
    if (id_str && content && content[0]) {
        db_comment_update(req->db, atoi(id_str), uid, content);
    }
    form_kv_free(kv);
    redirect(res, referer && referer[0] ? referer : "/");
}

void handler_comment_delete_get(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0;
    char role[32] = {0};
    if (!auth_require_login(req, res, &uid, role, sizeof(role))) return;
    const char *id_str = cwist_query_map_get(req->path_params, "id");
    const char *referer = cwist_http_header_get(req->headers, "Referer");
    if (id_str) {
        db_comment_delete(req->db, atoi(id_str), uid);
    }
    redirect(res, referer && referer[0] ? referer : "/");
}

/* ---- Admin ---- */

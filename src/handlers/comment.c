#define _POSIX_C_SOURCE 200809L
#include "handlers_internal.h"

void handler_comment_new_post(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0;
    char role[32] = {0};
    if (!auth_require_login(req, res, &uid, role, sizeof(role))) return;
    cwist_query_map *kv = cwist_query_map_create(); cwist_query_map_parse(kv, req->body->data);
    const char *target_type = cwist_query_map_get(kv, "target_type");
    const char *target_id_str = cwist_query_map_get(kv, "target_id");
    const char *parent_id_str = cwist_query_map_get(kv, "parent_id");
    const char *content = cwist_query_map_get(kv, "content");
    const char *referer = cwist_http_header_get(req->headers, "Referer");
    if (target_type && target_id_str && content && content[0]) {
        int target_id = atoi(target_id_str);
        int parent_id = parent_id_str ? atoi(parent_id_str) : 0;
        const char *author_name = NULL;
        cJSON *u = db_user_get_by_id(req->db, uid);
        if (u) {
            cJSON *uname = cJSON_GetObjectItem(u, "username");
            if (uname && uname->valuestring) author_name = uname->valuestring;
        }
        if (db_comment_create(req->db, target_type, target_id, uid, author_name, parent_id, content)) {
            CWIST_LOG_INFO("Comment created: target_type=%s target_id=%d uid=%d", target_type, target_id, uid);
        } else {
            CWIST_LOG_ERROR("Comment creation failed: target_type=%s target_id=%d uid=%d", target_type, target_id, uid);
        }
        if (u) cJSON_Delete(u);
    } else {
        CWIST_LOG_WARN("Comment creation failed: missing fields");
    }
    cwist_query_map_destroy(kv);
    redirect(res, referer && referer[0] ? referer : "/");
}

void handler_comment_edit_post(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0;
    char role[32] = {0};
    if (!auth_require_login(req, res, &uid, role, sizeof(role))) return;
    cwist_query_map *kv = cwist_query_map_create(); cwist_query_map_parse(kv, req->body->data);
    const char *id_str = cwist_query_map_get(kv, "id");
    const char *content = cwist_query_map_get(kv, "content");
    const char *referer = cwist_http_header_get(req->headers, "Referer");
    if (id_str && content && content[0]) {
        if (db_comment_update(req->db, atoi(id_str), uid, content)) {
            CWIST_LOG_INFO("Comment updated: id=%s uid=%d", id_str, uid);
        } else {
            CWIST_LOG_ERROR("Comment update failed: id=%s uid=%d", id_str, uid);
        }
    } else {
        CWIST_LOG_WARN("Comment update failed: missing fields");
    }
    cwist_query_map_destroy(kv);
    redirect(res, referer && referer[0] ? referer : "/");
}

void handler_comment_delete_get(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0;
    char role[32] = {0};
    if (!auth_require_login(req, res, &uid, role, sizeof(role))) return;
    const char *id_str = cwist_query_map_get(req->path_params, "id");
    const char *referer = cwist_http_header_get(req->headers, "Referer");
    if (id_str) {
        int cid = atoi(id_str);
        if (db_comment_delete(req->db, cid, uid)) {
            CWIST_LOG_INFO("Comment deleted: id=%d uid=%d", cid, uid);
        } else {
            CWIST_LOG_WARN("Comment delete failed: id=%d uid=%d", cid, uid);
        }
    }
    redirect(res, referer && referer[0] ? referer : "/");
}

/* ---- Admin ---- */

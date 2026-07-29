#define _POSIX_C_SOURCE 200809L
#include "handlers_internal.h"

static void invalidate_comment_target(cwist_db *db, const char *target_type, int target_id) {
    if (!target_type || !target_type[0] || target_id <= 0) return;
    if (strcmp(target_type, "post") == 0) {
        cJSON *post = db_post_get_by_id(db, target_id);
        if (post) {
            cJSON *slug = cJSON_GetObjectItem(post, "slug");
            if (slug && cJSON_IsString(slug) && slug->valuestring) {
                page_cache_invalidate_post(slug->valuestring);
            } else {
                page_cache_invalidate_all();
            }
            cJSON_Delete(post);
        }
    } else if (strcmp(target_type, "file") == 0) {
        /* File detail pages are cached by id; comment events are rare enough
         * that clearing everything is acceptable. */
        page_cache_invalidate_all();
    }
}

/* Notify the post author (new comment) or the parent comment author (reply)
 * and broadcast the event to federated nodes. Silently skips notification
 * when there is no resolvable recipient, the recipient is the actor, or any
 * lookup fails. */
static void notify_comment_recipient(cwist_db *db, int actor_uid, const char *actor_name,
                                     const char *target_type, int target_id, int parent_id,
                                     const char *content, int comment_id) {
    int recipient = 0;
    const char *kind = NULL;
    int post_id = 0;
    char post_slug[256] = {0};

    cJSON *post = NULL;
    if (target_type && strcmp(target_type, "post") == 0) {
        post = db_post_get_by_id(db, target_id);
    }

    if (parent_id > 0) {
        cJSON *parent = db_comment_get_by_id(db, parent_id);
        if (!parent) {
            if (post) cJSON_Delete(post);
            return;
        }
        cJSON *pu = cJSON_GetObjectItem(parent, "user_id");
        if (pu && cJSON_IsNumber(pu)) recipient = pu->valueint;
        cJSON_Delete(parent);
        kind = "reply";
    } else if (post) {
        cJSON *pu = cJSON_GetObjectItem(post, "user_id");
        if (pu && cJSON_IsNumber(pu)) recipient = pu->valueint;
        kind = "comment";
    }

    if (post) {
        cJSON *ps = cJSON_GetObjectItem(post, "slug");
        if (ps && cJSON_IsString(ps) && ps->valuestring) {
            snprintf(post_slug, sizeof(post_slug), "%s", ps->valuestring);
            post_id = target_id;
        }
        cJSON_Delete(post);
    }

    if (!kind || recipient <= 0 || recipient == actor_uid) return;

    char excerpt[121];
    snprintf(excerpt, sizeof(excerpt), "%s", content ? content : "");

    db_notification_create(db, recipient, actor_name, kind, post_id, post_slug, comment_id, excerpt);
    fly_nats_publish_comment(g_config.root_url, actor_name, kind, post_slug, recipient, excerpt);
}

void handler_comment_new_post(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0;
    char role[32] = {0};
    auth_is_logged_in(req, &uid, role, sizeof(role));
    cwist_query_map *kv = cwist_query_map_create();
    if (req->body && req->body->data) cwist_query_map_parse(kv, req->body->data);
    const char *target_type = cwist_query_map_get(kv, "target_type");
    const char *target_id_str = cwist_query_map_get(kv, "target_id");
    const char *parent_id_str = cwist_query_map_get(kv, "parent_id");
    const char *content = cwist_query_map_get(kv, "content");
    const char *author_name_input = cwist_query_map_get(kv, "author_name");
    const char *referer = cwist_http_header_get(req->headers, "Referer");
    if (target_type && target_id_str && content && content[0]) {
        int target_id = atoi(target_id_str);
        int parent_id = parent_id_str ? atoi(parent_id_str) : 0;
        const char *author_name = NULL;
        char author_name_buf[128] = {0};
        if (uid > 0) {
            cJSON *u = db_user_get_by_id(req->db, uid);
            if (u) {
                cJSON *uname = cJSON_GetObjectItem(u, "username");
                if (uname && uname->valuestring) {
                    snprintf(author_name_buf, sizeof(author_name_buf), "%s", uname->valuestring);
                    author_name = author_name_buf;
                }
            }
            if (u) cJSON_Delete(u);
        } else {
            author_name = (author_name_input && author_name_input[0]) ? author_name_input : "Anonymous";
        }
        int new_comment_id = db_comment_create(req->db, target_type, target_id, uid, author_name, parent_id, content);
        if (new_comment_id > 0) {
            CWIST_LOG_INFO("Comment created: target_type=%s target_id=%d uid=%d", target_type, target_id, uid);
            invalidate_comment_target(req->db, target_type, target_id);
            notify_comment_recipient(req->db, uid, author_name, target_type, target_id, parent_id, content, new_comment_id);
        } else {
            CWIST_LOG_ERROR("Comment creation failed: target_type=%s target_id=%d uid=%d", target_type, target_id, uid);
        }
    } else {
        CWIST_LOG_WARN("Comment creation failed: missing fields");
    }
    cwist_query_map_destroy(kv);
    redirect_referer_safe(res, referer, "/");
}

void handler_comment_edit_post(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0;
    char role[32] = {0};
    if (!auth_require_login(req, res, &uid, role, sizeof(role))) return;
    cwist_query_map *kv = cwist_query_map_create();
    if (req->body && req->body->data) cwist_query_map_parse(kv, req->body->data);
    const char *id_str = cwist_query_map_get(kv, "id");
    const char *content = cwist_query_map_get(kv, "content");
    const char *referer = cwist_http_header_get(req->headers, "Referer");
    if (id_str && content && content[0]) {
        cJSON *comment = db_comment_get_by_id(req->db, atoi(id_str));
        if (db_comment_update(req->db, atoi(id_str), uid, content)) {
            CWIST_LOG_INFO("Comment updated: id=%s uid=%d", id_str, uid);
            if (comment) {
                cJSON *tt = cJSON_GetObjectItem(comment, "target_type");
                cJSON *ti = cJSON_GetObjectItem(comment, "target_id");
                if (tt && cJSON_IsString(tt) && ti && cJSON_IsNumber(ti)) {
                    invalidate_comment_target(req->db, tt->valuestring, ti->valueint);
                }
            }
        } else {
            CWIST_LOG_ERROR("Comment update failed: id=%s uid=%d", id_str, uid);
        }
        if (comment) cJSON_Delete(comment);
    } else {
        CWIST_LOG_WARN("Comment update failed: missing fields");
    }
    cwist_query_map_destroy(kv);
    redirect_referer_safe(res, referer, "/");
}

void handler_comment_delete_get(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0;
    char role[32] = {0};
    if (!auth_require_login(req, res, &uid, role, sizeof(role))) return;
    const char *id_str = cwist_query_map_get(req->path_params, "id");
    const char *referer = cwist_http_header_get(req->headers, "Referer");
    if (id_str) {
        int cid = atoi(id_str);
        cJSON *comment = db_comment_get_by_id(req->db, cid);
        if (comment) {
            cJSON *tt = cJSON_GetObjectItem(comment, "target_type");
            cJSON *ti = cJSON_GetObjectItem(comment, "target_id");
            if (db_comment_delete(req->db, cid, uid)) {
                CWIST_LOG_INFO("Comment deleted: id=%d uid=%d", cid, uid);
                if (tt && cJSON_IsString(tt) && ti && cJSON_IsNumber(ti)) {
                    invalidate_comment_target(req->db, tt->valuestring, ti->valueint);
                }
            } else {
                CWIST_LOG_WARN("Comment delete failed: id=%d uid=%d", cid, uid);
            }
            cJSON_Delete(comment);
        } else {
            db_comment_delete(req->db, cid, uid);
        }
    }
    redirect_referer_safe(res, referer, "/");
}

/* ---- Admin ---- */

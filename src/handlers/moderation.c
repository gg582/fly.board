#define _POSIX_C_SOURCE 200809L
#include "handlers_internal.h"
#include "db/moderation.h"
#include "utils/post_schedule.h"

cwist_sstring *render_moderation_queue(cJSON *items, const char *msg, bool dark,
                                       const char *role, const char *profile_pic, bool is_mobile);

static bool require_reviewer(cwist_http_request *req, cwist_http_response *res, int *uid, char *role, size_t role_size) {
    if (!auth_require_login(req, res, uid, role, role_size)) return false;
    if (moderation_can_review(role)) return true;
    res->status_code = CWIST_HTTP_FORBIDDEN;
    cwist_sstring_assign(res->body, "Forbidden");
    return false;
}

void handler_admin_moderation_get(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0;
    char role[32] = {0};
    if (!require_reviewer(req, res, &uid, role, sizeof(role))) return;
    cJSON *items = db_moderation_list_pending(req->db);
    char *pp = get_profile_pic(req->db, uid, role);
    cwist_sstring *page = render_moderation_queue(items, cwist_query_map_get(req->query_params, "msg"),
                                                   is_dark(req), role, pp, is_mobile_request(req));
    if (items) cJSON_Delete(items);
    free(pp);
    cwist_http_header_add(&res->headers, "Cache-Control", "no-store, private");
    send_html_res(res, page);
}

static void invalidate_moderated_target(cwist_db *db, const char *target_type, int target_id) {
    if (strcmp(target_type, MODERATION_POST) == 0) {
        page_cache_invalidate_all();
        post_schedule_bump();
        return;
    }
    cJSON *comment = db_comment_get_by_id(db, target_id);
    if (!comment) return;
    int post_id = 0;
    cJSON *kind = cJSON_GetObjectItem(comment, "target_type");
    if (cJSON_IsString(kind) && strcmp(kind->valuestring, "post") == 0) post_id = json_int(comment, "target_id", 0);
    cJSON_Delete(comment);
    if (post_id > 0) {
        cJSON *post = db_post_get_by_id(db, post_id);
        cJSON *slug = post ? cJSON_GetObjectItem(post, "slug") : NULL;
        if (cJSON_IsString(slug)) page_cache_invalidate_post(slug->valuestring);
        else page_cache_invalidate_all();
        if (post) cJSON_Delete(post);
    } else {
        page_cache_invalidate_all();
    }
}

void handler_admin_moderation_action(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0;
    char role[32] = {0};
    if (!require_reviewer(req, res, &uid, role, sizeof(role))) return;
    cwist_query_map *form = cwist_query_map_create();
    if (req->body && req->body->data) cwist_query_map_parse(form, req->body->data);
    const char *target_type = cwist_query_map_get(form, "target_type");
    const char *id_string = cwist_query_map_get(form, "target_id");
    const char *action = cwist_query_map_get(form, "action");
    int target_id = id_string ? atoi(id_string) : 0;
    bool valid = target_id > 0 && target_type &&
                 (strcmp(target_type, MODERATION_POST) == 0 || strcmp(target_type, MODERATION_COMMENT) == 0);
    bool ok = false;
    if (valid && action && strcmp(action, "approve") == 0) {
        ok = db_moderation_approve(req->db, target_type, target_id, uid);
    } else if (valid && action && strcmp(action, "reject") == 0) {
        ok = db_moderation_reject(req->db, target_type, target_id, uid);
    }
    if (ok) {
        char target[96];
        snprintf(target, sizeof(target), "%s:%d", target_type, target_id);
        audit_record(uid, "moderation.decision", target, action, "approval queue");
    }
    cwist_query_map_destroy(form);
    if (ok) {
        invalidate_moderated_target(req->db, target_type, target_id);
        redirect(res, strcmp(action, "approve") == 0 ? "/admin/moderation?msg=Approved" : "/admin/moderation?msg=Rejected");
    } else {
        redirect(res, "/admin/moderation?msg=Action failed");
    }
}

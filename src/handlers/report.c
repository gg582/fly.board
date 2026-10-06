#define _POSIX_C_SOURCE 200809L
#include "handlers_internal.h"

/* Content reports: anyone who can see a post or comment may report it;
 * admins work the queue at /admin/reports. */

#define REPORT_DETAIL_MAX 1000

static const char *const k_reasons[] = {"spam", "abuse", "illegal", "privacy", "other"};

static bool reason_valid(const char *reason) {
    if (!reason) return false;
    for (size_t i = 0; i < sizeof(k_reasons) / sizeof(k_reasons[0]); i++) {
        if (strcmp(reason, k_reasons[i]) == 0) return true;
    }
    return false;
}

/* Post a target lives on, or 0 when the target is gone or hidden from this
 * viewer. *slug_out gets the post's slug. */
static int resolve_target(cwist_db *db, const char *type, int id, int uid, const char *role,
                          char *slug_out, size_t slug_size) {
    int post_id = 0;
    if (strcmp(type, "post") == 0) {
        post_id = id;
    } else if (strcmp(type, "comment") == 0) {
        cJSON *c = db_comment_get_by_id(db, id);
        if (!c) return 0;
        cJSON *tt = cJSON_GetObjectItem(c, "target_type");
        bool on_post = cJSON_IsString(tt) && strcmp(tt->valuestring, "post") == 0 && !json_int(c, "deleted", 0);
        post_id = on_post ? json_int(c, "target_id", 0) : 0;
        cJSON_Delete(c);
    }
    if (post_id <= 0) return 0;
    cJSON *post = db_post_get_by_id(db, post_id);
    bool visible = post && (post_is_public(post) || is_author_or_admin(post, uid, role));
    if (visible) {
        cJSON *slug = cJSON_GetObjectItem(post, "slug");
        snprintf(slug_out, slug_size, "%s", cJSON_IsString(slug) ? slug->valuestring : "");
    }
    if (post) cJSON_Delete(post);
    return visible ? post_id : 0;
}

void handler_report_post(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0;
    char role[32] = {0};
    auth_is_logged_in(req, &uid, role, sizeof(role));
    cwist_query_map *kv = cwist_query_map_create();
    if (req->body && req->body->data) cwist_query_map_parse(kv, req->body->data);

    spam_verdict_t verdict = spam_guard_check(req, uid, role, cwist_query_map_get(kv, SPAM_FIELD_TRAP),
                                              cwist_query_map_get(kv, SPAM_FIELD_TOKEN));
    if (verdict != SPAM_OK) {
        cwist_query_map_destroy(kv);
        spam_guard_reject(res, verdict);
        return;
    }

    const char *type = cwist_query_map_get(kv, "target_type");
    const char *id_str = cwist_query_map_get(kv, "target_id");
    const char *reason = cwist_query_map_get(kv, "reason");
    const char *detail = cwist_query_map_get(kv, "detail");
    int target_id = id_str ? atoi(id_str) : 0;
    char slug[256] = {0};
    int post_id = (type && target_id > 0) ? resolve_target(req->db, type, target_id, uid, role, slug, sizeof(slug)) : 0;
    if (post_id <= 0 || !reason_valid(reason) || (detail && strlen(detail) > REPORT_DETAIL_MAX)) {
        cwist_query_map_destroy(kv);
        res->status_code = CWIST_HTTP_BAD_REQUEST;
        cwist_sstring_assign(res->body, "Invalid report");
        return;
    }
    int stored = db_report_create(req->db, type, target_id, post_id, reason, detail, uid);
    cwist_query_map_destroy(kv);
    if (stored < 0) {
        res->status_code = CWIST_HTTP_INTERNAL_ERROR;
        cwist_sstring_assign(res->body, "Could not save the report");
        return;
    }
    if (stored > 0) CWIST_LOG_INFO("Report filed: %s %d reason=%s", type, target_id, reason);

    cwist_sstring *b = cwist_sstring_create();
    cwist_sstring_assign(b, "<div class='card' style='max-width:560px;margin:40px auto'><h2 style='margin-top:0'>Thank you</h2><p>");
    cwist_sstring_append(b, stored > 0 ? "Your report was sent to the moderators. They will review it soon."
                                       : "You have already reported this, and the report is still open.");
    cwist_sstring_append(b, "</p><a class='btn' href='/post/");
    cwist_sstring_append_escaped(b, slug);
    cwist_sstring_append(b, "'>Back to the post</a></div>");
    char *pp = get_profile_pic(req->db, uid, role);
    cwist_sstring *page = render_page("Report sent", b->data, is_dark(req), role, pp, is_mobile_request(req));
    cwist_sstring_destroy(b);
    free(pp);
    cwist_http_header_add(&res->headers, "Cache-Control", "no-store");
    send_html_res(res, page);
}

/* ---- Admin queue ---- */

/* Group reports by target, newest first, and attach what the moderator
 * needs to judge it: the post title and link, or the comment text. */
static cJSON *group_reports(cwist_db *db, cJSON *reports) {
    cJSON *groups = cJSON_CreateArray();
    cJSON *r = NULL;
    cJSON_ArrayForEach(r, reports) {
        cJSON *tt = cJSON_GetObjectItem(r, "target_type");
        const char *type = cJSON_IsString(tt) ? tt->valuestring : "";
        int id = json_int(r, "target_id", 0);
        cJSON *group = NULL;
        cJSON *g = NULL;
        cJSON_ArrayForEach(g, groups) {
            cJSON *gt = cJSON_GetObjectItem(g, "target_type");
            if (json_int(g, "target_id", 0) == id && cJSON_IsString(gt) && strcmp(gt->valuestring, type) == 0) {
                group = g;
                break;
            }
        }
        if (!group) {
            group = cJSON_CreateObject();
            cJSON_AddStringToObject(group, "target_type", type);
            cJSON_AddNumberToObject(group, "target_id", id);
            int post_id = json_int(r, "post_id", 0);
            cJSON_AddNumberToObject(group, "post_id", post_id);
            cJSON *post = db_post_get_by_id(db, post_id);
            bool exists = post != NULL;
            if (post) {
                cJSON *slug = cJSON_GetObjectItem(post, "slug");
                cJSON *title = cJSON_GetObjectItem(post, "title");
                cJSON_AddStringToObject(group, "post_slug", cJSON_IsString(slug) ? slug->valuestring : "");
                cJSON_AddStringToObject(group, "post_title", cJSON_IsString(title) ? title->valuestring : "");
                cJSON *author = cJSON_GetObjectItem(post, "author_name");
                if (strcmp(type, "post") == 0) {
                    cJSON_AddStringToObject(group, "author", cJSON_IsString(author) ? author->valuestring : "anonymous");
                    cJSON *content = cJSON_GetObjectItem(post, "content");
                    cJSON_AddStringToObject(group, "excerpt", cJSON_IsString(content) ? content->valuestring : "");
                }
                cJSON_Delete(post);
            }
            if (strcmp(type, "comment") == 0) {
                cJSON *c = db_comment_get_by_id(db, id);
                exists = c && !json_int(c, "deleted", 0);
                if (c) {
                    cJSON *content = cJSON_GetObjectItem(c, "content");
                    cJSON *author = cJSON_GetObjectItem(c, "author_name");
                    cJSON_AddStringToObject(group, "excerpt", cJSON_IsString(content) ? content->valuestring : "");
                    cJSON_AddStringToObject(group, "author", cJSON_IsString(author) ? author->valuestring : "anonymous");
                    cJSON_Delete(c);
                }
            }
            cJSON_AddBoolToObject(group, "exists", exists);
            cJSON_AddItemToObject(group, "reports", cJSON_CreateArray());
            cJSON_AddItemToArray(groups, group);
        }
        cJSON_AddItemToArray(cJSON_GetObjectItem(group, "reports"), cJSON_Duplicate(r, true));
    }
    return groups;
}

void handler_admin_reports_get(cwist_http_request *req, cwist_http_response *res) {
    if (!auth_require_admin(req, res)) return;
    int uid = 0;
    char role[32] = {0};
    auth_is_logged_in(req, &uid, role, sizeof(role));
    const char *status = cwist_query_map_get(req->query_params, "status");
    if (!status || !(strcmp(status, "open") == 0 || strcmp(status, "resolved") == 0 ||
                     strcmp(status, "dismissed") == 0 || strcmp(status, "all") == 0)) {
        status = "open";
    }
    cJSON *reports = db_report_list(req->db, status);
    cJSON *groups = group_reports(req->db, reports);
    char *pp = get_profile_pic(req->db, uid, role);
    cwist_sstring *page = render_admin_reports(groups, status, cwist_query_map_get(req->query_params, "msg"),
                                               is_dark(req), pp, is_mobile_request(req));
    if (reports) cJSON_Delete(reports);
    if (groups) cJSON_Delete(groups);
    free(pp);
    cwist_http_header_add(&res->headers, "Cache-Control", "no-store, private");
    send_html_res(res, page);
}

void handler_admin_reports_action(cwist_http_request *req, cwist_http_response *res) {
    if (!auth_require_admin(req, res)) return;
    int uid = 0;
    char role[32] = {0};
    auth_is_logged_in(req, &uid, role, sizeof(role));
    cwist_query_map *kv = cwist_query_map_create();
    if (req->body && req->body->data) cwist_query_map_parse(kv, req->body->data);
    const char *type = cwist_query_map_get(kv, "target_type");
    const char *id_str = cwist_query_map_get(kv, "target_id");
    const char *action = cwist_query_map_get(kv, "action");
    int id = id_str ? atoi(id_str) : 0;
    const char *msg = "error";

    bool is_post = type && strcmp(type, "post") == 0;
    bool is_comment = type && strcmp(type, "comment") == 0;
    if ((is_post || is_comment) && id > 0 && action) {
        if (strcmp(action, "dismiss") == 0) {
            if (db_report_close_target(req->db, type, id, REPORT_STATUS_DISMISSED, "dismissed", uid) >= 0) msg = "dismissed";
        } else if (strcmp(action, "resolve") == 0) {
            if (db_report_close_target(req->db, type, id, REPORT_STATUS_RESOLVED, "kept", uid) >= 0) msg = "resolved";
        } else if (strcmp(action, "delete") == 0) {
            bool removed = false;
            if (is_post) {
                cJSON *post = db_post_get_by_id(req->db, id);
                if (post) {
                    cJSON *slug = cJSON_GetObjectItem(post, "slug");
                    removed = post_delete_everything(req->db, id, cJSON_IsString(slug) ? slug->valuestring : NULL);
                    cJSON_Delete(post);
                } else {
                    removed = true; /* already gone */
                }
            } else {
                cJSON *c = db_comment_get_by_id(req->db, id);
                int post_id = c ? json_int(c, "target_id", 0) : 0;
                removed = !c || db_comment_delete_admin(req->db, id);
                if (c) cJSON_Delete(c);
                cJSON *post = post_id > 0 ? db_post_get_by_id(req->db, post_id) : NULL;
                cJSON *slug = post ? cJSON_GetObjectItem(post, "slug") : NULL;
                if (cJSON_IsString(slug)) page_cache_invalidate_post(slug->valuestring);
                else page_cache_invalidate_all();
                if (post) cJSON_Delete(post);
            }
            if (removed && db_report_close_target(req->db, type, id, REPORT_STATUS_RESOLVED, "deleted", uid) >= 0) {
                msg = "deleted";
            }
        }
        CWIST_LOG_INFO("Report action: %s %s %d by uid=%d -> %s", action, type, id, uid, msg);
    }
    cwist_query_map_destroy(kv);
    char url[64];
    snprintf(url, sizeof(url), "/admin/reports?msg=%s", msg);
    redirect(res, url);
}

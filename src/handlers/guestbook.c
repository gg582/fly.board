#define _POSIX_C_SOURCE 200809L
#include "handlers_internal.h"
#include "../db/guestbook.h"
#include "../render/render_guestbook.h"

/* ---------------------------------------------------------------------------
 * Guestbook handlers (see src/render/render_guestbook.h for how the section
 * reaches the profile page). Like every other POST form in this project the
 * requests carry no CSRF token: the session cookie is SameSite and the write
 * policy / spam guard below are the existing defenses, copied from
 * src/handlers/comment.c.
 * ------------------------------------------------------------------------- */

#define GUESTBOOK_MAX_CONTENT 2000

static void guestbook_redirect(cwist_http_response *res, int owner_uid, int page) {
    char url[128];
    if (page > 1) snprintf(url, sizeof(url), "/user/%d?page=%d", owner_uid, page);
    else snprintf(url, sizeof(url), "/user/%d", owner_uid);
    redirect(res, url);
}

/* GET /guestbook/list?owner=<uid>&page=<n> — partial HTML for the profile
 * page's guestbook section. Same-origin fetch() from the profile page sends
 * the session cookie, so viewer permissions are evaluated exactly like any
 * other authenticated request. */
void handler_guestbook_list_get(cwist_http_request *req, cwist_http_response *res) {
    int viewer_uid = 0;
    char viewer_role[32] = {0};
    auth_is_logged_in(req, &viewer_uid, viewer_role, sizeof(viewer_role));
    int owner_uid = atoi(cwist_query_map_get(req->query_params, "owner")
                             ? cwist_query_map_get(req->query_params, "owner") : "0");
    int page = atoi(cwist_query_map_get(req->query_params, "page")
                        ? cwist_query_map_get(req->query_params, "page") : "1");
    if (owner_uid <= 0) {
        res->status_code = CWIST_HTTP_BAD_REQUEST;
        cwist_sstring_assign(res->body, "Bad request");
        return;
    }
    cJSON *owner = db_user_get_by_id(req->db, owner_uid);
    if (!owner) {
        res->status_code = CWIST_HTTP_NOT_FOUND;
        cwist_sstring_assign(res->body, "Not found");
        return;
    }
    cJSON_Delete(owner);
    send_html_res(res, render_guestbook_section(req->db, owner_uid, viewer_uid, viewer_role, page));
}

/* POST /guestbook/post — fields: owner, content, optional name. Logged-in
 * visitors post as themselves; anonymous posts are accepted only when the
 * owner's guestbook_anon flag is set. */
void handler_guestbook_post(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0;
    char role[32] = {0};
    auth_is_logged_in(req, &uid, role, sizeof(role));

    cwist_query_map *kv = cwist_query_map_create();
    if (req->body && req->body->data) cwist_query_map_parse(kv, req->body->data);
    int owner_uid = atoi(cwist_query_map_get(kv, "owner") ? cwist_query_map_get(kv, "owner") : "0");
    const char *content_in = cwist_query_map_get(kv, "content");
    const char *name_in = cwist_query_map_get(kv, "name");

    spam_verdict_t verdict = spam_guard_check(req, uid, role, cwist_query_map_get(kv, SPAM_FIELD_TRAP),
                                              cwist_query_map_get(kv, SPAM_FIELD_TOKEN));
    if (verdict != SPAM_OK) {
        cwist_query_map_destroy(kv);
        spam_guard_reject(res, verdict);
        return;
    }

    /* Trim and validate content. */
    char content[GUESTBOOK_MAX_CONTENT + 8];
    snprintf(content, sizeof(content), "%s", content_in ? content_in : "");
    char *start = content;
    while (*start == ' ' || *start == '\t' || *start == '\n' || *start == '\r') start++;
    char *end = start + strlen(start);
    while (end > start && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\n' || end[-1] == '\r')) *--end = '\0';
    if (!start[0] || (content_in && strlen(content_in) > GUESTBOOK_MAX_CONTENT)) {
        cwist_query_map_destroy(kv);
        res->status_code = CWIST_HTTP_BAD_REQUEST;
        cwist_sstring_assign(res->body, "Bad request: empty or too long");
        return;
    }

    cJSON *owner = db_user_get_by_id(req->db, owner_uid);
    if (!owner) {
        cwist_query_map_destroy(kv);
        res->status_code = CWIST_HTTP_NOT_FOUND;
        cwist_sstring_assign(res->body, "Not found");
        return;
    }
    cJSON *guestbook_anon = cJSON_GetObjectItem(owner, "guestbook_anon");
    bool anon_allowed = (guestbook_anon && cJSON_IsNumber(guestbook_anon)) ? guestbook_anon->valueint != 0 : false;
    cJSON_Delete(owner);

    int author_uid = 0;
    char author_name[128] = {0};
    if (uid > 0) {
        cJSON *u = db_user_get_by_id(req->db, uid);
        if (u) {
            cJSON *n = cJSON_GetObjectItem(u, "username");
            if (n && cJSON_IsString(n) && n->valuestring) snprintf(author_name, sizeof(author_name), "%s", n->valuestring);
            cJSON_Delete(u);
        }
        if (!author_name[0]) snprintf(author_name, sizeof(author_name), "%s", role[0] ? role : "User");
        author_uid = uid;
    } else if (anon_allowed) {
        snprintf(author_name, sizeof(author_name), "%s", (name_in && name_in[0]) ? name_in : "Anonymous");
    } else {
        cwist_query_map_destroy(kv);
        /* Members only: anonymous visitors are pointed at the login page. */
        redirect(res, "/login");
        return;
    }

    db_guestbook_create(req->db, owner_uid, author_uid, author_name, start);
    cwist_query_map_destroy(kv);
    CWIST_LOG_INFO("Guestbook post: owner=%d author_uid=%d name='%s'", owner_uid, author_uid, author_name);
    guestbook_redirect(res, owner_uid, 1);
}

/* POST /guestbook/delete — field: id. Permission: profile owner, site admin,
 * or the entry's author (see db_guestbook_delete). */
void handler_guestbook_delete_post(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0;
    char role[32] = {0};
    if (!auth_require_login(req, res, &uid, role, sizeof(role))) return;
    cwist_query_map *kv = cwist_query_map_create();
    if (req->body && req->body->data) cwist_query_map_parse(kv, req->body->data);
    int id = atoi(cwist_query_map_get(kv, "id") ? cwist_query_map_get(kv, "id") : "0");
    int page = atoi(cwist_query_map_get(kv, "page") ? cwist_query_map_get(kv, "page") : "1");
    cJSON *entry = id > 0 ? db_guestbook_get(req->db, id) : NULL;
    int owner_uid = entry ? (cJSON_GetObjectItem(entry, "owner_uid")
                                 ? cJSON_GetObjectItem(entry, "owner_uid")->valueint : 0) : 0;
    if (entry) cJSON_Delete(entry);
    if (owner_uid <= 0 || !db_guestbook_delete(req->db, id, uid, role)) {
        cwist_query_map_destroy(kv);
        res->status_code = CWIST_HTTP_FORBIDDEN;
        cwist_sstring_assign(res->body, "Forbidden");
        return;
    }
    cwist_query_map_destroy(kv);
    guestbook_redirect(res, owner_uid, page);
}

/* POST /guestbook/settings — owner only. The checkbox posts guestbook_anon=1
 * when checked and nothing when unchecked. */
void handler_guestbook_settings_post(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0;
    char role[32] = {0};
    if (!auth_require_login(req, res, &uid, role, sizeof(role))) return;
    (void)role;
    cwist_query_map *kv = cwist_query_map_create();
    if (req->body && req->body->data) cwist_query_map_parse(kv, req->body->data);
    bool allow = cwist_query_map_get(kv, "guestbook_anon") != NULL;
    cwist_query_map_destroy(kv);
    db_guestbook_set_anon(req->db, uid, allow);
    CWIST_LOG_INFO("Guestbook anon toggle: uid=%d allow=%d", uid, (int)allow);
    guestbook_redirect(res, uid, 1);
}

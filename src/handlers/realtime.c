#define _POSIX_C_SOURCE 200809L
#include "handlers_internal.h"
#include "config/write_policy.h"
#include "realtime/realtime.h"
#include <cwist/net/http/sse.h>
#include <limits.h>

static void realtime_no_store(cwist_http_response *res) {
    cwist_http_header_add(&res->headers, "Cache-Control", "no-store");
}

static bool realtime_post_visible(cwist_http_request *req, int post_id) {
    if (post_id <= 0) return false;
    int uid = 0;
    char role[32] = {0};
    auth_is_logged_in(req, &uid, role, sizeof(role));
    cJSON *post = db_post_get_by_id(req->db, post_id);
    bool visible = post && (post_is_public(post) || is_author_or_admin(post, uid, role)) &&
                   post_lock_viewer_ok(req, post, uid, role);
    if (post) cJSON_Delete(post);
    return visible;
}

static int realtime_form_post_id(cwist_query_map *form) {
    const char *value = cwist_query_map_get(form, "post_id");
    if (!value || !value[0]) return 0;
    char *end = NULL;
    long id = strtol(value, &end, 10);
    return end && *end == '\0' && id > 0 && id <= INT_MAX ? (int)id : 0;
}

static void realtime_mutation_result(cwist_http_response *res, realtime_result_t result) {
    realtime_no_store(res);
    if (result == REALTIME_RATE_LIMITED) {
        res->status_code = CWIST_HTTP_TOO_MANY_REQUESTS;
        cwist_http_header_add(&res->headers, "Retry-After", "3");
        cwist_sstring_assign(res->body, "Typing updates are rate limited");
    } else if (result != REALTIME_OK) {
        res->status_code = result == REALTIME_FULL ? CWIST_HTTP_SERVICE_UNAVAILABLE
                                                   : CWIST_HTTP_BAD_REQUEST;
        cwist_sstring_assign(res->body, "Invalid or exhausted realtime session");
    } else {
        res->status_code = CWIST_HTTP_NO_CONTENT;
        cwist_sstring_assign(res->body, "");
    }
}

void handler_realtime_presence_post(cwist_http_request *req, cwist_http_response *res) {
    if (!g_config.realtime_enabled) {
        res->status_code = CWIST_HTTP_NOT_FOUND;
        return;
    }
    cwist_query_map *form = cwist_query_map_create();
    if (req->body && req->body->data) cwist_query_map_parse(form, req->body->data);
    int post_id = realtime_form_post_id(form);
    const char *session = cwist_query_map_get(form, "session");
    const char *active_value = cwist_query_map_get(form, "active");
    bool active = !active_value || strcmp(active_value, "0") != 0;
    if (!realtime_post_visible(req, post_id)) {
        cwist_query_map_destroy(form);
        res->status_code = CWIST_HTTP_NOT_FOUND;
        return;
    }
    realtime_result_t result = realtime_presence_at(post_id, session, active, realtime_now_ms());
    if (result == REALTIME_OK)
        (void)fly_nats_publish_realtime_presence(post_id, session, active);
    cwist_query_map_destroy(form);
    realtime_mutation_result(res, result);
}

void handler_realtime_typing_post(cwist_http_request *req, cwist_http_response *res) {
    if (!g_config.realtime_enabled || !g_config.realtime_typing) {
        res->status_code = CWIST_HTTP_NOT_FOUND;
        return;
    }
    int uid = 0;
    char role[32] = {0};
    auth_is_logged_in(req, &uid, role, sizeof(role));
    if (!write_policy_can_comment(role)) {
        res->status_code = CWIST_HTTP_FORBIDDEN;
        return;
    }
    cwist_query_map *form = cwist_query_map_create();
    if (req->body && req->body->data) cwist_query_map_parse(form, req->body->data);
    int post_id = realtime_form_post_id(form);
    const char *session = cwist_query_map_get(form, "session");
    const char *active_value = cwist_query_map_get(form, "active");
    bool active = active_value && strcmp(active_value, "0") != 0;
    if (!realtime_post_visible(req, post_id)) {
        cwist_query_map_destroy(form);
        res->status_code = CWIST_HTTP_NOT_FOUND;
        return;
    }
    realtime_result_t result = realtime_typing_at(post_id, session, active, realtime_now_ms());
    if (result == REALTIME_OK)
        (void)fly_nats_publish_realtime_typing(post_id, session, active);
    cwist_query_map_destroy(form);
    realtime_mutation_result(res, result);
}

static int realtime_after_id(cwist_http_request *req) {
    const char *last = cwist_http_header_get(req->headers, "Last-Event-ID");
    const char *value = last && last[0] ? last : cwist_query_map_get(req->query_params, "after");
    if (!value || !value[0]) return 0;
    char *end = NULL;
    long id = strtol(value, &end, 10);
    return end && *end == '\0' && id >= 0 && id <= INT_MAX ? (int)id : 0;
}

void handler_realtime_events_get(cwist_http_request *req, cwist_http_response *res) {
    if (!g_config.realtime_enabled) {
        res->status_code = CWIST_HTTP_NOT_FOUND;
        return;
    }
    const char *post_value = cwist_query_map_get(req->query_params, "post_id");
    int post_id = post_value ? atoi(post_value) : 0;
    const char *session = cwist_query_map_get(req->query_params, "session");
    if (!realtime_post_visible(req, post_id) || !realtime_session_valid(session)) {
        res->status_code = CWIST_HTTP_NOT_FOUND;
        return;
    }
    (void)realtime_presence_at(post_id, session, true, realtime_now_ms());
    int64_t now = realtime_now_ms();
    char viewers[32];
    realtime_viewer_label(realtime_viewer_count_at(post_id, now), viewers, sizeof(viewers));
    cJSON *state = cJSON_CreateObject();
    cJSON_AddStringToObject(state, "viewers", viewers);
    cJSON_AddNumberToObject(state, "typing",
                            g_config.realtime_typing ? realtime_typing_count_at(post_id, session, now) : 0);
    cJSON_AddStringToObject(state, "transport", fly_nats_is_connected() ? "nats" : "local");
    char *state_json = cJSON_PrintUnformatted(state);
    cJSON_Delete(state);

    cwist_sse_response_init(res);
    realtime_no_store(res);
    cwist_sse_response_event(res, "state", NULL, 2000, state_json ? state_json : "{}");
    cwist_free(state_json);

    int after = realtime_after_id(req);
    cJSON *comments = db_comment_list_by_target_after(req->db, "post", post_id, after, 50);
    cJSON *comment = NULL;
    cJSON_ArrayForEach(comment, comments) {
        int id = json_int(comment, "id", 0);
        if (id <= after) continue;
        char id_text[32];
        char data[96];
        snprintf(id_text, sizeof(id_text), "%d", id);
        snprintf(data, sizeof(data), "{\"post_id\":%d,\"comment_id\":%d}", post_id, id);
        cwist_sse_response_event(res, "comment", id_text, -1, data);
        after = id;
    }
    if (comments) cJSON_Delete(comments);
    cwist_sse_response_comment(res, "reconnect");
}

void handler_realtime_comments_get(cwist_http_request *req, cwist_http_response *res) {
    if (!g_config.realtime_enabled) {
        res->status_code = CWIST_HTTP_NOT_FOUND;
        return;
    }
    const char *id_value = cwist_query_map_get(req->path_params, "id");
    int post_id = id_value ? atoi(id_value) : 0;
    if (!realtime_post_visible(req, post_id)) {
        res->status_code = CWIST_HTTP_NOT_FOUND;
        return;
    }
    int uid = 0;
    char role[32] = {0};
    auth_is_logged_in(req, &uid, role, sizeof(role));
    cJSON *comments = db_comment_list_by_target(req->db, "post", post_id);
    cJSON *files = db_file_list_by_post(req->db, post_id);
    cwist_sstring *html = render_post_comment_list(comments, role, uid, post_id, files);
    if (files) cJSON_Delete(files);
    cwist_sstring_assign(res->body, html->data);
    cwist_sstring_destroy(html);
    if (comments) cJSON_Delete(comments);
    cwist_http_header_add(&res->headers, "Content-Type", "text/html; charset=utf-8");
    realtime_no_store(res);
}

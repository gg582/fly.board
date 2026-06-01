#define _POSIX_C_SOURCE 200809L
#include "handlers_internal.h"

/* ---- Home ---- */
void handler_home(cwist_http_request *req, cwist_http_response *res) {
    bool dark = is_dark(req);
    int uid = 0;
    char role[32] = {0};
    auth_is_logged_in(req, &uid, role, sizeof(role));
    char *pp = get_profile_pic(req->db, uid, role);
    cJSON *posts = db_post_recent(req->db, 12);
    cwist_sstring *page = render_post_list(posts, NULL, dark, role, 1, 1, "", NULL, NULL, pp, uid, is_mobile_request(req), NULL);
    if (posts) cJSON_Delete(posts);
    send_html_res(res, page);
    free(pp);
}

/* ---- Theme JSON ---- */
void handler_theme_json(cwist_http_request *req, cwist_http_response *res) {
    const char *mode = cwist_query_map_get(req->query_params, "mode");
    bool dark = mode && strcmp(mode, "dark") == 0;
    char *json = theme_build_json(dark);
    cwist_http_header_add(&res->headers, "Content-Type", "application/json; charset=utf-8");
    if (json) {
        cwist_sstring_assign(res->body, json);
        free(json);
    }
}

/* ---- Auth ---- */

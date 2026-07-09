#define _POSIX_C_SOURCE 200809L
#include "handlers_internal.h"

/* ---- Home ---- */
void handler_home(cwist_http_request *req, cwist_http_response *res) {
    bool dark = is_dark(req);
    int uid = 0;
    char role[32] = {0};
    auth_is_logged_in(req, &uid, role, sizeof(role));
    bool mobile = is_mobile_request(req);

    char key[256];
    page_cache_key_home(key, sizeof(key), dark, mobile, role, uid);
    const char *cached = NULL;
    size_t cached_len = 0;
    uint32_t ttl = 0;
    if (page_cache_get(key, &cached, &cached_len, &ttl)) {
        send_cached_html_res(res, cached, cached_len, ttl);
        page_cache_release(key);
        return;
    }

    bool leader = false;
    cwist_sstring *shared = reqshare_wait_or_start(key, &leader);
    if (!leader) {
        send_html_res(res, shared);
        return;
    }

    char *pp = get_profile_pic(req->db, uid, role);
    cJSON *posts = db_post_recent(req->db, 12);
    cwist_sstring *page = render_post_list(posts, NULL, dark, role, 1, 1, "", NULL, NULL, pp, uid, mobile, NULL);
    if (posts) cJSON_Delete(posts);
    if (page) {
        page_cache_set(key, page->data, page->size, 60);
        reqshare_finish(key, page);
    } else {
        reqshare_finish(key, NULL);
    }
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

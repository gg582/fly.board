#define _POSIX_C_SOURCE 200809L
#include "handlers_internal.h"
#include "db/db_internal.h"
#include "db/db_portability.h"
#include "engine/forkgate.h"
#include "engine/refresh.h"

/* ---- Home ---- */
typedef struct {
    char key[256];
    char role[32];
    uint64_t generation;
    int uid;
    bool dark;
    bool mobile;
} home_refresh_job;

static void home_refresh(void *arg) {
    home_refresh_job *job = arg;
    cwist_db db = {0};
    fly_forkgate_enter();
    bool opened = fly_db_open_main(&db.conn) == SQLITE_OK && db_configure_connection(db.conn);
    fly_forkgate_leave();
    if (opened) {
        char *pp = get_profile_pic(&db, job->uid, job->role);
        cJSON *posts = db_post_recent(&db, 12);
        social_apply_blocks(&db, job->uid, posts, "user_id", true);
        cwist_sstring *page = render_post_list(posts, NULL, job->dark, job->role, 1, 1, "",
                                               NULL, NULL, NULL, pp, job->uid, job->mobile, NULL);
        if (page) {
            page_cache_set_if_generation(job->key, page->data, page->size, 60, job->generation);
            cwist_sstring_destroy(page);
        }
        if (posts) cJSON_Delete(posts);
        free(pp);
    }
    if (db.conn) {
        fly_forkgate_enter();
        sqlite3_close(db.conn);
        fly_forkgate_leave();
    }
}

static void home_schedule_refresh(const char *key, bool dark, bool mobile,
                                  const char *role, int uid) {
    home_refresh_job *job = calloc(1, sizeof(*job));
    if (!job) return;
    snprintf(job->key, sizeof(job->key), "%s", key);
    snprintf(job->role, sizeof(job->role), "%s", role);
    job->generation = page_cache_generation();
    job->uid = uid;
    job->dark = dark;
    job->mobile = mobile;
    engine_refresh_schedule(key, home_refresh, free, job);
}
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
    if (page_cache_get_stale(key, &cached, &cached_len)) {
        home_schedule_refresh(key, dark, mobile, role, uid);
        cwist_http_header_add(&res->headers, "X-Fly-Cache-Status", "stale");
        send_cached_html_res(res, cached, cached_len, 0);
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
    social_apply_blocks(req->db, uid, posts, "user_id", true);
    cwist_sstring *page = render_post_list(posts, NULL, dark, role, 1, 1, "", NULL, NULL, NULL, pp, uid, mobile, NULL);
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
        cwist_free(json);
    }
}

/* ---- Auth ---- */

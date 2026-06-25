#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "tasfa_internal.h"

void handler_asset_tasfa_handshake(cwist_http_request *req, cwist_http_response *res) {
    char path[PATH_MAX], filename[512];
    const char *mime = NULL;
    const char *scope = cwist_query_map_get(req->path_params, "scope");
    if (!resolve_asset_scope_path(req->db, scope,
                                  cwist_query_map_get(req->path_params, "filename"),
                                  path, sizeof(path), filename, sizeof(filename), &mime)) {
        send_json_response(res, session_error_json("asset not found"), CWIST_HTTP_NOT_FOUND);
        return;
    }
    if (!strcmp(scope ? scope : "", "profile") && !is_profile_pic_asset(req->db, filename)) {
        send_json_response(res, session_error_json("asset not found"), CWIST_HTTP_NOT_FOUND);
        return;
    }
    bool is_image = mime && strncmp(mime, "image/", 6) == 0;
    if (is_image) {
        const char *w_str = cwist_query_map_get(req->query_params, "w");
        const char *h_str = cwist_query_map_get(req->query_params, "h");
        if (w_str && h_str) {
            int w = atoi(w_str);
            int h = atoi(h_str);
            if (w > 0 && h > 0) {
                if (w >= h) {
                    if (w < 1080) w = 1080;
                    if (h < 720) h = 720;
                } else {
                    if (w < 720) w = 720;
                    if (h < 1080) h = 1080;
                }
                if (w > 3072) w = 3072;
                if (h > 3072) h = 3072;

                char scope_fname[512] = {0};
                const char *raw_fname = cwist_query_map_get(req->path_params, "filename");
                snprintf(scope_fname, sizeof(scope_fname), "%s_", scope ? scope : "unknown");
                char *p_sf = scope_fname + strlen(scope_fname);
                for (int i = 0; raw_fname && raw_fname[i] && (size_t)(p_sf - scope_fname) < sizeof(scope_fname) - 1; i++) {
                    *p_sf++ = (raw_fname[i] == '/') ? '_' : raw_fname[i];
                }
                *p_sf = '\0';

                char thumb_img_path[PATH_MAX];
                snprintf(thumb_img_path, sizeof(thumb_img_path), "public/uploads/.thumbs/asset_%s_%dx%d.webp", scope_fname, w, h);
                struct stat pst;
                if (stat(thumb_img_path, &pst) == 0 && S_ISREG(pst.st_mode) && pst.st_size > 0) {
                    snprintf(path, sizeof(path), "%s", thumb_img_path);
                    mime = "image/webp";
                } else if (strcmp(scope, "img") != 0) {
                    /* uploads and other dynamic scopes: generate on demand if not pre-generated */
                    if (generate_image_thumb(path, thumb_img_path, w, h)) {
                        snprintf(path, sizeof(path), "%s", thumb_img_path);
                        mime = "image/webp";
                    }
                }
                /* For img scope, rely on startup pre-generation; fall through to original file if thumb missing */
            }
        }
    }

    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 0) {
        send_json_response(res, session_error_json("asset not found"), CWIST_HTTP_NOT_FOUND);
        return;
    }
    bool mobile = is_mobile_request(req);
    int requested_chunk_size = atoi(cwist_query_map_get(req->query_params, "chunk_size") ? cwist_query_map_get(req->query_params, "chunk_size") : "0");
    tasfa_queue_sweep(g_q_downloads, tasfa_download_session_limit(), 300);
    int score = link_score_from_inputs(
        cwist_query_map_get(req->query_params, "link_stability_score"),
        cwist_query_map_get(req->query_params, "link_effective_type"),
        cwist_query_map_get(req->query_params, "link_downlink_mbps"),
        cwist_query_map_get(req->query_params, "link_rtt_ms"),
        cwist_query_map_get(req->query_params, "link_retry_events"),
        cwist_query_map_get(req->query_params, "link_timeout_events"),
        cwist_query_map_get(req->query_params, "link_save_data")
    );
    cJSON *obj = NULL;
    char media_name[512];
    const char *raw_fname = cwist_query_map_get(req->path_params, "filename");
    snprintf(media_name, sizeof(media_name), "%s_", scope ? scope : "unknown");
    char *p = media_name + strlen(media_name);
    for (int i = 0; raw_fname && raw_fname[i] && (size_t)(p - media_name) < sizeof(media_name) - 1; i++) {
        *p++ = (raw_fname[i] == '/') ? '_' : raw_fname[i];
    }
    *p = '\0';

    int fast_link_max = fast_download_chunk_max_from_request(
        cwist_query_map_get(req->query_params, "link_downlink_mbps"),
        cwist_query_map_get(req->query_params, "link_rtt_ms"),
        cwist_query_map_get(req->query_params, "link_effective_type"),
        cwist_query_map_get(req->query_params, "link_save_data")
    );
    if (!init_download_session(filename, mime, path, (long long)st.st_size, mobile, score, requested_chunk_size, fast_link_max, 0, media_name, &obj)) {
        send_json_response(res, session_error_json("download handshake failed"), CWIST_HTTP_INTERNAL_ERROR);
        return;
    }
    const char *session_id = json_string(obj, "session_id", "");
    if (!tasfa_queue_try_enter(g_q_downloads, tasfa_download_session_limit(), session_id)) {
        cleanup_download_session(session_id);
        cJSON_Delete(obj);
        send_queued_json(res, "too many concurrent downloads", 3);
        return;
    }
    if (!is_client_connected(req)) {
        tasfa_queue_leave(g_q_downloads, tasfa_download_session_limit(), session_id);
        cleanup_download_session(session_id);
        cJSON_Delete(obj);
        send_json_response(res, session_error_json("client disconnected"), CWIST_HTTP_SERVICE_UNAVAILABLE);
        return;
    }
    send_json_response(res, obj, CWIST_HTTP_OK);
}

void handler_asset_tasfa_chunk(cwist_http_request *req, cwist_http_response *res) {
    handler_file_download_chunk(req, res);
}

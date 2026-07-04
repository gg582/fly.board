#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "tasfa_internal.h"

void handler_file_download_handshake(cwist_http_request *req, cwist_http_response *res) {
    const char *id_str = cwist_query_map_get(req->path_params, "id");
    if (!id_str) {
        send_json_response(res, session_error_json("download not found"), CWIST_HTTP_NOT_FOUND);
        return;
    }
    cJSON *file = db_file_get(req->db, atoi(id_str));
    if (!file) {
        send_json_response(res, session_error_json("download not found"), CWIST_HTTP_NOT_FOUND);
        return;
    }
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s", json_string(file, "file_path", ""));
    const char *filename = json_string(file, "filename", "download");
    const char *mime = json_string(file, "mime_type", "application/octet-stream");
    const char *thumb_path = json_string(file, "thumb_path", "");
    const char *preview_path = json_string(file, "preview_path", "");

    char detected_mime[128] = {0};
    if (strcmp(mime, "application/octet-stream") == 0) {
        if (mime_type_from_data(path, detected_mime, sizeof(detected_mime))) {
            mime = detected_mime;
        } else {
            mime = mime_type(filename);
        }
    }

    bool is_image = strncmp(mime, "image/", 6) == 0;
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

                char thumb_img_path[PATH_MAX];
                snprintf(thumb_img_path, sizeof(thumb_img_path), "public/uploads/.thumbs/%d_%dx%d.webp", atoi(id_str), w, h);
                struct stat pst;
                if (stat(thumb_img_path, &pst) != 0 || !S_ISREG(pst.st_mode) || pst.st_size <= 0) {
                    if (generate_image_thumb(path, thumb_img_path, w, h)) {
                        snprintf(path, sizeof(path), "%s", thumb_img_path);
                        mime = "image/webp";
                    }
                } else {
                    snprintf(path, sizeof(path), "%s", thumb_img_path);
                    mime = "image/webp";
                }
            }
        }
    }

    bool wants_preview = false;
    const char *preview_q = cwist_query_map_get(req->query_params, "preview");
    if (preview_q && (strcmp(preview_q, "1") == 0 || strcmp(preview_q, "true") == 0)) {
        wants_preview = true;
    }
    char generated_preview[PATH_MAX] = {0};
    if (wants_preview && strncmp(mime, "video/", 6) == 0) {
        struct stat pst;
        if (preview_path[0] && strncmp(preview_path, "public/uploads/", 15) == 0 &&
            stat(preview_path, &pst) == 0 && S_ISREG(pst.st_mode) && pst.st_size > 0) {
            snprintf(path, sizeof(path), "%s", preview_path);
        } else {
            snprintf(generated_preview, sizeof(generated_preview), "public/uploads/.previews/%d.mp4", atoi(id_str));
            if (!generate_video_preview(path, generated_preview, 1080)) {
                cJSON_Delete(file);
                send_json_response(res, session_error_json("download preview not found"), CWIST_HTTP_NOT_FOUND);
                return;
            }
            db_file_set_preview_paths(req->db, atoi(id_str), thumb_path, generated_preview);
            snprintf(path, sizeof(path), "%s", generated_preview);
        }
        filename = "preview.mp4";
        mime = "video/mp4";
    }

    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 0) {
        cJSON_Delete(file);
        send_json_response(res, session_error_json("download not found"), CWIST_HTTP_NOT_FOUND);
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
    snprintf(media_name, sizeof(media_name), "file_%s", id_str);
    int fast_link_max = fast_download_chunk_max_from_request(
        cwist_query_map_get(req->query_params, "link_downlink_mbps"),
        cwist_query_map_get(req->query_params, "link_rtt_ms"),
        cwist_query_map_get(req->query_params, "link_effective_type"),
        cwist_query_map_get(req->query_params, "link_save_data")
    );
    if (!init_download_session(filename, mime, path, (long long)st.st_size, mobile, score, requested_chunk_size, fast_link_max, atoi(id_str), media_name, &obj)) {
        cJSON_Delete(file);
        send_json_response(res, session_error_json("download handshake failed"), CWIST_HTTP_INTERNAL_ERROR);
        return;
    }
    const char *session_id = json_string(obj, "session_id", "");
    if (!tasfa_queue_try_enter(g_q_downloads, tasfa_download_session_limit(), session_id)) {
        cleanup_download_session(session_id);
        cJSON_Delete(obj);
        cJSON_Delete(file);
        send_queued_json(res, "too many concurrent downloads", 3);
        return;
    }
    if (!is_client_connected(req)) {
        tasfa_queue_leave(g_q_downloads, tasfa_download_session_limit(), session_id);
        cleanup_download_session(session_id);
        cJSON_Delete(obj);
        cJSON_Delete(file);
        send_json_response(res, session_error_json("client disconnected"), CWIST_HTTP_SERVICE_UNAVAILABLE);
        return;
    }
    cJSON_Delete(file);
    send_json_response(res, obj, CWIST_HTTP_OK);
}

void handler_file_download_chunk(cwist_http_request *req, cwist_http_response *res) {
    const char *chunk_str = cwist_query_map_get(req->path_params, "chunk_index");
    const char *session_id = cwist_query_map_get(req->query_params, "session_id");
    const char *session_token = cwist_query_map_get(req->query_params, "session_token");
    if (!chunk_str || !is_safe_segment(session_id) || !session_token) {
        send_json_response(res, session_error_json("invalid download chunk payload"), CWIST_HTTP_BAD_REQUEST);
        return;
    }
    tasfa_queue_sweep(g_q_downloads, tasfa_download_session_limit(), 300);
    tasfa_queue_touch(g_q_downloads, tasfa_download_session_limit(), session_id);
    cJSON *meta = load_download_session_cached(session_id);
    if (!meta) {
        send_json_response(res, session_error_json("download session not found"), CWIST_HTTP_NOT_FOUND);
        return;
    }
    if (!is_client_connected(req)) {
        cJSON_Delete(meta);
        tasfa_queue_leave(g_q_downloads, tasfa_download_session_limit(), session_id);
        cleanup_download_session(session_id);
        send_json_response(res, session_error_json("client disconnected"), CWIST_HTTP_SERVICE_UNAVAILABLE);
        return;
    }
    if (!secure_str_eq(session_token, json_string(meta, "session_token", ""))) {
        cJSON_Delete(meta);
        send_json_response(res, session_error_json("download chunk rejected"), CWIST_HTTP_FORBIDDEN);
        return;
    }
    time_t now = time(NULL);
    time_t expires_at = (time_t)json_long_long(meta, "expires_at", 0);
    if (expires_at > 0 && now > expires_at) {
        cJSON_Delete(meta);
        tasfa_queue_leave(g_q_downloads, tasfa_download_session_limit(), session_id);
        cleanup_download_session(session_id);
        send_json_response(res, session_error_json("download session expired"), (cwist_http_status_t)410);
        return;
    }
    int chunk_index = atoi(chunk_str);
    int chunk_count = json_int(meta, "chunk_count", 0);
    int chunk_size = json_int(meta, "chunk_size", TASFA_DOWNLOAD_CHUNK_SIZE_DEFAULT);
    int span = atoi(cwist_query_map_get(req->query_params, "span") ? cwist_query_map_get(req->query_params, "span") : "1");
    if (span < 1) span = 1;
    if (span > 64) span = 64;
    int max_span_by_bytes = chunk_size > 0 ? (TASFA_DOWNLOAD_RESPONSE_BYTES_MAX / chunk_size) : 1;
    if (max_span_by_bytes < 1) max_span_by_bytes = 1;
    if (span > max_span_by_bytes) span = max_span_by_bytes;
    if (chunk_index < 0 || chunk_index >= chunk_count) {
        cJSON_Delete(meta);
        send_json_response(res, session_error_json("download chunk out of range"), CWIST_HTTP_BAD_REQUEST);
        return;
    }
    if (chunk_index + span > chunk_count) span = chunk_count - chunk_index;
    long long offset = (long long)chunk_index * (long long)chunk_size;
    long long end = (long long)(chunk_index + span) * (long long)chunk_size;
    long long total_size = json_long_long(meta, "total_size", 0);
    if (end > total_size) end = total_size;
    size_t amount = (size_t)(end - offset);

    const char *storage_path = json_string(meta, "storage_path", "");
    struct stat st;
    if (stat(storage_path, &st) != 0 || st.st_size != total_size) {
        cJSON_Delete(meta);
        send_json_response(res, session_error_json("file size consistency check failed"), CWIST_HTTP_INTERNAL_ERROR);
        return;
    }

    /* Approximate progress tracking for tail prediction */
    int received_so_far = json_int(meta, "received_chunks", 0);
    if (chunk_index + span > received_so_far) {
        cJSON_ReplaceItemInObject(meta, "received_chunks", cJSON_CreateNumber(chunk_index + span));
    }
    const char *chunk_rtt_param = cwist_query_map_get(req->query_params, "chunk_rtt_ms");
    if (chunk_rtt_param) {
        double chunk_rtt = atof(chunk_rtt_param);
        if (chunk_rtt > 0.0) append_rtt_sample(meta, chunk_index, chunk_rtt);
    }
    save_download_session(session_id, meta);

    bool ok = send_file_slice_response(req, res, storage_path,
                                       json_string(meta, "mime_type", "application/octet-stream"), offset, amount,
                                       chunk_index, chunk_count, span);
    double pred = predict_remaining_ms(meta);
    cJSON_Delete(meta);
    if (ok && pred >= 0.0) {
        char pred_buf[32];
        snprintf(pred_buf, sizeof(pred_buf), "%.0f", pred);
        cwist_http_header_add(&res->headers, "X-TASFA-Predicted-Remaining-Ms", pred_buf);
    }
    if (!ok) {
        send_json_response(res, session_error_json("download slice read failed"), CWIST_HTTP_INTERNAL_ERROR);
    }
}

void handler_file_download_complete(cwist_http_request *req, cwist_http_response *res) {
    cwist_query_map *kv = cwist_query_map_create();
    cwist_query_map_parse(kv, req->body->data);
    const char *session_id = cwist_query_map_get(kv, "session_id");
    const char *session_token = cwist_query_map_get(kv, "session_token");
    if (!is_safe_segment(session_id) || !session_token) {
        cwist_query_map_destroy(kv);
        send_json_response(res, session_error_json("invalid download complete payload"), CWIST_HTTP_BAD_REQUEST);
        return;
    }
    cJSON *meta = load_download_session_cached(session_id);
    if (!meta) {
        cwist_query_map_destroy(kv);
        send_json_response(res, session_error_json("download session not found"), CWIST_HTTP_NOT_FOUND);
        return;
    }
    if (!secure_str_eq(session_token, json_string(meta, "session_token", ""))) {
        cJSON_Delete(meta);
        cwist_query_map_destroy(kv);
        send_json_response(res, session_error_json("download completion rejected"), CWIST_HTTP_FORBIDDEN);
        return;
    }
    int file_id = json_int(meta, "file_id", 0);
    if (file_id > 0) {
        db_file_increment_download(req->db, file_id);
    }
    tasfa_queue_leave(g_q_downloads, tasfa_download_session_limit(), session_id);
    cleanup_download_session(session_id);
    cJSON_Delete(meta);
    cwist_query_map_destroy(kv);
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddBoolToObject(obj, "ok", true);
    send_json_response(res, obj, CWIST_HTTP_OK);
}

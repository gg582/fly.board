#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "tasfa_internal.h"

void handler_file_upload_init(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0;
    char role[32] = {0};
    auth_is_logged_in(req, &uid, role, sizeof(role));
    cwist_query_map *kv = cwist_query_map_create();
    cwist_query_map_parse(kv, req->body->data);
    const char *filename = cwist_query_map_get(kv, "filename");
    const char *session_id = cwist_query_map_get(kv, "session_id");
    int chunk_count = atoi(cwist_query_map_get(kv, "chunk_count") ? cwist_query_map_get(kv, "chunk_count") : "0");
    long long total_size = atoll(cwist_query_map_get(kv, "total_size") ? cwist_query_map_get(kv, "total_size") : "0");
    int post_id = atoi(cwist_query_map_get(kv, "post_id") ? cwist_query_map_get(kv, "post_id") : "0");
    int requested_chunk_size = atoi(cwist_query_map_get(kv, "chunk_size") ? cwist_query_map_get(kv, "chunk_size") : "0");
    if (!filename || !is_safe_filename_simple(filename) || total_size <= 0 || !ensure_tasfa_roots()) {
        cwist_query_map_destroy(kv);
        send_json_response(res, session_error_json("invalid upload init payload"), CWIST_HTTP_BAD_REQUEST);
        return;
    }
    if (g_config.max_upload_size > 0 && total_size > g_config.max_upload_size) {
        cwist_query_map_destroy(kv);
        send_json_response(res, session_error_json("upload too large"), CWIST_HTTP_BAD_REQUEST);
        return;
    }
    bool mobile = is_mobile_request(req);
    int chunk_size = choose_chunk_size_upload(mobile, requested_chunk_size);
    int data_chunks = (int)((total_size + chunk_size - 1) / chunk_size);
    if (data_chunks < 1) data_chunks = 1;
    int parity_chunks = (data_chunks + 5) / 6;
    chunk_count = data_chunks + parity_chunks;
    tasfa_queue_sweep(g_q_uploads, tasfa_upload_session_limit(), 600);
    char upload_id[33];
    if (!random_hex(upload_id, 16)) {
        cwist_query_map_destroy(kv);
        send_json_response(res, session_error_json("upload init failed"), CWIST_HTTP_INTERNAL_ERROR);
        return;
    }
    if (!tasfa_queue_try_enter(g_q_uploads, tasfa_upload_session_limit(), upload_id)) {
        cwist_query_map_destroy(kv);
        send_queued_json(res, "Wait a second...", 5);
        return;
    }
    char dir_path[PATH_MAX], temp_path[PATH_MAX];
    upload_session_dir(dir_path, sizeof(dir_path), upload_id);
    upload_session_temp_path(temp_path, sizeof(temp_path), upload_id);
    if (!dir_ensure(dir_path) || !ensure_preallocated_file(temp_path, total_size)) {
        cleanup_upload_session(upload_id);
        tasfa_queue_leave(g_q_uploads, tasfa_upload_session_limit(), upload_id);
        cwist_query_map_destroy(kv);
        send_json_response(res, session_error_json("upload init failed"), CWIST_HTTP_INTERNAL_ERROR);
        return;
    }
    char upload_token[49], upload_secret[49];
    unsigned char stream_key[32] = {0}, stream_iv_seed[12] = {0};
    char *bitmap = bitmap_create(chunk_count);
    if (!bitmap || RAND_bytes(stream_key, sizeof(stream_key)) != 1 || RAND_bytes(stream_iv_seed, sizeof(stream_iv_seed)) != 1 ||
        !random_hex(upload_token, 24) || !random_hex(upload_secret, 24)) {
        if (bitmap) cwist_free(bitmap);
        cleanup_upload_session(upload_id);
        tasfa_queue_leave(g_q_uploads, tasfa_upload_session_limit(), upload_id);
        cwist_query_map_destroy(kv);
        send_json_response(res, session_error_json("upload init failed"), CWIST_HTTP_INTERNAL_ERROR);
        return;
    }
    char *stream_key_hex = hex_encode_alloc(stream_key, sizeof(stream_key));
    char *stream_iv_seed_hex = hex_encode_alloc(stream_iv_seed, sizeof(stream_iv_seed));
    if (!stream_key_hex || !stream_iv_seed_hex) {
        if (stream_key_hex) cwist_free(stream_key_hex);
        if (stream_iv_seed_hex) cwist_free(stream_iv_seed_hex);
        if (bitmap) cwist_free(bitmap);
        cleanup_upload_session(upload_id);
        tasfa_queue_leave(g_q_uploads, tasfa_upload_session_limit(), upload_id);
        cwist_query_map_destroy(kv);
        send_json_response(res, session_error_json("upload init failed"), CWIST_HTTP_INTERNAL_ERROR);
        return;
    }

    /* Generate random modulus_M for HTP scalar math */
    uint64_t modulus_M = 0;
    while (modulus_M == 0) {
        if (RAND_bytes((unsigned char *)&modulus_M, sizeof(modulus_M)) != 1) {
            modulus_M = ((uint64_t)time(NULL) << 32) ^ (uint64_t)getpid();
        }
        modulus_M &= ((1ULL << 53) - 1ULL);
        if (modulus_M == 0) modulus_M = 1;
    }
    int group_count = (chunk_count + 5) / 6;

    char *unique_filename = db_file_unique_filename(req->db, post_id, filename);
    if (unique_filename) filename = unique_filename;

    cJSON *meta = cJSON_CreateObject();
    cJSON_AddNumberToObject(meta, "uid", uid);
    cJSON_AddNumberToObject(meta, "post_id", post_id);
    cJSON_AddStringToObject(meta, "filename", filename);
    cJSON_AddStringToObject(meta, "session_id", session_id ? session_id : "");
    cJSON_AddStringToObject(meta, "upload_token", upload_token);
    cJSON_AddStringToObject(meta, "upload_secret", upload_secret);
    cJSON_AddStringToObject(meta, "stream_key_hex", stream_key_hex);
    cJSON_AddStringToObject(meta, "stream_iv_seed_hex", stream_iv_seed_hex);
    cJSON_AddStringToObject(meta, "temp_path", temp_path);
    cJSON_AddNumberToObject(meta, "chunk_size", chunk_size);
    cJSON_AddNumberToObject(meta, "chunk_count", chunk_count);
    cJSON_AddNumberToObject(meta, "total_size", (double)total_size);
    cJSON_AddStringToObject(meta, "received_bitmap", bitmap);
    cJSON_AddNumberToObject(meta, "received_chunks", 0);
    cJSON_AddNumberToObject(meta, "modulus_M", (double)modulus_M);
    cJSON_AddNumberToObject(meta, "group_count", group_count);
    int score = link_score_from_inputs(
        cwist_query_map_get(kv, "link_stability_score"),
        cwist_query_map_get(kv, "link_effective_type"),
        cwist_query_map_get(kv, "link_downlink_mbps"),
        cwist_query_map_get(kv, "link_rtt_ms"),
        cwist_query_map_get(kv, "link_retry_events"),
        cwist_query_map_get(kv, "link_timeout_events"),
        cwist_query_map_get(kv, "link_save_data")
    );
    int initial_parallel = 0, max_parallel = 0, pacing_ms = 0;
    choose_upload_window(mobile, score, &initial_parallel, &max_parallel, &pacing_ms);
    cJSON_AddNumberToObject(meta, "current_parallel_chunks", initial_parallel);
    cJSON_AddNumberToObject(meta, "max_parallel_chunks", max_parallel);
    cJSON_AddNumberToObject(meta, "dispatch_pacing_ms", pacing_ms);
    double rtt_ms = atof(cwist_query_map_get(kv, "link_rtt_ms") ? cwist_query_map_get(kv, "link_rtt_ms") : "0");
    cJSON_AddNumberToObject(meta, "link_rtt_ms", rtt_ms);
    char expires_at[32];
    snprintf(expires_at, sizeof(expires_at), "%lld", (long long)(time(NULL) + TASFA_UPLOAD_TTL));
    cJSON_AddStringToObject(meta, "expires_at", expires_at);

    /* Per-vertex metadata arrays for HTP (only when we have a full group of 6) */
    if (chunk_count >= 6) {
        cJSON *hash_tags = cJSON_CreateArray();
        cJSON *magic_scalars = cJSON_CreateArray();
        for (int i = 0; i < chunk_count; i++) {
            cJSON_AddItemToArray(hash_tags, cJSON_CreateString(""));
            cJSON_AddItemToArray(magic_scalars, cJSON_CreateNumber(0));
        }
        cJSON_AddItemToObject(meta, "hash_tags", hash_tags);
        cJSON_AddItemToObject(meta, "magic_scalars", magic_scalars);
    }

    tasfa_meta_bin_t mbin = {0};
    strncpy(mbin.upload_token, upload_token, sizeof(mbin.upload_token)-1);
    strncpy(mbin.upload_secret, upload_secret, sizeof(mbin.upload_secret)-1);
    memcpy(mbin.stream_key, stream_key, sizeof(stream_key));
    memcpy(mbin.stream_iv_seed, stream_iv_seed, sizeof(stream_iv_seed));
    mbin.chunk_count = chunk_count;
    mbin.chunk_size = chunk_size;
    mbin.total_size = total_size;
    strncpy(mbin.filename, filename, sizeof(mbin.filename)-1);
    mbin.post_id = post_id;
    mbin.uid = uid;
    snprintf(mbin.temp_path, sizeof(mbin.temp_path), "%s", temp_path);
    if (unique_filename) { cwist_free(unique_filename); unique_filename = NULL; }

    if (!save_upload_session(upload_id, meta) || !save_upload_session_state(upload_id, meta) ||
        !save_upload_session_state_bin(upload_id, chunk_count, bitmap) ||
        !save_upload_session_meta_bin(upload_id, &mbin)) {
        cwist_free(bitmap);
        cJSON_Delete(meta);
        cleanup_upload_session(upload_id);
        tasfa_queue_leave(g_q_uploads, tasfa_upload_session_limit(), upload_id);
        cwist_free(stream_key_hex);
        cwist_free(stream_iv_seed_hex);
        cwist_query_map_destroy(kv);
        send_json_response(res, session_error_json("upload init failed"), CWIST_HTTP_INTERNAL_ERROR);
        return;
    }
    cwist_free(bitmap);
    cwist_free(stream_key_hex);
    cwist_free(stream_iv_seed_hex);
    cwist_query_map_destroy(kv);
    cJSON *obj = build_upload_status_json(meta, upload_id);
    cJSON_Delete(meta);
    send_json_response(res, obj, CWIST_HTTP_OK);
}

void handler_file_upload_status(cwist_http_request *req, cwist_http_response *res) {
    cwist_query_map *kv = cwist_query_map_create();
    cwist_query_map_parse(kv, req->body->data);
    const char *upload_id = cwist_query_map_get(kv, "upload_id");
    const char *upload_token = cwist_query_map_get(kv, "upload_token");
    if (!is_safe_segment(upload_id) || !upload_token) {
        cwist_query_map_destroy(kv);
        send_json_response(res, session_error_json("invalid upload status payload"), CWIST_HTTP_BAD_REQUEST);
        return;
    }
    int lock_fd = open_upload_session_lock_sh(upload_id);
    cJSON *meta = lock_fd >= 0 ? load_upload_session(upload_id) : NULL;
    if (!meta) {
        close_upload_session_lock(lock_fd);
        cwist_query_map_destroy(kv);
        send_json_response(res, session_error_json("upload session not found"), CWIST_HTTP_NOT_FOUND);
        return;
    }
    if (!secure_str_eq(upload_token, json_string(meta, "upload_token", ""))) {
        cJSON_Delete(meta);
        close_upload_session_lock(lock_fd);
        cwist_query_map_destroy(kv);
        send_json_response(res, session_error_json("upload status rejected"), CWIST_HTTP_FORBIDDEN);
        return;
    }
    cJSON *obj = build_upload_status_json(meta, upload_id);
    cJSON_Delete(meta);
    close_upload_session_lock(lock_fd);
    cwist_query_map_destroy(kv);
    send_json_response(res, obj, CWIST_HTTP_OK);
}

void handler_file_upload_renegotiate(cwist_http_request *req, cwist_http_response *res) {
    cwist_query_map *kv = cwist_query_map_create();
    cwist_query_map_parse(kv, req->body->data);
    const char *upload_id = cwist_query_map_get(kv, "upload_id");
    const char *upload_token = cwist_query_map_get(kv, "upload_token");
    if (!is_safe_segment(upload_id) || !upload_token) {
        cwist_query_map_destroy(kv);
        send_json_response(res, session_error_json("invalid upload renegotiate payload"), CWIST_HTTP_BAD_REQUEST);
        return;
    }
    int lock_fd = open_upload_session_lock_sh(upload_id);
    cJSON *meta = lock_fd >= 0 ? load_upload_session(upload_id) : NULL;
    if (!meta) {
        close_upload_session_lock(lock_fd);
        cwist_query_map_destroy(kv);
        send_json_response(res, session_error_json("upload session not found"), CWIST_HTTP_NOT_FOUND);
        return;
    }
    if (!secure_str_eq(upload_token, json_string(meta, "upload_token", ""))) {
        cJSON_Delete(meta);
        close_upload_session_lock(lock_fd);
        cwist_query_map_destroy(kv);
        send_json_response(res, session_error_json("upload renegotiate rejected"), CWIST_HTTP_FORBIDDEN);
        return;
    }
    int score = link_score_from_inputs(
        cwist_query_map_get(kv, "link_stability_score"),
        cwist_query_map_get(kv, "link_effective_type"),
        cwist_query_map_get(kv, "link_downlink_mbps"),
        cwist_query_map_get(kv, "link_rtt_ms"),
        cwist_query_map_get(kv, "link_retry_events"),
        cwist_query_map_get(kv, "link_timeout_events"),
        cwist_query_map_get(kv, "link_save_data")
    );
    int suggested = atoi(cwist_query_map_get(kv, "suggested_parallel") ? cwist_query_map_get(kv, "suggested_parallel") : "0");
    int initial_parallel = 0, max_parallel = 0, pacing_ms = 0;
    bool mobile = is_mobile_request(req);
    choose_upload_window(mobile, score, &initial_parallel, &max_parallel, &pacing_ms);
    int current_parallel = json_int(meta, "current_parallel_chunks", initial_parallel);
    int current_max = json_int(meta, "max_parallel_chunks", max_parallel);
    int configured_max = tasfa_upload_parallel_limit();
    if (current_max < configured_max && max_parallel < current_max) max_parallel = current_max;
    if (max_parallel > configured_max) max_parallel = configured_max;
    if (suggested > 0) initial_parallel = clamp_int(suggested, 1, max_parallel);
    // Only allow tiny drops on renegotiate; wireless loss is normal, don't panic
    if (initial_parallel < current_parallel * 9 / 10) initial_parallel = current_parallel * 9 / 10;
    if (initial_parallel > max_parallel) initial_parallel = max_parallel;
    double rtt_ms = atof(cwist_query_map_get(kv, "link_rtt_ms") ? cwist_query_map_get(kv, "link_rtt_ms") : "0");
    cJSON_DeleteItemFromObject(meta, "link_rtt_ms");
    cJSON_AddNumberToObject(meta, "link_rtt_ms", rtt_ms);
    cJSON_ReplaceItemInObject(meta, "current_parallel_chunks", cJSON_CreateNumber(initial_parallel));
    cJSON_ReplaceItemInObject(meta, "max_parallel_chunks", cJSON_CreateNumber(max_parallel));
    cJSON_ReplaceItemInObject(meta, "dispatch_pacing_ms", cJSON_CreateNumber(pacing_ms));
    save_upload_session(upload_id, meta);
    save_upload_session_state(upload_id, meta);
    cJSON *obj = build_upload_status_json(meta, upload_id);
    cJSON_Delete(meta);
    close_upload_session_lock(lock_fd);
    cwist_query_map_destroy(kv);
    send_json_response(res, obj, CWIST_HTTP_OK);
}

static void upload_work_func(void *arg) {
    upload_work_t *w = (upload_work_t *)arg;

    long long store_start_ms = tasfa_monotonic_ms();
    char parity_path[PATH_MAX];
    int fd = -1;
    if (w->is_parity) {
        snprintf(parity_path, sizeof(parity_path), "%s/%s/parity_%d.bin", TASFA_UPLOAD_DIR, w->upload_id, w->chunk_index - w->data_chunks);
        fd = open(parity_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    } else {
        fd = open(w->temp_path, O_WRONLY);
    }
    if (fd < 0) {
        FLY_LOG_ERROR("[TASFA] chunk open failed: %s (parity=%d) errno=%d", w->is_parity ? parity_path : w->temp_path, w->is_parity, errno);
        return;
    }
    off_t write_offset = w->is_parity ? 0 : (off_t)w->offset;
    if (w->encrypted_stream) {
        size_t encrypted_plain_len = w->body_size > 16 ? w->body_size - 16 : 0;
        unsigned char *plaintext = ensure_decrypt_buf(w->compressed ? encrypted_plain_len : (size_t)w->expected_size);
        if (plaintext && encrypted_plain_len > 0 &&
            decrypt_stream_block(w->stream_key, w->stream_iv_seed, w->chunk_index, w->upload_id,
                                 (const unsigned char *)w->body_data, w->body_size,
                                 plaintext, encrypted_plain_len)) {
            if (w->compressed) {
                unsigned char *inflated = ensure_inflate_buf((size_t)w->expected_size);
                if (inflated && tasfa_decompress_to(plaintext, encrypted_plain_len, inflated, (size_t)w->expected_size, w->compress_type)) {
                    w->stored = pwrite_all(fd, inflated, (size_t)w->expected_size, write_offset);
                }
            } else if (encrypted_plain_len == (size_t)w->expected_size) {
                w->stored = pwrite_all(fd, plaintext, (size_t)w->expected_size, write_offset);
            }
        }
    } else if (w->compressed) {
        unsigned char *inflated = ensure_inflate_buf((size_t)w->expected_size);
        if (inflated && tasfa_decompress_to((const unsigned char *)w->body_data, w->body_size,
                                            inflated, (size_t)w->expected_size, w->compress_type)) {
            w->stored = pwrite_all(fd, inflated, (size_t)w->expected_size, write_offset);
        }
    } else if ((long long)w->body_size == w->expected_size) {
        w->stored = pwrite_all(fd, w->body_data, w->body_size, write_offset);
    } else {
        FLY_LOG_ERROR("[TASFA] chunk store size mismatch: index=%d expected=%lld got=%zu",
                      w->chunk_index, w->expected_size, w->body_size);
    }
    close(fd);
    long long store_end_ms = tasfa_monotonic_ms();
    if (store_start_ms > 0 && store_end_ms >= store_start_ms) {
        w->store_ms = store_end_ms - store_start_ms;
    }

    if (!w->stored) return;

    long long state_start_ms = tasfa_monotonic_ms();
    w->state_ok = mark_chunk_received_in_session_state_atomic(w->upload_id, w->chunk_index, &w->was_already_received);
    long long state_end_ms = tasfa_monotonic_ms();
    if (state_start_ms > 0 && state_end_ms >= state_start_ms) {
        w->state_ms = state_end_ms - state_start_ms;
    }
}

void handler_file_upload(cwist_http_request *req, cwist_http_response *res) {
    long long request_start_ms = tasfa_monotonic_ms();
    const char *upload_id = cwist_http_header_get(req->headers, "X-TASFA-Upload-ID");
    const char *upload_token = cwist_http_header_get(req->headers, "X-TASFA-Upload-Token");
    const char *stream_mode = cwist_http_header_get(req->headers, "X-TASFA-Stream-Mode");
    const char *chunk_index_str = cwist_http_header_get(req->headers, "X-TASFA-Chunk-Index");
    const char *hash_tag_hex = cwist_http_header_get(req->headers, "X-TASFA-Hash-Tag");
    const char *magic_scalar_str = cwist_http_header_get(req->headers, "X-TASFA-Magic-Scalar");
    const char *raw_scalar_str = cwist_http_header_get(req->headers, "X-TASFA-Raw-Scalar");
    if (!upload_id || !upload_token || !chunk_index_str) {
        send_json_response(res, session_error_json("missing headers"), CWIST_HTTP_BAD_REQUEST);
        return;
    }
    int chunk_index = atoi(chunk_index_str);
    if (!is_safe_segment(upload_id) || chunk_index < 0) {
        send_json_response(res, session_error_json("invalid chunk payload"), CWIST_HTTP_BAD_REQUEST);
        return;
    }

    tasfa_meta_bin_t mbin;
    tasfa_queue_sweep(g_q_uploads, tasfa_upload_session_limit(), 600);
    if (!load_upload_session_meta_bin_cached(upload_id, &mbin)) {
        send_json_response(res, session_error_json("upload session not found"), CWIST_HTTP_NOT_FOUND);
        return;
    }
    tasfa_queue_touch(g_q_uploads, tasfa_upload_session_limit(), upload_id);
    if (!secure_str_eq(upload_token, mbin.upload_token)) {
        send_json_response(res, session_error_json("upload chunk rejected"), CWIST_HTTP_FORBIDDEN);
        return;
    }
    if (chunk_index >= mbin.chunk_count) {
        send_json_response(res, session_error_json("invalid chunk index"), CWIST_HTTP_BAD_REQUEST);
        return;
    }

    bool was_already_received = false;
    if (!mark_chunk_received_in_session_state_atomic(upload_id, chunk_index, &was_already_received)) {
        send_json_response(res, session_error_json("chunk state check failed"), CWIST_HTTP_INTERNAL_ERROR);
        return;
    }

    bool is_retry_target = false;
    if (was_already_received) {
        /* Allow retransmission if this chunk is in the server's retry target list */
        cJSON *session_meta = load_upload_session(upload_id);
        if (session_meta) {
            cJSON *retry_arr = cJSON_GetObjectItem(session_meta, "htp_retry_targets");
            if (retry_arr && cJSON_IsArray(retry_arr)) {
                int n = cJSON_GetArraySize(retry_arr);
                for (int i = 0; i < n; i++) {
                    cJSON *item = cJSON_GetArrayItem(retry_arr, i);
                    if (item && cJSON_IsNumber(item) && (int)item->valuedouble == chunk_index) {
                        is_retry_target = true;
                        break;
                    }
                }
            }
            cJSON_Delete(session_meta);
        }
        if (!is_retry_target) {
            res->status_code = CWIST_HTTP_NO_CONTENT;
            cwist_http_header_add(&res->headers, "X-TASFA-Accepted", "1");
            cwist_http_header_add(&res->headers, "X-TASFA-Chunk-Complete", "1");
            cwist_sstring_assign(res->body, "");
            return;
        }
    }

    int data_chunks = (int)((mbin.total_size + (long long)mbin.chunk_size - 1) / (long long)mbin.chunk_size);
    if (data_chunks < 1) data_chunks = 1;
    bool is_parity = (chunk_index >= data_chunks);

    long long offset = 0;
    long long expected_size = 0;
    if (is_parity) {
        offset = 0;
        expected_size = mbin.chunk_size;
    } else {
        offset = (long long)chunk_index * (long long)mbin.chunk_size;
        expected_size = mbin.total_size - offset;
        if (expected_size > mbin.chunk_size) expected_size = mbin.chunk_size;
    }

    if (expected_size <= 0) {
        send_json_response(res, session_error_json("invalid chunk bounds"), CWIST_HTTP_BAD_REQUEST);
        return;
    }

    if (!req->body || req->body->size == 0) {
        send_json_response(res, session_error_json("empty chunk body"), CWIST_HTTP_BAD_REQUEST);
        return;
    }
    bool encrypted_stream = stream_mode && strcmp(stream_mode, "aes-256-gcm") == 0;
    const char *content_encoding = cwist_http_header_get(req->headers, "X-TASFA-Content-Encoding");
    bool compressed_chunk = content_encoding && (
        str_contains_ci_local(content_encoding, "zstd") ||
        str_contains_ci_local(content_encoding, "br") ||
        str_contains_ci_local(content_encoding, "gzip")
    );
    tasfa_compress_type_t compress_type = TASFA_COMPRESS_NONE;
    if (compressed_chunk) {
        if (str_contains_ci_local(content_encoding, "zstd")) compress_type = TASFA_COMPRESS_ZSTD;
        else if (str_contains_ci_local(content_encoding, "br")) compress_type = TASFA_COMPRESS_BROTLI;
        else if (str_contains_ci_local(content_encoding, "gzip")) compress_type = TASFA_COMPRESS_GZIP;
    }
    long long expected_body_size = encrypted_stream ? expected_size + 16 : expected_size;
    bool size_ok = compressed_chunk
        ? (req->body->size > (encrypted_stream ? 16U : 0U) && (long long)req->body->size <= expected_body_size)
        : ((long long)req->body->size == expected_body_size);
    /* For compressed chunks, the body size varies by algorithm; allow up to expected_body_size */
    if (!size_ok) {
        FLY_LOG_ERROR("[TASFA] chunk size mismatch: upload_id=%s index=%d mode=%s expected=%lld got=%zu",
                      upload_id, chunk_index, encrypted_stream ? "aes-256-gcm" : "plain",
                      expected_body_size, req->body->size);
        send_json_response(res, session_error_json("chunk size mismatch"), CWIST_HTTP_BAD_REQUEST);
        return;
    }

    upload_work_t work = {0};
    work.upload_id = upload_id;
    work.chunk_index = chunk_index;
    work.temp_path = mbin.temp_path;
    work.encrypted_stream = encrypted_stream;
    work.stream_key = mbin.stream_key;
    work.stream_iv_seed = mbin.stream_iv_seed;
    work.body_data = req->body->data;
    work.body_size = req->body->size;
    work.compressed = compressed_chunk;
    work.compress_type = compress_type;
    work.offset = offset;
    work.expected_size = expected_size;
    work.chunk_count = mbin.chunk_count;
    work.is_parity = is_parity;
    work.data_chunks = data_chunks;

    upload_work_func(&work);

    if (!work.stored) {
        send_json_response(res, session_error_json("chunk store failed"), CWIST_HTTP_INTERNAL_ERROR);
        return;
    }
    if (!work.state_ok) {
        send_json_response(res, session_error_json("chunk state update failed"), CWIST_HTTP_INTERNAL_ERROR);
        return;
    }

    if (is_retry_target) {
        cJSON *session_meta = load_upload_session(upload_id);
        if (session_meta) {
            cJSON *retry_arr = cJSON_GetObjectItem(session_meta, "htp_retry_targets");
            if (retry_arr && cJSON_IsArray(retry_arr)) {
                int n = cJSON_GetArraySize(retry_arr);
                for (int i = n - 1; i >= 0; i--) {
                    cJSON *item = cJSON_GetArrayItem(retry_arr, i);
                    if (item && cJSON_IsNumber(item) && (int)item->valuedouble == chunk_index) {
                        cJSON_DeleteItemFromArray(retry_arr, i);
                        break;
                    }
                }
                save_upload_session(upload_id, session_meta);
            }
            cJSON_Delete(session_meta);
        }
    }

    /* Collect per-chunk RTT for large-file tail prediction (Lagrange extrapolation) */
    const char *chunk_rtt_hdr = cwist_http_header_get(req->headers, "X-TASFA-Chunk-RTT");
    long long rtt_save_ms = 0;
    if (chunk_rtt_hdr) {
        double chunk_rtt = atof(chunk_rtt_hdr);
        if (chunk_rtt > 0.0) {
            long long rtt_start_ms = tasfa_monotonic_ms();
            cJSON *session_meta = load_upload_session(upload_id);
            if (session_meta) {
                append_rtt_sample(session_meta, chunk_index, chunk_rtt);
                save_upload_session(upload_id, session_meta);
                cJSON_Delete(session_meta);
            }
            long long rtt_end_ms = tasfa_monotonic_ms();
            if (rtt_start_ms > 0 && rtt_end_ms >= rtt_start_ms) rtt_save_ms = rtt_end_ms - rtt_start_ms;
        }
    }

    long long htp_save_ms = 0;
    if (hash_tag_hex && hash_tag_hex[0]) {
        uint64_t balanced_scalar = magic_scalar_str ? (uint64_t)strtoull(magic_scalar_str, NULL, 10) : 0;
        uint64_t raw_scalar = raw_scalar_str ? (uint64_t)strtoull(raw_scalar_str, NULL, 10) : 0;
        long long htp_start_ms = tasfa_monotonic_ms();
        save_htp_scalar(upload_id, chunk_index, hash_tag_hex, raw_scalar, balanced_scalar);
        long long htp_end_ms = tasfa_monotonic_ms();
        if (htp_start_ms > 0 && htp_end_ms >= htp_start_ms) htp_save_ms = htp_end_ms - htp_start_ms;
    }

    long long request_end_ms = tasfa_monotonic_ms();
    long long total_ms = (request_start_ms > 0 && request_end_ms >= request_start_ms) ? request_end_ms - request_start_ms : 0;
    if (total_ms >= 500 || work.store_ms >= 250 || work.state_ms >= 100 || chunk_index % 4 == 0) {
        FLY_LOG_DEBUG("[TASFA] chunk timing upload_id=%s index=%d mode=%s bytes=%zu total_ms=%lld store_ms=%lld state_ms=%lld rtt_save_ms=%lld htp_save_ms=%lld",
                      upload_id, chunk_index, encrypted_stream ? "aes-256-gcm" : "plain",
                      req->body->size, total_ms, work.store_ms, work.state_ms, rtt_save_ms, htp_save_ms);
    }

    res->status_code = CWIST_HTTP_NO_CONTENT;
    cwist_http_header_add(&res->headers, "X-TASFA-Accepted", "1");
    cwist_http_header_add(&res->headers, "X-TASFA-Chunk-Complete", "1");
    add_keepalive_headers(res);
    cwist_sstring_assign(res->body, "");
}

static void handler_file_upload_complete_sync(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0;
    char role[32] = {0};
    auth_is_logged_in(req, &uid, role, sizeof(role));
    cwist_query_map *kv = cwist_query_map_create();
    cwist_query_map_parse(kv, req->body->data);
    const char *upload_id = cwist_query_map_get(kv, "upload_id");
    const char *upload_token = cwist_query_map_get(kv, "upload_token");
    if (!is_safe_segment(upload_id) || !upload_token) {
        cwist_query_map_destroy(kv);
        send_json_response(res, session_error_json("invalid completion payload"), CWIST_HTTP_BAD_REQUEST);
        return;
    }
    int lock_fd = open_upload_session_lock(upload_id);
    cJSON *meta = lock_fd >= 0 ? load_upload_session(upload_id) : NULL;
    if (!meta) {
        close_upload_session_lock(lock_fd);
        cwist_query_map_destroy(kv);
        send_json_response(res, session_error_json("upload session not found"), CWIST_HTTP_NOT_FOUND);
        return;
    }
    if (!secure_str_eq(upload_token, json_string(meta, "upload_token", ""))) {
        cJSON_Delete(meta);
        close_upload_session_lock(lock_fd);
        cwist_query_map_destroy(kv);
        send_json_response(res, session_error_json("upload completion rejected"), CWIST_HTTP_FORBIDDEN);
        return;
    }
    int chunk_count = json_int(meta, "chunk_count", 0);
    const char *bitmap = json_string(meta, "received_bitmap", "");
    int received = bitmap_count_set(bitmap, chunk_count);

    long long total_size = json_long_long(meta, "total_size", 0);
    int chunk_size = json_int(meta, "chunk_size", TASFA_UPLOAD_CHUNK_SIZE_DEFAULT);
    int data_chunks = (int)((total_size + (long long)chunk_size - 1) / (long long)chunk_size);
    if (data_chunks < 1) data_chunks = 1;
    int parity_chunks = (data_chunks + 5) / 6;

    const char *temp_path = json_string(meta, "temp_path", "");

    /* === XOR Reconstruction for missing chunks === */
    if (received != chunk_count) {
        char *mutable_bitmap = (char *)cwist_alloc((size_t)chunk_count + 1);
        if (mutable_bitmap) {
            memcpy(mutable_bitmap, bitmap, (size_t)chunk_count + 1);
            bool recovered_any = false;

            for (int g = 0; g < parity_chunks; g++) {
                int missing_data_idx = -1;
                int missing_data_count = 0;
                int group_start = g * 6;
                int group_end = group_start + 6;
                if (group_end > data_chunks) group_end = data_chunks;

                for (int ci = group_start; ci < group_end; ci++) {
                    if (mutable_bitmap[ci] == '0') {
                        missing_data_count++;
                        missing_data_idx = ci;
                    }
                }

                int parity_idx = data_chunks + g;
                bool parity_received = (mutable_bitmap[parity_idx] == '1');

                if (missing_data_count == 1 && parity_received) {
                    if (perform_xor_recovery(upload_id, temp_path, chunk_size, group_start, group_end, missing_data_idx, parity_idx, data_chunks, total_size)) {
                        mutable_bitmap[missing_data_idx] = '1';
                        mark_chunk_received_in_session_state(upload_id, missing_data_idx);
                        recovered_any = true;
                        FLY_LOG_DEBUG("[TASFA] Successfully recovered missing chunk %d in group %d using XOR", missing_data_idx, g);
                    }
                }
            }

            if (recovered_any) {
                save_upload_session_state_bin(upload_id, chunk_count, mutable_bitmap);
                cJSON_ReplaceItemInObject(meta, "received_bitmap", cJSON_CreateString(mutable_bitmap));
                cJSON_ReplaceItemInObject(meta, "received_chunks", cJSON_CreateNumber(bitmap_count_set(mutable_bitmap, chunk_count)));
                save_upload_session_state(upload_id, meta);

                bitmap = json_string(meta, "received_bitmap", "");
                received = bitmap_count_set(bitmap, chunk_count);
            }
            cwist_free(mutable_bitmap);
        }
    }

    if (received != chunk_count) {
        cJSON *obj = build_upload_status_json(meta, upload_id);
        cJSON_ReplaceItemInObject(obj, "ok", cJSON_CreateBool(false));
        cJSON_AddStringToObject(obj, "error", "missing upload chunks");
        cJSON_Delete(meta);
        close_upload_session_lock(lock_fd);
        cwist_query_map_destroy(kv);
        send_json_response(res, obj, (cwist_http_status_t)409);
        return;
    }
    const char *filename = json_string(meta, "filename", "upload.bin");
    int post_id = json_int(meta, "post_id", 0);
    int owner_uid = json_int(meta, "uid", uid);
    bool media_acquired = false;

    /* === Server-Authoritative HTP Recovery === */
    uint64_t modulus_M = (uint64_t)json_long_long(meta, "modulus_M", 0);
    if (modulus_M == 0) modulus_M = 1;

    char *htp_tags = NULL;
    uint64_t *htp_raw_scalars = NULL;
    uint64_t *htp_balanced_scalars = NULL;
    long long htp_verify_start_ms = tasfa_monotonic_ms();
    bool htp_ok = load_htp_scalars(upload_id, chunk_count, &htp_tags, &htp_raw_scalars, &htp_balanced_scalars);
    htp_suspect_t *suspects = (htp_suspect_t *)cwist_alloc((size_t)chunk_count * sizeof(htp_suspect_t));
    int suspect_count = 0;
    int contraction_level = json_int(meta, "htp_contraction_level", 0);

    if (htp_ok) {
        int group_count = data_chunks / 6;
        for (int g = 0; g < group_count; g++) {
            uint64_t m[6] = {0};
            bool group_has_any = false;
            bool group_complete = true;
            for (int v = 0; v < 6; v++) {
                int ci = g * 6 + v;
                char *tag = htp_tags + (ci * HTP_TAG_LEN);
                if (tag[0]) group_has_any = true;
                else group_complete = false;
                m[v] = htp_balanced_scalars[ci];
            }
            if (!group_has_any || !group_complete) continue;
            if (htp_try_swap_repair(upload_id, htp_balanced_scalars, htp_raw_scalars, htp_tags, g * 6, modulus_M))
                continue;
            htp_suspect_t group_suspects[6];
            int gs_count = 0;
            htp_analyze_group(m, modulus_M, g * 6, group_suspects, &gs_count);
            for (int s = 0; s < gs_count && suspect_count < chunk_count; s++) {
                bool already = false;
                for (int i = 0; i < suspect_count; i++) {
                    if (suspects[i].chunk_index == group_suspects[s].chunk_index) {
                        if (group_suspects[s].suspicion_score > suspects[i].suspicion_score)
                            suspects[i].suspicion_score = group_suspects[s].suspicion_score;
                        already = true;
                        break;
                    }
                }
                if (!already) {
                    suspects[suspect_count].chunk_index = group_suspects[s].chunk_index;
                    suspects[suspect_count].suspicion_score = group_suspects[s].suspicion_score;
                    suspect_count++;
                }
            }
        }

        /* Server-side recursive contraction */
        int chunk_size = json_int(meta, "chunk_size", TASFA_UPLOAD_CHUNK_SIZE_DEFAULT);
        double rtt_ms = json_double(meta, "link_rtt_ms", 0.0);
        if (suspect_count > 0 && htp_repair_worthwhile(suspect_count, chunk_count, chunk_size, rtt_ms)) {
            int next_count = htp_contract_groups(htp_balanced_scalars, chunk_count, modulus_M,
                                                  suspects, suspect_count);
            if (next_count > 0 && next_count < suspect_count) {
                suspect_count = next_count;
                contraction_level++;
            }
        }

        cwist_free(htp_tags);
        cwist_free(htp_raw_scalars);
        cwist_free(htp_balanced_scalars);
    } else {
        cJSON *magic_scalars = cJSON_GetObjectItem(meta, "magic_scalars");
        if (magic_scalars) {
            int group_count = data_chunks / 6;
            bool has_any_scalar = false;
            for (int i = 0; i < chunk_count && i < cJSON_GetArraySize(magic_scalars); i++) {
                cJSON *item = cJSON_GetArrayItem(magic_scalars, i);
                if (item && item->valuedouble != 0) { has_any_scalar = true; break; }
            }
            if (has_any_scalar) {
                for (int g = 0; g < group_count; g++) {
                    uint64_t m[6] = {0};
                    for (int v = 0; v < 6; v++) {
                        int ci = g * 6 + v;
                        if (ci < chunk_count && ci < cJSON_GetArraySize(magic_scalars)) {
                            cJSON *item = cJSON_GetArrayItem(magic_scalars, ci);
                            m[v] = item ? (uint64_t)item->valuedouble : 0;
                        }
                    }
                    htp_line_sums_t sums = htp_line_sums(m, modulus_M);
                    uint64_t L1 = sums.l1;
                    uint64_t L2 = sums.l2;
                    uint64_t L3 = sums.l3;
                    if (L1 != L2 || L2 != L3) {
                        const char *current_bitmap = json_string(meta, "received_bitmap", "");
                        char *mutable_bitmap = (char *)cwist_alloc((size_t)chunk_count + 1);
                        if (mutable_bitmap) {
                            memcpy(mutable_bitmap, current_bitmap, chunk_count + 1);
                            for (int v = 0; v < 6; v++) {
                                int ci = g * 6 + v;
                                if (ci < chunk_count) mutable_bitmap[ci] = '0';
                            }
                            cJSON_ReplaceItemInObject(meta, "received_bitmap", cJSON_CreateString(mutable_bitmap));
                            cJSON_ReplaceItemInObject(meta, "received_chunks", cJSON_CreateNumber(bitmap_count_set(mutable_bitmap, chunk_count)));
                            save_upload_session_state_bin(upload_id, chunk_count, mutable_bitmap);
                            save_upload_session_state(upload_id, meta);
                            cwist_free(mutable_bitmap);
                        }
                        cJSON *obj = build_upload_status_json(meta, upload_id);
                        cJSON_ReplaceItemInObject(obj, "ok", cJSON_CreateBool(false));
                        cJSON_AddStringToObject(obj, "error", "htp line sum mismatch - chunks reset for retry");
                        cJSON_Delete(meta);
                        close_upload_session_lock(lock_fd);
                        cwist_query_map_destroy(kv);
                        cwist_free(suspects);
                        send_json_response(res, obj, (cwist_http_status_t)409);
                        return;
                    }
                }
            }
        }
    }

    long long htp_verify_end_ms = tasfa_monotonic_ms();
    if (htp_verify_start_ms > 0 && htp_verify_end_ms >= htp_verify_start_ms) {
        long long verify_ms = htp_verify_end_ms - htp_verify_start_ms;
        if (verify_ms >= 10 || chunk_count >= 6) {
            FLY_LOG_DEBUG("[TASFA] htp verify upload_id=%s chunks=%d groups=%d backend=%s suspects=%d contraction=%d ms=%lld",
                          upload_id, chunk_count, chunk_count / 6, htp_simd_backend(),
                          suspect_count, contraction_level, verify_ms);
        }
    }

    if (suspect_count > 0) {
        /* === XOR Reconstruction for corrupted chunks detected by HTP === */
        for (int g = 0; g < parity_chunks; g++) {
            int group_start = g * 6;
            int group_end = group_start + 6;
            if (group_end > data_chunks) group_end = data_chunks;

            int suspect_in_group_idx = -1;
            int suspect_in_group_count = 0;
            int suspect_array_pos = -1;

            for (int i = 0; i < suspect_count; i++) {
                int ci = suspects[i].chunk_index;
                if (ci >= group_start && ci < group_end) {
                    suspect_in_group_count++;
                    suspect_in_group_idx = ci;
                    suspect_array_pos = i;
                }
            }

            int parity_idx = data_chunks + g;
            bool parity_received = is_chunk_already_received(upload_id, parity_idx);

            if (suspect_in_group_count == 1 && parity_received) {
                if (perform_xor_recovery(upload_id, temp_path, chunk_size, group_start, group_end, suspect_in_group_idx, parity_idx, data_chunks, total_size)) {
                    for (int j = suspect_array_pos; j < suspect_count - 1; j++) {
                        suspects[j] = suspects[j + 1];
                    }
                    suspect_count--;
                    FLY_LOG_DEBUG("[TASFA] Successfully recovered HTP-suspect chunk %d in group %d using XOR", suspect_in_group_idx, g);
                }
            }
        }
    }

    if (suspect_count > 0) {
        qsort(suspects, suspect_count, sizeof(htp_suspect_t), compare_suspect_desc);

        cJSON *retry_arr = cJSON_CreateArray();
        cJSON *score_arr = cJSON_CreateArray();
        for (int i = 0; i < suspect_count; i++) {
            cJSON *item = cJSON_CreateObject();
            cJSON_AddNumberToObject(item, "chunk_index", suspects[i].chunk_index);
            cJSON_AddNumberToObject(item, "score", suspects[i].suspicion_score);
            cJSON_AddItemToArray(score_arr, item);
            cJSON_AddItemToArray(retry_arr, cJSON_CreateNumber(suspects[i].chunk_index));
        }

        cJSON_DeleteItemFromObject(meta, "htp_retry_targets");
        cJSON_DeleteItemFromObject(meta, "htp_suspicion_scores");
        cJSON_DeleteItemFromObject(meta, "htp_contraction_level");
        cJSON_AddItemToObject(meta, "htp_retry_targets", retry_arr);
        cJSON_AddItemToObject(meta, "htp_suspicion_scores", score_arr);
        cJSON_AddNumberToObject(meta, "htp_contraction_level", contraction_level);
        save_upload_session(upload_id, meta);

        cJSON *obj = build_upload_status_json(meta, upload_id);
        cJSON_ReplaceItemInObject(obj, "ok", cJSON_CreateBool(false));
        cJSON_AddStringToObject(obj, "htp_status", "needs_retry");
        cJSON_AddItemToObject(obj, "retry_targets", cJSON_Duplicate(retry_arr, 1));
        cJSON_AddItemToObject(obj, "suspicion_scores", cJSON_Duplicate(score_arr, 1));
        cJSON_AddNumberToObject(obj, "contraction_level", contraction_level);
        cJSON_AddStringToObject(obj, "retry_reason", "htp group inconsistency detected");

        cwist_free(suspects);
        cJSON_Delete(meta);
        close_upload_session_lock(lock_fd);
        cwist_query_map_destroy(kv);
        send_json_response(res, obj, (cwist_http_status_t)409);
        return;
    }
    cwist_free(suspects);

    finalize_cache_update_status(upload_id, "Processing media previews...");

    /* Concurrency limit for ffmpeg */
    {
        bool need_disconnect_check = (req && req->client_fd >= 0);
        pthread_mutex_lock(&g_media_mtx);
        while (g_media_concurrency >= TASFA_MAX_MEDIA_CONCURRENCY) {
            if (need_disconnect_check) {
                struct timespec ts;
                clock_gettime(CLOCK_REALTIME, &ts);
                ts.tv_nsec += 100000000; /* 100ms */
                if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
                int rc = pthread_cond_timedwait(&g_media_cond, &g_media_mtx, &ts);
                if (rc == ETIMEDOUT && !is_client_connected(req)) {
                    pthread_mutex_unlock(&g_media_mtx);
                    goto client_disconnect;
                }
            } else {
                pthread_cond_wait(&g_media_cond, &g_media_mtx);
            }
        }
        g_media_concurrency++;
        media_acquired = true;
        pthread_mutex_unlock(&g_media_mtx);
    }

    /* Post-quantum checksum (SHA-256 of final file) */
    if (!is_client_connected(req)) goto client_disconnect;
    unsigned char checksum[32];
    if (!sha256_file(temp_path, checksum)) {
        pthread_mutex_lock(&g_media_mtx); g_media_concurrency--; pthread_cond_signal(&g_media_cond); pthread_mutex_unlock(&g_media_mtx);
        cJSON_Delete(meta);
        close_upload_session_lock(lock_fd);
        cwist_query_map_destroy(kv);
        send_json_response(res, session_error_json("checksum compute failed"), CWIST_HTTP_INTERNAL_ERROR);
        return;
    }
    if (!is_client_connected(req)) goto client_disconnect;

    char final_path[PATH_MAX];
    snprintf(final_path, sizeof(final_path), "public/uploads/%s_%s", upload_id, filename);
    if (rename_fallback(temp_path, final_path) != 0) {
        FLY_LOG_ERROR("[TASFA] final file rename failed from %s to %s: errno=%d (%s)", temp_path, final_path, errno, strerror(errno));
        pthread_mutex_lock(&g_media_mtx); g_media_concurrency--; pthread_cond_signal(&g_media_cond); pthread_mutex_unlock(&g_media_mtx);
        cJSON_Delete(meta);
        close_upload_session_lock(lock_fd);
        cwist_query_map_destroy(kv);
        send_json_response(res, session_error_json("final file create failed"), CWIST_HTTP_INTERNAL_ERROR);
        return;
    }
    char mime_buf[128] = {0};
    if (!mime_type_from_data(final_path, mime_buf, sizeof(mime_buf))) {
        snprintf(mime_buf, sizeof(mime_buf), "%s", mime_type(filename));
    }

    /* Re-check uniqueness right before inserting: another concurrent upload
     * may have claimed the auto-renamed filename between init and finalize. */
    int fid = 0;
    char *active_filename = NULL;
    for (int attempt = 0; attempt < 10; attempt++) {
        if (active_filename) cwist_free(active_filename);
        active_filename = db_file_unique_filename(req->db, post_id, filename);
        if (!active_filename) { fid = 0; break; }
        if (strcmp(active_filename, filename) != 0) {
            char new_final_path[PATH_MAX];
            snprintf(new_final_path, sizeof(new_final_path), "public/uploads/%s_%s", upload_id, active_filename);
            if (rename_fallback(final_path, new_final_path) != 0) {
                FLY_LOG_ERROR("[TASFA] final file rename for uniqueness failed from %s to %s", final_path, new_final_path);
                fid = 0; break;
            }
            snprintf(final_path, sizeof(final_path), "%s", new_final_path);
            cJSON_ReplaceItemInObject(meta, "filename", cJSON_CreateString(active_filename));
            filename = json_string(meta, "filename", "upload.bin");
            if (!mime_type_from_data(final_path, mime_buf, sizeof(mime_buf))) {
                snprintf(mime_buf, sizeof(mime_buf), "%s", mime_type(filename));
            }
        }
        fid = db_file_create_volume_get_id(req->db, post_id, owner_uid, filename, mime_buf, final_path, (size_t)json_long_long(meta, "total_size", 0));
        if (fid != -1) break;
    }
    if (active_filename) cwist_free(active_filename);

    if (fid <= 0) {
        pthread_mutex_lock(&g_media_mtx); g_media_concurrency--; pthread_cond_signal(&g_media_cond); pthread_mutex_unlock(&g_media_mtx);
        unlink(final_path);
        cJSON_Delete(meta);
        close_upload_session_lock(lock_fd);
        cwist_query_map_destroy(kv);
        send_json_response(res, session_error_json("db insert failed"), CWIST_HTTP_INTERNAL_ERROR);
        return;
    }
    if (!is_client_connected(req)) goto client_disconnect;

    /* Generate thumbnails/previews via ffmpeg */
    char thumb_path[PATH_MAX] = {0};
    char preview_path[PATH_MAX] = {0};
    if (strncmp(mime_buf, "image/", 6) == 0) {
        if (strcmp(mime_buf, "image/gif") == 0) {
            snprintf(thumb_path, sizeof(thumb_path), "public/uploads/.thumbs/%d_animated_v2.gif", fid);
            if (generate_gif_thumb(final_path, thumb_path, 1024, 1024, 12)) {
                char media_name[64]; snprintf(media_name, sizeof(media_name), "thumb_%d", fid);
                tasfa_generate_htp_metadata_for_file(thumb_path, TASFA_DOWNLOAD_CHUNK_SIZE_DEFAULT, HTP_MODULUS_STABLE, media_name);
            } else thumb_path[0] = '\0';
        } else {
            snprintf(thumb_path, sizeof(thumb_path), "public/uploads/.thumbs/%d.webp", fid);
            if (generate_image_thumb(final_path, thumb_path, 1280, 1280)) {
                char media_name[64]; snprintf(media_name, sizeof(media_name), "thumb_%d", fid);
                tasfa_generate_htp_metadata_for_file(thumb_path, TASFA_DOWNLOAD_CHUNK_SIZE_DEFAULT, HTP_MODULUS_STABLE, media_name);
            } else thumb_path[0] = '\0';
        }
    } else if (strncmp(mime_buf, "video/", 6) == 0) {
        snprintf(thumb_path, sizeof(thumb_path), "public/uploads/.thumbs/%d.webp", fid);
        if (generate_video_thumb(final_path, thumb_path, 480, 270)) {
            char media_name[64]; snprintf(media_name, sizeof(media_name), "thumb_%d", fid);
            tasfa_generate_htp_metadata_for_file(thumb_path, TASFA_DOWNLOAD_CHUNK_SIZE_DEFAULT, HTP_MODULUS_STABLE, media_name);
        } else thumb_path[0] = '\0';
        snprintf(preview_path, sizeof(preview_path), "public/uploads/.previews/%d.mp4", fid);
        if (generate_video_preview(final_path, preview_path, 1080)) {
            char media_name[64]; snprintf(media_name, sizeof(media_name), "preview_%d", fid);
            tasfa_generate_htp_metadata_for_file(preview_path, TASFA_DOWNLOAD_CHUNK_SIZE_DEFAULT, HTP_MODULUS_STABLE, media_name);
        } else preview_path[0] = '\0';
    } else if (strncmp(mime_buf, "audio/", 6) == 0) {
        snprintf(preview_path, sizeof(preview_path), "public/uploads/.previews/%d.mp3", fid);
        if (generate_audio_preview(final_path, preview_path, 192)) {
            char media_name[64]; snprintf(media_name, sizeof(media_name), "preview_%d", fid);
            tasfa_generate_htp_metadata_for_file(preview_path, TASFA_DOWNLOAD_CHUNK_SIZE_DEFAULT, HTP_MODULUS_STABLE, media_name);
        } else preview_path[0] = '\0';
    }
    pthread_mutex_lock(&g_media_mtx); g_media_concurrency--; pthread_cond_signal(&g_media_cond); pthread_mutex_unlock(&g_media_mtx);
    media_acquired = false;

    if (thumb_path[0] || preview_path[0]) {
        db_file_set_preview_paths(req->db, fid, thumb_path[0] ? thumb_path : "", preview_path[0] ? preview_path : "");
    }

    char delete_pin[13], delete_pin_hash[512];
    delete_pin[0] = '\0';
    if (random_hex(delete_pin, 6) && auth_hash_password(delete_pin, delete_pin_hash, sizeof(delete_pin_hash))) {
        (void)db_file_set_delete_pin_hash(req->db, fid, delete_pin_hash);
    } else {
        delete_pin[0] = '\0';
    }
    /* Copy filename out of meta before freeing it; the response still needs it. */
    char filename_buf[256];
    snprintf(filename_buf, sizeof(filename_buf), "%s", filename);
    cJSON_Delete(meta);
    close_upload_session_lock(lock_fd);
    cleanup_upload_session(upload_id);
    tasfa_queue_leave(g_q_uploads, tasfa_upload_session_limit(), upload_id);
    cwist_query_map_destroy(kv);
    cJSON *obj = cJSON_CreateObject();

goto skip_disconnect;

client_disconnect:
    if (media_acquired) {
        pthread_mutex_lock(&g_media_mtx); g_media_concurrency--; pthread_cond_signal(&g_media_cond); pthread_mutex_unlock(&g_media_mtx);
    }
    cJSON_Delete(meta);
    close_upload_session_lock(lock_fd);
    cleanup_upload_session(upload_id);
    tasfa_queue_leave(g_q_uploads, tasfa_upload_session_limit(), upload_id);
    cwist_query_map_destroy(kv);
    send_json_response(res, session_error_json("client disconnected"), CWIST_HTTP_BAD_REQUEST);
    return;

skip_disconnect:
    cJSON_AddBoolToObject(obj, "ok", true);
    cJSON_AddNumberToObject(obj, "id", fid);
    cJSON_AddNumberToObject(obj, "fid", fid);
    cJSON_AddStringToObject(obj, "filename", filename_buf);
    char url[512], checksum_hex[65];
    snprintf(url, sizeof(url), "/file/download/%d", fid);
    cJSON_AddStringToObject(obj, "url", url);
    cJSON_AddStringToObject(obj, "blob_url", url);
    cJSON_AddStringToObject(obj, "file_path", final_path);
    if (thumb_path[0]) {
        cJSON_AddStringToObject(obj, "thumb_path", thumb_path);
    }
    if (preview_path[0]) {
        cJSON_AddStringToObject(obj, "preview_path", preview_path);
    }
    cJSON_AddStringToObject(obj, "delete_pin", delete_pin);
    for (int i = 0; i < 32; i++) snprintf(checksum_hex + (i * 2), 3, "%02x", checksum[i]);
    checksum_hex[64] = '\0';
    cJSON_AddStringToObject(obj, "checksum", checksum_hex);
    send_json_response(res, obj, CWIST_HTTP_OK);
}

static void upload_finalize_worker(void *arg) {
    upload_finalize_job_t *job = (upload_finalize_job_t *)arg;
    if (!job) return;

    cwist_http_request *fake_req = cwist_http_request_create();
    cwist_http_response *fake_res = cwist_http_response_create();
    if (fake_req && fake_res) {
        fake_req->db = job->db;
        if (!fake_req->body) fake_req->body = cwist_sstring_create();
        char body[256];
        snprintf(body, sizeof(body), "upload_id=%s&upload_token=%s", job->upload_id, job->upload_token);
        cwist_sstring_assign(fake_req->body, body);
        handler_file_upload_complete_sync(fake_req, fake_res);
        finalize_cache_mark_done(job->upload_id, (int)fake_res->status_code,
                                 fake_res->body && fake_res->body->data ? fake_res->body->data : "{}");
    } else {
        finalize_cache_mark_done(job->upload_id, 500, "{\"ok\":false,\"error\":\"finalize worker failed\"}");
    }
    if (fake_res) cwist_http_response_destroy(fake_res);
    if (fake_req) cwist_http_request_destroy(fake_req);
    free(job);
}

void handler_file_upload_complete(cwist_http_request *req, cwist_http_response *res) {
    cwist_query_map *kv = cwist_query_map_create();
    cwist_query_map_parse(kv, req->body->data);
    const char *upload_id = cwist_query_map_get(kv, "upload_id");
    const char *upload_token = cwist_query_map_get(kv, "upload_token");
    if (!is_safe_segment(upload_id) || !upload_token) {
        cwist_query_map_destroy(kv);
        send_json_response(res, session_error_json("invalid completion payload"), CWIST_HTTP_BAD_REQUEST);
        return;
    }

    int cached_status = 0;
    char *cached_body = NULL;
    bool cached_active = false;
    if (finalize_cache_get(upload_id, upload_token, &cached_status, &cached_body, &cached_active)) {
        res->status_code = (cwist_http_status_t)(cached_active ? 202 : cached_status);
        cwist_http_header_add(&res->headers, "Content-Type", "application/json");
        add_keepalive_headers(res);
        cwist_sstring_assign(res->body, cached_body ? cached_body : "{}");
        free(cached_body);
        cwist_query_map_destroy(kv);
        return;
    }

    int lock_fd = open_upload_session_lock(upload_id);
    cJSON *meta = lock_fd >= 0 ? load_upload_session(upload_id) : NULL;
    if (!meta) {
        close_upload_session_lock(lock_fd);
        cwist_query_map_destroy(kv);
        send_json_response(res, session_error_json("upload session not found"), CWIST_HTTP_NOT_FOUND);
        return;
    }
    if (!secure_str_eq(upload_token, json_string(meta, "upload_token", ""))) {
        cJSON_Delete(meta);
        close_upload_session_lock(lock_fd);
        cwist_query_map_destroy(kv);
        send_json_response(res, session_error_json("upload completion rejected"), CWIST_HTTP_FORBIDDEN);
        return;
    }

    int chunk_count = json_int(meta, "chunk_count", 0);
    const char *bitmap = json_string(meta, "received_bitmap", "");
    int received = bitmap_count_set(bitmap, chunk_count);
    if (received != chunk_count) {
        cJSON *obj = build_upload_status_json(meta, upload_id);
        cJSON_ReplaceItemInObject(obj, "ok", cJSON_CreateBool(false));
        cJSON_AddStringToObject(obj, "error", "missing upload chunks");
        cJSON_Delete(meta);
        close_upload_session_lock(lock_fd);
        cwist_query_map_destroy(kv);
        send_json_response(res, obj, (cwist_http_status_t)409);
        return;
    }
    cJSON_Delete(meta);
    close_upload_session_lock(lock_fd);

    if (!finalize_cache_mark_started(upload_id, upload_token)) {
        /* May have lost a race with another completion request. Re-check the
           cache and return the cached status if another worker already started. */
        if (finalize_cache_get(upload_id, upload_token, &cached_status, &cached_body, &cached_active)) {
            res->status_code = (cwist_http_status_t)(cached_active ? 202 : cached_status);
            cwist_http_header_add(&res->headers, "Content-Type", "application/json");
            add_keepalive_headers(res);
            cwist_sstring_assign(res->body, cached_body ? cached_body : "{}");
            free(cached_body);
            cwist_query_map_destroy(kv);
            return;
        }
        cwist_query_map_destroy(kv);
        send_json_response(res, session_error_json("finalize queue full"), CWIST_HTTP_SERVICE_UNAVAILABLE);
        return;
    }

    upload_finalize_job_t *job = (upload_finalize_job_t *)calloc(1, sizeof(upload_finalize_job_t));
    if (!job) {
        finalize_cache_mark_done(upload_id, 500, "{\"ok\":false,\"error\":\"finalize queue failed\"}");
        cwist_query_map_destroy(kv);
        send_json_response(res, session_error_json("finalize queue failed"), CWIST_HTTP_INTERNAL_ERROR);
        return;
    }
    job->db = req->db;
    snprintf(job->upload_id, sizeof(job->upload_id), "%s", upload_id);
    snprintf(job->upload_token, sizeof(job->upload_token), "%s", upload_token);

    tasfa_job_t *tjob = (tasfa_job_t *)calloc(1, sizeof(tasfa_job_t));
    if (!tjob) {
        free(job);
        finalize_cache_mark_done(upload_id, 500, "{\"ok\":false,\"error\":\"finalize queue failed\"}");
        cwist_query_map_destroy(kv);
        send_json_response(res, session_error_json("finalize queue failed"), CWIST_HTTP_INTERNAL_ERROR);
        return;
    }
    tjob->free_after_done = true;
    tasfa_scheduler_submit(NULL, upload_finalize_worker, job, tjob);

    cJSON *obj = cJSON_CreateObject();
    cJSON_AddBoolToObject(obj, "ok", false);
    cJSON_AddBoolToObject(obj, "processing", true);
    cJSON_AddStringToObject(obj, "status", "finalizing");
    cJSON_AddNumberToObject(obj, "retry_after", 1);
    cwist_query_map_destroy(kv);
    send_json_response(res, obj, (cwist_http_status_t)202);
}

void handler_file_upload_cancel(cwist_http_request *req, cwist_http_response *res) {
    cwist_query_map *kv = cwist_query_map_create();
    cwist_query_map_parse(kv, req->body->data);
    const char *upload_ids = cwist_query_map_get(kv, "upload_ids");
    if (upload_ids && upload_ids[0] == '[') {
        cJSON *arr = cJSON_Parse(upload_ids);
        if (arr && cJSON_IsArray(arr)) {
            cJSON *item = NULL;
            cJSON_ArrayForEach(item, arr) {
                if (cJSON_IsString(item) && is_safe_segment(item->valuestring)) {
                    cleanup_upload_session(item->valuestring);
                    tasfa_queue_leave(g_q_uploads, tasfa_upload_session_limit(), item->valuestring);
                }
            }
        }
        if (arr) cJSON_Delete(arr);
    }
    cwist_query_map_destroy(kv);
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddBoolToObject(obj, "ok", true);
    send_json_response(res, obj, CWIST_HTTP_OK);
}

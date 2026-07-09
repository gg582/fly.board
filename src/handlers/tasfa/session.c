#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "tasfa_internal.h"

/* --- Session Path Helpers --- */

void upload_session_dir(char *out, size_t out_len, const char *upload_id) {
    snprintf(out, out_len, "%s/%s", TASFA_UPLOAD_DIR, upload_id);
}

void upload_session_meta_path(char *out, size_t out_len, const char *upload_id) {
    snprintf(out, out_len, "%s/%s/meta.json", TASFA_UPLOAD_DIR, upload_id);
}

void upload_session_state_path(char *out, size_t out_len, const char *upload_id) {
    snprintf(out, out_len, "%s/%s/state.json", TASFA_UPLOAD_DIR, upload_id);
}

void upload_session_meta_bin_path(char *out, size_t out_len, const char *upload_id) {
    snprintf(out, out_len, "%s/%s/meta.bin", TASFA_UPLOAD_DIR, upload_id);
}

void upload_session_state_bin_path(char *out, size_t out_len, const char *upload_id) {
    snprintf(out, out_len, "%s/%s/state.bin", TASFA_UPLOAD_DIR, upload_id);
}

void upload_session_temp_path(char *out, size_t out_len, const char *upload_id) {
    snprintf(out, out_len, "%s/%s/upload.bin.part", TASFA_UPLOAD_DIR, upload_id);
}

void download_session_dir(char *out, size_t out_len, const char *session_id) {
    snprintf(out, out_len, "%s/%s", TASFA_DOWNLOAD_DIR, session_id);
}

void download_session_meta_path(char *out, size_t out_len, const char *session_id) {
    snprintf(out, out_len, "%s/%s/meta.json", TASFA_DOWNLOAD_DIR, session_id);
}

bool ensure_tasfa_roots(void) {
    return dir_ensure("data") && dir_ensure("data/tasfa") &&
           dir_ensure(TASFA_UPLOAD_DIR) && dir_ensure(TASFA_DOWNLOAD_DIR) &&
           dir_ensure("public/uploads");
}

bool ensure_preallocated_file(const char *path, long long total_size) {
    int fd = open(path, O_CREAT | O_RDWR, 0644);
    if (fd < 0) return false;
    bool ok = total_size <= 0 || ftruncate(fd, (off_t)total_size) == 0;
    close(fd);
    return ok;
}

static int open_upload_session_lock_impl(const char *upload_id, bool shared) {
    char lock_path[PATH_MAX];
    upload_session_dir(lock_path, sizeof(lock_path), upload_id);
    strncat(lock_path, "/session.lock", sizeof(lock_path) - strlen(lock_path) - 1);
    int fd = open(lock_path, O_CREAT | O_RDWR, 0644);
    if (fd < 0) return -1;
    int op = shared ? LOCK_SH : LOCK_EX;
    if (flock(fd, op) != 0) { close(fd); return -1; }
    return fd;
}

int open_upload_session_lock(const char *upload_id) {
    return open_upload_session_lock_impl(upload_id, false);
}

int open_upload_session_lock_sh(const char *upload_id) {
    return open_upload_session_lock_impl(upload_id, true);
}

void close_upload_session_lock(int lock_fd) {
    if (lock_fd < 0) return;
    flock(lock_fd, LOCK_UN);
    close(lock_fd);
}

/* --- JSON Persistence --- */

static cJSON *load_json_file(const char *path) {
    size_t len = 0;
    char *data = file_read(path, &len);
    if (!data) return NULL;
    cJSON *root = cJSON_Parse(data);
    cwist_free(data);
    return root;
}

static bool save_json_file(const char *path, cJSON *root) {
    char *json = cJSON_PrintUnformatted(root);
    if (!json) return false;
    size_t path_len = strlen(path);
    if (path_len + 5 > PATH_MAX) { free(json); return false; }
    char temp_path[PATH_MAX];
    memcpy(temp_path, path, path_len);
    memcpy(temp_path + path_len, ".tmp", 5);
    if (!file_write(temp_path, json, strlen(json))) { free(json); return false; }
    free(json);
    if (rename(temp_path, path) != 0) { unlink(temp_path); return false; }
    return true;
}

/* --- Bitmap Persistence --- */

int bitmap_count_set(const char *bitmap, int len) {
    int count = 0;
    if (!bitmap || len <= 0) return 0;
    for (int i = 0; i < len; i++) if (bitmap[i] == '1') count++;
    return count;
}

char *bitmap_create(int chunk_count) {
    if (chunk_count <= 0) return NULL;
    size_t count = (size_t)chunk_count;
    char *bitmap = (char *)cwist_alloc(count + 1);
    if (!bitmap) return NULL;
    memset(bitmap, '0', count);
    bitmap[count] = '\0';
    return bitmap;
}

bool save_upload_session_state_bin(const char *upload_id, int chunk_count, const char *bitmap) {
    char path[PATH_MAX];
    upload_session_state_bin_path(path, sizeof(path), upload_id);
    if (!bitmap) return false;
    return file_write(path, bitmap, (size_t)chunk_count);
}

char* load_upload_session_state_bin(const char *upload_id, int chunk_count) {
    char path[PATH_MAX];
    upload_session_state_bin_path(path, sizeof(path), upload_id);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    char *data = (char *)cwist_alloc((size_t)chunk_count + 1);
    if (!data) { close(fd); return NULL; }
    ssize_t r = read(fd, data, (size_t)chunk_count);
    close(fd);
    if (r != chunk_count) { cwist_free(data); return NULL; }
    data[chunk_count] = '\0';
    return data;
}

bool mark_chunk_received_in_session_state(const char *upload_id, int chunk_index) {
    char path[PATH_MAX];
    upload_session_state_bin_path(path, sizeof(path), upload_id);
    int fd = open(path, O_RDWR);
    if (fd < 0) return false;
    unsigned char val = '1';
    bool ok = (pwrite(fd, &val, 1, (off_t)chunk_index) == 1);
    close(fd);
    return ok;
}

/* Atomically check and set the bitmap byte for chunk_index.
   Returns true on success. If the byte was already '1', sets
   *was_already_received to true and does not write. Otherwise writes '1' and
   * sets it to false. The file is locked for the whole read-modify-write
   * sequence so concurrent chunk uploads cannot see a stale "not received"
   * state and then both write the same byte. */
bool mark_chunk_received_in_session_state_atomic(const char *upload_id, int chunk_index, bool *was_already_received) {
    char path[PATH_MAX];
    upload_session_state_bin_path(path, sizeof(path), upload_id);
    int fd = open(path, O_RDWR);
    if (fd < 0) return false;

    bool locked = false;
#ifdef F_OFD_SETLKW
    struct flock fl = {
        .l_type = F_WRLCK,
        .l_whence = SEEK_SET,
        .l_start = (off_t)chunk_index,
        .l_len = 1,
    };
    locked = (fcntl(fd, F_OFD_SETLKW, &fl) == 0);
#endif
    if (!locked) {
        locked = (flock(fd, LOCK_EX) == 0);
    }

    bool ok = false;
    if (locked) {
        unsigned char val = '0';
        if (pread(fd, &val, 1, (off_t)chunk_index) == 1) {
            if (val == '1') {
                if (was_already_received) *was_already_received = true;
                ok = true;
            } else {
                if (was_already_received) *was_already_received = false;
                val = '1';
                ok = (pwrite(fd, &val, 1, (off_t)chunk_index) == 1);
            }
        }
#ifdef F_OFD_SETLKW
        struct flock fl_unlock = {
            .l_type = F_UNLCK,
            .l_whence = SEEK_SET,
            .l_start = (off_t)chunk_index,
            .l_len = 1,
        };
        fcntl(fd, F_OFD_SETLK, &fl_unlock);
#endif
        flock(fd, LOCK_UN);
    }

    close(fd);
    return ok;
}

bool is_chunk_already_received(const char *upload_id, int chunk_index) {
    char path[PATH_MAX];
    upload_session_state_bin_path(path, sizeof(path), upload_id);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    unsigned char val = '0';
    bool ok = (pread(fd, &val, 1, (off_t)chunk_index) == 1);
    close(fd);
    return ok && val == '1';
}

/* --- Upload/Download Session Load/Save --- */

bool save_upload_session_meta_bin(const char *upload_id, tasfa_meta_bin_t *meta) {
    cache_invalidate(upload_id);
    char path[PATH_MAX];
    upload_session_meta_bin_path(path, sizeof(path), upload_id);
    return file_write(path, meta, sizeof(tasfa_meta_bin_t));
}

bool load_upload_session_meta_bin(const char *upload_id, tasfa_meta_bin_t *out) {
    char path[PATH_MAX];
    upload_session_meta_bin_path(path, sizeof(path), upload_id);
    size_t len = 0;
    void *data = file_read(path, &len);
    if (!data) return false;
    if (len != sizeof(tasfa_meta_bin_t)) { cwist_free(data); return false; }
    memcpy(out, data, sizeof(tasfa_meta_bin_t));
    cwist_free(data);
    return true;
}

cJSON *load_upload_session(const char *upload_id) {
    char meta_path[PATH_MAX];
    upload_session_meta_path(meta_path, sizeof(meta_path), upload_id);
    cJSON *meta = load_json_file(meta_path);
    if (!meta) return NULL;
    int chunk_count = json_int(meta, "chunk_count", 0);
    char *bin_bitmap = load_upload_session_state_bin(upload_id, chunk_count);
    if (bin_bitmap) {
        cJSON_ReplaceItemInObject(meta, "received_bitmap", cJSON_CreateString(bin_bitmap));
        cJSON_ReplaceItemInObject(meta, "received_chunks", cJSON_CreateNumber(bitmap_count_set(bin_bitmap, chunk_count)));
        cwist_free(bin_bitmap);
    }
    char state_path[PATH_MAX];
    upload_session_state_path(state_path, sizeof(state_path), upload_id);
    cJSON *state = load_json_file(state_path);
    if (state) {
        cJSON *current_parallel = cJSON_DetachItemFromObject(state, "current_parallel_chunks");
        cJSON *max_parallel = cJSON_DetachItemFromObject(state, "max_parallel_chunks");
        if (current_parallel) cJSON_ReplaceItemInObject(meta, "current_parallel_chunks", current_parallel);
        if (max_parallel) cJSON_ReplaceItemInObject(meta, "max_parallel_chunks", max_parallel);
        cJSON_Delete(state);
    }
    return meta;
}

bool save_upload_session(const char *upload_id, cJSON *root) {
    char path[PATH_MAX];
    upload_session_meta_path(path, sizeof(path), upload_id);
    return save_json_file(path, root);
}

bool save_upload_session_state(const char *upload_id, cJSON *root) {
    char path[PATH_MAX];
    upload_session_state_path(path, sizeof(path), upload_id);
    cJSON *state = cJSON_CreateObject();
    if (!state) return false;
    cJSON_AddStringToObject(state, "received_bitmap", json_string(root, "received_bitmap", ""));
    cJSON_AddNumberToObject(state, "received_chunks", json_int(root, "received_chunks", 0));
    cJSON_AddNumberToObject(state, "current_parallel_chunks", json_int(root, "current_parallel_chunks", TASFA_UPLOAD_DEFAULT_PARALLEL));
    cJSON_AddNumberToObject(state, "max_parallel_chunks", json_int(root, "max_parallel_chunks", TASFA_UPLOAD_MAX_PARALLEL));
    bool ok = save_json_file(path, state);
    cJSON_Delete(state);
    return ok;
}

cJSON *load_download_session(const char *session_id) {
    char path[PATH_MAX];
    download_session_meta_path(path, sizeof(path), session_id);
    return load_json_file(path);
}

bool save_download_session(const char *session_id, cJSON *root) {
    char path[PATH_MAX];
    download_session_meta_path(path, sizeof(path), session_id);
    return save_json_file(path, root);
}

/* --- Cleanup --- */

static void cleanup_dir_tree(const char *dir_path) {
    DIR *dir = opendir(dir_path);
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
            char child[PATH_MAX];
            snprintf(child, sizeof(child), "%s/%s", dir_path, ent->d_name);
            unlink(child);
        }
        closedir(dir);
    }
    rmdir(dir_path);
}

void cleanup_upload_session(const char *upload_id) {
    cache_invalidate(upload_id);
    char dir_path[PATH_MAX];
    upload_session_dir(dir_path, sizeof(dir_path), upload_id);
    cleanup_dir_tree(dir_path);
}

void cleanup_download_session(const char *session_id) {
    char dir_path[PATH_MAX];
    download_session_dir(dir_path, sizeof(dir_path), session_id);
    cleanup_dir_tree(dir_path);
}

/* --- Response Helpers --- */

cJSON *build_upload_status_json(cJSON *meta, const char *upload_id) {
    const char *bitmap = json_string(meta, "received_bitmap", "");
    int chunk_count = json_int(meta, "chunk_count", 0);
    int received_chunks = json_int(meta, "received_chunks", bitmap_count_set(bitmap, chunk_count));
    long long total_size = json_long_long(meta, "total_size", 0);
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddBoolToObject(obj, "ok", true);
    cJSON_AddStringToObject(obj, "upload_id", upload_id ? upload_id : "");
    cJSON_AddStringToObject(obj, "upload_token", json_string(meta, "upload_token", ""));
    cJSON_AddStringToObject(obj, "upload_secret", json_string(meta, "upload_secret", ""));
    cJSON_AddStringToObject(obj, "stream_key_hex", json_string(meta, "stream_key_hex", ""));
    cJSON_AddStringToObject(obj, "stream_iv_seed_hex", json_string(meta, "stream_iv_seed_hex", ""));
    cJSON_AddStringToObject(obj, "stream_mode", "aes-256-gcm");
    cJSON_AddNumberToObject(obj, "chunk_size", json_int(meta, "chunk_size", TASFA_UPLOAD_CHUNK_SIZE_DEFAULT));
    cJSON_AddNumberToObject(obj, "chunk_count", chunk_count);
    cJSON_AddNumberToObject(obj, "total_size", (double)total_size);
    cJSON_AddStringToObject(obj, "received_bitmap", bitmap);
    cJSON_AddNumberToObject(obj, "received_chunks", received_chunks);
    cJSON_AddNumberToObject(obj, "current_parallel_chunks", json_int(meta, "current_parallel_chunks", 6));
    cJSON_AddNumberToObject(obj, "initial_parallel_chunks", json_int(meta, "current_parallel_chunks", 6));
    cJSON_AddNumberToObject(obj, "max_parallel_chunks", json_int(meta, "max_parallel_chunks", 6));
    cJSON_AddNumberToObject(obj, "modulus_M", json_long_long(meta, "modulus_M", 0));
    cJSON_AddNumberToObject(obj, "group_count", json_int(meta, "group_count", 0));
    cJSON_AddStringToObject(obj, "expires_at", json_string(meta, "expires_at", "0"));
    cJSON_AddBoolToObject(obj, "topology_closure_complete", chunk_count > 0 && received_chunks == chunk_count);
    cJSON_AddNumberToObject(obj, "client_stripes", TASFA_CLIENT_STRIPES);
    cJSON_AddNumberToObject(obj, "dispatch_pacing_ms", json_int(meta, "dispatch_pacing_ms", 0));
    int percent = chunk_count > 0 ? (int)((received_chunks * 100LL) / chunk_count) : 0;
    cJSON_AddNumberToObject(obj, "percent", percent);

    double pred = predict_remaining_ms(meta);
    cJSON_AddNumberToObject(obj, "predicted_remaining_ms", pred >= 0.0 ? pred : 0.0);

    /* Missing vertices for HTP per-vertex retry */
    cJSON *missing = cJSON_CreateArray();
    for (int i = 0; i < chunk_count; i++) {
        if (!bitmap || bitmap[i] != '1') {
            cJSON *item = cJSON_CreateObject();
            cJSON_AddNumberToObject(item, "group_index", i / 6);
            cJSON_AddNumberToObject(item, "vertex_index", i % 6);
            cJSON_AddNumberToObject(item, "chunk_index", i);
            cJSON_AddItemToArray(missing, item);
        }
    }
    cJSON_AddItemToObject(obj, "missing_vertices", missing);
    cJSON *retry_targets = cJSON_GetObjectItem(meta, "htp_retry_targets");
    if (retry_targets) {
        cJSON_AddItemToObject(obj, "retry_targets", cJSON_Duplicate(retry_targets, 1));
    }
    cJSON *suspicion_scores = cJSON_GetObjectItem(meta, "htp_suspicion_scores");
    if (suspicion_scores) {
        cJSON_AddItemToObject(obj, "suspicion_scores", cJSON_Duplicate(suspicion_scores, 1));
    }
    cJSON_AddNumberToObject(obj, "contraction_level", json_int(meta, "htp_contraction_level", 0));
    cJSON_AddStringToObject(obj, "htp_status", (retry_targets && cJSON_GetArraySize(retry_targets) > 0) ? "needs_retry" : "ok");
    return obj;
}

bool send_file_slice_response(cwist_http_request *req, cwist_http_response *res, const char *path, const char *mime, long long offset, size_t amount,
                              int chunk_index, int chunk_count, int span) {
    int fd = -1;
    for (int retry = 0; retry < 3; retry++) {
        fd = open(path, O_RDONLY);
        if (fd >= 0) break;
        struct timespec ts = {0, 10000000}; // 10ms
        nanosleep(&ts, NULL);
    }
    if (fd < 0) return false;

    char *buf = amount > 0 ? ensure_read_buf(amount) : NULL;
    if (amount > 0 && !buf) {
        close(fd);
        return false;
    }
    size_t total = 0;
    while (total < amount) {
        ssize_t rc = pread(fd, buf + total, amount - total, (off_t)(offset + (long long)total));
        if (rc <= 0) break;
        total += (size_t)rc;
    }
    close(fd);
    if (total != amount) return false;
    const char *accept_tasfa_encoding = cwist_http_header_get(req->headers, "X-TASFA-Accept-Encoding");
    bool client_accepts_zstd = str_contains_ci_local(accept_tasfa_encoding, "zstd");
    bool client_accepts_brotli = str_contains_ci_local(accept_tasfa_encoding, "br");
    bool client_accepts_gzip = str_contains_ci_local(accept_tasfa_encoding, "gzip");
    unsigned char *comp_buf = NULL;
    size_t comp_len = 0;
    const unsigned char *payload = (const unsigned char *)buf;
    size_t payload_len = total;
    tasfa_compress_type_t comp_type = TASFA_COMPRESS_NONE;
    const char *encoding_name = NULL;

    if (total >= TASFA_COMPRESS_MIN_GAIN_BYTES) {
        if (client_accepts_zstd) {
            if (tasfa_compress_alloc((const unsigned char *)buf, total, &comp_buf, &comp_len, &comp_type) &&
                comp_type == TASFA_COMPRESS_ZSTD && comp_len + TASFA_COMPRESS_MIN_GAIN_BYTES < total) {
                payload = comp_buf;
                payload_len = comp_len;
                encoding_name = "zstd";
            }
        } else if (client_accepts_brotli) {
            if (tasfa_compress_alloc((const unsigned char *)buf, total, &comp_buf, &comp_len, &comp_type) &&
                comp_type == TASFA_COMPRESS_BROTLI && comp_len + TASFA_COMPRESS_MIN_GAIN_BYTES < total) {
                payload = comp_buf;
                payload_len = comp_len;
                encoding_name = "br";
            }
        } else if (client_accepts_gzip) {
            if (tasfa_compress_alloc((const unsigned char *)buf, total, &comp_buf, &comp_len, &comp_type) &&
                comp_type == TASFA_COMPRESS_GZIP && comp_len + TASFA_COMPRESS_MIN_GAIN_BYTES < total) {
                payload = comp_buf;
                payload_len = comp_len;
                encoding_name = "gzip";
            }
        }
        if (comp_buf && !encoding_name) {
            cwist_free(comp_buf);
            comp_buf = NULL;
        }
    }
    if (encoding_name) {
        cwist_http_header_add(&res->headers, "X-TASFA-Content-Encoding", encoding_name);
        char plain_len_buf[32];
        snprintf(plain_len_buf, sizeof(plain_len_buf), "%zu", total);
        cwist_http_header_add(&res->headers, "X-TASFA-Uncompressed-Length", plain_len_buf);
    }
    cwist_http_header_add(&res->headers, "Content-Type", mime ? mime : "application/octet-stream");
    /* TASFA chunks are session-bound and must never be cached by shared or private caches. */
    cwist_http_header_add(&res->headers, "Cache-Control", "no-store, no-cache, must-revalidate, private");
    cwist_http_header_add(&res->headers, "Accept-Ranges", "bytes");
    cwist_http_header_add(&res->headers, "Vary", "Origin, Accept-Encoding");
    res->status_code = CWIST_HTTP_OK;
    add_keepalive_headers(res);
    char idx_buf[32], cnt_buf[32], span_buf[32];
    snprintf(idx_buf, sizeof(idx_buf), "%d", chunk_index);
    snprintf(cnt_buf, sizeof(cnt_buf), "%d", chunk_count);
    snprintf(span_buf, sizeof(span_buf), "%d", span > 0 ? span : 1);
    cwist_http_header_add(&res->headers, "X-TASFA-Chunk-Index", idx_buf);
    cwist_http_header_add(&res->headers, "X-TASFA-Chunk-Count", cnt_buf);
    cwist_http_header_add(&res->headers, "X-TASFA-Chunk-Span", span_buf);

    /* Attach HTP headers if available for this session */
    const char *session_id = cwist_query_map_get(req->query_params, "session_id");
    unsigned char stream_key[32], stream_iv_seed[12];
    bool use_encryption = false;

    if (session_id) {
        char htp_path[PATH_MAX];
        download_session_dir(htp_path, sizeof(htp_path), session_id);
        strncat(htp_path, "/htp.bin", sizeof(htp_path) - strlen(htp_path) - 1);
        int hfd = open(htp_path, O_RDONLY);
        if (hfd >= 0) {
            char tag[HTP_TAG_LEN] = {0};
            uint64_t raw = 0, balanced = 0;
            off_t off = (off_t)chunk_index * HTP_RECORD_SIZE;
            if (pread(hfd, tag, HTP_TAG_LEN, off) == HTP_TAG_LEN &&
                pread(hfd, &raw, 8, off + HTP_TAG_LEN) == 8 &&
                pread(hfd, &balanced, 8, off + HTP_TAG_LEN + 8) == 8) {
                cwist_http_header_add(&res->headers, "X-TASFA-Hash-Tag", tag);
                char bal_buf[32];
                snprintf(bal_buf, sizeof(bal_buf), "%llu", (unsigned long long)balanced);
                cwist_http_header_add(&res->headers, "X-TASFA-Magic-Scalar", bal_buf);
            }
            close(hfd);
        }

        /* Check for encryption keys in session metadata */
        cJSON *sess = load_download_session_cached(session_id);
        if (sess) {
            const char *sk_hex = json_string(sess, "stream_key_hex", NULL);
            const char *ivs_hex = json_string(sess, "stream_iv_seed_hex", NULL);
            if (sk_hex && ivs_hex) {
                for (int i = 0; i < 32; i++) {
                    unsigned int val = 0;
                    sscanf(sk_hex + (i * 2), "%02x", &val);
                    stream_key[i] = (unsigned char)val;
                }
                for (int i = 0; i < 12; i++) {
                    unsigned int val = 0;
                    sscanf(ivs_hex + (i * 2), "%02x", &val);
                    stream_iv_seed[i] = (unsigned char)val;
                }
                use_encryption = true;
                cwist_http_header_add(&res->headers, "X-TASFA-Stream-Mode", "aes-256-gcm");
            }
            cJSON_Delete(sess);
        }
    }

    if (use_encryption) {
        size_t cipher_len = payload_len + 16;
        unsigned char *cipher_buf = (unsigned char *)cwist_alloc(cipher_len);
        size_t actual_cipher_len = 0;
        if (cipher_buf && encrypt_stream_block(stream_key, stream_iv_seed, chunk_index, session_id, payload, payload_len, cipher_buf, &actual_cipher_len)) {
            cwist_sstring_assign(res->body, "");
            cwist_sstring_append_len(res->body, (char *)cipher_buf, actual_cipher_len);
            char len_buf[32];
            snprintf(len_buf, sizeof(len_buf), "%zu", actual_cipher_len);
            cwist_http_header_add(&res->headers, "Content-Length", len_buf);
            cwist_free(cipher_buf);
            if (comp_buf) cwist_free(comp_buf);
            return true;
        }
        cwist_free(cipher_buf);
    }

    char len_buf[32];
    snprintf(len_buf, sizeof(len_buf), "%zu", payload_len);
    cwist_http_header_add(&res->headers, "Content-Length", len_buf);
    cwist_sstring_assign(res->body, "");
    if (payload_len > 0) cwist_sstring_append_len(res->body, (const char *)payload, payload_len);
    if (comp_buf) cwist_free(comp_buf);
    (void)comp_type;
    return true;
}

bool resolve_asset_scope_path(cwist_db *db, const char *scope, const char *encoded, char *storage_path, size_t storage_len,
                               char *filename, size_t filename_len, const char **mime_out) {
    (void)db;
    char *decoded = decode_segment(encoded ? encoded : "");
    bool ok = false;
    if (!decoded) return false;
    if (!strcmp(scope ? scope : "", "img") && is_safe_filename_simple(decoded)) {
        snprintf(storage_path, storage_len, "public/img/%s", decoded);
        snprintf(filename, filename_len, "%s", decoded);
        if (mime_out) *mime_out = mime_type(decoded);
        ok = true;
    } else if (!strcmp(scope ? scope : "", "uploads") && is_safe_upload_asset_name(decoded)) {
        snprintf(storage_path, storage_len, "public/uploads/%s", decoded);
        snprintf(filename, filename_len, "%s", decoded);
        if (mime_out) *mime_out = mime_type(decoded);
        ok = true;
    } else if (!strcmp(scope ? scope : "", "profile") && is_safe_filename_simple(decoded)) {
        snprintf(storage_path, storage_len, "public/profile/%s", decoded);
        snprintf(filename, filename_len, "%s", decoded);
        if (mime_out) *mime_out = mime_type(decoded);
        ok = true;
    }
    cwist_free(decoded);
    return ok;
}

bool init_download_session(const char *filename, const char *mime, const char *storage_path, long long total_size,
                           bool mobile, int score, int requested_chunk_size, int fast_link_max, int file_id, const char *media_name, cJSON **out) {
    if (!ensure_tasfa_roots()) return false;
    int chunk_size = choose_chunk_size_download(mobile, requested_chunk_size, total_size, fast_link_max);
    char session_id[33], session_token[49];
    if (!random_hex(session_id, 16) || !random_hex(session_token, 24)) return false;
    char dir_path[PATH_MAX];
    download_session_dir(dir_path, sizeof(dir_path), session_id);
    if (!dir_ensure(dir_path)) return false;

    uint64_t modulus_M = 0;

    /* Load pre-calculated HTP metadata if available */
    if (media_name && media_name[0]) {
        char src_htp[PATH_MAX], dst_htp[PATH_MAX];
        snprintf(src_htp, sizeof(src_htp), "data/tasfa/media_htp/%s/htp.bin", media_name);
        snprintf(dst_htp, sizeof(dst_htp), "%s/htp.bin", dir_path);
        struct stat st;
        if (stat(src_htp, &st) == 0 && st.st_size > 0) {
            /* Copy HTP metadata to session dir */
            FILE *fsrc = fopen(src_htp, "rb");
            FILE *fdst = fopen(dst_htp, "wb");
            if (fsrc && fdst) {
                char buf[8192]; size_t n;
                while ((n = fread(buf, 1, sizeof(buf), fsrc)) > 0) fwrite(buf, 1, n, fdst);
                modulus_M = HTP_MODULUS_STABLE;
            }
            if (fsrc) fclose(fsrc);
            if (fdst) fclose(fdst);
        }
    }

    if (modulus_M == 0) {
        if (RAND_bytes((unsigned char *)&modulus_M, sizeof(modulus_M)) != 1) {
            modulus_M = ((uint64_t)time(NULL) << 32) ^ (uint64_t)getpid();
        }
        modulus_M &= ((1ULL << 53) - 1ULL);
        if (modulus_M == 0) modulus_M = 1;
    }

    int initial_parallel = 0, max_parallel = 0, pacing_ms = 0, coalesce = 0;
    choose_download_profile(mobile, score, &initial_parallel, &max_parallel, &pacing_ms, &coalesce);
    if (mobile && total_size > 100 * 1024 * 1024LL && coalesce < 16) coalesce = 16;
    int chunk_count = (int)((total_size + chunk_size - 1) / chunk_size);
    if (chunk_count < 1) chunk_count = 1;
    cJSON *meta = cJSON_CreateObject();
    cJSON_AddStringToObject(meta, "session_id", session_id);
    cJSON_AddStringToObject(meta, "session_token", session_token);
    cJSON_AddStringToObject(meta, "filename", filename ? filename : "download");
    cJSON_AddStringToObject(meta, "mime_type", mime ? mime : "application/octet-stream");
    cJSON_AddStringToObject(meta, "storage_path", storage_path ? storage_path : "");
    cJSON_AddNumberToObject(meta, "total_size", (double)total_size);
    cJSON_AddNumberToObject(meta, "chunk_size", chunk_size);
    cJSON_AddNumberToObject(meta, "chunk_count", chunk_count);
    cJSON_AddNumberToObject(meta, "initial_parallel_chunks", initial_parallel);
    cJSON_AddNumberToObject(meta, "max_parallel_chunks", max_parallel);
    cJSON_AddNumberToObject(meta, "dispatch_pacing_ms", pacing_ms);
    cJSON_AddNumberToObject(meta, "coalesce_chunks", coalesce);
    cJSON_AddBoolToObject(meta, "supports_progressive_streaming", true);
    cJSON_AddNumberToObject(meta, "file_id", file_id);
    cJSON_AddNumberToObject(meta, "received_chunks", 0);
    cJSON_AddNumberToObject(meta, "modulus_M", (double)modulus_M);
    char expires_at[32];
    snprintf(expires_at, sizeof(expires_at), "%lld", (long long)(time(NULL) + TASFA_DOWNLOAD_TTL));
    cJSON_AddStringToObject(meta, "expires_at", expires_at);
    if (!save_download_session(session_id, meta)) { cJSON_Delete(meta); cleanup_download_session(session_id); return false; }

    unsigned char stream_key[32], stream_iv_seed[12];
    if (RAND_bytes(stream_key, sizeof(stream_key)) != 1 || RAND_bytes(stream_iv_seed, sizeof(stream_iv_seed)) != 1) {
        cJSON_Delete(meta); cleanup_download_session(session_id); return false;
    }
    char *stream_key_hex = hex_encode_alloc(stream_key, sizeof(stream_key));
    char *stream_iv_seed_hex = hex_encode_alloc(stream_iv_seed, sizeof(stream_iv_seed));
    if (stream_key_hex && stream_iv_seed_hex) {
        cJSON_AddStringToObject(meta, "stream_key_hex", stream_key_hex);
        cJSON_AddStringToObject(meta, "stream_iv_seed_hex", stream_iv_seed_hex);
        cJSON_AddStringToObject(meta, "stream_mode", "aes-256-gcm");
        save_download_session(session_id, meta); // update with keys
    }
    cwist_free(stream_key_hex);
    cwist_free(stream_iv_seed_hex);

    if (out) *out = meta;
    else cJSON_Delete(meta);
    return true;
}

bool pwrite_all(int fd, const void *buf, size_t len, off_t offset) {
    const unsigned char *cursor = (const unsigned char *)buf;
    size_t written = 0;
    while (written < len) {
        ssize_t rc = pwrite(fd, cursor + written, len - written, offset + (off_t)written);
        if (rc < 0) { if (errno == EINTR) continue; return false; }
        if (rc == 0) return false;
        written += (size_t)rc;
    }
    return true;
}

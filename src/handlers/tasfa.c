#define _POSIX_C_SOURCE 200809L
#include "handlers_internal.h"
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <openssl/crypto.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <strings.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#define TASFA_UPLOAD_DIR "data/tasfa/uploads"
#define TASFA_DOWNLOAD_DIR "data/tasfa/downloads"
#define TASFA_CHUNK_SIZE (1 * 1024 * 1024)
#define TASFA_TRANSPORT_BLOCK_SIZE (128 * 1024)

#define TASFA_UPLOAD_TTL 86400
#define TASFA_DOWNLOAD_TTL 86400
#define TASFA_UPLOAD_DEFAULT_PARALLEL 16
#define TASFA_UPLOAD_MAX_PARALLEL 64
#define TASFA_DOWNLOAD_DEFAULT_PARALLEL 24
#define TASFA_DOWNLOAD_MAX_PARALLEL 256
#define TASFA_CLIENT_STRIPES 32

static const int TASFA_DIRS[6][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, -1}, {-1, 1}};

/* Binary Metadata Structure for Fast Path */
typedef struct {
    char upload_token[64];
    char upload_secret[64];
    int chunk_count;
    int chunk_size;
    int transport_block_size;
    int transport_block_count;
    long long total_size;
    char filename[256];
    int post_id;
    int uid;
    char temp_path[PATH_MAX];
} tasfa_meta_bin_t;

/* --- Utility Functions --- */

static void send_json_response(cwist_http_response *res, cJSON *obj, int status_code) {
    char *json = cJSON_PrintUnformatted(obj);
    res->status_code = (cwist_http_status_t)status_code;
    cwist_http_header_add(&res->headers, "Content-Type", "application/json");
    cwist_sstring_assign(res->body, json ? json : "{}");
    if (json) free(json);
    cJSON_Delete(obj);
}

static cJSON *session_error_json(const char *error) {
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddBoolToObject(obj, "ok", false);
    cJSON_AddStringToObject(obj, "error", error ? error : "unknown error");
    return obj;
}

static bool is_safe_segment(const char *value) {
    if (!value || !value[0]) return false;
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p) {
        if (!(isalnum(*p) || *p == '-' || *p == '_' || *p == '.')) return false;
    }
    return true;
}

static bool is_safe_filename_simple(const char *name) {
    return name && name[0] && strchr(name, '/') == NULL && strchr(name, '\\') == NULL;
}

static char *decode_segment(const char *src) {
    size_t len = strlen(src);
    char *out = (char *)cwist_alloc(len + 1);
    if (!out) return NULL;
    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        if (src[i] == '%' && i + 2 < len && isxdigit((unsigned char)src[i + 1]) && isxdigit((unsigned char)src[i + 2])) {
            unsigned int value = 0;
            sscanf(src + i + 1, "%2x", &value);
            out[j++] = (char)value;
            i += 2;
        } else {
            out[j++] = src[i];
        }
    }
    out[j] = '\0';
    return out;
}

static const char *json_string(cJSON *obj, const char *key, const char *def) {
    cJSON *item = cJSON_GetObjectItem(obj, key);
    if (item && cJSON_IsString(item) && item->valuestring) return item->valuestring;
    return def;
}

static long long json_long_long(cJSON *obj, const char *key, long long def) {
    cJSON *item = cJSON_GetObjectItem(obj, key);
    if (!item) return def;
    if (cJSON_IsNumber(item)) return (long long)item->valuedouble;
    if (cJSON_IsString(item) && item->valuestring) return atoll(item->valuestring);
    return def;
}

static bool random_hex(char *out, size_t bytes_len) {
    unsigned char buf[64];
    if (!out || bytes_len == 0 || bytes_len > sizeof(buf)) return false;
    if (RAND_bytes(buf, (int)bytes_len) != 1) return false;
    for (size_t i = 0; i < bytes_len; i++) snprintf(out + (i * 2), 3, "%02x", buf[i]);
    out[bytes_len * 2] = '\0';
    return true;
}

static bool secure_str_eq(const char *a, const char *b) {
    if (!a || !b) return false;
    size_t a_len = strlen(a);
    size_t b_len = strlen(b);
    if (a_len != b_len) return false;
    return CRYPTO_memcmp(a, b, a_len) == 0;
}

static bool hmac_sha256_hex(const char *secret, const char *message, char *out, size_t out_len) {
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int md_len = 0;
    if (!secret || !message || !out || out_len < 65) return false;
    if (!HMAC(EVP_sha256(), secret, (int)strlen(secret),
              (const unsigned char *)message, strlen(message), md, &md_len)) {
        return false;
    }
    if (md_len != 32) return false;
    for (unsigned int i = 0; i < md_len; i++) snprintf(out + (i * 2), out_len - (i * 2), "%02x", md[i]);
    out[64] = '\0';
    return true;
}

static bool verify_upload_signature(const char *secret, const char *upload_id, int chunk_index, long long block_offset,
                                    const char *nonce, const char *vertex_id, int magic_sum, const char *signature) {
    char message[512];
    char expected[65];
    snprintf(message, sizeof(message), "%s:%d:%lld:%s:%s:%d",
             upload_id, chunk_index, block_offset, nonce ? nonce : "", vertex_id ? vertex_id : "", magic_sum);
    if (!hmac_sha256_hex(secret, message, expected, sizeof(expected))) return false;
    return secure_str_eq(expected, signature);
}

static int clamp_int(int value, int min_value, int max_value) {
    if (value < min_value) return min_value;
    if (value > max_value) return max_value;
    return value;
}

static int link_score_from_inputs(const char *score_str, const char *effective_type, const char *downlink_str,
                                  const char *rtt_str, const char *retry_str, const char *timeout_str, const char *save_data_str) {
    int explicit_score = score_str ? atoi(score_str) : 0;
    if (explicit_score > 0) return clamp_int(explicit_score, 10, 100);
    double downlink = downlink_str ? atof(downlink_str) : 0.0;
    double rtt = rtt_str ? atof(rtt_str) : 0.0;
    int retries = retry_str ? atoi(retry_str) : 0;
    int timeouts = timeout_str ? atoi(timeout_str) : 0;
    int score = 55;
    if (effective_type && strcmp(effective_type, "4g") == 0) score += 24;
    else if (effective_type && strcmp(effective_type, "3g") == 0) score += 10;
    else if (effective_type && (!strcmp(effective_type, "2g") || !strcmp(effective_type, "slow-2g"))) score -= 10;
    if (downlink >= 30.0) score += 18;
    else if (downlink >= 10.0) score += 12;
    else if (downlink >= 3.0) score += 6;
    else if (downlink > 0.0 && downlink < 1.5) score -= 10;
    if (rtt > 0.0) {
        if (rtt <= 60.0) score += 16;
        else if (rtt <= 120.0) score += 8;
        else if (rtt <= 220.0) score += 2;
        else if (rtt <= 450.0) score -= 10;
        else score -= 18;
    }
    score -= retries * 5;
    score -= timeouts * 12;
    if (save_data_str && (!strcmp(save_data_str, "1") || !strcasecmp(save_data_str, "true"))) score -= 10;
    return clamp_int(score, 10, 100);
}

static void choose_upload_window(int score, int *initial_parallel, int *max_parallel) {
    int initial_value = 12;
    int max_value = TASFA_UPLOAD_MAX_PARALLEL;
    if (score >= 85) {
        initial_value = 16;
        max_value = TASFA_UPLOAD_MAX_PARALLEL;
    } else if (score >= 65) {
        initial_value = 14;
        max_value = TASFA_UPLOAD_MAX_PARALLEL;
    } else if (score >= 45) {
        initial_value = 10;
        max_value = 32;
    } else {
        initial_value = 4;
        max_value = 16;
    }
    if (initial_parallel) *initial_parallel = initial_value;
    if (max_parallel) *max_parallel = max_value;
}

static void choose_download_profile(int score, int *initial_parallel, int *max_parallel, int *pacing_ms, int *coalesce_chunks) {
    int initial_value = 112;
    int max_value = 256;
    int pace = 0;
    int coalesce = 64;
    if (score < 45) {
        initial_value = 32;
        max_value = 96;
        coalesce = 24;
    } else if (score < 65) {
        initial_value = 64;
        max_value = 160;
        coalesce = 32;
    } else if (score < 85) {
        initial_value = 96;
        max_value = 224;
        coalesce = 48;
    }
    if (initial_parallel) *initial_parallel = initial_value;
    if (max_parallel) *max_parallel = max_value;
    if (pacing_ms) *pacing_ms = pace;
    if (coalesce_chunks) *coalesce_chunks = coalesce;
}

/* --- Session Path Helpers --- */

static void upload_session_dir(char *out, size_t out_len, const char *upload_id) {
    snprintf(out, out_len, "%s/%s", TASFA_UPLOAD_DIR, upload_id);
}

static void upload_session_meta_path(char *out, size_t out_len, const char *upload_id) {
    snprintf(out, out_len, "%s/%s/meta.json", TASFA_UPLOAD_DIR, upload_id);
}

static void upload_session_state_path(char *out, size_t out_len, const char *upload_id) {
    snprintf(out, out_len, "%s/%s/state.json", TASFA_UPLOAD_DIR, upload_id);
}

static void upload_session_meta_bin_path(char *out, size_t out_len, const char *upload_id) {
    snprintf(out, out_len, "%s/%s/meta.bin", TASFA_UPLOAD_DIR, upload_id);
}

static void upload_session_state_bin_path(char *out, size_t out_len, const char *upload_id) {
    snprintf(out, out_len, "%s/%s/state.bin", TASFA_UPLOAD_DIR, upload_id);
}

static void upload_session_block_state_bin_path(char *out, size_t out_len, const char *upload_id) {
    snprintf(out, out_len, "%s/%s/blocks.bin", TASFA_UPLOAD_DIR, upload_id);
}

static void upload_session_temp_path(char *out, size_t out_len, const char *upload_id) {
    snprintf(out, out_len, "%s/%s/upload.bin.part", TASFA_UPLOAD_DIR, upload_id);
}

static void download_session_dir(char *out, size_t out_len, const char *session_id) {
    snprintf(out, out_len, "%s/%s", TASFA_DOWNLOAD_DIR, session_id);
}

static void download_session_meta_path(char *out, size_t out_len, const char *session_id) {
    snprintf(out, out_len, "%s/%s/meta.json", TASFA_DOWNLOAD_DIR, session_id);
}

static bool ensure_tasfa_roots(void) {
    return dir_ensure("data") && dir_ensure("data/tasfa") && 
           dir_ensure(TASFA_UPLOAD_DIR) && dir_ensure(TASFA_DOWNLOAD_DIR) &&
           dir_ensure("public/uploads");
}

static bool ensure_preallocated_file(const char *path, long long total_size) {
    int fd = open(path, O_CREAT | O_RDWR, 0644);
    if (fd < 0) return false;
    bool ok = true;
    if (total_size > 0) {
        if (posix_fallocate(fd, 0, (off_t)total_size) != 0) {
            if (ftruncate(fd, (off_t)total_size) != 0) ok = false;
        }
    }
    close(fd);
    return ok;
}

static int open_upload_session_lock(const char *upload_id) {
    char lock_path[PATH_MAX];
    upload_session_dir(lock_path, sizeof(lock_path), upload_id);
    strncat(lock_path, "/session.lock", sizeof(lock_path) - strlen(lock_path) - 1);
    int fd = open(lock_path, O_CREAT | O_RDWR, 0644);
    if (fd < 0) return -1;
    if (flock(fd, LOCK_EX) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void close_upload_session_lock(int lock_fd) {
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
    if (path_len + 5 > PATH_MAX) {
        free(json);
        return false;
    }
    char temp_path[PATH_MAX];
    memcpy(temp_path, path, path_len);
    memcpy(temp_path + path_len, ".tmp", 5);
    if (!file_write(temp_path, json, strlen(json))) {
        free(json);
        return false;
    }
    free(json);
    if (rename(temp_path, path) != 0) {
        unlink(temp_path);
        return false;
    }
    return true;
}

/* --- Bitmap Persistence --- */

static int bitmap_count_set(const char *bitmap, int len) {
    int count = 0;
    if (!bitmap || len <= 0) return 0;
    for (int i = 0; i < len; i++) if (bitmap[i] == '1') count++;
    return count;
}

static char *bitmap_create(int chunk_count) {
    if (chunk_count <= 0) return NULL;
    size_t count = (size_t)chunk_count;
    char *bitmap = (char *)cwist_alloc(count + 1);
    if (!bitmap) return NULL;
    memset(bitmap, '0', count);
    bitmap[count] = '\0';
    return bitmap;
}

static int transport_block_count_for_size(long long total_size) {
    if (total_size <= 0) return 0;
    return (int)((total_size + TASFA_TRANSPORT_BLOCK_SIZE - 1) / TASFA_TRANSPORT_BLOCK_SIZE);
}

static long long contiguous_transport_bytes_from_bitmap(const char *bitmap, int block_count, int block_size, long long total_size) {
    if (!bitmap || block_count <= 0 || block_size <= 0 || total_size <= 0) return 0;
    long long confirmed = 0;
    for (int i = 0; i < block_count; i++) {
        if (bitmap[i] != '1') break;
        long long next = confirmed + (long long)block_size;
        confirmed = next > total_size ? total_size : next;
    }
    return confirmed;
}

static bool save_upload_session_state_bin(const char *upload_id, int chunk_count, const char *bitmap) {
    char path[PATH_MAX];
    upload_session_state_bin_path(path, sizeof(path), upload_id);
    if (!bitmap) return false;
    return file_write(path, bitmap, (size_t)chunk_count);
}

static bool save_upload_block_state_bin(const char *upload_id, int block_count, const char *bitmap) {
    char path[PATH_MAX];
    upload_session_block_state_bin_path(path, sizeof(path), upload_id);
    if (!bitmap) return false;
    return file_write(path, bitmap, (size_t)block_count);
}

static char* load_upload_session_state_bin(const char *upload_id, int chunk_count) {
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

static char* load_upload_block_state_bin(const char *upload_id, int block_count) {
    char path[PATH_MAX];
    upload_session_block_state_bin_path(path, sizeof(path), upload_id);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    char *data = (char *)cwist_alloc((size_t)block_count + 1);
    if (!data) { close(fd); return NULL; }
    ssize_t r = read(fd, data, (size_t)block_count);
    close(fd);
    if (r != block_count) { cwist_free(data); return NULL; }
    data[block_count] = '\0';
    return data;
}

static bool mark_chunk_received_in_session_state(const char *upload_id, int chunk_index) {
    char path[PATH_MAX];
    upload_session_state_bin_path(path, sizeof(path), upload_id);
    int fd = open(path, O_RDWR);
    if (fd < 0) return false;
    unsigned char val = '1';
    bool ok = (pwrite(fd, &val, 1, (off_t)chunk_index) == 1);
    close(fd);
    return ok;
}

static bool mark_transport_block_received(const char *upload_id, int block_index) {
    char path[PATH_MAX];
    upload_session_block_state_bin_path(path, sizeof(path), upload_id);
    int fd = open(path, O_RDWR);
    if (fd < 0) return false;
    unsigned char val = '1';
    bool ok = (pwrite(fd, &val, 1, (off_t)block_index) == 1);
    close(fd);
    return ok;
}

static int blocks_per_chunk(void) {
    return TASFA_CHUNK_SIZE / TASFA_TRANSPORT_BLOCK_SIZE;
}

static int first_block_index_for_chunk(int chunk_index) {
    return chunk_index * blocks_per_chunk();
}

static int block_count_for_chunk(int chunk_index, long long total_size) {
    long long chunk_start = (long long)chunk_index * (long long)TASFA_CHUNK_SIZE;
    long long chunk_end = chunk_start + (long long)TASFA_CHUNK_SIZE;
    if (chunk_end > total_size) chunk_end = total_size;
    if (chunk_end <= chunk_start) return 0;
    return (int)(((chunk_end - chunk_start) + TASFA_TRANSPORT_BLOCK_SIZE - 1) / TASFA_TRANSPORT_BLOCK_SIZE);
}

static bool is_chunk_complete_from_block_bitmap(const char *block_bitmap, int chunk_index, long long total_size) {
    if (!block_bitmap) return false;
    int first = first_block_index_for_chunk(chunk_index);
    int count = block_count_for_chunk(chunk_index, total_size);
    if (count <= 0) return false;
    for (int i = 0; i < count; i++) {
        if (block_bitmap[first + i] != '1') return false;
    }
    return true;
}

/* --- Session Management --- */

static bool save_upload_session_meta_bin(const char *upload_id, tasfa_meta_bin_t *meta) {
    char path[PATH_MAX];
    upload_session_meta_bin_path(path, sizeof(path), upload_id);
    return file_write(path, meta, sizeof(tasfa_meta_bin_t));
}

static bool load_upload_session_meta_bin(const char *upload_id, tasfa_meta_bin_t *out) {
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

static cJSON *load_upload_session(const char *upload_id) {
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

static bool save_upload_session(const char *upload_id, cJSON *root) {
    char path[PATH_MAX];
    upload_session_meta_path(path, sizeof(path), upload_id);
    return save_json_file(path, root);
}

static bool save_upload_session_state(const char *upload_id, cJSON *root) {
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

static cJSON *load_download_session(const char *session_id) {
    char path[PATH_MAX];
    download_session_meta_path(path, sizeof(path), session_id);
    return load_json_file(path);
}

static bool save_download_session(const char *session_id, cJSON *root) {
    char path[PATH_MAX];
    download_session_meta_path(path, sizeof(path), session_id);
    return save_json_file(path, root);
}

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

static void cleanup_upload_session(const char *upload_id) {
    char dir_path[PATH_MAX];
    upload_session_dir(dir_path, sizeof(dir_path), upload_id);
    cleanup_dir_tree(dir_path);
}

static void cleanup_download_session(const char *session_id) {
    char dir_path[PATH_MAX];
    download_session_dir(dir_path, sizeof(dir_path), session_id);
    cleanup_dir_tree(dir_path);
}

/* --- Topology Logic --- */

static int chunk_grid_cols(int chunk_count) {
    int cols = 1;
    while (cols * cols < chunk_count) cols++;
    return cols;
}

static void upload_vertex_for_index(int chunk_index, int chunk_count, int *q, int *r) {
    int cols = chunk_grid_cols(chunk_count);
    if (q) *q = chunk_index % cols;
    if (r) *r = chunk_index / cols;
}

static int upload_collect_neighbor_indices(int chunk_index, int chunk_count, int *out_indices, int out_cap) {
    int q = 0, r = 0;
    int cols = chunk_grid_cols(chunk_count);
    int total = 0;
    upload_vertex_for_index(chunk_index, chunk_count, &q, &r);
    for (int i = 0; i < 6 && total < out_cap; i++) {
        int nq = q + TASFA_DIRS[i][0];
        int nr = r + TASFA_DIRS[i][1];
        if (nq < 0 || nr < 0) continue;
        int next = nr * cols + nq;
        if (next >= 0 && next < chunk_count) out_indices[total++] = next;
    }
    return total;
}

static int upload_magic_sum_for_index(int chunk_index, int chunk_count) {
    int sum = chunk_index + 1;
    int neighbors[6];
    int count = upload_collect_neighbor_indices(chunk_index, chunk_count, neighbors, 6);
    for (int i = 0; i < count; i++) sum += neighbors[i] + 1;
    return sum;
}

static const char *upload_topology_rule_violation(int chunk_index, int chunk_count, const char *vertex_id, int magic_sum) {
    int expected_q = 0, expected_r = 0, got_q = 0, got_r = 0;
    if (!vertex_id || sscanf(vertex_id, "%d:%d", &got_q, &got_r) != 2) return "vertex_parse_mismatch";
    upload_vertex_for_index(chunk_index, chunk_count, &expected_q, &expected_r);
    if (got_q != expected_q || got_r != expected_r) return "vertex_position_mismatch";
    if (magic_sum != upload_magic_sum_for_index(chunk_index, chunk_count)) return "magic_sum_mismatch";
    return NULL;
}

static char *topology_frontier_bitmap_create(const char *bitmap, int chunk_count) {
    char *frontier = bitmap_create(chunk_count);
    if (!frontier) return NULL;
    if (!bitmap) return frontier;
    for (int i = 0; i < chunk_count; i++) {
        if (bitmap[i] == '1') continue;
        int neighbors[6];
        int count = upload_collect_neighbor_indices(i, chunk_count, neighbors, 6);
        for (int j = 0; j < count; j++) {
            if (bitmap[neighbors[j]] == '1') {
                frontier[i] = '1';
                break;
            }
        }
    }
    return frontier;
}

static char *topology_damage_bitmap_create(const char *bitmap, int chunk_count, int anchor_chunk) {
    char *damage = bitmap_create(chunk_count);
    if (!damage) return NULL;
    if (anchor_chunk < 0 || anchor_chunk >= chunk_count) return damage;
    damage[anchor_chunk] = '1';
    int neighbors[6];
    int count = upload_collect_neighbor_indices(anchor_chunk, chunk_count, neighbors, 6);
    for (int i = 0; i < count; i++) {
        int idx = neighbors[i];
        if (!bitmap || bitmap[idx] != '1') damage[idx] = '1';
    }
    return damage;
}

/* --- Response Helpers --- */

static cJSON *build_upload_status_json(cJSON *meta, const char *upload_id) {
    const char *bitmap = json_string(meta, "received_bitmap", "");
    int chunk_count = json_int(meta, "chunk_count", 0);
    int received_chunks = json_int(meta, "received_chunks", bitmap_count_set(bitmap, chunk_count));
    cJSON *obj = cJSON_CreateObject();
    char *frontier = topology_frontier_bitmap_create(bitmap, chunk_count);
    int transport_block_count = json_int(meta, "transport_block_count", transport_block_count_for_size(json_long_long(meta, "total_size", 0)));
    char *block_bitmap = load_upload_block_state_bin(upload_id, transport_block_count);
    long long total_size = json_long_long(meta, "total_size", 0);
    long long resume_from_byte = contiguous_transport_bytes_from_bitmap(
        block_bitmap, transport_block_count, json_int(meta, "transport_block_size", TASFA_TRANSPORT_BLOCK_SIZE), total_size);
    cJSON_AddBoolToObject(obj, "ok", true);
    cJSON_AddStringToObject(obj, "upload_id", upload_id ? upload_id : "");
    cJSON_AddStringToObject(obj, "upload_token", json_string(meta, "upload_token", ""));
    cJSON_AddStringToObject(obj, "upload_secret", json_string(meta, "upload_secret", ""));
    cJSON_AddNumberToObject(obj, "chunk_size", json_int(meta, "chunk_size", TASFA_CHUNK_SIZE));
    cJSON_AddNumberToObject(obj, "chunk_count", chunk_count);
    cJSON_AddNumberToObject(obj, "transport_block_size", json_int(meta, "transport_block_size", TASFA_TRANSPORT_BLOCK_SIZE));
    cJSON_AddNumberToObject(obj, "transport_block_count", transport_block_count);
    cJSON_AddNumberToObject(obj, "total_size", (double)total_size);
    cJSON_AddNumberToObject(obj, "resume_from_byte", (double)resume_from_byte);
    cJSON_AddStringToObject(obj, "received_bitmap", bitmap);
    cJSON_AddStringToObject(obj, "received_block_bitmap", block_bitmap ? block_bitmap : "");
    cJSON_AddNumberToObject(obj, "received_chunks", received_chunks);
    cJSON_AddNumberToObject(obj, "current_parallel_chunks", json_int(meta, "current_parallel_chunks", TASFA_UPLOAD_DEFAULT_PARALLEL));
    cJSON_AddNumberToObject(obj, "initial_parallel_chunks", json_int(meta, "current_parallel_chunks", TASFA_UPLOAD_DEFAULT_PARALLEL));
    cJSON_AddNumberToObject(obj, "max_parallel_chunks", json_int(meta, "max_parallel_chunks", TASFA_UPLOAD_MAX_PARALLEL));
    cJSON_AddStringToObject(obj, "expires_at", json_string(meta, "expires_at", "0"));
    cJSON_AddStringToObject(obj, "topology_frontier_bitmap", frontier ? frontier : "");
    cJSON_AddStringToObject(obj, "topology_closed_bitmap", bitmap);
    cJSON_AddStringToObject(obj, "topology_damage_bitmap", "");
    cJSON_AddStringToObject(obj, "topology_rule", "");
    cJSON_AddBoolToObject(obj, "topology_closure_complete", chunk_count > 0 && received_chunks == chunk_count);
    cJSON_AddNumberToObject(obj, "client_stripes", TASFA_CLIENT_STRIPES);
    if (frontier) cwist_free(frontier);
    if (block_bitmap) cwist_free(block_bitmap);
    return obj;
}

static cJSON *build_upload_recoverable_json(cJSON *meta, const char *upload_id, int chunk_index, const char *reason) {
    cJSON *obj = build_upload_status_json(meta, upload_id);
    char *damage = topology_damage_bitmap_create(json_string(meta, "received_bitmap", ""), json_int(meta, "chunk_count", 0), chunk_index);
    cJSON_AddBoolToObject(obj, "partial", true);
    cJSON_AddBoolToObject(obj, "accepted", false);
    cJSON_AddBoolToObject(obj, "recoverable", true);
    cJSON_AddNumberToObject(obj, "chunk_index", chunk_index);
    cJSON_ReplaceItemInObject(obj, "topology_damage_bitmap", cJSON_CreateString(damage ? damage : ""));
    cJSON_ReplaceItemInObject(obj, "topology_rule", cJSON_CreateString(reason ? reason : "chunk_rejected"));
    if (damage) cwist_free(damage);
    return obj;
}

static bool send_file_slice_response(cwist_http_response *res, const char *path, const char *mime, long long offset, size_t amount) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    char *buf = (char *)cwist_alloc(amount ? amount : 1);
    if (!buf) {
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
    if (total != amount) {
        cwist_free(buf);
        return false;
    }
    char len_buf[32];
    snprintf(len_buf, sizeof(len_buf), "%zu", amount);
    cwist_http_header_add(&res->headers, "Content-Type", mime ? mime : "application/octet-stream");
    cwist_http_header_add(&res->headers, "Content-Length", len_buf);
    cwist_http_header_add(&res->headers, "Cache-Control", "public, max-age=86400");
    cwist_sstring_assign(res->body, "");
    cwist_sstring_append_len(res->body, buf, amount);
    cwist_free(buf);
    return true;
}

static bool resolve_asset_scope_path(const char *scope, const char *encoded, char *storage_path, size_t storage_len,
                                     char *filename, size_t filename_len, const char **mime_out) {
    char *decoded = decode_segment(encoded ? encoded : "");
    bool ok = false;
    if (!decoded) return false;
    if (!strcmp(scope ? scope : "", "img") && is_safe_filename_simple(decoded)) {
        snprintf(storage_path, storage_len, "public/img/%s", decoded);
        snprintf(filename, filename_len, "%s", decoded);
        if (mime_out) *mime_out = mime_type(decoded);
        ok = true;
    } else if (!strcmp(scope ? scope : "", "uploads") && is_safe_filename_simple(decoded)) {
        snprintf(storage_path, storage_len, "public/uploads/%s", decoded);
        snprintf(filename, filename_len, "%s", decoded);
        if (mime_out) *mime_out = mime_type(decoded);
        ok = true;
    }
    cwist_free(decoded);
    return ok;
}

static bool init_download_session(const char *filename, const char *mime, const char *storage_path, long long total_size,
                                  int score, cJSON **out) {
    if (!ensure_tasfa_roots()) return false;
    char session_id[33];
    char session_token[49];
    if (!random_hex(session_id, 16) || !random_hex(session_token, 24)) return false;
    char dir_path[PATH_MAX];
    download_session_dir(dir_path, sizeof(dir_path), session_id);
    if (!dir_ensure(dir_path)) return false;
    int initial_parallel = 0, max_parallel = 0, pacing_ms = 0, coalesce = 0;
    choose_download_profile(score, &initial_parallel, &max_parallel, &pacing_ms, &coalesce);
    int chunk_count = (int)((total_size + TASFA_CHUNK_SIZE - 1) / TASFA_CHUNK_SIZE);
    if (chunk_count < 1) chunk_count = 1;
    cJSON *meta = cJSON_CreateObject();
    cJSON_AddStringToObject(meta, "session_id", session_id);
    cJSON_AddStringToObject(meta, "session_token", session_token);
    cJSON_AddStringToObject(meta, "filename", filename ? filename : "download");
    cJSON_AddStringToObject(meta, "mime_type", mime ? mime : "application/octet-stream");
    cJSON_AddStringToObject(meta, "storage_path", storage_path ? storage_path : "");
    cJSON_AddNumberToObject(meta, "total_size", (double)total_size);
    cJSON_AddNumberToObject(meta, "chunk_size", TASFA_CHUNK_SIZE);
    cJSON_AddNumberToObject(meta, "chunk_count", chunk_count);
    cJSON_AddNumberToObject(meta, "initial_parallel_chunks", initial_parallel);
    cJSON_AddNumberToObject(meta, "max_parallel_chunks", max_parallel);
    cJSON_AddNumberToObject(meta, "dispatch_pacing_ms", pacing_ms);
    cJSON_AddNumberToObject(meta, "coalesce_chunks", coalesce);
    char expires_at[32];
    snprintf(expires_at, sizeof(expires_at), "%lld", (long long)(time(NULL) + TASFA_DOWNLOAD_TTL));
    cJSON_AddStringToObject(meta, "expires_at", expires_at);
    if (!save_download_session(session_id, meta)) {
        cJSON_Delete(meta);
        cleanup_download_session(session_id);
        return false;
    }
    if (out) *out = meta;
    else cJSON_Delete(meta);
    return true;
}

static bool find_upload_id_by_session_id(const char *session_id, char *out_upload_id, size_t out_len) {
    DIR *dir = opendir(TASFA_UPLOAD_DIR);
    if (!dir) return false;
    struct dirent *ent;
    bool found = false;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        char meta_path[PATH_MAX];
        snprintf(meta_path, sizeof(meta_path), "%s/%s/meta.json", TASFA_UPLOAD_DIR, ent->d_name);
        cJSON *meta = load_json_file(meta_path);
        if (!meta) continue;
        if (strcmp(json_string(meta, "session_id", ""), session_id) == 0) {
            size_t copy_len = strlen(ent->d_name);
            if (copy_len >= out_len) copy_len = out_len - 1;
            memcpy(out_upload_id, ent->d_name, copy_len);
            out_upload_id[copy_len] = '\0';
            found = true;
            cJSON_Delete(meta);
            break;
        }
        cJSON_Delete(meta);
    }
    closedir(dir);
    return found;
}

static bool pwrite_all(int fd, const void *buf, size_t len, off_t offset) {
    const unsigned char *cursor = (const unsigned char *)buf;
    size_t written = 0;
    while (written < len) {
        ssize_t rc = pwrite(fd, cursor + written, len - written, offset + (off_t)written);
        if (rc < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (rc == 0) return false;
        written += (size_t)rc;
    }
    return true;
}

static bool inflate_gzip_exact(const unsigned char *src, size_t src_len, unsigned char *dst, size_t dst_len) {
    z_stream stream;
    memset(&stream, 0, sizeof(stream));
    stream.next_in = (Bytef *)src;
    stream.avail_in = (uInt)src_len;
    stream.next_out = dst;
    stream.avail_out = (uInt)dst_len;
    if (inflateInit2(&stream, 16 + MAX_WBITS) != Z_OK) return false;
    int rc = inflate(&stream, Z_FINISH);
    bool ok = rc == Z_STREAM_END && stream.total_out == dst_len;
    inflateEnd(&stream);
    return ok;
}

/* --- Handlers --- */

void handler_file_upload_init(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0;
    char role[32] = {0};
    auth_is_logged_in(req, &uid, role, sizeof(role));
    cwist_query_map *kv = cwist_query_map_create();
    cwist_query_map_parse(kv, req->body->data);
    const char *filename = cwist_query_map_get(kv, "filename");
    const char *session_id = cwist_query_map_get(kv, "session_id");
    const char *resume_upload_id = cwist_query_map_get(kv, "resume_upload_id");
    const char *resume_upload_token = cwist_query_map_get(kv, "resume_upload_token");
    const char *rollover_upload_id = cwist_query_map_get(kv, "rollover_upload_id");
    const char *rollover_upload_token = cwist_query_map_get(kv, "rollover_upload_token");
    long long client_confirmed_bytes = atoll(cwist_query_map_get(kv, "client_confirmed_bytes") ? cwist_query_map_get(kv, "client_confirmed_bytes") : "0");
    int suggested_parallel = atoi(cwist_query_map_get(kv, "suggested_parallel") ? cwist_query_map_get(kv, "suggested_parallel") : "0");
    int chunk_count = atoi(cwist_query_map_get(kv, "chunk_count") ? cwist_query_map_get(kv, "chunk_count") : "0");
    long long total_size = atoll(cwist_query_map_get(kv, "total_size") ? cwist_query_map_get(kv, "total_size") : "0");
    int post_id = atoi(cwist_query_map_get(kv, "post_id") ? cwist_query_map_get(kv, "post_id") : "0");
    int score = link_score_from_inputs(
        cwist_query_map_get(kv, "link_stability_score"),
        cwist_query_map_get(kv, "link_effective_type"),
        cwist_query_map_get(kv, "link_downlink_mbps"),
        cwist_query_map_get(kv, "link_rtt_ms"),
        cwist_query_map_get(kv, "link_retry_events"),
        cwist_query_map_get(kv, "link_timeout_events"),
        cwist_query_map_get(kv, "link_save_data")
    );
    if (!filename || !is_safe_filename_simple(filename) || chunk_count <= 0 || total_size <= 0 || !ensure_tasfa_roots()) {
        cwist_query_map_destroy(kv);
        send_json_response(res, session_error_json("invalid upload init payload"), CWIST_HTTP_BAD_REQUEST);
        return;
    }
    char upload_id[33];
    cJSON *meta = NULL;
    bool resumed = false;
    bool rolled_over = false;
    char previous_upload_id[33] = {0};
    int previous_lock_fd = -1;
    cJSON *previous_meta = NULL;
    char *previous_chunk_bitmap = NULL;
    char *previous_block_bitmap = NULL;
    char previous_temp_path[PATH_MAX] = {0};
    int previous_chunk_count = 0;
    int previous_transport_block_count = 0;
    if (rollover_upload_id && rollover_upload_id[0] && rollover_upload_token && rollover_upload_token[0] && is_safe_segment(rollover_upload_id)) {
        previous_lock_fd = open_upload_session_lock(rollover_upload_id);
        previous_meta = previous_lock_fd >= 0 ? load_upload_session(rollover_upload_id) : NULL;
        if (previous_meta &&
            secure_str_eq(rollover_upload_token, json_string(previous_meta, "upload_token", "")) &&
            json_long_long(previous_meta, "total_size", 0) == total_size &&
            strcmp(json_string(previous_meta, "filename", ""), filename) == 0) {
            snprintf(previous_upload_id, sizeof(previous_upload_id), "%s", rollover_upload_id);
            previous_chunk_count = json_int(previous_meta, "chunk_count", 0);
            previous_transport_block_count = json_int(previous_meta, "transport_block_count",
                                                      transport_block_count_for_size(total_size));
            snprintf(previous_temp_path, sizeof(previous_temp_path), "%s", json_string(previous_meta, "temp_path", ""));
            previous_chunk_bitmap = load_upload_session_state_bin(rollover_upload_id, previous_chunk_count);
            previous_block_bitmap = load_upload_block_state_bin(rollover_upload_id, previous_transport_block_count);
            rolled_over = previous_chunk_bitmap && previous_block_bitmap && previous_temp_path[0];
        }
        if (!rolled_over) {
            if (previous_chunk_bitmap) cwist_free(previous_chunk_bitmap);
            if (previous_block_bitmap) cwist_free(previous_block_bitmap);
            previous_chunk_bitmap = NULL;
            previous_block_bitmap = NULL;
            if (previous_meta) {
                cJSON_Delete(previous_meta);
                previous_meta = NULL;
            }
            close_upload_session_lock(previous_lock_fd);
            previous_lock_fd = -1;
        }
    }
    if (rolled_over) resumed = false;
    if (resume_upload_id && resume_upload_id[0] && resume_upload_token && resume_upload_token[0] && is_safe_segment(resume_upload_id)) {
        meta = load_upload_session(resume_upload_id);
        if (meta &&
            secure_str_eq(resume_upload_token, json_string(meta, "upload_token", "")) &&
            json_long_long(meta, "total_size", 0) == total_size &&
            strcmp(json_string(meta, "filename", ""), filename) == 0) {
            snprintf(upload_id, sizeof(upload_id), "%s", resume_upload_id);
            resumed = true;
        } else if (meta) {
            cJSON_Delete(meta);
            meta = NULL;
        }
    }
    if (!resumed && session_id && session_id[0] && find_upload_id_by_session_id(session_id, upload_id, sizeof(upload_id))) {
        meta = load_upload_session(upload_id);
        if (meta && json_long_long(meta, "total_size", 0) == total_size && strcmp(json_string(meta, "filename", ""), filename) == 0) {
            resumed = true;
        } else if (meta) {
            cJSON_Delete(meta);
            meta = NULL;
        }
    }
    if (!resumed) {
        if (!random_hex(upload_id, 16)) {
            if (previous_chunk_bitmap) cwist_free(previous_chunk_bitmap);
            if (previous_block_bitmap) cwist_free(previous_block_bitmap);
            if (previous_meta) cJSON_Delete(previous_meta);
            close_upload_session_lock(previous_lock_fd);
            cwist_query_map_destroy(kv);
            send_json_response(res, session_error_json("upload init failed"), CWIST_HTTP_INTERNAL_ERROR);
            return;
        }
        char dir_path[PATH_MAX];
        char temp_path[PATH_MAX];
        upload_session_dir(dir_path, sizeof(dir_path), upload_id);
        upload_session_temp_path(temp_path, sizeof(temp_path), upload_id);
        bool temp_ready = false;
        if (dir_ensure(dir_path)) {
            if (rolled_over) {
                temp_ready = rename(previous_temp_path, temp_path) == 0;
            } else {
                temp_ready = ensure_preallocated_file(temp_path, total_size);
            }
        }
        if (!temp_ready) {
            if (previous_chunk_bitmap) cwist_free(previous_chunk_bitmap);
            if (previous_block_bitmap) cwist_free(previous_block_bitmap);
            if (previous_meta) cJSON_Delete(previous_meta);
            close_upload_session_lock(previous_lock_fd);
            cwist_query_map_destroy(kv);
            send_json_response(res, session_error_json("upload init failed"), CWIST_HTTP_INTERNAL_ERROR);
            return;
        }
        char upload_token[49];
        char upload_secret[49];
        char *bitmap = rolled_over ? previous_chunk_bitmap : bitmap_create(chunk_count);
        if (!bitmap || !random_hex(upload_token, 24) || !random_hex(upload_secret, 24)) {
            if (!rolled_over && bitmap) cwist_free(bitmap);
            if (previous_block_bitmap) cwist_free(previous_block_bitmap);
            if (previous_meta) cJSON_Delete(previous_meta);
            close_upload_session_lock(previous_lock_fd);
            cwist_query_map_destroy(kv);
            send_json_response(res, session_error_json("upload init failed"), CWIST_HTTP_INTERNAL_ERROR);
            return;
        }
        int initial_parallel = 0, max_parallel = 0;
        choose_upload_window(score, &initial_parallel, &max_parallel);
        if (suggested_parallel > 0) {
            int suggested = clamp_int(suggested_parallel, 2, TASFA_UPLOAD_MAX_PARALLEL);
            if (initial_parallel < suggested) initial_parallel = suggested;
            if (max_parallel < suggested) max_parallel = suggested;
        }
        meta = cJSON_CreateObject();
        cJSON_AddNumberToObject(meta, "uid", uid);
        cJSON_AddNumberToObject(meta, "post_id", post_id);
        cJSON_AddStringToObject(meta, "filename", filename);
        cJSON_AddStringToObject(meta, "session_id", session_id ? session_id : "");
        cJSON_AddStringToObject(meta, "upload_token", upload_token);
        cJSON_AddStringToObject(meta, "upload_secret", upload_secret);
        cJSON_AddStringToObject(meta, "temp_path", temp_path);
        cJSON_AddNumberToObject(meta, "chunk_size", TASFA_CHUNK_SIZE);
        cJSON_AddNumberToObject(meta, "chunk_count", chunk_count);
        cJSON_AddNumberToObject(meta, "transport_block_size", TASFA_TRANSPORT_BLOCK_SIZE);
        cJSON_AddNumberToObject(meta, "transport_block_count", transport_block_count_for_size(total_size));
        cJSON_AddNumberToObject(meta, "total_size", (double)total_size);
        cJSON_AddStringToObject(meta, "received_bitmap", bitmap);
        cJSON_AddNumberToObject(meta, "received_chunks", 0);
        cJSON_AddNumberToObject(meta, "current_parallel_chunks", initial_parallel);
        cJSON_AddNumberToObject(meta, "max_parallel_chunks", max_parallel);
        if (rolled_over) cJSON_AddStringToObject(meta, "rolled_over_from", previous_upload_id);
        if (rolled_over) cJSON_AddNumberToObject(meta, "client_confirmed_bytes", (double)client_confirmed_bytes);
        char expires_at[32];
        snprintf(expires_at, sizeof(expires_at), "%lld", (long long)(time(NULL) + TASFA_UPLOAD_TTL));
        cJSON_AddStringToObject(meta, "expires_at", expires_at);
        
        /* Save Binary Meta for Fast Path */
        tasfa_meta_bin_t mbin = {0};
        strncpy(mbin.upload_token, upload_token, sizeof(mbin.upload_token)-1);
        strncpy(mbin.upload_secret, upload_secret, sizeof(mbin.upload_secret)-1);
        mbin.chunk_count = chunk_count;
        mbin.chunk_size = TASFA_CHUNK_SIZE;
        mbin.transport_block_size = TASFA_TRANSPORT_BLOCK_SIZE;
        mbin.transport_block_count = transport_block_count_for_size(total_size);
        mbin.total_size = total_size;
        strncpy(mbin.filename, filename, sizeof(mbin.filename)-1);
        mbin.post_id = post_id;
        mbin.uid = uid;
        snprintf(mbin.temp_path, sizeof(mbin.temp_path), "%s", temp_path);
        
        char *block_bitmap = rolled_over ? previous_block_bitmap : bitmap_create(mbin.transport_block_count);
        if (!block_bitmap ||
            !save_upload_session(upload_id, meta) || !save_upload_session_state(upload_id, meta) ||
            !save_upload_session_state_bin(upload_id, chunk_count, bitmap) ||
            !save_upload_block_state_bin(upload_id, mbin.transport_block_count, block_bitmap) ||
            !save_upload_session_meta_bin(upload_id, &mbin)) {
            if (!rolled_over && block_bitmap) cwist_free(block_bitmap);
            if (!rolled_over && bitmap) cwist_free(bitmap);
            cJSON_Delete(meta);
            cleanup_upload_session(upload_id);
            if (rolled_over && previous_temp_path[0]) rename(temp_path, previous_temp_path);
            if (previous_meta) cJSON_Delete(previous_meta);
            close_upload_session_lock(previous_lock_fd);
            cwist_query_map_destroy(kv);
            send_json_response(res, session_error_json("upload init failed"), CWIST_HTTP_INTERNAL_ERROR);
            return;
        }
        cwist_free(block_bitmap);
        cwist_free(bitmap);
        previous_block_bitmap = NULL;
        previous_chunk_bitmap = NULL;
        if (rolled_over) {
            cleanup_upload_session(previous_upload_id);
            if (previous_meta) cJSON_Delete(previous_meta);
            close_upload_session_lock(previous_lock_fd);
            previous_meta = NULL;
            previous_lock_fd = -1;
        }
    } else {
        int initial_parallel = 0, max_parallel = 0;
        choose_upload_window(score, &initial_parallel, &max_parallel);
        if (suggested_parallel > 0) {
            int suggested = clamp_int(suggested_parallel, 2, TASFA_UPLOAD_MAX_PARALLEL);
            if (initial_parallel < suggested) initial_parallel = suggested;
            if (max_parallel < suggested) max_parallel = suggested;
        }
        int current_parallel = json_int(meta, "current_parallel_chunks", initial_parallel);
        int current_max = json_int(meta, "max_parallel_chunks", max_parallel);
        if (initial_parallel < current_parallel) initial_parallel = current_parallel;
        if (max_parallel < current_max) max_parallel = current_max;
        cJSON_ReplaceItemInObject(meta, "current_parallel_chunks", cJSON_CreateNumber(initial_parallel));
        cJSON_ReplaceItemInObject(meta, "max_parallel_chunks", cJSON_CreateNumber(max_parallel));
        char upload_token[49];
        if (random_hex(upload_token, 24)) cJSON_ReplaceItemInObject(meta, "upload_token", cJSON_CreateString(upload_token));
        char upload_secret[49];
        if (random_hex(upload_secret, 24)) cJSON_ReplaceItemInObject(meta, "upload_secret", cJSON_CreateString(upload_secret));
        char expires_at[32];
        snprintf(expires_at, sizeof(expires_at), "%lld", (long long)(time(NULL) + TASFA_UPLOAD_TTL));
        cJSON_ReplaceItemInObject(meta, "expires_at", cJSON_CreateString(expires_at));
        
        /* Update Binary Meta */
        tasfa_meta_bin_t mbin = {0};
        strncpy(mbin.upload_token, json_string(meta, "upload_token", ""), sizeof(mbin.upload_token)-1);
        strncpy(mbin.upload_secret, json_string(meta, "upload_secret", ""), sizeof(mbin.upload_secret)-1);
        mbin.chunk_count = json_int(meta, "chunk_count", 0);
        mbin.chunk_size = TASFA_CHUNK_SIZE;
        mbin.total_size = json_long_long(meta, "total_size", 0);
        mbin.transport_block_size = json_int(meta, "transport_block_size", TASFA_TRANSPORT_BLOCK_SIZE);
        mbin.transport_block_count = json_int(meta, "transport_block_count", transport_block_count_for_size(mbin.total_size));
        strncpy(mbin.filename, json_string(meta, "filename", ""), sizeof(mbin.filename)-1);
        mbin.post_id = json_int(meta, "post_id", 0);
        mbin.uid = json_int(meta, "uid", 0);
        strncpy(mbin.temp_path, json_string(meta, "temp_path", ""), sizeof(mbin.temp_path)-1);
        
        save_upload_session(upload_id, meta);
        save_upload_session_state(upload_id, meta);
        save_upload_session_meta_bin(upload_id, &mbin);
    }
    if (previous_chunk_bitmap) cwist_free(previous_chunk_bitmap);
    if (previous_block_bitmap) cwist_free(previous_block_bitmap);
    if (previous_meta) cJSON_Delete(previous_meta);
    close_upload_session_lock(previous_lock_fd);
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
    int initial_parallel = 0, max_parallel = 0;
    choose_upload_window(score, &initial_parallel, &max_parallel);
    int current_parallel = json_int(meta, "current_parallel_chunks", initial_parallel);
    int current_max = json_int(meta, "max_parallel_chunks", max_parallel);
    if (max_parallel < current_max) max_parallel = current_max;
    if (suggested > 0) initial_parallel = clamp_int(suggested, 2, max_parallel);
    if (initial_parallel < current_parallel) initial_parallel = current_parallel;
    cJSON_ReplaceItemInObject(meta, "current_parallel_chunks", cJSON_CreateNumber(initial_parallel));
    cJSON_ReplaceItemInObject(meta, "max_parallel_chunks", cJSON_CreateNumber(max_parallel));
    save_upload_session(upload_id, meta);
    save_upload_session_state(upload_id, meta);
    cJSON *obj = build_upload_status_json(meta, upload_id);
    cJSON_Delete(meta);
    close_upload_session_lock(lock_fd);
    cwist_query_map_destroy(kv);
    send_json_response(res, obj, CWIST_HTTP_OK);
}

static int count_received_chunks_in_session(const char *upload_id, int chunk_count) {
    char *bitmap = load_upload_session_state_bin(upload_id, chunk_count);
    if (!bitmap) return 0;
    int count = bitmap_count_set(bitmap, chunk_count);
    cwist_free(bitmap);
    return count;
}

void handler_file_upload(cwist_http_request *req, cwist_http_response *res) {
    const char *upload_id = cwist_http_header_get(req->headers, "X-TASFA-Upload-ID");
    const char *upload_token = cwist_http_header_get(req->headers, "X-TASFA-Upload-Token");
    const char *nonce = cwist_http_header_get(req->headers, "X-TASFA-Nonce");
    const char *vertex_id = cwist_http_header_get(req->headers, "X-TASFA-Vertex-Id");
    const char *signature = cwist_http_header_get(req->headers, "X-TASFA-Chunk-Signature");
    const char *content_encoding = cwist_http_header_get(req->headers, "Content-Encoding");
    const char *chunk_index_str = cwist_http_header_get(req->headers, "X-TASFA-Chunk-Index");
    const char *transport_block_index_str = cwist_http_header_get(req->headers, "X-TASFA-Transport-Block-Index");
    const char *block_offset_str = cwist_http_header_get(req->headers, "X-TASFA-Block-Offset");
    const char *magic_sum_str = cwist_http_header_get(req->headers, "X-TASFA-Magic-Sum");
    if (!upload_id || !upload_token || !chunk_index_str || !transport_block_index_str) {
        send_json_response(res, session_error_json("resumable upload headers required"), CWIST_HTTP_BAD_REQUEST);
        return;
    }
    int chunk_index = atoi(chunk_index_str);
    int transport_block_index = atoi(transport_block_index_str);
    long long block_offset = atoll(block_offset_str ? block_offset_str : "0");
    int magic_sum = atoi(magic_sum_str ? magic_sum_str : "0");
    if (!is_safe_segment(upload_id) || chunk_index < 0 || transport_block_index < 0) {
        send_json_response(res, session_error_json("invalid upload chunk payload"), CWIST_HTTP_BAD_REQUEST);
        return;
    }

    /* Hot upload path: load compact binary session metadata instead of parsing JSON. */
    tasfa_meta_bin_t mbin;
    if (!load_upload_session_meta_bin(upload_id, &mbin)) {
        send_json_response(res, session_error_json("upload session not found"), CWIST_HTTP_NOT_FOUND);
        return;
    }
    if (!secure_str_eq(upload_token, mbin.upload_token)) {
        send_json_response(res, session_error_json("upload chunk rejected"), CWIST_HTTP_FORBIDDEN);
        return;
    }
    
    if (transport_block_index >= mbin.transport_block_count) {
        send_json_response(res, session_error_json("invalid upload chunk payload"), CWIST_HTTP_BAD_REQUEST);
        return;
    }
    long long expected_offset = (long long)transport_block_index * (long long)mbin.transport_block_size;
    long long expected_size = expected_offset + mbin.transport_block_size > mbin.total_size ? (mbin.total_size - expected_offset) : mbin.transport_block_size;
    if (expected_size < 0) expected_size = 0;
    if ((int)(expected_offset / (long long)mbin.chunk_size) != chunk_index) {
        send_json_response(res, session_error_json("invalid upload chunk payload"), CWIST_HTTP_BAD_REQUEST);
        return;
    }
    
    const char *rule = upload_topology_rule_violation(chunk_index, mbin.chunk_count, vertex_id, magic_sum);
    if (!nonce || !signature || !mbin.upload_secret[0] || rule ||
        !verify_upload_signature(mbin.upload_secret, upload_id, chunk_index, block_offset, nonce, vertex_id, magic_sum, signature)) {
        cJSON *meta = load_upload_session(upload_id);
        cJSON *obj = build_upload_recoverable_json(meta, upload_id, chunk_index, rule ? rule : "signature_mismatch");
        cJSON_Delete(meta);
        send_json_response(res, obj, CWIST_HTTP_OK);
        return;
    }
    if (block_offset != expected_offset || expected_size <= 0) {
        cJSON *meta = load_upload_session(upload_id);
        cJSON *obj = build_upload_recoverable_json(meta, upload_id, chunk_index, "invalid_upload_chunk_bounds");
        cJSON_Delete(meta);
        send_json_response(res, obj, CWIST_HTTP_OK);
        return;
    }

    bool stored = false;
    int fd = open(mbin.temp_path, O_WRONLY);
    if (fd >= 0) {
        if (content_encoding && strcmp(content_encoding, "gzip") == 0) {
            unsigned char *inflated = (unsigned char *)cwist_alloc((size_t)expected_size);
            if (inflated && inflate_gzip_exact((const unsigned char *)req->body->data, req->body->size, inflated, (size_t)expected_size)) {
                stored = pwrite_all(fd, inflated, (size_t)expected_size, (off_t)expected_offset);
            }
            if (inflated) cwist_free(inflated);
        } else if ((long long)req->body->size == expected_size) {
            stored = pwrite_all(fd, req->body->data, req->body->size, (off_t)expected_offset);
        }
        close(fd);
    }
    if (!stored) {
        send_json_response(res, session_error_json("chunk store failed"), CWIST_HTTP_INTERNAL_ERROR);
        return;
    }

    mark_transport_block_received(upload_id, transport_block_index);
    bool chunk_complete = false;
    char *block_bitmap = load_upload_block_state_bin(upload_id, mbin.transport_block_count);
    if (block_bitmap && is_chunk_complete_from_block_bitmap(block_bitmap, chunk_index, mbin.total_size)) {
        mark_chunk_received_in_session_state(upload_id, chunk_index);
        chunk_complete = true;
    }

    /* Return a small ACK payload and only send full progress snapshots occasionally. */
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddBoolToObject(obj, "ok", true);
    cJSON_AddBoolToObject(obj, "partial", true);
    cJSON_AddBoolToObject(obj, "accepted", true);
    cJSON_AddStringToObject(obj, "upload_id", upload_id);
    cJSON_AddNumberToObject(obj, "transport_block_index", transport_block_index);
    cJSON_AddBoolToObject(obj, "chunk_complete", chunk_complete);
    
    /* Return progress occasionally or at the end */
    if (transport_block_index % 32 == 0 || chunk_complete) {
        int received_count = count_received_chunks_in_session(upload_id, mbin.chunk_count);
        cJSON_AddNumberToObject(obj, "received_chunks", received_count);
        
        char *bin_bitmap = load_upload_session_state_bin(upload_id, mbin.chunk_count);
        if (bin_bitmap) {
            cJSON_AddStringToObject(obj, "received_bitmap", bin_bitmap);
            cwist_free(bin_bitmap);
        }
        if (block_bitmap) cJSON_AddStringToObject(obj, "received_block_bitmap", block_bitmap);
    }
    if (block_bitmap) cwist_free(block_bitmap);

    cJSON_AddNumberToObject(obj, "chunk_index", chunk_index);
    send_json_response(res, obj, CWIST_HTTP_OK);
}

void handler_file_upload_complete(cwist_http_request *req, cwist_http_response *res) {
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
    if (bitmap_count_set(bitmap, chunk_count) != chunk_count) {
        cJSON_Delete(meta);
        close_upload_session_lock(lock_fd);
        cwist_query_map_destroy(kv);
        send_json_response(res, session_error_json("missing upload chunks"), CWIST_HTTP_BAD_REQUEST);
        return;
    }
    const char *temp_path = json_string(meta, "temp_path", "");
    const char *filename = json_string(meta, "filename", "upload.bin");
    int post_id = json_int(meta, "post_id", 0);
    int owner_uid = json_int(meta, "uid", uid);
    char final_path[PATH_MAX];
    snprintf(final_path, sizeof(final_path), "public/uploads/%ld_%s", (long)time(NULL), filename);
    if (rename(temp_path, final_path) != 0) {
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
    int fid = db_file_create_volume_get_id(req->db, post_id, owner_uid, filename, mime_buf, final_path, (size_t)json_long_long(meta, "total_size", 0));
    if (fid <= 0) {
        unlink(final_path);
        cJSON_Delete(meta);
        close_upload_session_lock(lock_fd);
        cwist_query_map_destroy(kv);
        send_json_response(res, session_error_json("db insert failed"), CWIST_HTTP_INTERNAL_ERROR);
        return;
    }
    char delete_pin[13];
    char delete_pin_hash[512];
    delete_pin[0] = '\0';
    if (random_hex(delete_pin, 6) && auth_hash_password(delete_pin, delete_pin_hash, sizeof(delete_pin_hash))) {
        (void)db_file_set_delete_pin_hash(req->db, fid, delete_pin_hash);
    } else {
        delete_pin[0] = '\0';
    }
    cJSON_Delete(meta);
    close_upload_session_lock(lock_fd);
    cleanup_upload_session(upload_id);
    cwist_query_map_destroy(kv);
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddBoolToObject(obj, "ok", true);
    cJSON_AddNumberToObject(obj, "id", fid);
    cJSON_AddNumberToObject(obj, "fid", fid);
    cJSON_AddStringToObject(obj, "filename", filename);
    char url[512];
    snprintf(url, sizeof(url), "/file/download/%d", fid);
    cJSON_AddStringToObject(obj, "url", url);
    cJSON_AddStringToObject(obj, "delete_pin", delete_pin);
    send_json_response(res, obj, CWIST_HTTP_OK);
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
                if (cJSON_IsString(item) && is_safe_segment(item->valuestring)) cleanup_upload_session(item->valuestring);
            }
        }
        if (arr) cJSON_Delete(arr);
    }
    cwist_query_map_destroy(kv);
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddBoolToObject(obj, "ok", true);
    send_json_response(res, obj, CWIST_HTTP_OK);
}

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
    const char *path = json_string(file, "file_path", "");
    const char *filename = json_string(file, "filename", "download");
    const char *mime = json_string(file, "mime_type", "application/octet-stream");
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 0) {
        cJSON_Delete(file);
        send_json_response(res, session_error_json("download not found"), CWIST_HTTP_NOT_FOUND);
        return;
    }
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
    if (!init_download_session(filename, mime, path, (long long)st.st_size, score, &obj)) {
        cJSON_Delete(file);
        send_json_response(res, session_error_json("download handshake failed"), CWIST_HTTP_INTERNAL_ERROR);
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
    cJSON *meta = load_download_session(session_id);
    if (!meta) {
        send_json_response(res, session_error_json("download session not found"), CWIST_HTTP_NOT_FOUND);
        return;
    }
    if (!secure_str_eq(session_token, json_string(meta, "session_token", ""))) {
        cJSON_Delete(meta);
        send_json_response(res, session_error_json("download chunk rejected"), CWIST_HTTP_FORBIDDEN);
        return;
    }
    int chunk_index = atoi(chunk_str);
    int chunk_count = json_int(meta, "chunk_count", 0);
    int chunk_size = json_int(meta, "chunk_size", TASFA_CHUNK_SIZE);
    int span = atoi(cwist_query_map_get(req->query_params, "span") ? cwist_query_map_get(req->query_params, "span") : "1");
    if (span < 1) span = 1;
    if (span > 64) span = 64;
    if (chunk_index < 0 || chunk_index >= chunk_count) {
        cJSON_Delete(meta);
        send_json_response(res, session_error_json("download chunk rejected"), CWIST_HTTP_FORBIDDEN);
        return;
    }
    if (chunk_index + span > chunk_count) span = chunk_count - chunk_index;
    long long offset = (long long)chunk_index * (long long)chunk_size;
    long long end = (long long)(chunk_index + span) * (long long)chunk_size;
    long long total_size = json_long_long(meta, "total_size", 0);
    if (end > total_size) end = total_size;
    size_t amount = (size_t)(end - offset);
    bool ok = send_file_slice_response(res, json_string(meta, "storage_path", ""),
                                       json_string(meta, "mime_type", "application/octet-stream"), offset, amount);
    cJSON_Delete(meta);
    if (!ok) {
        send_json_response(res, session_error_json("download not found"), CWIST_HTTP_NOT_FOUND);
    }
}

void handler_asset_tasfa_handshake(cwist_http_request *req, cwist_http_response *res) {
    char path[PATH_MAX];
    char filename[512];
    const char *mime = NULL;
    if (!resolve_asset_scope_path(cwist_query_map_get(req->path_params, "scope"),
                                  cwist_query_map_get(req->path_params, "filename"),
                                  path, sizeof(path), filename, sizeof(filename), &mime)) {
        send_json_response(res, session_error_json("asset not found"), CWIST_HTTP_NOT_FOUND);
        return;
    }
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 0) {
        send_json_response(res, session_error_json("asset not found"), CWIST_HTTP_NOT_FOUND);
        return;
    }
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
    if (!init_download_session(filename, mime, path, (long long)st.st_size, score, &obj)) {
        send_json_response(res, session_error_json("download handshake failed"), CWIST_HTTP_INTERNAL_ERROR);
        return;
    }
    send_json_response(res, obj, CWIST_HTTP_OK);
}

void handler_asset_tasfa_chunk(cwist_http_request *req, cwist_http_response *res) {
    handler_file_download_chunk(req, res);
}

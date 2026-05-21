#define _POSIX_C_SOURCE 200809L
#include "handlers_internal.h"
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <strings.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pthread.h>

#define TASFA_UPLOAD_DIR "data/tasfa/uploads"
#define TASFA_DOWNLOAD_DIR "data/tasfa/downloads"
#define TASFA_UPLOAD_CHUNK_SIZE_DEFAULT (16 * 1024 * 1024)
#define TASFA_UPLOAD_CHUNK_SIZE_MOBILE  (8 * 1024 * 1024)
#define TASFA_DOWNLOAD_CHUNK_SIZE_DEFAULT (32 * 1024 * 1024)
#define TASFA_DOWNLOAD_CHUNK_SIZE_MOBILE  (8 * 1024 * 1024)

#define TASFA_UPLOAD_TTL 86400
#define TASFA_DOWNLOAD_TTL 86400
#define TASFA_UPLOAD_DEFAULT_PARALLEL 6
#define TASFA_UPLOAD_MAX_PARALLEL 12
#define TASFA_DOWNLOAD_DEFAULT_PARALLEL 6
#define TASFA_DOWNLOAD_MAX_PARALLEL 24
#define TASFA_CLIENT_STRIPES 32
#define TASFA_CACHE_SLOTS 512

#define TASFA_MAX_CONCURRENT_UPLOADS 4
#define TASFA_MAX_CONCURRENT_DOWNLOADS 8

/* Binary Metadata Structure for Fast Path */
typedef struct {
    char upload_token[64];
    char upload_secret[64];
    unsigned char stream_key[32];
    unsigned char stream_iv_seed[12];
    int chunk_count;
    int chunk_size;
    long long total_size;
    char filename[256];
    int post_id;
    int uid;
    char temp_path[PATH_MAX];
} tasfa_meta_bin_t;

typedef struct {
    char key[33];
    uint8_t valid;
    uint8_t type;
    time_t expires;
    union {
        tasfa_meta_bin_t mbin;
        cJSON *json;
    } data;
} cache_slot_t;

static cache_slot_t g_tasfa_cache[TASFA_CACHE_SLOTS];
static pthread_mutex_t g_tasfa_cache_mtx = PTHREAD_MUTEX_INITIALIZER;

/* --- Queue / Backpressure --- */

typedef struct {
    char id[64];
    time_t ts;
} queue_slot_t;

static queue_slot_t g_q_uploads[TASFA_MAX_CONCURRENT_UPLOADS];
static queue_slot_t g_q_downloads[TASFA_MAX_CONCURRENT_DOWNLOADS];
static pthread_mutex_t g_tasfa_queue_mtx = PTHREAD_MUTEX_INITIALIZER;

static bool is_mobile_request(cwist_http_request *req) {
    const char *ua = cwist_http_header_get(req->headers, "User-Agent");
    if (!ua) return false;
    const char *p = ua;
    while (*p) {
        if (strncasecmp(p, "Mobile", 6) == 0 || strncasecmp(p, "Android", 7) == 0 ||
            strncasecmp(p, "iPhone", 6) == 0 || strncasecmp(p, "iPad", 4) == 0 ||
            strncasecmp(p, "Mobi", 4) == 0) {
            return true;
        }
        p++;
    }
    return false;
}

static int choose_chunk_size_upload(bool mobile) {
    return mobile ? TASFA_UPLOAD_CHUNK_SIZE_MOBILE : TASFA_UPLOAD_CHUNK_SIZE_DEFAULT;
}

static int choose_chunk_size_download(bool mobile) {
    return mobile ? TASFA_DOWNLOAD_CHUNK_SIZE_MOBILE : TASFA_DOWNLOAD_CHUNK_SIZE_DEFAULT;
}

static bool tasfa_queue_try_enter(queue_slot_t *slots, int max_slots, const char *id) {
    pthread_mutex_lock(&g_tasfa_queue_mtx);
    int empty = -1;
    for (int i = 0; i < max_slots; i++) {
        if (slots[i].id[0] == '\0') { empty = i; break; }
    }
    if (empty < 0) { pthread_mutex_unlock(&g_tasfa_queue_mtx); return false; }
    snprintf(slots[empty].id, sizeof(slots[empty].id), "%s", id ? id : "");
    slots[empty].ts = time(NULL);
    pthread_mutex_unlock(&g_tasfa_queue_mtx);
    return true;
}

static void tasfa_queue_leave(queue_slot_t *slots, int max_slots, const char *id) {
    if (!id || !id[0]) return;
    pthread_mutex_lock(&g_tasfa_queue_mtx);
    for (int i = 0; i < max_slots; i++) {
        if (strcmp(slots[i].id, id) == 0) {
            slots[i].id[0] = '\0';
            slots[i].ts = 0;
            break;
        }
    }
    pthread_mutex_unlock(&g_tasfa_queue_mtx);
}

static void tasfa_queue_sweep(queue_slot_t *slots, int max_slots, int ttl) {
    time_t now = time(NULL);
    pthread_mutex_lock(&g_tasfa_queue_mtx);
    for (int i = 0; i < max_slots; i++) {
        if (slots[i].id[0] && (now - slots[i].ts) > ttl) {
            slots[i].id[0] = '\0';
            slots[i].ts = 0;
        }
    }
    pthread_mutex_unlock(&g_tasfa_queue_mtx);
}

static void add_keepalive_headers(cwist_http_response *res) {
    cwist_http_header_add(&res->headers, "Connection", "keep-alive");
    cwist_http_header_add(&res->headers, "Keep-Alive", "timeout=300, max=1000");
}

static void send_queued_json(cwist_http_response *res, const char *msg, int retry_after) {
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddBoolToObject(obj, "ok", false);
    cJSON_AddBoolToObject(obj, "queued", true);
    cJSON_AddStringToObject(obj, "error", msg ? msg : "server busy");
    cJSON_AddNumberToObject(obj, "retry_after", retry_after > 0 ? retry_after : 3);
    res->status_code = (cwist_http_status_t)429;
    cwist_http_header_add(&res->headers, "Content-Type", "application/json");
    cwist_http_header_add(&res->headers, "Retry-After", retry_after > 0 ? "3" : "3");
    add_keepalive_headers(res);
    char *json = cJSON_PrintUnformatted(obj);
    cwist_sstring_assign(res->body, json ? json : "{}");
    if (json) free(json);
    cJSON_Delete(obj);
}

/* --- Utility Functions --- */

static void send_json_response(cwist_http_response *res, cJSON *obj, int status_code) {
    char *json = cJSON_PrintUnformatted(obj);
    res->status_code = (cwist_http_status_t)status_code;
    cwist_http_header_add(&res->headers, "Content-Type", "application/json");
    add_keepalive_headers(res);
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

static char *hex_encode_alloc(const unsigned char *data, size_t len) {
    if (!data || !len) return NULL;
    char *out = (char *)cwist_alloc((len * 2) + 1);
    if (!out) return NULL;
    for (size_t i = 0; i < len; i++) snprintf(out + (i * 2), 3, "%02x", data[i]);
    out[len * 2] = '\0';
    return out;
}

static void derive_stream_iv(unsigned char out[12], const unsigned char seed[12], int chunk_index) {
    memcpy(out, seed, 12);
    out[8] ^= (unsigned char)((chunk_index >> 24) & 0xff);
    out[9] ^= (unsigned char)((chunk_index >> 16) & 0xff);
    out[10] ^= (unsigned char)((chunk_index >> 8) & 0xff);
    out[11] ^= (unsigned char)(chunk_index & 0xff);
}

static int build_stream_aad(unsigned char *out, size_t out_len, const char *upload_id, int chunk_index) {
    if (!out || !out_len) return -1;
    return snprintf((char *)out, out_len, "%s:%d", upload_id ? upload_id : "", chunk_index);
}

static bool decrypt_stream_block(const unsigned char *key, const unsigned char *iv_seed, int chunk_index,
                                 const char *upload_id,
                                 const unsigned char *ciphertext, size_t ciphertext_len,
                                 unsigned char *plaintext, size_t plaintext_len) {
    if (!key || !iv_seed || !ciphertext || ciphertext_len < 16 || !plaintext) return false;
    if (plaintext_len + 16 != ciphertext_len) return false;
    unsigned char iv[12];
    unsigned char aad[512];
    int aad_len = build_stream_aad(aad, sizeof(aad), upload_id, chunk_index);
    if (aad_len <= 0) return false;
    derive_stream_iv(iv, iv_seed, chunk_index);
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return false;
    bool ok = false;
    int out_len = 0;
    int final_len = 0;
    const unsigned char *tag = ciphertext + plaintext_len;
    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1) goto cleanup;
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, sizeof(iv), NULL) != 1) goto cleanup;
    if (EVP_DecryptInit_ex(ctx, NULL, NULL, key, iv) != 1) goto cleanup;
    if (EVP_DecryptUpdate(ctx, NULL, &out_len, aad, aad_len) != 1) goto cleanup;
    if (EVP_DecryptUpdate(ctx, plaintext, &out_len, ciphertext, (int)plaintext_len) != 1) goto cleanup;
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, (void *)tag) != 1) goto cleanup;
    if (EVP_DecryptFinal_ex(ctx, plaintext + out_len, &final_len) != 1) goto cleanup;
    ok = (size_t)(out_len + final_len) == plaintext_len;
cleanup:
    EVP_CIPHER_CTX_free(ctx);
    return ok;
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

static void choose_upload_window(int score, int *initial_parallel, int *max_parallel, int *pacing_ms) {
    int initial_value = 6;
    int max_value = TASFA_UPLOAD_MAX_PARALLEL;
    int pace = 0;
    if (score >= 85) { initial_value = 8; max_value = 12; }
    else if (score >= 65) { initial_value = 6; max_value = 10; }
    else if (score >= 45) { initial_value = 6; max_value = 8; pace = 15; }
    else { initial_value = 4; max_value = 6; pace = 35; }
    if (initial_parallel) *initial_parallel = initial_value;
    if (max_parallel) *max_parallel = max_value;
    if (pacing_ms) *pacing_ms = pace;
}

static void choose_download_profile(int score, int *initial_parallel, int *max_parallel, int *pacing_ms, int *coalesce_chunks) {
    int initial_value = 112, max_value = 256, pace = 0, coalesce = 64;
    if (score < 45) { initial_value = 48; max_value = 96; coalesce = 16; pace = 30; }
    else if (score < 65) { initial_value = 64; max_value = 160; coalesce = 32; pace = 10; }
    else if (score < 85) { initial_value = 96; max_value = 224; coalesce = 48; pace = 0; }
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
    if (flock(fd, LOCK_EX) != 0) { close(fd); return -1; }
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

static bool save_upload_session_state_bin(const char *upload_id, int chunk_count, const char *bitmap) {
    char path[PATH_MAX];
    upload_session_state_bin_path(path, sizeof(path), upload_id);
    if (!bitmap) return false;
    return file_write(path, bitmap, (size_t)chunk_count);
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

static bool is_chunk_already_received(const char *upload_id, int chunk_index) {
    char path[PATH_MAX];
    upload_session_state_bin_path(path, sizeof(path), upload_id);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    unsigned char val = '0';
    bool ok = (pread(fd, &val, 1, (off_t)chunk_index) == 1);
    close(fd);
    return ok && val == '1';
}

/* --- Sticky Round-Robin Worker Scheduler --- */
/* Limits concurrent chunk processing to CPU count while keeping same-upload-id
   chunks on the same worker for cache locality and ordering. */

#define TASFA_MAX_WORKERS 8

typedef struct tasfa_job {
    void (*func)(void *);
    void *arg;
    volatile bool done;
    pthread_mutex_t mtx;
    pthread_cond_t cond;
    struct tasfa_job *next;
} tasfa_job_t;

typedef struct {
    pthread_t tid;
    pthread_mutex_t queue_lock;
    pthread_cond_t queue_cond;
    tasfa_job_t *head;
    tasfa_job_t *tail;
    bool shutdown;
} tasfa_worker_t;

static tasfa_worker_t g_workers[TASFA_MAX_WORKERS];
static int g_worker_count = 0;
static pthread_once_t g_scheduler_once = PTHREAD_ONCE_INIT;

static unsigned int hash_upload_id(const char *id) {
    unsigned int h = 5381;
    int c;
    while ((c = *id++)) h = ((h << 5) + h) + c;
    return h;
}

static void *tasfa_worker_loop(void *arg) {
    tasfa_worker_t *w = (tasfa_worker_t *)arg;
    while (1) {
        pthread_mutex_lock(&w->queue_lock);
        while (!w->shutdown && !w->head) {
            pthread_cond_wait(&w->queue_cond, &w->queue_lock);
        }
        if (w->shutdown && !w->head) {
            pthread_mutex_unlock(&w->queue_lock);
            break;
        }
        tasfa_job_t *job = w->head;
        w->head = job->next;
        if (!w->head) w->tail = NULL;
        pthread_mutex_unlock(&w->queue_lock);

        job->func(job->arg);

        pthread_mutex_lock(&job->mtx);
        job->done = true;
        pthread_cond_signal(&job->cond);
        pthread_mutex_unlock(&job->mtx);
    }
    return NULL;
}

static void tasfa_scheduler_init_impl(void) {
    int n = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 2) n = 2;
    if (n > TASFA_MAX_WORKERS) n = TASFA_MAX_WORKERS;
    g_worker_count = n;
    for (int i = 0; i < n; i++) {
        pthread_mutex_init(&g_workers[i].queue_lock, NULL);
        pthread_cond_init(&g_workers[i].queue_cond, NULL);
        g_workers[i].head = NULL;
        g_workers[i].tail = NULL;
        g_workers[i].shutdown = false;
        pthread_create(&g_workers[i].tid, NULL, tasfa_worker_loop, &g_workers[i]);
    }
}

static void tasfa_scheduler_ensure_init(void) {
    pthread_once(&g_scheduler_once, tasfa_scheduler_init_impl);
}

static void tasfa_scheduler_submit(const char *upload_id, void (*func)(void *), void *arg, tasfa_job_t *job) {
    tasfa_scheduler_ensure_init();
    unsigned int h = hash_upload_id(upload_id);
    int idx = (int)(h % (unsigned int)g_worker_count);
    tasfa_worker_t *w = &g_workers[idx];

    job->func = func;
    job->arg = arg;
    job->done = false;
    job->next = NULL;
    pthread_mutex_init(&job->mtx, NULL);
    pthread_cond_init(&job->cond, NULL);

    pthread_mutex_lock(&w->queue_lock);
    if (w->tail) {
        w->tail->next = job;
    } else {
        w->head = job;
    }
    w->tail = job;
    pthread_cond_signal(&w->queue_cond);
    pthread_mutex_unlock(&w->queue_lock);
}

static void tasfa_scheduler_wait(tasfa_job_t *job) {
    pthread_mutex_lock(&job->mtx);
    while (!job->done) {
        pthread_cond_wait(&job->cond, &job->mtx);
    }
    pthread_mutex_unlock(&job->mtx);
    pthread_mutex_destroy(&job->mtx);
    pthread_cond_destroy(&job->cond);
}

/* --- Session Management --- */

static void cache_invalidate(const char *key);

static bool save_upload_session_meta_bin(const char *upload_id, tasfa_meta_bin_t *meta) {
    cache_invalidate(upload_id);
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

static unsigned int cache_hash(const char *str) {
    unsigned int h = 5381;
    int c;
    while ((c = *str++))
        h = ((h << 5) + h) + c;
    return h % TASFA_CACHE_SLOTS;
}

static void cache_clear_slot(cache_slot_t *slot) {
    if (!slot->valid) return;
    if (slot->type == 3 && slot->data.json) {
        cJSON_Delete(slot->data.json);
        slot->data.json = NULL;
    }
    slot->valid = 0;
    slot->key[0] = '\0';
}

static void cache_invalidate(const char *key) {
    unsigned int idx = cache_hash(key);
    pthread_mutex_lock(&g_tasfa_cache_mtx);
    for (int i = 0; i < TASFA_CACHE_SLOTS; i++) {
        int probe = (idx + i) % TASFA_CACHE_SLOTS;
        if (!g_tasfa_cache[probe].valid) break;
        if (strcmp(g_tasfa_cache[probe].key, key) == 0) {
            cache_clear_slot(&g_tasfa_cache[probe]);
            break;
        }
    }
    pthread_mutex_unlock(&g_tasfa_cache_mtx);
}

static bool load_upload_session_meta_bin_cached(const char *upload_id, tasfa_meta_bin_t *out) {
    unsigned int idx = cache_hash(upload_id);
    pthread_mutex_lock(&g_tasfa_cache_mtx);
    for (int i = 0; i < TASFA_CACHE_SLOTS; i++) {
        int probe = (idx + i) % TASFA_CACHE_SLOTS;
        if (!g_tasfa_cache[probe].valid) break;
        if (g_tasfa_cache[probe].type == 1 && strcmp(g_tasfa_cache[probe].key, upload_id) == 0) {
            if (time(NULL) <= g_tasfa_cache[probe].expires) {
                memcpy(out, &g_tasfa_cache[probe].data.mbin, sizeof(tasfa_meta_bin_t));
                pthread_mutex_unlock(&g_tasfa_cache_mtx);
                return true;
            }
            cache_clear_slot(&g_tasfa_cache[probe]);
            break;
        }
    }
    pthread_mutex_unlock(&g_tasfa_cache_mtx);
    if (load_upload_session_meta_bin(upload_id, out)) {
        pthread_mutex_lock(&g_tasfa_cache_mtx);
        for (int i = 0; i < TASFA_CACHE_SLOTS; i++) {
            int probe = (idx + i) % TASFA_CACHE_SLOTS;
            if (!g_tasfa_cache[probe].valid) {
                g_tasfa_cache[probe].valid = 1;
                g_tasfa_cache[probe].type = 1;
                strncpy(g_tasfa_cache[probe].key, upload_id, sizeof(g_tasfa_cache[probe].key)-1);
                g_tasfa_cache[probe].key[sizeof(g_tasfa_cache[probe].key)-1] = '\0';
                memcpy(&g_tasfa_cache[probe].data.mbin, out, sizeof(tasfa_meta_bin_t));
                g_tasfa_cache[probe].expires = time(NULL) + TASFA_UPLOAD_TTL;
                break;
            }
        }
        pthread_mutex_unlock(&g_tasfa_cache_mtx);
        return true;
    }
    return false;
}

static cJSON *load_download_session_cached(const char *session_id) {
    unsigned int idx = cache_hash(session_id);
    pthread_mutex_lock(&g_tasfa_cache_mtx);
    for (int i = 0; i < TASFA_CACHE_SLOTS; i++) {
        int probe = (idx + i) % TASFA_CACHE_SLOTS;
        if (!g_tasfa_cache[probe].valid) break;
        if (g_tasfa_cache[probe].type == 3 && strcmp(g_tasfa_cache[probe].key, session_id) == 0) {
            if (time(NULL) <= g_tasfa_cache[probe].expires) {
                cJSON *copy = cJSON_Duplicate(g_tasfa_cache[probe].data.json, 1);
                pthread_mutex_unlock(&g_tasfa_cache_mtx);
                return copy;
            }
            cache_clear_slot(&g_tasfa_cache[probe]);
            break;
        }
    }
    pthread_mutex_unlock(&g_tasfa_cache_mtx);
    cJSON *meta = load_download_session(session_id);
    if (meta) {
        pthread_mutex_lock(&g_tasfa_cache_mtx);
        for (int i = 0; i < TASFA_CACHE_SLOTS; i++) {
            int probe = (idx + i) % TASFA_CACHE_SLOTS;
            if (!g_tasfa_cache[probe].valid) {
                g_tasfa_cache[probe].valid = 1;
                g_tasfa_cache[probe].type = 3;
                strncpy(g_tasfa_cache[probe].key, session_id, sizeof(g_tasfa_cache[probe].key)-1);
                g_tasfa_cache[probe].key[sizeof(g_tasfa_cache[probe].key)-1] = '\0';
                g_tasfa_cache[probe].data.json = cJSON_Duplicate(meta, 1);
                g_tasfa_cache[probe].expires = time(NULL) + TASFA_DOWNLOAD_TTL;
                break;
            }
        }
        pthread_mutex_unlock(&g_tasfa_cache_mtx);
    }
    return meta;
}

static __thread unsigned char *g_tasfa_decrypt_buf = NULL;
static __thread size_t g_tasfa_decrypt_buf_size = 0;

static unsigned char *ensure_decrypt_buf(size_t need) {
    if (g_tasfa_decrypt_buf_size >= need) return g_tasfa_decrypt_buf;
    if (g_tasfa_decrypt_buf) free(g_tasfa_decrypt_buf);
    g_tasfa_decrypt_buf = (unsigned char *)malloc(need);
    g_tasfa_decrypt_buf_size = need;
    return g_tasfa_decrypt_buf;
}

static __thread char *g_tasfa_read_buf = NULL;
static __thread size_t g_tasfa_read_buf_size = 0;

static char *ensure_read_buf(size_t need) {
    if (g_tasfa_read_buf_size >= need) return g_tasfa_read_buf;
    if (g_tasfa_read_buf) free(g_tasfa_read_buf);
    g_tasfa_read_buf = (char *)malloc(need);
    g_tasfa_read_buf_size = need;
    return g_tasfa_read_buf;
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
    cache_invalidate(upload_id);
    char dir_path[PATH_MAX];
    upload_session_dir(dir_path, sizeof(dir_path), upload_id);
    cleanup_dir_tree(dir_path);
}

static void cleanup_download_session(const char *session_id) {
    char dir_path[PATH_MAX];
    download_session_dir(dir_path, sizeof(dir_path), session_id);
    cleanup_dir_tree(dir_path);
}

/* --- Response Helpers --- */

static cJSON *build_upload_status_json(cJSON *meta, const char *upload_id) {
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
    return obj;
}

static bool send_file_slice_response(cwist_http_response *res, const char *path, const char *mime, long long offset, size_t amount,
                                       int chunk_index, int chunk_count) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    char *buf = ensure_read_buf(amount ? amount : 1);
    if (!buf) { close(fd); return false; }
    size_t total = 0;
    while (total < amount) {
        ssize_t rc = pread(fd, buf + total, amount - total, (off_t)(offset + (long long)total));
        if (rc <= 0) break;
        total += (size_t)rc;
    }
    close(fd);
    if (total != amount) return false;
    char len_buf[32];
    snprintf(len_buf, sizeof(len_buf), "%zu", amount);
    cwist_http_header_add(&res->headers, "Content-Type", mime ? mime : "application/octet-stream");
    cwist_http_header_add(&res->headers, "Content-Length", len_buf);
    cwist_http_header_add(&res->headers, "Cache-Control", "public, max-age=86400");
    add_keepalive_headers(res);
    if (chunk_index >= 0) {
        char idx_buf[32], cnt_buf[32];
        snprintf(idx_buf, sizeof(idx_buf), "%d", chunk_index);
        snprintf(cnt_buf, sizeof(cnt_buf), "%d", chunk_count);
        cwist_http_header_add(&res->headers, "X-TASFA-Chunk-Index", idx_buf);
        cwist_http_header_add(&res->headers, "X-TASFA-Chunk-Count", cnt_buf);
    }
    cwist_sstring_assign(res->body, "");
    cwist_sstring_append_len(res->body, buf, amount);
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
                                  int score, int chunk_size, cJSON **out) {
    if (!ensure_tasfa_roots()) return false;
    if (chunk_size <= 0) chunk_size = TASFA_DOWNLOAD_CHUNK_SIZE_DEFAULT;
    char session_id[33], session_token[49];
    if (!random_hex(session_id, 16) || !random_hex(session_token, 24)) return false;
    char dir_path[PATH_MAX];
    download_session_dir(dir_path, sizeof(dir_path), session_id);
    if (!dir_ensure(dir_path)) return false;
    int initial_parallel = 0, max_parallel = 0, pacing_ms = 0, coalesce = 0;
    choose_download_profile(score, &initial_parallel, &max_parallel, &pacing_ms, &coalesce);
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
    char expires_at[32];
    snprintf(expires_at, sizeof(expires_at), "%lld", (long long)(time(NULL) + TASFA_DOWNLOAD_TTL));
    cJSON_AddStringToObject(meta, "expires_at", expires_at);
    if (!save_download_session(session_id, meta)) { cJSON_Delete(meta); cleanup_download_session(session_id); return false; }
    if (out) *out = meta;
    else cJSON_Delete(meta);
    return true;
}

static bool pwrite_all(int fd, const void *buf, size_t len, off_t offset) {
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

static bool sha256_file(const char *path, unsigned char out[32]) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    SHA256_CTX ctx;
    SHA256_Init(&ctx);
    unsigned char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) SHA256_Update(&ctx, buf, n);
    fclose(f);
    SHA256_Final(out, &ctx);
    return true;
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
    int chunk_count = atoi(cwist_query_map_get(kv, "chunk_count") ? cwist_query_map_get(kv, "chunk_count") : "0");
    long long total_size = atoll(cwist_query_map_get(kv, "total_size") ? cwist_query_map_get(kv, "total_size") : "0");
    if (chunk_count <= 0 && total_size > 0) chunk_count = (int)((total_size + TASFA_UPLOAD_CHUNK_SIZE_DEFAULT - 1) / TASFA_UPLOAD_CHUNK_SIZE_DEFAULT);
    if (chunk_count < 1) chunk_count = 1;
    int post_id = atoi(cwist_query_map_get(kv, "post_id") ? cwist_query_map_get(kv, "post_id") : "0");
    if (!filename || !is_safe_filename_simple(filename) || chunk_count <= 0 || total_size <= 0 || !ensure_tasfa_roots()) {
        cwist_query_map_destroy(kv);
        send_json_response(res, session_error_json("invalid upload init payload"), CWIST_HTTP_BAD_REQUEST);
        return;
    }
    bool mobile = is_mobile_request(req);
    int chunk_size = choose_chunk_size_upload(mobile);
    tasfa_queue_sweep(g_q_uploads, TASFA_MAX_CONCURRENT_UPLOADS, 3600);
    char upload_id[33];
    if (!random_hex(upload_id, 16)) {
        cwist_query_map_destroy(kv);
        send_json_response(res, session_error_json("upload init failed"), CWIST_HTTP_INTERNAL_ERROR);
        return;
    }
    if (!tasfa_queue_try_enter(g_q_uploads, TASFA_MAX_CONCURRENT_UPLOADS, upload_id)) {
        cwist_query_map_destroy(kv);
        send_queued_json(res, "too many concurrent uploads", 5);
        return;
    }
    char dir_path[PATH_MAX], temp_path[PATH_MAX];
    upload_session_dir(dir_path, sizeof(dir_path), upload_id);
    upload_session_temp_path(temp_path, sizeof(temp_path), upload_id);
    if (!dir_ensure(dir_path) || !ensure_preallocated_file(temp_path, total_size)) {
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
        cwist_query_map_destroy(kv);
        send_json_response(res, session_error_json("upload init failed"), CWIST_HTTP_INTERNAL_ERROR);
        return;
    }

    /* Generate random modulus_M for HTP scalar math */
    uint64_t modulus_M = 0;
    while (modulus_M == 0) {
        if (RAND_bytes((unsigned char *)&modulus_M, sizeof(modulus_M)) != 1) {
            modulus_M = ((uint64_t)time(NULL) << 32) ^ (uint64_t)getpid();
            if (modulus_M == 0) modulus_M = 1;
        }
    }
    int group_count = (chunk_count + 5) / 6;

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
    cJSON_AddNumberToObject(meta, "current_parallel_chunks", 6);
    cJSON_AddNumberToObject(meta, "max_parallel_chunks", 6);
    cJSON_AddNumberToObject(meta, "dispatch_pacing_ms", 0);
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

    if (!save_upload_session(upload_id, meta) || !save_upload_session_state(upload_id, meta) ||
        !save_upload_session_state_bin(upload_id, chunk_count, bitmap) ||
        !save_upload_session_meta_bin(upload_id, &mbin)) {
        cwist_free(bitmap);
        cJSON_Delete(meta);
        cleanup_upload_session(upload_id);
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
    int initial_parallel = 0, max_parallel = 0, pacing_ms = 0;
    choose_upload_window(score, &initial_parallel, &max_parallel, &pacing_ms);
    int current_parallel = json_int(meta, "current_parallel_chunks", initial_parallel);
    int current_max = json_int(meta, "max_parallel_chunks", max_parallel);
    if (max_parallel < current_max) max_parallel = current_max;
    if (suggested > 0) initial_parallel = clamp_int(suggested, 2, max_parallel);
    if (initial_parallel < current_parallel) initial_parallel = current_parallel;
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

typedef struct {
    const char *upload_id;
    int chunk_index;
    const char *temp_path;
    bool encrypted_stream;
    const unsigned char *stream_key;
    const unsigned char *stream_iv_seed;
    const void *body_data;
    size_t body_size;
    long long offset;
    long long expected_size;
    int chunk_count;
    const char *hash_tag_hex;
    const char *magic_scalar_str;
    /* outputs */
    bool stored;
    bool state_ok;
} upload_work_t;

static void upload_work_func(void *arg) {
    upload_work_t *w = (upload_work_t *)arg;

    int fd = open(w->temp_path, O_WRONLY);
    if (fd >= 0) {
        if (w->encrypted_stream) {
            unsigned char *plaintext = ensure_decrypt_buf((size_t)w->expected_size);
            if (plaintext && w->body_size == (size_t)w->expected_size + 16 &&
                decrypt_stream_block(w->stream_key, w->stream_iv_seed, w->chunk_index, w->upload_id,
                                     (const unsigned char *)w->body_data, w->body_size,
                                     plaintext, (size_t)w->expected_size)) {
                w->stored = pwrite_all(fd, plaintext, (size_t)w->expected_size, (off_t)w->offset);
            }
        } else if ((long long)w->body_size == w->expected_size) {
            w->stored = pwrite_all(fd, w->body_data, w->body_size, (off_t)w->offset);
        }
        close(fd);
    }

    if (!w->stored) return;

    w->state_ok = mark_chunk_received_in_session_state(w->upload_id, w->chunk_index);
    if (!w->state_ok) return;

    /* Persist HTP per-vertex metadata */
    cJSON *meta = load_upload_session(w->upload_id);
    if (meta) {
        cJSON *hash_tags = cJSON_GetObjectItem(meta, "hash_tags");
        cJSON *magic_scalars = cJSON_GetObjectItem(meta, "magic_scalars");
        if (hash_tags && magic_scalars && w->chunk_index < cJSON_GetArraySize(hash_tags)) {
            /* Preserve existing HTP metadata when a fallback chunk arrives for an already-received vertex */
            if (w->hash_tag_hex) {
                cJSON *existing = cJSON_GetArrayItem(hash_tags, w->chunk_index);
                if (!existing || !existing->valuestring || existing->valuestring[0] == '\0') {
                    cJSON_ReplaceItemInArray(hash_tags, w->chunk_index, cJSON_CreateString(w->hash_tag_hex));
                }
            }
            if (w->magic_scalar_str) {
                cJSON *existing = cJSON_GetArrayItem(magic_scalars, w->chunk_index);
                if (!existing || existing->valuedouble == 0) {
                    cJSON_ReplaceItemInArray(magic_scalars, w->chunk_index, cJSON_CreateNumber(atoll(w->magic_scalar_str)));
                }
            }
            const char *bitmap = json_string(meta, "received_bitmap", "");
            cJSON_ReplaceItemInObject(meta, "received_chunks", cJSON_CreateNumber(bitmap_count_set(bitmap, w->chunk_count)));
            save_upload_session(w->upload_id, meta);
        }
        cJSON_Delete(meta);
    }
}

void handler_file_upload(cwist_http_request *req, cwist_http_response *res) {
    const char *upload_id = cwist_http_header_get(req->headers, "X-TASFA-Upload-ID");
    const char *upload_token = cwist_http_header_get(req->headers, "X-TASFA-Upload-Token");
    const char *stream_mode = cwist_http_header_get(req->headers, "X-TASFA-Stream-Mode");
    const char *chunk_index_str = cwist_http_header_get(req->headers, "X-TASFA-Chunk-Index");
    const char *hash_tag_hex = cwist_http_header_get(req->headers, "X-TASFA-Hash-Tag");
    const char *magic_scalar_str = cwist_http_header_get(req->headers, "X-TASFA-Magic-Scalar");
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
    if (!load_upload_session_meta_bin_cached(upload_id, &mbin)) {
        send_json_response(res, session_error_json("upload session not found"), CWIST_HTTP_NOT_FOUND);
        return;
    }
    if (!secure_str_eq(upload_token, mbin.upload_token)) {
        send_json_response(res, session_error_json("upload chunk rejected"), CWIST_HTTP_FORBIDDEN);
        return;
    }
    if (chunk_index >= mbin.chunk_count) {
        send_json_response(res, session_error_json("invalid chunk index"), CWIST_HTTP_BAD_REQUEST);
        return;
    }

    /* Defensive: drop duplicate/fallback chunks for already-received vertices to save I/O and prevent attacks */
    if (is_chunk_already_received(upload_id, chunk_index)) {
        res->status_code = CWIST_HTTP_NO_CONTENT;
        cwist_http_header_add(&res->headers, "X-TASFA-Accepted", "1");
        cwist_http_header_add(&res->headers, "X-TASFA-Chunk-Complete", "1");
        cwist_sstring_assign(res->body, "");
        return;
    }

    long long offset = (long long)chunk_index * (long long)mbin.chunk_size;
    long long expected_size = mbin.total_size - offset;
    if (expected_size > mbin.chunk_size) expected_size = mbin.chunk_size;
    if (expected_size <= 0) {
        send_json_response(res, session_error_json("invalid chunk bounds"), CWIST_HTTP_BAD_REQUEST);
        return;
    }

    bool encrypted_stream = stream_mode && strcmp(stream_mode, "aes-256-gcm") == 0;

    upload_work_t work = {0};
    work.upload_id = upload_id;
    work.chunk_index = chunk_index;
    work.temp_path = mbin.temp_path;
    work.encrypted_stream = encrypted_stream;
    work.stream_key = mbin.stream_key;
    work.stream_iv_seed = mbin.stream_iv_seed;
    work.body_data = req->body->data;
    work.body_size = req->body->size;
    work.offset = offset;
    work.expected_size = expected_size;
    work.chunk_count = mbin.chunk_count;
    work.hash_tag_hex = hash_tag_hex;
    work.magic_scalar_str = magic_scalar_str;

    tasfa_job_t job;
    tasfa_scheduler_submit(upload_id, upload_work_func, &work, &job);
    tasfa_scheduler_wait(&job);

    if (!work.stored) {
        send_json_response(res, session_error_json("chunk store failed"), CWIST_HTTP_INTERNAL_ERROR);
        return;
    }
    if (!work.state_ok) {
        send_json_response(res, session_error_json("chunk state update failed"), CWIST_HTTP_INTERNAL_ERROR);
        return;
    }

    res->status_code = CWIST_HTTP_NO_CONTENT;
    cwist_http_header_add(&res->headers, "X-TASFA-Accepted", "1");
    cwist_http_header_add(&res->headers, "X-TASFA-Chunk-Complete", "1");
    add_keepalive_headers(res);
    cwist_sstring_assign(res->body, "");
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

    /* HTP line-sum validation (skip if no magic scalars were submitted) */
    uint64_t modulus_M = (uint64_t)json_long_long(meta, "modulus_M", 0);
    if (modulus_M == 0) modulus_M = 1;
    cJSON *magic_scalars = cJSON_GetObjectItem(meta, "magic_scalars");
    if (magic_scalars) {
        int group_count = (chunk_count + 5) / 6;
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
                uint64_t L1 = (m[0] + m[1] + m[2]) % modulus_M;
                uint64_t L2 = (m[2] + m[3] + m[4]) % modulus_M;
                uint64_t L3 = (m[4] + m[5] + m[0]) % modulus_M;
                if (L1 != L2 || L2 != L3) {
                    cJSON_Delete(meta);
                    close_upload_session_lock(lock_fd);
                    cwist_query_map_destroy(kv);
                    send_json_response(res, session_error_json("htp line sum mismatch"), CWIST_HTTP_BAD_REQUEST);
                    return;
                }
            }
        }
    }

    /* Post-quantum checksum (SHA-256 of final file) */
    unsigned char checksum[32];
    if (!sha256_file(temp_path, checksum)) {
        cJSON_Delete(meta);
        close_upload_session_lock(lock_fd);
        cwist_query_map_destroy(kv);
        send_json_response(res, session_error_json("checksum compute failed"), CWIST_HTTP_INTERNAL_ERROR);
        return;
    }

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
    char delete_pin[13], delete_pin_hash[512];
    delete_pin[0] = '\0';
    if (random_hex(delete_pin, 6) && auth_hash_password(delete_pin, delete_pin_hash, sizeof(delete_pin_hash))) {
        (void)db_file_set_delete_pin_hash(req->db, fid, delete_pin_hash);
    } else {
        delete_pin[0] = '\0';
    }
    cJSON_Delete(meta);
    close_upload_session_lock(lock_fd);
    cleanup_upload_session(upload_id);
    tasfa_queue_leave(g_q_uploads, TASFA_MAX_CONCURRENT_UPLOADS, upload_id);
    cwist_query_map_destroy(kv);
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddBoolToObject(obj, "ok", true);
    cJSON_AddNumberToObject(obj, "id", fid);
    cJSON_AddNumberToObject(obj, "fid", fid);
    cJSON_AddStringToObject(obj, "filename", filename);
    char url[512], checksum_hex[65];
    snprintf(url, sizeof(url), "/file/download/%d", fid);
    cJSON_AddStringToObject(obj, "url", url);
    cJSON_AddStringToObject(obj, "blob_url", url);
    cJSON_AddStringToObject(obj, "delete_pin", delete_pin);
    for (int i = 0; i < 32; i++) snprintf(checksum_hex + (i * 2), 3, "%02x", checksum[i]);
    checksum_hex[64] = '\0';
    cJSON_AddStringToObject(obj, "checksum", checksum_hex);
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
                if (cJSON_IsString(item) && is_safe_segment(item->valuestring)) {
                    cleanup_upload_session(item->valuestring);
                    tasfa_queue_leave(g_q_uploads, TASFA_MAX_CONCURRENT_UPLOADS, item->valuestring);
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
    bool mobile = is_mobile_request(req);
    int chunk_size = choose_chunk_size_download(mobile);
    tasfa_queue_sweep(g_q_downloads, TASFA_MAX_CONCURRENT_DOWNLOADS, 3600);
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
    if (!init_download_session(filename, mime, path, (long long)st.st_size, score, chunk_size, &obj)) {
        cJSON_Delete(file);
        send_json_response(res, session_error_json("download handshake failed"), CWIST_HTTP_INTERNAL_ERROR);
        return;
    }
    const char *session_id = json_string(obj, "session_id", "");
    if (!tasfa_queue_try_enter(g_q_downloads, TASFA_MAX_CONCURRENT_DOWNLOADS, session_id)) {
        cleanup_download_session(session_id);
        cJSON_Delete(obj);
        cJSON_Delete(file);
        send_queued_json(res, "too many concurrent downloads", 3);
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
    cJSON *meta = load_download_session_cached(session_id);
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
    int chunk_size = json_int(meta, "chunk_size", TASFA_DOWNLOAD_CHUNK_SIZE_DEFAULT);
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
                                       json_string(meta, "mime_type", "application/octet-stream"), offset, amount,
                                       chunk_index, chunk_count);
    cJSON_Delete(meta);
    if (!ok) {
        send_json_response(res, session_error_json("download not found"), CWIST_HTTP_NOT_FOUND);
    }
}

void handler_asset_tasfa_handshake(cwist_http_request *req, cwist_http_response *res) {
    char path[PATH_MAX], filename[512];
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
    bool mobile = is_mobile_request(req);
    int chunk_size = choose_chunk_size_download(mobile);
    tasfa_queue_sweep(g_q_downloads, TASFA_MAX_CONCURRENT_DOWNLOADS, 3600);
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
    if (!init_download_session(filename, mime, path, (long long)st.st_size, score, chunk_size, &obj)) {
        send_json_response(res, session_error_json("download handshake failed"), CWIST_HTTP_INTERNAL_ERROR);
        return;
    }
    const char *session_id = json_string(obj, "session_id", "");
    if (!tasfa_queue_try_enter(g_q_downloads, TASFA_MAX_CONCURRENT_DOWNLOADS, session_id)) {
        cleanup_download_session(session_id);
        cJSON_Delete(obj);
        send_queued_json(res, "too many concurrent downloads", 3);
        return;
    }
    send_json_response(res, obj, CWIST_HTTP_OK);
}

void handler_asset_tasfa_chunk(cwist_http_request *req, cwist_http_response *res) {
    handler_file_download_chunk(req, res);
}

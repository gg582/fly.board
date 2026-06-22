#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "handlers_internal.h"
#include "../utils/media_preview.h"
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <ttak/async/task.h>
#include <ttak/thread/pool.h>
#include <ttak/timing/timing.h>
#include <strings.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pthread.h>
#include <zlib.h>

#define TASFA_UPLOAD_DIR "data/tasfa/uploads"
#define TASFA_DOWNLOAD_DIR "data/tasfa/downloads"
#define TASFA_UPLOAD_CHUNK_SIZE_DEFAULT (16 * 1024 * 1024)
#define TASFA_UPLOAD_CHUNK_SIZE_MOBILE  (8 * 1024 * 1024)
#define TASFA_UPLOAD_CHUNK_SIZE_MIN     (8 * 1024 * 1024)
#define TASFA_UPLOAD_CHUNK_SIZE_MAX     (32 * 1024 * 1024)
#define TASFA_UPLOAD_CHUNK_SIZE_MOBILE_MAX (16 * 1024 * 1024)
#define TASFA_DOWNLOAD_CHUNK_SIZE_DEFAULT (8 * 1024 * 1024)
#define TASFA_DOWNLOAD_CHUNK_SIZE_MOBILE  (6 * 1024 * 1024)
#define TASFA_DOWNLOAD_CHUNK_SIZE_MIN     (2 * 1024 * 1024)
#define TASFA_DOWNLOAD_CHUNK_SIZE_MAX     (32 * 1024 * 1024)
#define TASFA_DOWNLOAD_CHUNK_SIZE_FAST_MAX (48 * 1024 * 1024)
#define TASFA_DOWNLOAD_CHUNK_SIZE_ULTRA_MAX (64 * 1024 * 1024)
#define TASFA_DOWNLOAD_RESPONSE_BYTES_MAX (128 * 1024 * 1024)
#define TASFA_GZIP_MIN_GAIN_BYTES         1024

#define TASFA_UPLOAD_TTL 86400
#define TASFA_DOWNLOAD_TTL 86400
#define TASFA_UPLOAD_DEFAULT_PARALLEL 16
#define TASFA_UPLOAD_MAX_PARALLEL 40
#define TASFA_DOWNLOAD_DEFAULT_PARALLEL 16
#define TASFA_DOWNLOAD_MAX_PARALLEL 48
#define TASFA_CLIENT_STRIPES 32
#define TASFA_CACHE_SLOTS 512

#define TASFA_MAX_CONCURRENT_UPLOADS 512
#define TASFA_MAX_CONCURRENT_DOWNLOADS 512
#define TASFA_FINALIZE_CACHE_SLOTS 128

#define HTP_TAG_LEN 129
#define HTP_RECORD_SIZE (HTP_TAG_LEN + 8 + 8)
#define HTP_MODULUS_STABLE 4294967291ULL

#define TASFA_MAX_MEDIA_CONCURRENCY 4
static int g_media_concurrency = 0;
static pthread_mutex_t g_media_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_media_cond = PTHREAD_COND_INITIALIZER;

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

typedef struct {
    char upload_id[33];
    char upload_token[49];
    bool active;
    bool done;
    int status_code;
    char *body;
    time_t expires;
} finalize_slot_t;

static finalize_slot_t g_finalize_slots[TASFA_FINALIZE_CACHE_SLOTS];
static pthread_mutex_t g_finalize_mtx = PTHREAD_MUTEX_INITIALIZER;

static long long tasfa_monotonic_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return ((long long)ts.tv_sec * 1000LL) + ((long long)ts.tv_nsec / 1000000LL);
}

bool is_mobile_request(cwist_http_request *req) {
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

static int normalize_chunk_size_hint(int requested, int fallback, int min_value, int max_value) {
    if (requested <= 0) return fallback;
    int quantum = 128 * 1024;
    requested = (requested / quantum) * quantum;
    if (requested < quantum) requested = quantum;
    if (requested < min_value) {
        while (requested < min_value && requested > 0) requested *= 2;
    }
    if (requested > max_value) {
        while (requested > max_value) requested /= 2;
    }
    if (requested < min_value) return min_value;
    if (requested > max_value) return max_value;
    return requested;
}

static int choose_chunk_size_upload(bool mobile, int requested) {
    int fallback = mobile ? TASFA_UPLOAD_CHUNK_SIZE_MOBILE : TASFA_UPLOAD_CHUNK_SIZE_DEFAULT;
    int max_value = mobile ? TASFA_UPLOAD_CHUNK_SIZE_MOBILE_MAX : TASFA_UPLOAD_CHUNK_SIZE_MAX;
    return normalize_chunk_size_hint(requested, fallback, TASFA_UPLOAD_CHUNK_SIZE_MIN, max_value);
}

static int tasfa_upload_session_limit(void) {
    int limit = g_config.max_total_parallel_uploads;
    if (limit < 1) limit = 1;
    if (limit > TASFA_MAX_CONCURRENT_UPLOADS) limit = TASFA_MAX_CONCURRENT_UPLOADS;
    return limit;
}

static int tasfa_download_session_limit(void) {
    int limit = g_config.max_concurrent_downloads;
    if (limit < 1) limit = 1;
    if (limit > TASFA_MAX_CONCURRENT_DOWNLOADS) limit = TASFA_MAX_CONCURRENT_DOWNLOADS;
    return limit;
}

static int tasfa_upload_parallel_limit(void) {
    int limit = g_config.max_upload_parallel_chunks;
    if (limit < 1) limit = 1;
    if (limit > TASFA_UPLOAD_MAX_PARALLEL) limit = TASFA_UPLOAD_MAX_PARALLEL;
    return limit;
}

static int fast_download_chunk_max_from_request(const char *downlink_mbps, const char *rtt_ms, const char *effective_type, const char *save_data) {
    if (!downlink_mbps || !downlink_mbps[0]) return TASFA_DOWNLOAD_CHUNK_SIZE_MAX;
    char *end = NULL;
    double downlink = strtod(downlink_mbps, &end);
    if (end == downlink_mbps || downlink < 500.0) return TASFA_DOWNLOAD_CHUNK_SIZE_MAX;
    if (save_data && strcmp(save_data, "1") == 0) return TASFA_DOWNLOAD_CHUNK_SIZE_MAX;
    if (effective_type && (strcmp(effective_type, "slow-2g") == 0 || strcmp(effective_type, "2g") == 0 || strcmp(effective_type, "3g") == 0)) {
        return TASFA_DOWNLOAD_CHUNK_SIZE_MAX;
    }
    if (rtt_ms && rtt_ms[0]) {
        end = NULL;
        double rtt = strtod(rtt_ms, &end);
        if (end != rtt_ms && rtt > 80.0) return TASFA_DOWNLOAD_CHUNK_SIZE_MAX;
    }
    return downlink >= 900.0 ? TASFA_DOWNLOAD_CHUNK_SIZE_ULTRA_MAX : TASFA_DOWNLOAD_CHUNK_SIZE_FAST_MAX;
}

static int choose_chunk_size_download(bool mobile, int requested, long long total_size, int fast_link_max) {
    int fallback = mobile ? TASFA_DOWNLOAD_CHUNK_SIZE_MOBILE : TASFA_DOWNLOAD_CHUNK_SIZE_DEFAULT;
    int max_value = fast_link_max > TASFA_DOWNLOAD_CHUNK_SIZE_MAX ? fast_link_max : TASFA_DOWNLOAD_CHUNK_SIZE_MAX;
    /* Large files: bigger chunks reduce per-chunk RTT overhead, critical for mobile HD video */
    if (total_size > 100 * 1024 * 1024LL) {
        fallback = mobile ? (10 * 1024 * 1024) : (16 * 1024 * 1024);
    }
    if (total_size > 1024 * 1024 * 1024LL) {
        fallback = max_value > TASFA_DOWNLOAD_CHUNK_SIZE_MAX ? max_value : (mobile ? (14 * 1024 * 1024) : (32 * 1024 * 1024));
    }
    if (total_size > 100 * 1024 * 1024LL && requested > 0 && requested < fallback) {
        requested = fallback;
    }
    return normalize_chunk_size_hint(requested, fallback, TASFA_DOWNLOAD_CHUNK_SIZE_MIN, max_value);
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

static void tasfa_queue_touch(queue_slot_t *slots, int max_slots, const char *id) {
    if (!id || !id[0]) return;
    pthread_mutex_lock(&g_tasfa_queue_mtx);
    for (int i = 0; i < max_slots; i++) {
        if (strcmp(slots[i].id, id) == 0) {
            slots[i].ts = time(NULL);
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
    /* Connection and Keep-Alive are handled globally by global_middleware. */
    (void)res;
}

static void send_queued_json(cwist_http_response *res, const char *msg, int retry_after) {
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddBoolToObject(obj, "ok", false);
    cJSON_AddBoolToObject(obj, "queued", true);
    cJSON_AddStringToObject(obj, "error", msg ? msg : "server busy");
    cJSON_AddNumberToObject(obj, "retry_after", retry_after > 0 ? retry_after : 0);
    res->status_code = (cwist_http_status_t)429;
    cwist_http_header_add(&res->headers, "Content-Type", "application/json");
    char retry_buf[16];
    snprintf(retry_buf, sizeof(retry_buf), "%d", retry_after > 0 ? retry_after : 0);
    cwist_http_header_add(&res->headers, "Retry-After", retry_buf);
    add_keepalive_headers(res);
    char *json = cJSON_PrintUnformatted(obj);
    cwist_sstring_assign(res->body, json ? json : "{}");
    if (json) free(json);
    cJSON_Delete(obj);
}

/* --- Utility Functions --- */

static const char *http_status_text(int code) {
    switch (code) {
        case 200: return "OK";
        case 201: return "Created";
        case 202: return "Accepted";
        case 204: return "No Content";
        case 206: return "Partial Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 304: return "Not Modified";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 410: return "Gone";
        case 413: return "Payload Too Large";
        case 429: return "Too Many Requests";
        case 500: return "Internal Server Error";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout";
        default:  return "Unknown";
    }
}

static void send_json_response(cwist_http_response *res, cJSON *obj, int status_code) {
    char *json = cJSON_PrintUnformatted(obj);
    res->status_code = (cwist_http_status_t)status_code;
    cwist_sstring_assign(res->status_text, (char *)http_status_text(status_code));
    cwist_http_header_add(&res->headers, "Content-Type", "application/json");
    /* Dynamic JSON (sessions, handshake, chunk errors) must not be cached. */
    cwist_http_header_add(&res->headers, "Cache-Control", "no-store, no-cache, must-revalidate, private");
    cwist_http_header_add(&res->headers, "Vary", "Origin, Accept-Encoding");
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

static bool is_safe_upload_asset_name(const char *name) {
    if (is_safe_filename_simple(name)) return true;
    if (!name) return false;
    if (strncmp(name, ".thumbs/", 8) == 0) return is_safe_filename_simple(name + 8);
    if (strncmp(name, ".previews/", 10) == 0) return is_safe_filename_simple(name + 10);
    return false;
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

static double json_double(cJSON *obj, const char *key, double def) {
    cJSON *item = cJSON_GetObjectItem(obj, key);
    if (!item) return def;
    if (cJSON_IsNumber(item)) return item->valuedouble;
    if (cJSON_IsString(item) && item->valuestring) return atof(item->valuestring);
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

bool secure_str_eq(const char *a, const char *b) {
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

static bool encrypt_stream_block(const unsigned char *key, const unsigned char *iv_seed, int chunk_index,
                                  const char *session_id,
                                  const unsigned char *plaintext, size_t plaintext_len,
                                  unsigned char *ciphertext, size_t *ciphertext_len_out) {
    if (!key || !iv_seed || !plaintext || !ciphertext || !ciphertext_len_out) return false;
    unsigned char iv[12];
    unsigned char aad[512];
    int aad_len = build_stream_aad(aad, sizeof(aad), session_id, chunk_index);
    if (aad_len <= 0) return false;
    derive_stream_iv(iv, iv_seed, chunk_index);
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return false;
    bool ok = false;
    int out_len = 0;
    int final_len = 0;
    if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1) goto cleanup;
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, sizeof(iv), NULL) != 1) goto cleanup;
    if (EVP_EncryptInit_ex(ctx, NULL, NULL, key, iv) != 1) goto cleanup;
    if (EVP_EncryptUpdate(ctx, NULL, &out_len, aad, aad_len) != 1) goto cleanup;
    if (EVP_EncryptUpdate(ctx, ciphertext, &out_len, plaintext, (int)plaintext_len) != 1) goto cleanup;
    if (EVP_EncryptFinal_ex(ctx, ciphertext + out_len, &final_len) != 1) goto cleanup;
    unsigned char tag[16];
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag) != 1) goto cleanup;
    memcpy(ciphertext + out_len + final_len, tag, 16);
    *ciphertext_len_out = (size_t)(out_len + final_len + 16);
    ok = true;
cleanup:
    EVP_CIPHER_CTX_free(ctx);
    return ok;
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

/* --- Lagrange Extrapolation for RTT Prediction ---
 * For large files we collect per-chunk RTT samples and use
 * Lagrange polynomial extrapolation to estimate the RTT of
 * the tail chunks.  Math is kept light: max 8 samples,
 * O(n^2) with tiny n, and clamped to avoid runaway values.
 */
#define TASFA_RTT_MAX_SAMPLES 8
#define TASFA_RTT_CLAMP_MAX   30000.0  /* 30 s ceiling */

static double lagrange_predict(const double *xs, const double *ys, int n, double x) {
    double result = 0.0;
    for (int i = 0; i < n; i++) {
        double term = ys[i];
        for (int j = 0; j < n; j++) {
            if (i == j) continue;
            double denom = xs[i] - xs[j];
            if (denom == 0.0) denom = 1e-9;
            term *= (x - xs[j]) / denom;
        }
        result += term;
    }
    return result;
}

static void append_rtt_sample(cJSON *meta, int chunk_index, double rtt_ms) {
    if (!meta || rtt_ms < 0.0) return;
    cJSON *samples = cJSON_GetObjectItem(meta, "rtt_samples");
    if (!samples || !cJSON_IsArray(samples)) {
        samples = cJSON_CreateArray();
        cJSON_AddItemToObject(meta, "rtt_samples", samples);
    }
    cJSON *item = cJSON_CreateObject();
    cJSON_AddNumberToObject(item, "chunk_index", chunk_index);
    cJSON_AddNumberToObject(item, "rtt_ms", rtt_ms);
    cJSON_AddItemToArray(samples, item);
    while (cJSON_GetArraySize(samples) > TASFA_RTT_MAX_SAMPLES)
        cJSON_DeleteItemFromArray(samples, 0);
}

static double predict_remaining_ms(cJSON *meta) {
    cJSON *samples = cJSON_GetObjectItem(meta, "rtt_samples");
    if (!samples || !cJSON_IsArray(samples)) return -1.0;
    int n = cJSON_GetArraySize(samples);
    if (n < 3) return -1.0; /* need at least 3 points for extrapolation */

    double xs[TASFA_RTT_MAX_SAMPLES];
    double ys[TASFA_RTT_MAX_SAMPLES];
    for (int i = 0; i < n; i++) {
        cJSON *item = cJSON_GetArrayItem(samples, i);
        xs[i] = json_double(item, "chunk_index", 0.0);
        ys[i] = json_double(item, "rtt_ms", 0.0);
    }

    int chunk_count = json_int(meta, "chunk_count", 0);
    int received    = json_int(meta, "received_chunks", 0);
    int remaining   = chunk_count - received;
    if (remaining <= 0) return 0.0;

    /* Extrapolate to the last chunk index to get a tail RTT estimate. */
    double target_x = (double)(chunk_count - 1);
    double predicted_rtt = lagrange_predict(xs, ys, n, target_x);
    if (predicted_rtt < 0.0) predicted_rtt = 0.0;
    if (predicted_rtt > TASFA_RTT_CLAMP_MAX) predicted_rtt = TASFA_RTT_CLAMP_MAX;

    /* Naive total: remaining chunks * predicted single-chunk RTT.
     * This is intentionally simple to keep CPU load negligible.
     */
    return predicted_rtt * (double)remaining;
}

static int link_score_from_inputs(const char *score_str, const char *effective_type, const char *downlink_str,
                                  const char *rtt_str, const char *retry_str, const char *timeout_str, const char *save_data_str) {
    return media_quality_score_from_link(score_str, effective_type, downlink_str, rtt_str, retry_str, timeout_str, save_data_str);
}

static void choose_upload_window(bool mobile, int score, int *initial_parallel, int *max_parallel, int *pacing_ms) {
    int initial_value = TASFA_UPLOAD_DEFAULT_PARALLEL;
    int max_value = TASFA_UPLOAD_MAX_PARALLEL;
    int pace = 0;
    if (score >= 45) { initial_value = 32; max_value = 40; }
    else if (score >= 25) { initial_value = 24; max_value = 32; pace = 1; }
    else if (score >= 10) { initial_value = 16; max_value = 24; pace = 3; }
    else { initial_value = 10; max_value = 16; pace = 6; }

    int floor = mobile ? 4 : 8;
    if (initial_value < floor) initial_value = floor;

    int configured_max = tasfa_upload_parallel_limit();
    if (max_value > configured_max) max_value = configured_max;
    if (initial_value > max_value) initial_value = max_value;
    if (initial_parallel) *initial_parallel = initial_value;
    if (max_parallel) *max_parallel = max_value;
    if (pacing_ms) *pacing_ms = pace;
}

static void choose_download_profile(bool mobile, int score, int *initial_parallel, int *max_parallel, int *pacing_ms, int *coalesce_chunks) {
    int initial_value = 18, max_value = 36, pace = 0, coalesce = 12;
    if (score < 25) { initial_value = 10; max_value = 18; pace = 4; coalesce = 4; }
    else if (score < 45) { initial_value = 14; max_value = 28; pace = 1; coalesce = 8; }
    else if (score < 65) { initial_value = 18; max_value = 36; pace = 0; coalesce = 12; }
    else { initial_value = 24; max_value = 48; coalesce = 16; }
    /* Mobile high-res video: ensure enough parallelism to keep pipe full despite high RTT */
    if (mobile) {
        if (initial_value < 14) initial_value = 14;
        if (max_value < 28) max_value = 28;
        if (coalesce < 8) coalesce = 8;
    }
    if (max_value > TASFA_DOWNLOAD_MAX_PARALLEL) max_value = TASFA_DOWNLOAD_MAX_PARALLEL;
    if (initial_value > max_value) initial_value = max_value;
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
static int open_upload_session_lock(const char *upload_id) {
    return open_upload_session_lock_impl(upload_id, false);
}
static int open_upload_session_lock_sh(const char *upload_id) {
    return open_upload_session_lock_impl(upload_id, true);
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

/* --- TASFA Worker Scheduler --- */
/* Uses libttak's sharded thread pool so upload/finalize jobs are routed by
   stable hashes and managed by the shared pool policy instead of local pthreads. */

#define TASFA_MAX_WORKERS 64

typedef struct tasfa_job {
    void (*func)(void *);
    void *arg;
    bool free_after_done;
} tasfa_job_t;

static ttak_thread_pool_t *g_tasfa_pool = NULL;
static pthread_once_t g_scheduler_once = PTHREAD_ONCE_INIT;
static volatile unsigned int g_round_robin_idx = 0;

static unsigned int hash_upload_id(const char *id) {
    unsigned int h = 5381;
    int c;
    while ((c = *id++)) h = ((h << 5) + h) + c;
    return h;
}

static void *tasfa_pool_job_run(void *arg) {
    tasfa_job_t *job = (tasfa_job_t *)arg;
    if (job && job->func) {
        job->func(job->arg);
        if (job->free_after_done) {
            free(job);
        }
    }
    return NULL;
}

static void tasfa_scheduler_init_impl(void) {
    int n = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 2) n = 2;
    if (n > TASFA_MAX_WORKERS) n = TASFA_MAX_WORKERS;
    g_tasfa_pool = ttak_thread_pool_create((size_t)n, 0, ttak_get_tick_count());
    if (!g_tasfa_pool) {
        fprintf(stderr, "[TASFA] Failed to initialize libttak scheduler pool.\n");
    }
}

static void tasfa_scheduler_ensure_init(void) {
    pthread_once(&g_scheduler_once, tasfa_scheduler_init_impl);
}

static void tasfa_scheduler_submit(const char *upload_id, void (*func)(void *), void *arg, tasfa_job_t *job) {
    tasfa_scheduler_ensure_init();
    uint64_t hash;
    if (upload_id) {
        hash = (uint64_t)hash_upload_id(upload_id);
    } else {
        hash = (uint64_t)__sync_fetch_and_add(&g_round_robin_idx, 1);
    }

    job->func = func;
    job->arg = arg;

    uint64_t now = ttak_get_tick_count();
    ttak_task_t *task = ttak_task_create(tasfa_pool_job_run, job, NULL, now);
    if (task) {
        ttak_task_set_hash(task, hash);
        ttak_task_set_domain(task, TTAK_TASK_DOMAIN_IO);
        ttak_task_set_urgency(task, 70);
        if (g_tasfa_pool && ttak_thread_pool_schedule_task(g_tasfa_pool, task, 0, now)) {
            return;
        }
        ttak_task_destroy(task, now);
    }

    tasfa_pool_job_run(job);
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

cJSON *load_download_session_cached(const char *session_id) {
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

static __thread unsigned char *g_tasfa_inflate_buf = NULL;
static __thread size_t g_tasfa_inflate_buf_size = 0;

static unsigned char *ensure_inflate_buf(size_t need) {
    if (g_tasfa_inflate_buf_size >= need) return g_tasfa_inflate_buf;
    if (g_tasfa_inflate_buf) free(g_tasfa_inflate_buf);
    g_tasfa_inflate_buf = (unsigned char *)malloc(need);
    g_tasfa_inflate_buf_size = need;
    return g_tasfa_inflate_buf;
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

static bool str_contains_ci_local(const char *haystack, const char *needle) {
    if (!haystack || !needle || !needle[0]) return false;
    size_t nlen = strlen(needle);
    for (const char *p = haystack; *p; p++) {
        size_t i = 0;
        while (i < nlen && p[i] &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) {
            i++;
        }
        if (i == nlen) return true;
    }
    return false;
}

static bool tasfa_gzip_compress_alloc(const unsigned char *input, size_t input_len,
                                      unsigned char **out, size_t *out_len) {
    if (!input || input_len == 0 || !out || !out_len) return false;
    *out = NULL;
    *out_len = 0;

    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    if (deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        return false;
    }

    size_t cap = deflateBound(&zs, (uLong)input_len);
    unsigned char *buf = (unsigned char *)cwist_alloc(cap);
    if (!buf) {
        deflateEnd(&zs);
        return false;
    }

    zs.next_in = (Bytef *)input;
    zs.avail_in = (uInt)input_len;
    zs.next_out = buf;
    zs.avail_out = (uInt)cap;
    int rc = deflate(&zs, Z_FINISH);
    if (rc != Z_STREAM_END) {
        cwist_free(buf);
        deflateEnd(&zs);
        return false;
    }

    *out_len = cap - zs.avail_out;
    *out = buf;
    deflateEnd(&zs);
    return true;
}

static bool tasfa_gzip_decompress_to(const unsigned char *input, size_t input_len,
                                     unsigned char *out, size_t expected_len) {
    if (!input || input_len == 0 || !out) return false;

    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    if (inflateInit2(&zs, 15 + 16) != Z_OK) return false;

    zs.next_in = (Bytef *)input;
    zs.avail_in = (uInt)input_len;
    zs.next_out = out;
    zs.avail_out = (uInt)expected_len;
    int rc = inflate(&zs, Z_FINISH);
    bool ok = (rc == Z_STREAM_END && zs.total_out == expected_len);
    inflateEnd(&zs);
    return ok;
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

static bool send_file_slice_response(cwist_http_request *req, cwist_http_response *res, const char *path, const char *mime, long long offset, size_t amount,
                                       int chunk_index, int chunk_count, int span) {
    int fd = open(path, O_RDONLY);
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
    bool client_accepts_gzip = str_contains_ci_local(accept_tasfa_encoding, "gzip");
    unsigned char *gzip_buf = NULL;
    size_t gzip_len = 0;
    const unsigned char *payload = (const unsigned char *)buf;
    size_t payload_len = total;
    bool payload_gzip = false;
    if (client_accepts_gzip && total >= TASFA_GZIP_MIN_GAIN_BYTES &&
        tasfa_gzip_compress_alloc((const unsigned char *)buf, total, &gzip_buf, &gzip_len) &&
        gzip_len + TASFA_GZIP_MIN_GAIN_BYTES < total) {
        payload = gzip_buf;
        payload_len = gzip_len;
        payload_gzip = true;
        cwist_http_header_add(&res->headers, "X-TASFA-Content-Encoding", "gzip");
        char plain_len_buf[32];
        snprintf(plain_len_buf, sizeof(plain_len_buf), "%zu", total);
        cwist_http_header_add(&res->headers, "X-TASFA-Uncompressed-Length", plain_len_buf);
    } else if (gzip_buf) {
        cwist_free(gzip_buf);
        gzip_buf = NULL;
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
            if (gzip_buf) cwist_free(gzip_buf);
            return true;
        }
        cwist_free(cipher_buf);
    }

    char len_buf[32];
    snprintf(len_buf, sizeof(len_buf), "%zu", payload_len);
    cwist_http_header_add(&res->headers, "Content-Length", len_buf);
    cwist_sstring_assign(res->body, "");
    if (payload_len > 0) cwist_sstring_append_len(res->body, (const char *)payload, payload_len);
    if (gzip_buf) cwist_free(gzip_buf);
    (void)payload_gzip;
    return true;
}

static bool resolve_asset_scope_path(cwist_db *db, const char *scope, const char *encoded, char *storage_path, size_t storage_len,
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
    }
    cwist_free(decoded);
    return ok;
}

static bool init_download_session(const char *filename, const char *mime, const char *storage_path, long long total_size,
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

static void finalize_cache_sweep_locked(void) {
    time_t now = time(NULL);
    for (int i = 0; i < TASFA_FINALIZE_CACHE_SLOTS; i++) {
        if (g_finalize_slots[i].upload_id[0] && g_finalize_slots[i].expires > 0 && g_finalize_slots[i].expires < now) {
            free(g_finalize_slots[i].body);
            memset(&g_finalize_slots[i], 0, sizeof(g_finalize_slots[i]));
        }
    }
}

static finalize_slot_t *finalize_cache_find_locked(const char *upload_id) {
    if (!upload_id || !upload_id[0]) return NULL;
    for (int i = 0; i < TASFA_FINALIZE_CACHE_SLOTS; i++) {
        if (strcmp(g_finalize_slots[i].upload_id, upload_id) == 0) return &g_finalize_slots[i];
    }
    return NULL;
}

static bool finalize_cache_get(const char *upload_id, const char *upload_token, int *status_code, char **body, bool *active) {
    bool found = false;
    pthread_mutex_lock(&g_finalize_mtx);
    finalize_cache_sweep_locked();
    finalize_slot_t *slot = finalize_cache_find_locked(upload_id);
    if (slot && secure_str_eq(upload_token, slot->upload_token)) {
        if (status_code) *status_code = slot->status_code;
        if (body) *body = slot->body ? strdup(slot->body) : strdup("{}");
        if (active) *active = slot->active && !slot->done;
        found = true;
    }
    pthread_mutex_unlock(&g_finalize_mtx);
    return found;
}

static bool finalize_cache_mark_started(const char *upload_id, const char *upload_token) {
    bool ok = false;
    pthread_mutex_lock(&g_finalize_mtx);
    finalize_cache_sweep_locked();
    finalize_slot_t *slot = finalize_cache_find_locked(upload_id);
    if (!slot) {
        for (int i = 0; i < TASFA_FINALIZE_CACHE_SLOTS; i++) {
            if (!g_finalize_slots[i].upload_id[0]) {
                slot = &g_finalize_slots[i];
                break;
            }
        }
    }
    if (slot) {
        free(slot->body);
        memset(slot, 0, sizeof(*slot));
        snprintf(slot->upload_id, sizeof(slot->upload_id), "%s", upload_id);
        snprintf(slot->upload_token, sizeof(slot->upload_token), "%s", upload_token);
        slot->active = true;
        slot->done = false;
        slot->status_code = 202;
        slot->body = strdup("{\"ok\":false,\"processing\":true}");
        slot->expires = time(NULL) + 3600;
        ok = true;
    }
    pthread_mutex_unlock(&g_finalize_mtx);
    return ok;
}

static void finalize_cache_mark_done(const char *upload_id, int status_code, const char *body) {
    pthread_mutex_lock(&g_finalize_mtx);
    finalize_slot_t *slot = finalize_cache_find_locked(upload_id);
    if (slot) {
        free(slot->body);
        slot->body = strdup(body ? body : "{}");
        slot->status_code = status_code > 0 ? status_code : 500;
        slot->active = false;
        slot->done = true;
        slot->expires = time(NULL) + 3600;
    }
    pthread_mutex_unlock(&g_finalize_mtx);
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
    chunk_count = (int)((total_size + chunk_size - 1) / chunk_size);
    if (chunk_count < 1) chunk_count = 1;
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

typedef struct {
    const char *upload_id;
    int chunk_index;
    const char *temp_path;
    bool encrypted_stream;
    const unsigned char *stream_key;
    const unsigned char *stream_iv_seed;
    const void *body_data;
    size_t body_size;
    bool compressed;
    long long offset;
    long long expected_size;
    int chunk_count;
    const char *hash_tag_hex;
    const char *magic_scalar_str;
    /* outputs */
    bool stored;
    bool state_ok;
    long long store_ms;
    long long state_ms;
} upload_work_t;

#define HTP_TAG_LEN 129
#define HTP_RECORD_SIZE (HTP_TAG_LEN + 8 + 8)

static bool save_htp_scalar_to_dir(const char *dir_path, int chunk_index, const char *hash_tag_hex, uint64_t raw_scalar, uint64_t balanced_scalar) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/htp.bin", dir_path);
    int fd = open(path, O_CREAT | O_WRONLY, 0644);
    if (fd < 0) return false;
    char tag[HTP_TAG_LEN] = {0};
    if (hash_tag_hex) strncpy(tag, hash_tag_hex, HTP_TAG_LEN - 1);
    off_t offset = (off_t)chunk_index * HTP_RECORD_SIZE;
    pwrite(fd, tag, HTP_TAG_LEN, offset);
    pwrite(fd, &raw_scalar, 8, offset + HTP_TAG_LEN);
    pwrite(fd, &balanced_scalar, 8, offset + HTP_TAG_LEN + 8);
    close(fd);
    return true;
}

static bool save_htp_scalar(const char *upload_id, int chunk_index, const char *hash_tag_hex, uint64_t raw_scalar, uint64_t balanced_scalar) {
    char path[PATH_MAX];
    upload_session_dir(path, sizeof(path), upload_id);
    return save_htp_scalar_to_dir(path, chunk_index, hash_tag_hex, raw_scalar, balanced_scalar);
}

static void finalize_cache_update_status(const char *upload_id, const char *msg) {
    pthread_mutex_lock(&g_finalize_mtx);
    finalize_slot_t *slot = finalize_cache_find_locked(upload_id);
    if (slot) {
        free(slot->body);
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddBoolToObject(obj, "ok", false);
        cJSON_AddBoolToObject(obj, "processing", true);
        cJSON_AddStringToObject(obj, "status", msg ? msg : "");
        slot->body = cJSON_PrintUnformatted(obj);
        cJSON_Delete(obj);
    }
    pthread_mutex_unlock(&g_finalize_mtx);
}

static bool load_htp_scalars(const char *upload_id, int chunk_count, char **hash_tags_out, uint64_t **raw_scalars_out, uint64_t **balanced_scalars_out) {
    char path[PATH_MAX];
    upload_session_dir(path, sizeof(path), upload_id);
    strncat(path, "/htp.bin", sizeof(path) - strlen(path) - 1);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    char *tags = (char *)cwist_alloc((size_t)chunk_count * HTP_TAG_LEN);
    uint64_t *raw_scalars = (uint64_t *)cwist_alloc((size_t)chunk_count * sizeof(uint64_t));
    uint64_t *balanced_scalars = (uint64_t *)cwist_alloc((size_t)chunk_count * sizeof(uint64_t));
    if (!tags || !raw_scalars || !balanced_scalars) {
        cwist_free(tags); cwist_free(raw_scalars); cwist_free(balanced_scalars); close(fd); return false;
    }
    for (int i = 0; i < chunk_count; i++) {
        off_t offset = (off_t)i * HTP_RECORD_SIZE;
        if (pread(fd, tags + (i * HTP_TAG_LEN), HTP_TAG_LEN, offset) != HTP_TAG_LEN) {
            tags[i * HTP_TAG_LEN] = '\0';
        }
        if (pread(fd, &raw_scalars[i], 8, offset + HTP_TAG_LEN) != 8) {
            raw_scalars[i] = 0;
        }
        if (pread(fd, &balanced_scalars[i], 8, offset + HTP_TAG_LEN + 8) != 8) {
            balanced_scalars[i] = 0;
        }
    }
    close(fd);
    *hash_tags_out = tags;
    *raw_scalars_out = raw_scalars;
    *balanced_scalars_out = balanced_scalars;
    return true;
}

/* --- Server-Authoritative HTP Recovery --- */

typedef struct {
    int chunk_index;
    double suspicion_score;
} htp_suspect_t;

typedef struct {
    uint64_t l1;
    uint64_t l2;
    uint64_t l3;
} htp_line_sums_t;

static int compare_suspect_desc(const void *a, const void *b) {
    const htp_suspect_t *sa = (const htp_suspect_t *)a;
    const htp_suspect_t *sb = (const htp_suspect_t *)b;
    if (sa->suspicion_score > sb->suspicion_score) return -1;
    if (sa->suspicion_score < sb->suspicion_score) return 1;
    return sa->chunk_index - sb->chunk_index;
}

#if defined(__AVX2__)
#define TASFA_HTP_SIMD 1
#define TASFA_HTP_AVX2 1
typedef uint64_t tasfa_u64x4 __attribute__((vector_size(32)));
#elif defined(__SSE2__) || defined(__ARM_NEON) || defined(__ARM_NEON__) || defined(__VSX__) || defined(__mips_msa)
#define TASFA_HTP_SIMD 1
#define TASFA_HTP_SSE2 1
typedef uint64_t tasfa_u64x2 __attribute__((vector_size(16)));
#else
#define TASFA_HTP_SIMD 0
#endif

static const char *htp_simd_backend(void) {
#if defined(TASFA_HTP_AVX2)
    return "avx2";
#elif defined(__SSE2__)
    return "sse2";
#elif defined(__ARM_NEON) || defined(__ARM_NEON__)
    return "neon";
#elif defined(__VSX__)
    return "vsx";
#elif defined(__mips_msa)
    return "msa";
#else
    return "scalar";
#endif
}

static htp_line_sums_t htp_line_sums(const uint64_t v[6], uint64_t modulus_M) {
    if (modulus_M == 0) modulus_M = 1;
    htp_line_sums_t sums;
#if defined(TASFA_HTP_AVX2)
    tasfa_u64x4 v_left  = { v[0], v[2], v[4], 0 };
    tasfa_u64x4 v_mid   = { v[1], v[3], v[5], 0 };
    tasfa_u64x4 v_right = { v[2], v[4], v[0], 0 };
    tasfa_u64x4 v_sum   = v_left + v_mid + v_right;
    sums.l1 = v_sum[0] % modulus_M;
    sums.l2 = v_sum[1] % modulus_M;
    sums.l3 = v_sum[2] % modulus_M;
#elif defined(TASFA_HTP_SSE2)
    tasfa_u64x2 a = { v[0], v[2] };
    tasfa_u64x2 b = { v[1], v[3] };
    tasfa_u64x2 c = { v[2], v[4] };
    tasfa_u64x2 pair = a + b + c;
    sums.l1 = pair[0] % modulus_M;
    sums.l2 = pair[1] % modulus_M;
    sums.l3 = (v[4] + v[5] + v[0]) % modulus_M;
#else
    sums.l1 = (v[0] + v[1] + v[2]) % modulus_M;
    sums.l2 = (v[2] + v[3] + v[4]) % modulus_M;
    sums.l3 = (v[4] + v[5] + v[0]) % modulus_M;
#endif
    return sums;
}

static void htp_analyze_group(uint64_t v[6], uint64_t modulus_M, int group_start,
                              htp_suspect_t *out, int *out_count) {
    htp_line_sums_t sums = htp_line_sums(v, modulus_M);
    uint64_t L1 = sums.l1;
    uint64_t L2 = sums.l2;
    uint64_t L3 = sums.l3;

    bool all_equal = (L1 == L2 && L2 == L3);
    if (all_equal) {
        *out_count = 0;
        return;
    }

    bool e1_fail = false, e2_fail = false, e3_fail = false;
    if (L1 != L2) { e1_fail = true; e2_fail = true; }
    if (L2 != L3) { e2_fail = true; e3_fail = true; }
    if (L1 != L3) { e1_fail = true; e3_fail = true; }

    int total_fail = (e1_fail ? 1 : 0) + (e2_fail ? 1 : 0) + (e3_fail ? 1 : 0);
    if (total_fail == 0) total_fail = 1;

    for (int i = 0; i < 6; i++) {
        int in_fail = 0;
        if (i == 0) { if (e1_fail) in_fail++; if (e3_fail) in_fail++; }
        if (i == 1) { if (e1_fail) in_fail++; }
        if (i == 2) { if (e1_fail) in_fail++; if (e2_fail) in_fail++; }
        if (i == 3) { if (e2_fail) in_fail++; }
        if (i == 4) { if (e2_fail) in_fail++; if (e3_fail) in_fail++; }
        if (i == 5) { if (e3_fail) in_fail++; }

        if (in_fail == 0) continue;

        /* Deterministic score derived directly from topology:
         * fraction of failed equations this slot participates in. */
        double score = (double)in_fail / (double)total_fail;

        out[*out_count].chunk_index = group_start + i;
        out[*out_count].suspicion_score = score;
        (*out_count)++;
    }
}

/* Implementation-specific tuning parameters for the abstract cost model.
 * The protocol spec defines the threshold conceptually as:
 *   retry_cost  ≈ suspect_bytes × RTT_factor
 *   repair_cost ≈ metadata_bytes + server_cpu_cost + extra_rtt_cost
 * Concrete numbers below are server-side implementation details, not protocol constants.
 */
#define HTP_REPAIR_CPU_PER_SUSPECT  2048.0
#define HTP_REPAIR_CPU_PER_L0_GROUP 512.0

/* Try every permutation of a 6-slot group's balanced scalars.
 * If any permutation restores L1==L2==L3, apply it to memory and htp.bin
 * so the server recovers without retransmission. */
static bool next_permutation_int(int *a, int n) {
    int i = n - 2;
    while (i >= 0 && a[i] >= a[i + 1]) i--;
    if (i < 0) return false;
    int j = n - 1;
    while (a[j] <= a[i]) j--;
    int tmp = a[i]; a[i] = a[j]; a[j] = tmp;
    for (int l = i + 1, r = n - 1; l < r; l++, r--) {
        tmp = a[l]; a[l] = a[r]; a[r] = tmp;
    }
    return true;
}

static bool htp_try_swap_repair(const char *upload_id, uint64_t *balanced_scalars,
                                uint64_t *raw_scalars, char *tags,
                                int group_start, uint64_t modulus_M) {
    uint64_t v[6];
    for (int i = 0; i < 6; i++) v[i] = balanced_scalars[group_start + i];

    htp_line_sums_t sums0 = htp_line_sums(v, modulus_M);
    if (sums0.l1 == sums0.l2 && sums0.l2 == sums0.l3) return false;

    int perm[6] = {0, 1, 2, 3, 4, 5};
    while (next_permutation_int(perm, 6)) {
        uint64_t p[6];
        for (int i = 0; i < 6; i++) p[i] = v[perm[i]];
        htp_line_sums_t sums = htp_line_sums(p, modulus_M);
        if (sums.l1 == sums.l2 && sums.l2 == sums.l3) {
            uint64_t new_balanced[6], new_raw[6];
            char new_tags[6][HTP_TAG_LEN];
            for (int i = 0; i < 6; i++) {
                int src = group_start + perm[i];
                new_balanced[i] = balanced_scalars[src];
                new_raw[i] = raw_scalars[src];
                memcpy(new_tags[i], tags + (src * HTP_TAG_LEN), HTP_TAG_LEN);
            }
            for (int i = 0; i < 6; i++) {
                int dst = group_start + i;
                balanced_scalars[dst] = new_balanced[i];
                raw_scalars[dst] = new_raw[i];
                memcpy(tags + (dst * HTP_TAG_LEN), new_tags[i], HTP_TAG_LEN);
            }

            char path[PATH_MAX];
            upload_session_dir(path, sizeof(path), upload_id);
            strncat(path, "/htp.bin", sizeof(path) - strlen(path) - 1);
            int fd = open(path, O_RDWR);
            if (fd >= 0) {
                char recs[6][HTP_RECORD_SIZE];
                bool ok = true;
                for (int i = 0; i < 6; i++) {
                    off_t off = (off_t)(group_start + i) * HTP_RECORD_SIZE;
                    if (pread(fd, recs[i], HTP_RECORD_SIZE, off) != HTP_RECORD_SIZE) ok = false;
                }
                if (ok) {
                    for (int i = 0; i < 6; i++) {
                        off_t off = (off_t)(group_start + i) * HTP_RECORD_SIZE;
                        pwrite(fd, recs[perm[i]], HTP_RECORD_SIZE, off);
                    }
                }
                close(fd);
            }
            return true;
        }
    }
    return false;
}

static bool htp_repair_worthwhile(int suspect_count, int total_chunks, int chunk_size, double rtt_ms) {
    (void)total_chunks; (void)chunk_size; (void)rtt_ms;
    if (suspect_count < 12) return false;
    long long retry_bytes = (long long)suspect_count * chunk_size;
    double rtt_factor = (rtt_ms <= 0.0) ? 1.0 : (rtt_ms / 100.0);
    double retry_cost = (double)retry_bytes * rtt_factor;
    double repair_cost = (double)suspect_count * HTP_REPAIR_CPU_PER_SUSPECT
                       + (double)(total_chunks / 6) * HTP_REPAIR_CPU_PER_L0_GROUP;
    return retry_cost > repair_cost;
}

/* Contraction: failed groups become higher-level vertices.
 * Each complete level-0 group is collapsed to a single scalar (sum of its
 * balanced scalars). Level-1 groups are consecutive 6-slot groups of these
 * aggregates. Suspects from level-0 groups in failing level-1 groups are kept.
 */
static int htp_contract_groups(const uint64_t *balanced_scalars, int chunk_count, uint64_t modulus_M,
                                htp_suspect_t *suspects, int suspect_count) {
    int group_count = chunk_count / 6;
    if (group_count < 1 || suspect_count < 1) return suspect_count;

    uint64_t *group_agg = (uint64_t *)cwist_alloc((size_t)group_count * sizeof(uint64_t));
    int *group_suspect_mask = (int *)cwist_alloc((size_t)group_count * sizeof(int));
    memset(group_agg, 0, (size_t)group_count * sizeof(uint64_t));
    memset(group_suspect_mask, 0, (size_t)group_count * sizeof(int));

    for (int g = 0; g < group_count; g++) {
        uint64_t v0 = balanced_scalars[g * 6 + 0];
        uint64_t v1 = balanced_scalars[g * 6 + 1];
        uint64_t v2 = balanced_scalars[g * 6 + 2];
        uint64_t v3 = balanced_scalars[g * 6 + 3];
        uint64_t v4 = balanced_scalars[g * 6 + 4];
        uint64_t v5 = balanced_scalars[g * 6 + 5];
        uint64_t v[6] = {v0, v1, v2, v3, v4, v5};
        htp_line_sums_t sums = htp_line_sums(v, modulus_M);
        uint64_t L1 = sums.l1;
        uint64_t L2 = sums.l2;
        uint64_t L3 = sums.l3;
        /* Topology-preserving invariant signature:
         * encode the failure residuals (L1-L2, L2-L3) into a single scalar.
         * Multiplication in mod M is deterministic and sensitive to both
         * residuals without arbitrary constants. A passing group has
         * r12=r23=0, so its aggregate is 0. */
        uint64_t r12 = (L1 + modulus_M - L2) % modulus_M;
        uint64_t r23 = (L2 + modulus_M - L3) % modulus_M;
        group_agg[g] = (r12 * r23) % modulus_M;
    }

    for (int s = 0; s < suspect_count; s++) {
        int g = suspects[s].chunk_index / 6;
        if (g >= 0 && g < group_count) {
            group_suspect_mask[g] |= (1 << (suspects[s].chunk_index % 6));
        }
    }

    htp_suspect_t *orig = (htp_suspect_t *)cwist_alloc((size_t)suspect_count * sizeof(htp_suspect_t));
    memcpy(orig, suspects, (size_t)suspect_count * sizeof(htp_suspect_t));

    int next_count = 0;
    int l1_group_count = (group_count + 5) / 6;
    for (int lg = 0; lg < l1_group_count; lg++) {
        uint64_t lv[6] = {0};
        int lidx[6];
        int lgs = 0;
        for (int v = 0; v < 6; v++) {
            int g = lg * 6 + v;
            if (g < group_count) {
                lv[v] = group_agg[g];
                lidx[v] = g;
                lgs++;
            }
        }
        if (lgs < 3) continue;

        htp_line_sums_t level_sums = htp_line_sums(lv, modulus_M);
        uint64_t LL1 = level_sums.l1;
        uint64_t LL2 = (lgs > 3) ? level_sums.l2 : LL1;
        uint64_t LL3 = (lgs > 5) ? level_sums.l3 : LL1;

        if (LL1 == LL2 && LL2 == LL3) continue;

        /* Keep suspects from failing level-1 super-group */
        for (int v = 0; v < lgs; v++) {
            int g = lidx[v];
            for (int s = 0; s < 6; s++) {
                if (group_suspect_mask[g] & (1 << s)) {
                    int ci = g * 6 + s;
                    bool already = false;
                    for (int k = 0; k < next_count; k++) {
                        if (suspects[k].chunk_index == ci) { already = true; break; }
                    }
                    if (!already) {
                        /* Preserve original score */
                        double preserved_score = 0.0;
                        for (int j = 0; j < suspect_count; j++) {
                            if (orig[j].chunk_index == ci) {
                                preserved_score = orig[j].suspicion_score;
                                break;
                            }
                        }
                        suspects[next_count].chunk_index = ci;
                        suspects[next_count].suspicion_score = preserved_score;
                        next_count++;
                    }
                }
            }
        }
    }

    cwist_free(group_agg);
    cwist_free(group_suspect_mask);
    cwist_free(orig);
    return next_count;
}

static void upload_work_func(void *arg) {
    upload_work_t *w = (upload_work_t *)arg;

    long long store_start_ms = tasfa_monotonic_ms();
    int fd = open(w->temp_path, O_WRONLY);
    if (fd < 0) {
        FLY_LOG_ERROR("[TASFA] chunk open failed: %s errno=%d", w->temp_path, errno);
        return;
    }
    if (w->encrypted_stream) {
        size_t encrypted_plain_len = w->body_size > 16 ? w->body_size - 16 : 0;
        unsigned char *plaintext = ensure_decrypt_buf(w->compressed ? encrypted_plain_len : (size_t)w->expected_size);
        if (plaintext && encrypted_plain_len > 0 &&
            decrypt_stream_block(w->stream_key, w->stream_iv_seed, w->chunk_index, w->upload_id,
                                 (const unsigned char *)w->body_data, w->body_size,
                                 plaintext, encrypted_plain_len)) {
            if (w->compressed) {
                unsigned char *inflated = ensure_inflate_buf((size_t)w->expected_size);
                if (inflated && tasfa_gzip_decompress_to(plaintext, encrypted_plain_len, inflated, (size_t)w->expected_size)) {
                    w->stored = pwrite_all(fd, inflated, (size_t)w->expected_size, (off_t)w->offset);
                }
            } else if (encrypted_plain_len == (size_t)w->expected_size) {
                w->stored = pwrite_all(fd, plaintext, (size_t)w->expected_size, (off_t)w->offset);
            }
        }
    } else if (w->compressed) {
        unsigned char *inflated = ensure_inflate_buf((size_t)w->expected_size);
        if (inflated && tasfa_gzip_decompress_to((const unsigned char *)w->body_data, w->body_size,
                                                 inflated, (size_t)w->expected_size)) {
            w->stored = pwrite_all(fd, inflated, (size_t)w->expected_size, (off_t)w->offset);
        }
    } else if ((long long)w->body_size == w->expected_size) {
        w->stored = pwrite_all(fd, w->body_data, w->body_size, (off_t)w->offset);
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
    w->state_ok = mark_chunk_received_in_session_state(w->upload_id, w->chunk_index);
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

    bool is_retry_target = false;
    if (is_chunk_already_received(upload_id, chunk_index)) {
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

    long long offset = (long long)chunk_index * (long long)mbin.chunk_size;
    long long expected_size = mbin.total_size - offset;
    if (expected_size > mbin.chunk_size) expected_size = mbin.chunk_size;
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
    bool compressed_chunk = content_encoding && str_contains_ci_local(content_encoding, "gzip");
    long long expected_body_size = encrypted_stream ? expected_size + 16 : expected_size;
    bool size_ok = compressed_chunk
        ? (req->body->size > (encrypted_stream ? 16U : 0U) && (long long)req->body->size <= expected_body_size)
        : ((long long)req->body->size == expected_body_size);
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
    work.offset = offset;
    work.expected_size = expected_size;
    work.chunk_count = mbin.chunk_count;

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

static bool tasfa_generate_htp_metadata_for_file(const char *file_path, int chunk_size, uint64_t modulus_M, const char *media_name) {
    if (!file_path || chunk_size <= 0 || modulus_M <= 1) return false;
    FILE *f = fopen(file_path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long long total_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char htp_dir[PATH_MAX];
    snprintf(htp_dir, sizeof(htp_dir), "data/tasfa/media_htp/%s", media_name);
    if (!dir_ensure("data/tasfa/media_htp") || !dir_ensure(htp_dir)) {
        fclose(f);
        return false;
    }

    int chunk_count = (int)((total_size + (long long)chunk_size - 1) / (long long)chunk_size);
    unsigned char *buf = (unsigned char *)cwist_alloc((size_t)chunk_size);
    if (!buf) { fclose(f); return false; }

    for (int i = 0; i < chunk_count; i++) {
        size_t n = fread(buf, 1, (size_t)chunk_size, f);
        if (n <= 0) break;
        unsigned char hash[32];
        SHA256(buf, n, hash);
        char hash_hex[65];
        for (int j = 0; j < 32; j++) snprintf(hash_hex + (j * 2), 3, "%02x", hash[j]);
        uint64_t raw_scalar = 0;
        memcpy(&raw_scalar, hash, 8);
        raw_scalar %= modulus_M;
        save_htp_scalar_to_dir(htp_dir, i, hash_hex, raw_scalar, raw_scalar);
    }
    cwist_free(buf);
    fclose(f);
    return true;
}

static bool is_client_connected(cwist_http_request *req) {
    if (!req || req->client_fd < 0) return true; /* background worker: no fd, assume ok */
    char buf;
    ssize_t r = recv(req->client_fd, &buf, 1, MSG_PEEK | MSG_DONTWAIT);
    if (r == 0) {
        return false; /* EOF: client closed connection */
    }
    if (r < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            return true; /* Still connected, no data */
        }
        return false; /* Client disconnected due to error */
    }
    return true; /* Data available, client still connected */
}

static bool copy_file_chunked(const char *src, const char *dst) {
    FILE *fsrc = fopen(src, "rb");
    if (!fsrc) return false;
    FILE *fdst = fopen(dst, "wb");
    if (!fdst) {
        fclose(fsrc);
        return false;
    }
    char buf[65536];
    size_t n;
    bool ok = true;
    while ((n = fread(buf, 1, sizeof(buf), fsrc)) > 0) {
        if (fwrite(buf, 1, n, fdst) != n) {
            ok = false;
            break;
        }
    }
    fclose(fsrc);
    fclose(fdst);
    return ok;
}

static int rename_fallback(const char *src, const char *dst) {
    if (rename(src, dst) == 0) return 0;
    if (errno == EXDEV) {
        if (copy_file_chunked(src, dst)) {
            unlink(src);
            return 0;
        }
    }
    return -1;
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
    const char *temp_path = json_string(meta, "temp_path", "");
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
        int group_count = chunk_count / 6;
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
            int group_count = chunk_count / 6;
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
    snprintf(final_path, sizeof(final_path), "public/uploads/%ld_%s", (long)time(NULL), filename);
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
    int fid = db_file_create_volume_get_id(req->db, post_id, owner_uid, filename, mime_buf, final_path, (size_t)json_long_long(meta, "total_size", 0));
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
        snprintf(thumb_path, sizeof(thumb_path), "public/uploads/.thumbs/%d.webp", fid);
        if (generate_image_thumb(final_path, thumb_path, 1280, 1280)) {
            char media_name[64]; snprintf(media_name, sizeof(media_name), "thumb_%d", fid);
            tasfa_generate_htp_metadata_for_file(thumb_path, TASFA_DOWNLOAD_CHUNK_SIZE_DEFAULT, HTP_MODULUS_STABLE, media_name);
        } else thumb_path[0] = '\0';
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
    cJSON_AddStringToObject(obj, "filename", filename);
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

typedef struct {
    cwist_db *db;
    char upload_id[33];
    char upload_token[49];
} upload_finalize_job_t;

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
                if (w < 50) w = 50;
                if (w > 2048) w = 2048;
                if (h < 50) h = 50;
                if (h > 2048) h = 2048;

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

    bool ok = send_file_slice_response(req, res, json_string(meta, "storage_path", ""),
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
        send_json_response(res, session_error_json("download not found"), CWIST_HTTP_NOT_FOUND);
    }
}

void handler_asset_tasfa_handshake(cwist_http_request *req, cwist_http_response *res) {
    char path[PATH_MAX], filename[512];
    const char *mime = NULL;
    if (!resolve_asset_scope_path(req->db, cwist_query_map_get(req->path_params, "scope"),
                                  cwist_query_map_get(req->path_params, "filename"),
                                  path, sizeof(path), filename, sizeof(filename), &mime)) {
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
                if (w < 50) w = 50;
                if (w > 2048) w = 2048;
                if (h < 50) h = 50;
                if (h > 2048) h = 2048;

                char scope_fname[512] = {0};
                const char *scope = cwist_query_map_get(req->path_params, "scope");
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
    const char *scope = cwist_query_map_get(req->path_params, "scope");
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

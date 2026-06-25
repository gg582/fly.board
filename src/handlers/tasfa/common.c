#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "tasfa_internal.h"

int g_media_concurrency = 0;
pthread_mutex_t g_media_mtx = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t g_media_cond = PTHREAD_COND_INITIALIZER;

long long tasfa_monotonic_ms(void) {
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

int choose_chunk_size_upload(bool mobile, int requested) {
    int fallback = mobile ? TASFA_UPLOAD_CHUNK_SIZE_MOBILE : TASFA_UPLOAD_CHUNK_SIZE_DEFAULT;
    int max_value = mobile ? TASFA_UPLOAD_CHUNK_SIZE_MOBILE_MAX : TASFA_UPLOAD_CHUNK_SIZE_MAX;
    return normalize_chunk_size_hint(requested, fallback, TASFA_UPLOAD_CHUNK_SIZE_MIN, max_value);
}

int tasfa_upload_session_limit(void) {
    int limit = g_config.max_total_parallel_uploads;
    if (limit < 1) limit = 1;
    if (limit > TASFA_MAX_CONCURRENT_UPLOADS) limit = TASFA_MAX_CONCURRENT_UPLOADS;
    return limit;
}

int tasfa_download_session_limit(void) {
    int limit = g_config.max_concurrent_downloads;
    if (limit < 1) limit = 1;
    if (limit > TASFA_MAX_CONCURRENT_DOWNLOADS) limit = TASFA_MAX_CONCURRENT_DOWNLOADS;
    return limit;
}

int tasfa_upload_parallel_limit(void) {
    int limit = g_config.max_upload_parallel_chunks;
    if (limit < 1) limit = 1;
    if (limit > TASFA_UPLOAD_MAX_PARALLEL) limit = TASFA_UPLOAD_MAX_PARALLEL;
    return limit;
}

int fast_download_chunk_max_from_request(const char *downlink_mbps, const char *rtt_ms, const char *effective_type, const char *save_data) {
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

int choose_chunk_size_download(bool mobile, int requested, long long total_size, int fast_link_max) {
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

void add_keepalive_headers(cwist_http_response *res) {
    /* Connection and Keep-Alive are handled globally by global_middleware. */
    (void)res;
}

void send_queued_json(cwist_http_response *res, const char *msg, int retry_after) {
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

void send_json_response(cwist_http_response *res, cJSON *obj, int status_code) {
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

cJSON *session_error_json(const char *error) {
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddBoolToObject(obj, "ok", false);
    cJSON_AddStringToObject(obj, "error", error ? error : "unknown error");
    return obj;
}

bool is_safe_segment(const char *value) {
    if (!value || !value[0]) return false;
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p) {
        if (!(isalnum(*p) || *p == '-' || *p == '_' || *p == '.')) return false;
    }
    return true;
}

bool is_safe_filename_simple(const char *name) {
    return name && name[0] && strchr(name, '/') == NULL && strchr(name, '\\') == NULL;
}

bool is_safe_upload_asset_name(const char *name) {
    if (is_safe_filename_simple(name)) return true;
    if (!name) return false;
    if (strncmp(name, ".thumbs/", 8) == 0) return is_safe_filename_simple(name + 8);
    if (strncmp(name, ".previews/", 10) == 0) return is_safe_filename_simple(name + 10);
    return false;
}

char *decode_segment(const char *src) {
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

const char *json_string(cJSON *obj, const char *key, const char *def) {
    cJSON *item = cJSON_GetObjectItem(obj, key);
    if (item && cJSON_IsString(item) && item->valuestring) return item->valuestring;
    return def;
}

long long json_long_long(cJSON *obj, const char *key, long long def) {
    cJSON *item = cJSON_GetObjectItem(obj, key);
    if (!item) return def;
    if (cJSON_IsNumber(item)) return (long long)item->valuedouble;
    if (cJSON_IsString(item) && item->valuestring) return atoll(item->valuestring);
    return def;
}

double json_double(cJSON *obj, const char *key, double def) {
    cJSON *item = cJSON_GetObjectItem(obj, key);
    if (!item) return def;
    if (cJSON_IsNumber(item)) return item->valuedouble;
    if (cJSON_IsString(item) && item->valuestring) return atof(item->valuestring);
    return def;
}

bool random_hex(char *out, size_t bytes_len) {
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

char *hex_encode_alloc(const unsigned char *data, size_t len) {
    if (!data || !len) return NULL;
    char *out = (char *)cwist_alloc((len * 2) + 1);
    if (!out) return NULL;
    for (size_t i = 0; i < len; i++) snprintf(out + (i * 2), 3, "%02x", data[i]);
    out[len * 2] = '\0';
    return out;
}

int link_score_from_inputs(const char *score_str, const char *effective_type, const char *downlink_str,
                           const char *rtt_str, const char *retry_str, const char *timeout_str, const char *save_data_str) {
    return media_quality_score_from_link(score_str, effective_type, downlink_str, rtt_str, retry_str, timeout_str, save_data_str);
}

void choose_upload_window(bool mobile, int score, int *initial_parallel, int *max_parallel, int *pacing_ms) {
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

void choose_download_profile(bool mobile, int score, int *initial_parallel, int *max_parallel, int *pacing_ms, int *coalesce_chunks) {
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

bool str_contains_ci_local(const char *haystack, const char *needle) {
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

bool is_client_connected(cwist_http_request *req) {
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

int rename_fallback(const char *src, const char *dst) {
    if (rename(src, dst) == 0) return 0;
    if (errno == EXDEV) {
        if (copy_file_chunked(src, dst)) {
            unlink(src);
            return 0;
        }
    }
    return -1;
}

bool sha256_file(const char *path, unsigned char out[32]) {
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

int clamp_int(int value, int min_value, int max_value) {
    if (value < min_value) return min_value;
    if (value > max_value) return max_value;
    return value;
}

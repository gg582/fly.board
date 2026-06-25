#ifndef TASFA_INTERNAL_H
#define TASFA_INTERNAL_H

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "../handlers_internal.h"
#include "../../utils/media_preview.h"

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

/* --- Constants --- */

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

#define TASFA_RTT_MAX_SAMPLES 8
#define TASFA_RTT_CLAMP_MAX   30000.0

#define HTP_REPAIR_CPU_PER_SUSPECT  2048.0
#define HTP_REPAIR_CPU_PER_L0_GROUP 512.0

#define TASFA_MAX_WORKERS 64

/* --- Shared structs --- */

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

typedef struct {
    char id[64];
    time_t ts;
} queue_slot_t;

typedef struct {
    char upload_id[33];
    char upload_token[49];
    bool active;
    bool done;
    int status_code;
    char *body;
    time_t expires;
} finalize_slot_t;

typedef struct tasfa_job {
    void (*func)(void *);
    void *arg;
    bool free_after_done;
} tasfa_job_t;

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
    bool is_parity;
    int data_chunks;
    /* outputs */
    bool stored;
    bool state_ok;
    long long store_ms;
    long long state_ms;
} upload_work_t;

typedef struct {
    int chunk_index;
    double suspicion_score;
} htp_suspect_t;

typedef struct {
    uint64_t l1;
    uint64_t l2;
    uint64_t l3;
} htp_line_sums_t;

typedef struct {
    cwist_db *db;
    char upload_id[33];
    char upload_token[49];
} upload_finalize_job_t;

/* --- Shared global variables (defined in one .c file each) --- */

extern int g_media_concurrency;
extern pthread_mutex_t g_media_mtx;
extern pthread_cond_t g_media_cond;

extern cache_slot_t g_tasfa_cache[TASFA_CACHE_SLOTS];
extern pthread_mutex_t g_tasfa_cache_mtx;

extern queue_slot_t g_q_uploads[TASFA_MAX_CONCURRENT_UPLOADS];
extern queue_slot_t g_q_downloads[TASFA_MAX_CONCURRENT_DOWNLOADS];
extern pthread_mutex_t g_tasfa_queue_mtx;

extern finalize_slot_t g_finalize_slots[TASFA_FINALIZE_CACHE_SLOTS];
extern pthread_mutex_t g_finalize_mtx;

extern ttak_thread_pool_t *g_tasfa_pool;
extern pthread_once_t g_scheduler_once;
extern volatile unsigned int g_round_robin_idx;

/* --- Function declarations --- */

/* common.c */
long long tasfa_monotonic_ms(void);
int choose_chunk_size_upload(bool mobile, int requested);
int tasfa_upload_session_limit(void);
int tasfa_download_session_limit(void);
int tasfa_upload_parallel_limit(void);
int fast_download_chunk_max_from_request(const char *downlink_mbps, const char *rtt_ms, const char *effective_type, const char *save_data);
int choose_chunk_size_download(bool mobile, int requested, long long total_size, int fast_link_max);
void add_keepalive_headers(cwist_http_response *res);
void send_queued_json(cwist_http_response *res, const char *msg, int retry_after);
void send_json_response(cwist_http_response *res, cJSON *obj, int status_code);
cJSON *session_error_json(const char *error);
bool is_safe_segment(const char *value);
bool is_safe_filename_simple(const char *name);
bool is_safe_upload_asset_name(const char *name);
char *decode_segment(const char *src);
const char *json_string(cJSON *obj, const char *key, const char *def);
long long json_long_long(cJSON *obj, const char *key, long long def);
double json_double(cJSON *obj, const char *key, double def);
bool random_hex(char *out, size_t bytes_len);
char *hex_encode_alloc(const unsigned char *data, size_t len);
int link_score_from_inputs(const char *score_str, const char *effective_type, const char *downlink_str,
                           const char *rtt_str, const char *retry_str, const char *timeout_str, const char *save_data_str);
void choose_upload_window(bool mobile, int score, int *initial_parallel, int *max_parallel, int *pacing_ms);
void choose_download_profile(bool mobile, int score, int *initial_parallel, int *max_parallel, int *pacing_ms, int *coalesce_chunks);
bool is_client_connected(cwist_http_request *req);
bool str_contains_ci_local(const char *haystack, const char *needle);
int rename_fallback(const char *src, const char *dst);
bool sha256_file(const char *path, unsigned char out[32]);
int clamp_int(int value, int min_value, int max_value);

/* crypto.c */
bool encrypt_stream_block(const unsigned char *key, const unsigned char *iv_seed, int chunk_index,
                          const char *session_id, const unsigned char *plaintext, size_t plaintext_len,
                          unsigned char *ciphertext, size_t *ciphertext_len_out);
bool decrypt_stream_block(const unsigned char *key, const unsigned char *iv_seed, int chunk_index,
                          const char *upload_id, const unsigned char *ciphertext, size_t ciphertext_len,
                          unsigned char *plaintext, size_t plaintext_len);
bool tasfa_gzip_compress_alloc(const unsigned char *input, size_t input_len,
                               unsigned char **out, size_t *out_len);
bool tasfa_gzip_decompress_to(const unsigned char *input, size_t input_len,
                              unsigned char *out, size_t expected_len);
unsigned char *ensure_decrypt_buf(size_t need);
unsigned char *ensure_inflate_buf(size_t need);
char *ensure_read_buf(size_t need);

/* queue.c */
bool tasfa_queue_try_enter(queue_slot_t *slots, int max_slots, const char *id);
void tasfa_queue_leave(queue_slot_t *slots, int max_slots, const char *id);
void tasfa_queue_touch(queue_slot_t *slots, int max_slots, const char *id);
void tasfa_queue_sweep(queue_slot_t *slots, int max_slots, int ttl);

/* cache.c */
void cache_invalidate(const char *key);
bool load_upload_session_meta_bin_cached(const char *upload_id, tasfa_meta_bin_t *out);
cJSON *load_download_session_cached(const char *session_id);
bool finalize_cache_get(const char *upload_id, const char *upload_token, int *status_code, char **body, bool *active);
bool finalize_cache_mark_started(const char *upload_id, const char *upload_token);
void finalize_cache_mark_done(const char *upload_id, int status_code, const char *body);
void finalize_cache_update_status(const char *upload_id, const char *msg);

/* session.c */
void upload_session_dir(char *out, size_t out_len, const char *upload_id);
void upload_session_meta_path(char *out, size_t out_len, const char *upload_id);
void upload_session_state_path(char *out, size_t out_len, const char *upload_id);
void upload_session_meta_bin_path(char *out, size_t out_len, const char *upload_id);
void upload_session_state_bin_path(char *out, size_t out_len, const char *upload_id);
void upload_session_temp_path(char *out, size_t out_len, const char *upload_id);
void download_session_dir(char *out, size_t out_len, const char *session_id);
void download_session_meta_path(char *out, size_t out_len, const char *session_id);
bool ensure_tasfa_roots(void);
bool ensure_preallocated_file(const char *path, long long total_size);
int open_upload_session_lock(const char *upload_id);
int open_upload_session_lock_sh(const char *upload_id);
void close_upload_session_lock(int lock_fd);
int bitmap_count_set(const char *bitmap, int len);
char *bitmap_create(int chunk_count);
bool save_upload_session_state_bin(const char *upload_id, int chunk_count, const char *bitmap);
char* load_upload_session_state_bin(const char *upload_id, int chunk_count);
bool mark_chunk_received_in_session_state(const char *upload_id, int chunk_index);
bool is_chunk_already_received(const char *upload_id, int chunk_index);
bool save_upload_session_meta_bin(const char *upload_id, tasfa_meta_bin_t *meta);
bool load_upload_session_meta_bin(const char *upload_id, tasfa_meta_bin_t *out);
cJSON *load_upload_session(const char *upload_id);
bool save_upload_session(const char *upload_id, cJSON *root);
bool save_upload_session_state(const char *upload_id, cJSON *root);
cJSON *load_download_session(const char *session_id);
bool save_download_session(const char *session_id, cJSON *root);
void cleanup_upload_session(const char *upload_id);
void cleanup_download_session(const char *session_id);
cJSON *build_upload_status_json(cJSON *meta, const char *upload_id);
bool init_download_session(const char *filename, const char *mime, const char *storage_path, long long total_size,
                           bool mobile, int score, int requested_chunk_size, int fast_link_max, int file_id,
                           const char *media_name, cJSON **out);
bool send_file_slice_response(cwist_http_request *req, cwist_http_response *res, const char *path, const char *mime,
                              long long offset, size_t amount, int chunk_index, int chunk_count, int span);
bool resolve_asset_scope_path(cwist_db *db, const char *scope, const char *encoded, char *storage_path,
                              size_t storage_len, char *filename, size_t filename_len, const char **mime_out);
bool pwrite_all(int fd, const void *buf, size_t len, off_t offset);

/* scheduler.c */
void tasfa_scheduler_submit(const char *upload_id, void (*func)(void *), void *arg, tasfa_job_t *job);

/* htp.c */
double predict_remaining_ms(cJSON *meta);
void append_rtt_sample(cJSON *meta, int chunk_index, double rtt_ms);
const char *htp_simd_backend(void);
htp_line_sums_t htp_line_sums(const uint64_t v[6], uint64_t modulus_M);
bool htp_try_swap_repair(const char *upload_id, uint64_t *balanced_scalars,
                         uint64_t *raw_scalars, char *tags, int group_start, uint64_t modulus_M);
void htp_analyze_group(uint64_t v[6], uint64_t modulus_M, int group_start,
                       htp_suspect_t *out, int *out_count);
int compare_suspect_desc(const void *a, const void *b);
bool htp_repair_worthwhile(int suspect_count, int total_chunks, int chunk_size, double rtt_ms);
int htp_contract_groups(const uint64_t *balanced_scalars, int chunk_count, uint64_t modulus_M,
                        htp_suspect_t *suspects, int suspect_count);
bool save_htp_scalar_to_dir(const char *dir_path, int chunk_index, const char *hash_tag_hex,
                            uint64_t raw_scalar, uint64_t balanced_scalar);
bool save_htp_scalar(const char *upload_id, int chunk_index, const char *hash_tag_hex,
                     uint64_t raw_scalar, uint64_t balanced_scalar);
bool load_htp_scalars(const char *upload_id, int chunk_count, char **hash_tags_out,
                      uint64_t **raw_scalars_out, uint64_t **balanced_scalars_out);
bool perform_xor_recovery(const char *upload_id, const char *temp_path, int chunk_size,
                          int group_start, int group_end, int target_chunk, int parity_chunk_idx,
                          int data_chunks, long long total_size);
bool tasfa_generate_htp_metadata_for_file(const char *file_path, int chunk_size, uint64_t modulus_M,
                                          const char *media_name);

/* upload.c */
void handler_file_upload_init(cwist_http_request *req, cwist_http_response *res);
void handler_file_upload_status(cwist_http_request *req, cwist_http_response *res);
void handler_file_upload_renegotiate(cwist_http_request *req, cwist_http_response *res);
void handler_file_upload(cwist_http_request *req, cwist_http_response *res);
void handler_file_upload_complete(cwist_http_request *req, cwist_http_response *res);
void handler_file_upload_cancel(cwist_http_request *req, cwist_http_response *res);

/* download.c */
void handler_file_download_handshake(cwist_http_request *req, cwist_http_response *res);
void handler_file_download_chunk(cwist_http_request *req, cwist_http_response *res);
void handler_file_download_complete(cwist_http_request *req, cwist_http_response *res);

/* asset.c */
void handler_asset_tasfa_handshake(cwist_http_request *req, cwist_http_response *res);
void handler_asset_tasfa_chunk(cwist_http_request *req, cwist_http_response *res);

#endif

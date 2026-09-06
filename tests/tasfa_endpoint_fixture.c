/* Test adapter only: real TASFA codec/crypto and slice-response bodies are
 * inserted by tools/benchmark_tasfa_compression.py. No production server,
 * routing, authentication, or CWIST allocator is mocked as being tested. */
#define _POSIX_C_SOURCE 200809L
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <limits.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <openssl/evp.h>
#include <zstd.h>
#include <zlib.h>
#include <brotli/encode.h>
#include <brotli/decode.h>

static _Thread_local uint64_t calls[3], input_bytes[3], codec_ns[3];
static uint64_t tick(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ULL + (uint64_t)t.tv_nsec;
}
static BROTLI_BOOL measured_br(int q, int w, BrotliEncoderMode m, size_t n,
                              const uint8_t *p, size_t *out_n, uint8_t *out) {
    uint64_t start = tick();
    BROTLI_BOOL rc = BrotliEncoderCompress(q, w, m, n, p, out_n, out);
    calls[0]++; input_bytes[0] += n; codec_ns[0] += tick() - start;
    return rc;
}
static size_t measured_zstd(void *out, size_t cap, const void *p, size_t n, int level) {
    uint64_t start = tick();
    size_t rc = ZSTD_compress(out, cap, p, n, level);
    calls[1]++; input_bytes[1] += n; codec_ns[1] += tick() - start;
    return rc;
}
static int measured_gzip(z_streamp s, int flush) {
    uint64_t start = tick();
    size_t n = s->avail_in;
    int rc = deflate(s, flush);
    calls[2]++; input_bytes[2] += n; codec_ns[2] += tick() - start;
    return rc;
}
#define BrotliEncoderCompress measured_br
#define ZSTD_compress measured_zstd
#define deflate measured_gzip
#define cwist_alloc malloc
#define cwist_free free
#define TASFA_INTERNAL_H
/* Generated verbatim enum/constants, followed by entire production crypto.c. */
#include "tasfa_fixture_crypto.inc"

typedef struct { char key[64]; char value[256]; } header;
typedef struct { header entries[32]; size_t count; } headers;
typedef struct { char *data; size_t length; } string;
typedef struct { headers *headers; const char *query_params; } cwist_http_request;
typedef struct { headers *headers; string *body; int status_code; } cwist_http_response;
typedef struct { int unused; } cJSON;
#define CWIST_HTTP_OK 200
#define HTP_TAG_LEN 129
#define HTP_RECORD_SIZE (HTP_TAG_LEN + 16)
static const char *cwist_http_header_get(headers *h, const char *key) {
    for (size_t i = 0; h && i < h->count; i++)
        if (strcasecmp(h->entries[i].key, key) == 0) return h->entries[i].value;
    return NULL;
}
static void cwist_http_header_add(headers **h, const char *key, const char *value) {
    if (!*h || (*h)->count >= 32) abort();
    header *v = &(*h)->entries[(*h)->count++];
    snprintf(v->key, sizeof(v->key), "%s", key);
    snprintf(v->value, sizeof(v->value), "%s", value);
}
static void cwist_sstring_assign(string *s, const char *p) {
    free(s->data); s->length = strlen(p); s->data = strdup(p);
    if (!s->data) abort();
}
static void cwist_sstring_append_len(string *s, const char *p, size_t n) {
    char *next = realloc(s->data, s->length + n + 1);
    if (!next) abort();
    s->data = next; memcpy(s->data + s->length, p, n);
    s->length += n; s->data[s->length] = 0;
}
static bool str_contains_ci_local(const char *s, const char *needle) {
    for (; *s; s++) if (strncasecmp(s, needle, strlen(needle)) == 0) return true;
    return false;
}
static void add_keepalive_headers(cwist_http_response *r) { (void)r; }
static const char *cwist_query_map_get(const char *q, const char *key) {
    return strcmp(key, "session_id") == 0 ? q : NULL;
}
static void download_session_dir(char *p, size_t n, const char *id) {
    (void)id; snprintf(p, n, "/dev/null"); /* no HTP file in this adapter */
}
static cJSON *load_download_session_cached(const char *id) {
    (void)id; return calloc(1, sizeof(cJSON));
}
static const char *json_string(cJSON *j, const char *key, const char *fallback) {
    (void)j;
    /* Public, fixed test vectors. Never real session credentials. */
    if (strcmp(key, "stream_key_hex") == 0)
        return "4242424242424242424242424242424242424242424242424242424242424242";
    if (strcmp(key, "stream_iv_seed_hex") == 0) return "242424242424242424242424" "24";
    return fallback;
}
static void cJSON_Delete(cJSON *j) { free(j); }
/* Generated verbatim send_file_slice_response from production session.c. */
#include "tasfa_fixture_response.inc"

typedef struct {
    void *body;
    size_t length;
    char encoding[16];
    size_t uncompressed_length;
    int encrypted;
    int headers_ok;
    uint64_t codec_calls[3], codec_bytes[3], codec_time_ns[3];
} fixture_response;

int fixture_serve(const char *path, size_t offset, size_t n, const char *mime,
                  const char *accept, int encrypted, fixture_response *out) {
    headers request_headers = {0}, response_headers = {0};
    headers *rh = &request_headers;
    cwist_http_header_add(&rh, "X-TASFA-Accept-Encoding", accept);
    string body = {0};
    cwist_http_request request = {rh, encrypted ? "fixture-session" : NULL};
    cwist_http_response response = {&response_headers, &body, 0};
    memset(calls, 0, sizeof(calls)); memset(input_bytes, 0, sizeof(input_bytes));
    memset(codec_ns, 0, sizeof(codec_ns)); memset(out, 0, sizeof(*out));
    bool ok = send_file_slice_response(&request, &response, path, mime,
                                      (long long)offset, n, 2, 4, 1);
    free(g_tasfa_read_buf); g_tasfa_read_buf = NULL; g_tasfa_read_buf_size = 0;
    if (!ok) { free(body.data); return 0; }
    out->body = body.data; out->length = body.length;
    const char *encoding = cwist_http_header_get(response.headers, "X-TASFA-Content-Encoding");
    const char *plain_n = cwist_http_header_get(response.headers, "X-TASFA-Uncompressed-Length");
    const char *content_n = cwist_http_header_get(response.headers, "Content-Length");
    const char *stream = cwist_http_header_get(response.headers, "X-TASFA-Stream-Mode");
    if (encoding) snprintf(out->encoding, sizeof(out->encoding), "%s", encoding);
    out->uncompressed_length = plain_n ? strtoull(plain_n, NULL, 10) : 0;
    out->encrypted = stream && strcmp(stream, "aes-256-gcm") == 0;
    out->headers_ok = content_n && strtoull(content_n, NULL, 10) == body.length &&
        (!!encoding == !!plain_n) && (!encoding || out->uncompressed_length == n) &&
        (out->encrypted == encrypted) && response.status_code == 200;
    memcpy(out->codec_calls, calls, sizeof(calls));
    memcpy(out->codec_bytes, input_bytes, sizeof(input_bytes));
    memcpy(out->codec_time_ns, codec_ns, sizeof(codec_ns));
    return 1;
}
void fixture_free(void *p) { free(p); }

int fixture_verify(const void *wire, size_t wire_n, const char *encoding,
                   int encrypted, const void *expected, size_t n) {
    unsigned char *decrypted = malloc(wire_n ? wire_n : 1);
    unsigned char *plain = malloc(n ? n : 1);
    if (!decrypted || !plain) { free(decrypted); free(plain); return 0; }
    const unsigned char *payload = wire;
    size_t payload_n = wire_n;
    bool ok = true;
    if (encrypted) {
        unsigned char key[32], iv[12]; memset(key, 0x42, sizeof(key)); memset(iv, 0x24, sizeof(iv));
        ok = wire_n >= 16 && decrypt_stream_block(key, iv, 2, "fixture-session", wire, wire_n,
                                                decrypted, wire_n - 16);
        payload = decrypted; payload_n = wire_n >= 16 ? wire_n - 16 : 0;
    }
    if (*encoding) {
        tasfa_compress_type_t type = strcmp(encoding, "br") == 0 ? TASFA_COMPRESS_BROTLI :
            strcmp(encoding, "zstd") == 0 ? TASFA_COMPRESS_ZSTD : TASFA_COMPRESS_GZIP;
        ok = ok && tasfa_decompress_to(payload, payload_n, plain, n, type);
        payload = plain; payload_n = n;
    }
    ok = ok && payload_n == n && memcmp(payload, expected, n) == 0;
    free(decrypted); free(plain); return ok;
}

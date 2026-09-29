#include "engine/bdr.h"
#include "auth/auth.h"
#include "handlers/handlers_internal.h"
#include <cwist/core/log.h>
#include <cwist/sys/app/big_dumb_reply.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <time.h>

#define BDR_MAGIC 0x52444246u /* "FBDR" */
#define BDR_BODY_MAX ((size_t)8 << 20)

static cwist_bdr_t *g_bdr = NULL;
/* MAP_SHARED before fork: every worker process sees every bump. */
static _Atomic uint64_t *g_version = NULL;
static time_t g_ttl_sec = 30;

void engine_bdr_init(cwist_app *app) {
    g_bdr = app ? app->bdr_ctx : NULL;
    void *mem = mmap(NULL, sizeof(*g_version), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS,
                     -1, 0);
    if (mem == MAP_FAILED) {
        /* Without a shared counter, other processes could not see a bump. */
        FLY_LOG_ERROR("BDR: shared version counter unavailable; route BDR disabled");
        g_bdr = NULL;
        return;
    }
    g_version = mem;
    atomic_init(g_version, 1);
    const char *ttl = getenv("FLY_BDR_TTL");
    if (ttl && ttl[0]) {
        char *end = NULL;
        long parsed = strtol(ttl, &end, 10);
        if (end && *end == '\0' && parsed >= 0 && parsed <= 86400) g_ttl_sec = (time_t)parsed;
    }
    if (g_ttl_sec == 0) g_bdr = NULL; /* FLY_BDR_TTL=0 turns the layer off. */
}

static uint64_t current_version(void) {
    return g_version ? atomic_load_explicit(g_version, memory_order_acquire) : 0;
}

uint64_t engine_bdr_version(void) {
    return current_version();
}

void engine_bdr_bump(void) {
    if (g_version) atomic_fetch_add_explicit(g_version, 1, memory_order_acq_rel);
}

/* Static files and TASFA transfers have their own caches or must never be
 * replayed; skip the key build for them outright. */
static bool path_is_excluded(const char *path) {
    static const char *const prefixes[] = {"/assets/", "/js/", "/sw.js", "/__tasfa_stream__/",
                                           "/file/download", "/file/preview", "/file/upload",
                                           "/api/", "/metrics"};
    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
        if (strncmp(path, prefixes[i], strlen(prefixes[i])) == 0) return true;
    }
    return false;
}

static const char *header_or_empty(cwist_http_request *req, const char *name) {
    const char *v = cwist_http_header_get(req->headers, name);
    return v ? v : "";
}

bool engine_bdr_key(cwist_http_request *req, char *out, size_t out_len) {
    if (!g_bdr || !req || !req->path || !req->path->data) return false;
    if (req->method != CWIST_HTTP_GET && req->method != CWIST_HTTP_HEAD) return false;
    const char *path = req->path->data;
    if (path_is_excluded(path)) return false;
    /* Per-user pages and conditional/partial requests go to the route. */
    if (cwist_http_header_get(req->headers, "Authorization") ||
        cwist_http_header_get(req->headers, "If-None-Match") ||
        cwist_http_header_get(req->headers, "If-Modified-Since") ||
        cwist_http_header_get(req->headers, "Range") || auth_has_session_cookie(req)) {
        return false;
    }
    /* Everything below changes the rendered bytes or headers: query, the
     * negotiated encoding, theme/lang cookies, the mobile layout, the CORS
     * echo of Origin, and the scheme proxies report for absolute URLs. */
    int n = snprintf(out, out_len, "fb|%s?%s|ae=%s|c=%s|m=%d|o=%s|x=%s,%s,%s,%s,%s", path,
                     (req->query && req->query->data) ? req->query->data : "",
                     header_or_empty(req, "Accept-Encoding"), header_or_empty(req, "Cookie"),
                     is_mobile_request(req) ? 1 : 0, header_or_empty(req, "Origin"),
                     header_or_empty(req, "X-Forwarded-Proto"), header_or_empty(req, "Forwarded"),
                     header_or_empty(req, "CF-Visitor"), header_or_empty(req, "X-Forwarded-Ssl"),
                     header_or_empty(req, "X-Forwarded-Scheme"));
    return n > 0 && (size_t)n < out_len;
}

/* Blob layout (host byte order, same process family only):
 *   u32 magic | u64 version | i64 created | u32 status | u32 key_len | key
 *   u32 header_count | { u32 klen | key | u32 vlen | value }* | u64 body_len | body */

typedef struct {
    const unsigned char *p;
    const unsigned char *end;
} reader_t;

static bool rd(reader_t *r, void *dst, size_t n) {
    if ((size_t)(r->end - r->p) < n) return false;
    memcpy(dst, r->p, n);
    r->p += n;
    return true;
}

static bool rd_span(reader_t *r, const char **s, uint32_t *len) {
    if (!rd(r, len, sizeof(*len))) return false;
    if ((size_t)(r->end - r->p) < *len) return false;
    *s = (const char *)r->p;
    r->p += *len;
    return true;
}

static void clear_headers(cwist_http_response *res) {
    char name[256];
    while (res->headers && res->headers->key && res->headers->key->data) {
        snprintf(name, sizeof(name), "%s", res->headers->key->data);
        if (cwist_http_header_remove(&res->headers, name) == 0) break;
    }
}

bool engine_bdr_serve(cwist_http_request *req, cwist_http_response *res, const char *key) {
    if (!g_bdr) return false;
    size_t len = 0;
    bdr_blob_t *pin = NULL;
    const void *blob = cwist_bdr_get_pinned(g_bdr, "GET", key, &len, &pin);
    if (!blob || len == 0) {
        if (pin) cwist_bdr_unpin(pin);
        return false;
    }
    reader_t r = {blob, (const unsigned char *)blob + len};
    uint32_t magic = 0, status = 0, key_len = 0, count = 0;
    uint64_t version = 0, body_len = 0;
    int64_t created = 0;
    const char *stored_key = NULL;
    bool ok = rd(&r, &magic, sizeof(magic)) && magic == BDR_MAGIC &&
              rd(&r, &version, sizeof(version)) && rd(&r, &created, sizeof(created)) &&
              rd(&r, &status, sizeof(status)) && rd_span(&r, &stored_key, &key_len) &&
              key_len == strlen(key) && memcmp(stored_key, key, key_len) == 0 &&
              rd(&r, &count, sizeof(count));
    /* Stale after any content bump or past the TTL (view counts and other
     * counters render into pages without bumping). */
    if (ok && (version != current_version() || time(NULL) - (time_t)created > g_ttl_sec)) ok = false;
    const unsigned char *headers_at = r.p;
    for (uint32_t i = 0; ok && i < count; i++) {
        const char *k, *v;
        uint32_t kl, vl;
        ok = rd_span(&r, &k, &kl) && rd_span(&r, &v, &vl);
    }
    ok = ok && rd(&r, &body_len, sizeof(body_len)) && (size_t)(r.end - r.p) == body_len;
    if (!ok) {
        cwist_bdr_unpin(pin);
        return false;
    }
    const unsigned char *body = r.p;

    clear_headers(res);
    res->status_code = (cwist_http_status_t)status;
    r.p = headers_at;
    char kbuf[256];
    for (uint32_t i = 0; i < count; i++) {
        const char *k = NULL, *v = NULL;
        uint32_t kl = 0, vl = 0;
        /* Already bounds-checked by the validation pass above. */
        if (!rd_span(&r, &k, &kl) || !rd_span(&r, &v, &vl)) break;
        char *val = malloc((size_t)vl + 1);
        if (!val || kl >= sizeof(kbuf)) {
            free(val);
            continue;
        }
        memcpy(kbuf, k, kl);
        kbuf[kl] = '\0';
        memcpy(val, v, vl);
        val[vl] = '\0';
        cwist_http_header_add(&res->headers, kbuf, val);
        free(val);
    }
    cwist_http_header_add(&res->headers, "X-Fly-BDR", "hit");
    if (req->method == CWIST_HTTP_HEAD) {
        cwist_sstring_assign(res->body, "");
    } else {
        cwist_sstring_assign_len(res->body, (const char *)body, (size_t)body_len);
    }
    cwist_bdr_unpin(pin);
    return true;
}

typedef struct {
    unsigned char *buf;
    size_t len;
    size_t cap;
    bool failed;
} writer_t;

static void wr(writer_t *w, const void *src, size_t n) {
    if (w->failed) return;
    if (w->len + n > w->cap) {
        size_t cap = w->cap ? w->cap : 4096;
        while (cap < w->len + n) cap *= 2;
        unsigned char *grown = realloc(w->buf, cap);
        if (!grown) {
            w->failed = true;
            return;
        }
        w->buf = grown;
        w->cap = cap;
    }
    memcpy(w->buf + w->len, src, n);
    w->len += n;
}

static void wr_span(writer_t *w, const char *s, size_t n) {
    uint32_t len = (uint32_t)n;
    wr(w, &len, sizeof(len));
    wr(w, s, n);
}

void engine_bdr_store(cwist_http_request *req, cwist_http_response *res, const char *key,
                      uint64_t version) {
    if (!g_bdr || !req || !res || !key || !key[0]) return;
    /* HEAD bodies are stripped; only a real GET may seed the entry. */
    if (req->method != CWIST_HTTP_GET) return;
    if (res->status_code != CWIST_HTTP_OK) return;
    if (res->use_file_stream || res->stream_mode) return;
    if (cwist_http_header_get(res->headers, "Set-Cookie")) return;

    const char *body;
    size_t body_len;
    if (res->is_ptr_body) {
        body = (const char *)res->ptr_body;
        body_len = res->ptr_body_len;
    } else {
        body = res->body ? res->body->data : NULL;
        body_len = res->body ? res->body->size : 0;
    }
    if (body_len > BDR_BODY_MAX || (body_len > 0 && !body)) return;

    writer_t w = {0};
    uint32_t magic = BDR_MAGIC, status = (uint32_t)res->status_code, count = 0;
    int64_t created = (int64_t)time(NULL);
    wr(&w, &magic, sizeof(magic));
    wr(&w, &version, sizeof(version));
    wr(&w, &created, sizeof(created));
    wr(&w, &status, sizeof(status));
    wr_span(&w, key, strlen(key));
    size_t count_at = w.len;
    wr(&w, &count, sizeof(count));
    for (cwist_http_header_node *h = res->headers; h; h = h->next) {
        if (!h->key || !h->key->data || !h->value || !h->value->data) continue;
        wr_span(&w, h->key->data, h->key->size);
        wr_span(&w, h->value->data, h->value->size);
        count++;
    }
    uint64_t blen = body_len;
    wr(&w, &blen, sizeof(blen));
    if (body_len) wr(&w, body, body_len);
    if (!w.failed) {
        memcpy(w.buf + count_at, &count, sizeof(count));
        cwist_bdr_put_fixed(g_bdr, "GET", key, w.buf, w.len);
    }
    free(w.buf);
}

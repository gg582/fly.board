#define _POSIX_C_SOURCE 200809L
#include "handlers.h"
#include "handlers_internal.h"
#include "../auth/auth.h"
#include "../crypto/fly_crypto.h"
#include "../db/db.h"
#include "../nats/fly_nats.h"
#include "../render/render.h"
#include "../render/theme.h"
#include "../utils/utils.h"
#include "../utils/image_inline.h"
#include "../config/config.h"
#include "../engine/pool.h"
#include <cwist/core/sstring/sstring.h>
#include <unistd.h>
#include <cwist/core/mem/alloc.h>
#include <cwist/core/log.h>
#include <cwist/net/http/query.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <limits.h>
#include <time.h>
#include <stdatomic.h>
#include <sqlite3.h>

/* Returns true when the requested theme needs the dark highlight.js stylesheet.
   This includes the explicit dark theme as well as the dark-themed variants
   (ocean, forest, sepia). A missing or unrecognized theme defaults to light. */
bool is_dark(cwist_http_request *req) {
    char theme_name[64] = "light";
    auth_get_cookie(req, "theme", theme_name, sizeof(theme_name));
    theme_color_t *t = theme_by_name(theme_name);
    return (t == &dark || t == &ocean || t == &forest);
}

void redirect(cwist_http_response *res, const char *url) {
    res->status_code = (cwist_http_status_t)302;
    cwist_http_header_add(&res->headers, "Location", url);
    cwist_sstring_assign(res->body, "Redirecting...");
    char len_buf[32];
    snprintf(len_buf, sizeof(len_buf), "%zu", res->body->size);
    cwist_http_header_add(&res->headers, "Content-Length", len_buf);
}

/* Extract scheme://host[:port] from root_url (e.g. https://example.com:8443/).
   Returns a pointer to a static buffer; caller must copy if needed.
   Initialization is guarded by a CAS loop so concurrent callers cannot observe
   a partially written buffer or race during the first write. */
static const char *site_origin(void) {
    static char buf[256];
    static _Atomic int init_state = 0; /* 0=uninit, 1=in-progress, 2=done */

    if (atomic_load_explicit(&init_state, memory_order_acquire) == 2)
        return buf;

    int expected = 0;
    if (atomic_compare_exchange_strong_explicit(
            &init_state, &expected, 1,
            memory_order_acquire, memory_order_relaxed)) {
        const char *url = g_config.root_url;
        if (!url || !url[0]) url = "https://localhost";
        const char *p = strstr(url, "://");
        if (!p) {
            snprintf(buf, sizeof(buf), "%s", url);
        } else {
            p += 3;
            const char *end = p;
            while (*end && *end != '/' && *end != '?' && *end != '#') end++;
            size_t len = (size_t)(end - url);
            if (len >= sizeof(buf)) len = sizeof(buf) - 1;
            memcpy(buf, url, len);
            buf[len] = '\0';
        }
        atomic_store_explicit(&init_state, 2, memory_order_release);
    } else {
        /* Another thread is initializing; wait until it finishes. */
        while (atomic_load_explicit(&init_state, memory_order_acquire) != 2) {
            /* spin; initialization is short */
        }
    }
    return buf;
}

static bool origins_match(const char *a, const char *b) {
    if (!a || !b) return false;
    /* scheme://host are case-insensitive; ports must match exactly. */
    size_t la = strlen(a), lb = strlen(b);
    if (la != lb) return false;
    for (size_t i = 0; i < la; i++) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca += 'a' - 'A';
        if (cb >= 'A' && cb <= 'Z') cb += 'a' - 'A';
        if (ca != cb) return false;
    }
    return true;
}

/* Redirect to the Referer URL only when it is same-origin or a local path.
 * Prevents open-redirect attacks via a forged Referer header. */
void redirect_referer_safe(cwist_http_response *res, const char *referer, const char *fallback) {
    if (referer && referer[0]) {
        if (referer[0] == '/' && referer[1] != '/') {
            redirect(res, referer);
            return;
        }

        const char *origin = site_origin();
        size_t origin_len = strlen(origin);
        if (strncmp(referer, origin, origin_len) == 0 &&
            (referer[origin_len] == '/' || referer[origin_len] == '\0')) {
            const char *path = referer + origin_len;
            redirect(res, (path[0] == '/') ? path : "/");
            return;
        }

        /* Extract pathname if scheme://host:port/... */
        const char *scheme_sep = strstr(referer, "://");
        if (scheme_sep) {
            const char *host_start = scheme_sep + 3;
            const char *path_start = strchr(host_start, '/');
            if (path_start && path_start[0] == '/' && path_start[1] != '/') {
                redirect(res, path_start);
                return;
            }
        }
    }
    redirect(res, fallback);
}

char *get_admin_logo(void) {
    const char *logo_url = image_inline_logo();
    return strdup(logo_url ? logo_url : "/assets/img/logo.png");
}

char *get_profile_pic(cwist_db *db, int uid, const char *role) {
    cJSON *user = (uid > 0) ? db_user_get_by_id(db, uid) : NULL;
    if (!user) {
        render_set_nav_notifications(0);
        if (role && strcmp(role, "admin") == 0) {
            render_set_nav_profile("Admin", "@admin");
            return get_admin_logo();
        }
        render_set_nav_profile(NULL, NULL);
        return NULL;
    }

    cJSON *username = cJSON_GetObjectItem(user, "username");
    cJSON *nickname = cJSON_GetObjectItem(user, "nickname");
    const char *uname = (username && username->type == cJSON_String && username->valuestring) ? username->valuestring : "";
    const char *nname = (nickname && nickname->type == cJSON_String && nickname->valuestring) ? nickname->valuestring : "";
    char account[140];
    if (uname[0]) snprintf(account, sizeof(account), "@%s", uname);
    else snprintf(account, sizeof(account), "%s", role && role[0] ? role : "@account");
    render_set_nav_profile(nname[0] ? nname : (uname[0] ? uname : "Profile"), account);
    render_set_nav_notifications(db_notification_unread_count(db, uid));

    cJSON *pp = cJSON_GetObjectItem(user, "profile_pic");
    char *res = NULL;
    if (pp && pp->type == cJSON_String && pp->valuestring[0]) {
        res = strdup(pp->valuestring);
    } else if (role && strcmp(role, "admin") == 0) {
        res = get_admin_logo();
    }
    cJSON_Delete(user);
    return res;
}

void send_html_res(cwist_http_response *res, cwist_sstring *html) {
    cwist_http_header_add(&res->headers, "Content-Type", "text/html; charset=utf-8");
    cwist_http_header_add(&res->headers, "Cache-Control", "private, no-cache, no-store, must-revalidate");
    cwist_http_header_add(&res->headers, "Pragma", "no-cache");
    cwist_http_header_add(&res->headers, "Vary", "Cookie, Authorization");
    if (html) {
        cwist_sstring_assign_len(res->body, html->data, strlen(html->data));
        cwist_sstring_destroy(html);
    } else {
        res->status_code = CWIST_HTTP_INTERNAL_ERROR;
        cwist_sstring_assign(res->body, "render error");
    }
    char len_buf[32];
    snprintf(len_buf, sizeof(len_buf), "%zu", res->body->size);
    cwist_http_header_add(&res->headers, "Content-Length", len_buf);
}

void send_cached_html_res(cwist_http_response *res, const char *html, size_t len, uint32_t ttl_remaining) {
    (void)ttl_remaining;
    cwist_http_header_add(&res->headers, "Content-Type", "text/html; charset=utf-8");
    cwist_http_header_add(&res->headers, "Cache-Control", "private, no-cache, no-store, must-revalidate");
    cwist_http_header_add(&res->headers, "Pragma", "no-cache");
    cwist_http_header_add(&res->headers, "Vary", "Cookie, Authorization");
    cwist_sstring_assign(res->body, "");
    cwist_sstring_append_len(res->body, html, len);
    char len_buf[32];
    snprintf(len_buf, sizeof(len_buf), "%zu", len);
    cwist_http_header_add(&res->headers, "Content-Length", len_buf);
}

/* Apply the modern security header set consistently. This helper is used both by
   the global middleware (normal routes) and by the 404 error handler (unmatched
   routes), because CWIST may skip middleware when invoking registered error handlers. */
static void apply_security_headers(cwist_http_response *res, bool is_static_asset) {
    cwist_http_header_add(&res->headers, "Strict-Transport-Security", "max-age=31536000; includeSubDomains; preload");
    cwist_http_header_add(&res->headers, "X-Content-Type-Options", "nosniff");
    cwist_http_header_add(&res->headers, "X-Frame-Options", "DENY");
    cwist_http_header_add(&res->headers, "Referrer-Policy", "strict-origin-when-cross-origin");
    cwist_http_header_add(&res->headers, "Cross-Origin-Resource-Policy", is_static_asset ? "cross-origin" : "same-origin");
    cwist_http_header_add(&res->headers, "Cross-Origin-Opener-Policy", "same-origin-allow-popups");
    cwist_http_header_add(&res->headers, "Cross-Origin-Embedder-Policy", "credentialless");
    cwist_http_header_add(&res->headers, "Permissions-Policy",
        "accelerometer=(), camera=(), geolocation=(), gyroscope=(), magnetometer=(), "
        "microphone=(), payment=(), usb=(), interest-cohort=(), browsing-topics=(), "
        "display-capture=(), document-domain=(), encrypted-media=(), fullscreen=(self), "
        "picture-in-picture=(self), publickey-credentials-get=(), screen-wake-lock=(), "
        "web-share=(), xr-spatial-tracking=()");
    cwist_http_header_add(&res->headers, "X-DNS-Prefetch-Control", "off");
    cwist_http_header_add(&res->headers, "Origin-Agent-Cluster", "?1");
    cwist_http_header_add(&res->headers, "X-Permitted-Cross-Domain-Policies", "none");
    cwist_http_header_add(&res->headers, "Accept-CH", "DPR, Width, Viewport-Width");
    cwist_http_header_add(&res->headers, "Critical-CH", "DPR");
    cwist_http_header_add(&res->headers, "Report-To",
        "{\"group\":\"default\",\"max_age\":31536000,\"endpoints\":[{\"url\":\"/api/reports\"}]}");
    cwist_http_header_add(&res->headers, "NEL",
        "{\"report_to\":\"default\",\"max_age\":31536000,\"include_subdomains\":true,\"success_fraction\":0.0,\"failure_fraction\":1.0}");
}

/* Replace the framework's default CSP (if any) with our own policy. */
static void replace_csp(cwist_http_response *res) {
    cwist_http_header_node **cur = &res->headers;
    while (*cur) {
        if (strcmp((*cur)->key->data, "Content-Security-Policy") == 0) {
            cwist_http_header_node *to_remove = *cur;
            *cur = (*cur)->next;
            cwist_sstring_destroy(to_remove->key);
            cwist_sstring_destroy(to_remove->value);
            if (!to_remove->arena_owned) {
                cwist_free(to_remove);
            }
        } else {
            cur = &(*cur)->next;
        }
    }
    char csp[1024];
    snprintf(csp, sizeof(csp),
        "default-src 'self' 'unsafe-inline' 'unsafe-eval' https://cdnjs.cloudflare.com https://cdn.jsdelivr.net https://cdn.plyr.io https://tikzjax.com data: blob:; "
        "script-src 'self' 'unsafe-inline' 'unsafe-eval' https://cdnjs.cloudflare.com https://cdn.jsdelivr.net https://cdn.plyr.io https://tikzjax.com data: blob:; "
        "style-src 'self' 'unsafe-inline' 'unsafe-eval' https://fonts.googleapis.com https://cdn.jsdelivr.net https://cdnjs.cloudflare.com https://cdn.plyr.io https://tikzjax.com data: blob:; "
        "font-src 'self' https://fonts.gstatic.com https://cdn.jsdelivr.net https://tikzjax.com data:; "
        "img-src 'self' blob: data: https:; "
        "media-src 'self' blob: data: https:; "
        "connect-src 'self' https:; "
        "frame-src 'self' https:; "
        "worker-src 'self' blob:; "
        "manifest-src 'self'; "
        "frame-ancestors 'none'; base-uri 'self'; form-action 'self'; object-src 'self' https://latexonline.cc https://texlive.net%s",
        g_config.use_tls ? "; upgrade-insecure-requests" : "");
    cwist_http_header_add(&res->headers, "Content-Security-Policy", csp);
}

int json_int(cJSON *obj, const char *key, int def) {
    if (!obj) return def;
    cJSON *item = cJSON_GetObjectItem(obj, key);
    if (!item) return def;
    if (cJSON_IsNumber(item)) return item->valueint;
    if (cJSON_IsString(item) && item->valuestring) return atoi(item->valuestring);
    return def;
}

bool is_author_or_admin(cJSON *post, int uid, const char *role) {
    if (!post) return false;
    if (role && strcmp(role, "admin") == 0) return true;
    int author_id = json_int(post, "user_id", 0);
    return author_id > 0 && author_id == uid;
}

cJSON *board_by_route_key(cwist_db *db, const char *key) {
    if (!key || !key[0]) return NULL;
    errno = 0;
    char *end;
    long id = strtol(key, &end, 10);
    if (errno == 0 && *end == '\0' && id > 0 && id <= INT_MAX) return db_board_get_by_id(db, (int)id);
    return db_board_get_by_slug(db, key);
}

static int env_int_clamped(const char *name, int def, int min_value, int max_value) {
    const char *value = getenv(name);
    if (!value || !value[0]) return def;
    char *end = NULL;
    long parsed = strtol(value, &end, 10);
    if (end == value || *end != '\0') return def;
    if (parsed < min_value) return min_value;
    if (parsed > max_value) return max_value;
    return (int)parsed;
}

static bool env_flag_enabled(const char *name, bool def) {
    const char *value = getenv(name);
    if (!value || !value[0]) return def;
    if (strcmp(value, "0") == 0 || strcasecmp(value, "false") == 0 || strcasecmp(value, "off") == 0) return false;
    if (strcmp(value, "1") == 0 || strcasecmp(value, "true") == 0 || strcasecmp(value, "on") == 0) return true;
    return def;
}

#include <malloc.h>
#include <time.h>

static _Atomic int g_active_requests = 0;
static _Atomic time_t g_last_trim_time = 0;

/* Some CWIST response paths can leave transient binary bytes before the HTML
 * doctype.  Normalize at the final middleware boundary, after the handler has
 * built its response and immediately before the transport serializes it. */
static void strip_html_binary_prefix(cwist_http_response *res) {
    if (!res || !res->body || !res->body->data) return;
    const char *doctype = strstr(res->body->data, "<!doctype html>");
    if (!doctype || doctype == res->body->data) return;

    size_t len = strlen(doctype);
    memmove(res->body->data, doctype, len + 1);
    res->body->size = len;

    cwist_http_header_node **node = &res->headers;
    while (*node) {
        if (strcmp((*node)->key->data, "Content-Length") == 0) {
            cwist_http_header_node *old = *node;
            *node = old->next;
            cwist_sstring_destroy(old->key);
            cwist_sstring_destroy(old->value);
            if (!old->arena_owned) cwist_free(old);
        } else {
            node = &(*node)->next;
        }
    }
    char length[32];
    snprintf(length, sizeof(length), "%zu", len);
    cwist_http_header_add(&res->headers, "Content-Length", length);
}

/* Offload malloc_trim to a worker thread so the HTTP-serving thread never
 * stalls on the sbrk syscall.  malloc_trim can take tens of milliseconds on
 * large heaps, long enough to cause an HTTP/1.1 keep-alive connection to time
 * out or appear completely blocked to the client.  The 5-second cooldown and
 * active-request guard are preserved; only the execution context changes. */
static void *trim_heap_worker(void *arg) {
    (void)arg;
    malloc_trim(0);
    return NULL;
}

static void maybe_trim_heap(void) {
    int active = atomic_fetch_sub_explicit(&g_active_requests, 1, memory_order_relaxed) - 1;
    if (active != 0) return;

    time_t now = time(NULL);
    time_t last = atomic_load_explicit(&g_last_trim_time, memory_order_relaxed);
    if (now - last < 5) return;

    if (atomic_compare_exchange_strong_explicit(
            &g_last_trim_time, &last, now,
            memory_order_relaxed, memory_order_relaxed)) {
        /* Fire-and-forget: if the pool is exhausted we skip this trim cycle
         * rather than blocking the response path. */
        engine_pool_schedule(trim_heap_worker, NULL, 0x4D414C4C54524D00ULL,
                             TTAK_TASK_DOMAIN_THREAD, 0);
    }
}

void global_middleware(cwist_http_request *req, cwist_http_response *res, cwist_handler_func next) {
    atomic_fetch_add_explicit(&g_active_requests, 1, memory_order_relaxed);
    /* Keep-alive is enabled. The CWIST framework patch fixes the bug that
     * stripped Cookie headers on reused connections, so sessions stay valid
     * across connection reuse. */

    if (g_config.use_http3 && env_flag_enabled("FLYBOARD_ADVERTISE_H3", true)) {
        /* Do not re-advertise h2 to HTTP/1.x clients: h2 is already
         * negotiated via ALPN on every TLS handshake, and a client that
         * deliberately fell back to 1.1 (broken h2 path, debugging, legacy
         * proxy) gets bounced back onto the broken path on every response
         * when we keep advertising it.  h3 stays advertised since Alt-Svc is
         * its only discovery mechanism and it uses a separate UDP path. */
        bool req_is_http1x = req->stream_id == 0 && req->version && req->version->data &&
            strncmp(req->version->data, "HTTP/1.", 7) == 0;
        char altsvc[128];
        int alt_ma = env_int_clamped("FLYBOARD_ALT_SVC_MAX_AGE", 300, 0, 86400);
        if (g_config.use_http2 && !req_is_http1x) {
            snprintf(altsvc, sizeof(altsvc), "h3=\":%d\"; ma=%d, h2=\":%d\"; ma=%d", g_config.port, alt_ma, g_config.port, alt_ma);
        } else {
            snprintf(altsvc, sizeof(altsvc), "h3=\":%d\"; ma=%d", g_config.port, alt_ma);
        }
        cwist_http_header_add(&res->headers, "Alt-Svc", altsvc);
    }

    const char *path = (req->path && req->path->data) ? req->path->data : "";
    bool is_static_asset = (strncmp(path, "/assets/", 8) == 0) || strcmp(path, "/sw.js") == 0;

    const char *origin = cwist_http_header_get(req->headers, "Origin");
    const char *site = site_origin();
    if (origin && origin[0]) {
        /* Only echo the Origin back when it matches the configured site origin.
           This avoids leaking credentialed responses to arbitrary third-party sites
           while still allowing same-origin and approved cross-origin requests. */
        if (origins_match(origin, site)) {
            cwist_http_header_add(&res->headers, "Access-Control-Allow-Origin", origin);
            cwist_http_header_add(&res->headers, "Vary", "Origin");
            cwist_http_header_add(&res->headers, "Access-Control-Allow-Credentials", "true");
        } else {
            /* Untrusted origin: allow anonymous read-only access for public assets
               but never allow credentials. Static assets may be hot-linked safely. */
            cwist_http_header_add(&res->headers, "Access-Control-Allow-Origin", "*");
        }
    } else {
        /* No Origin header usually means a same-origin request or non-CORS fetch.
           Advertise the site origin explicitly instead of wildcard so that credentialed
           same-origin requests are unambiguous. */
        cwist_http_header_add(&res->headers, "Access-Control-Allow-Origin", site);
    }
    cwist_http_header_add(&res->headers, "Access-Control-Allow-Methods", "GET, POST, PUT, PATCH, DELETE, OPTIONS, HEAD");
    cwist_http_header_add(&res->headers, "Access-Control-Allow-Headers", "*");
    cwist_http_header_add(&res->headers, "Access-Control-Expose-Headers", "Content-Length, Content-Type, X-Request-Id, X-TASFA-Chunk-Index, X-TASFA-Chunk-Count, X-TASFA-Predicted-Remaining-Ms");
    cwist_http_header_add(&res->headers, "Access-Control-Max-Age", "86400");


    /* Remove any existing Content-Security-Policy header added by the framework
       so that our policy (which allows blob: URLs for img/media) is the only one sent. */
    cwist_http_header_node **cur = &res->headers;
    while (*cur) {
        if (strcmp((*cur)->key->data, "Content-Security-Policy") == 0) {
            cwist_http_header_node *to_remove = *cur;
            *cur = (*cur)->next;
            cwist_sstring_destroy(to_remove->key);
            cwist_sstring_destroy(to_remove->value);
            if (!to_remove->arena_owned) {
                cwist_free(to_remove);
            }
        } else {
            cur = &(*cur)->next;
        }
    }

    /* Static assets (images, js, css, media) do not need a CSP. Adding one can
       confuse Firefox when it evaluates img-src for cached/subresource loads. */
    if (!is_static_asset) {
        replace_csp(res);
    }

    apply_security_headers(res, is_static_asset);

    /* Resource Timing: allow external origins to measure static asset loads without
       exposing sensitive path-level data. Applied only to public static assets. */
    if (is_static_asset) {
        cwist_http_header_add(&res->headers, "Timing-Allow-Origin", "*");
    }

    /* Resource hints for the critical rendering path. Preconnect removes the
     * TCP+TLS handshake latency for third-party fonts and scripts on high-RTT
     * links; dns-prefetch acts as a fallback for older browsers. Preloading the
     * site logo lets the browser start the image fetch before the HTML parser
     * reaches the footer. */
    if (!is_static_asset) {
        cwist_http_header_add(&res->headers, "Link", "<https://fonts.googleapis.com>; rel=preconnect");
        cwist_http_header_add(&res->headers, "Link", "<https://fonts.googleapis.com>; rel=dns-prefetch");
        cwist_http_header_add(&res->headers, "Link", "<https://fonts.gstatic.com>; rel=preconnect; crossorigin");
        cwist_http_header_add(&res->headers, "Link", "<https://fonts.gstatic.com>; rel=dns-prefetch; crossorigin");
        cwist_http_header_add(&res->headers, "Link", "<https://cdnjs.cloudflare.com>; rel=preconnect");
        cwist_http_header_add(&res->headers, "Link", "<https://cdnjs.cloudflare.com>; rel=dns-prefetch");
        cwist_http_header_add(&res->headers, "Link", "<https://cdn.jsdelivr.net>; rel=preconnect");
        cwist_http_header_add(&res->headers, "Link", "<https://cdn.jsdelivr.net>; rel=dns-prefetch");
        const char *logo_url = image_inline_logo();
        if (!logo_url) logo_url = "/assets/img/logo.png";
        /* Only preload external logo URLs; data-URIs are already inline and
         * preloading them would waste a high-RTT Link header slot. */
        if (strncmp(logo_url, "data:", 5) != 0) {
            char logo_link[384];
            snprintf(logo_link, sizeof(logo_link), "<%s>; rel=preload; as=image; fetchpriority=high", logo_url);
            cwist_http_header_add(&res->headers, "Link", logo_link);
        }
        if (g_config.use_rss) {
            char rss_link[320];
            snprintf(rss_link, sizeof(rss_link), "</rss.xml>; rel=alternate; type=\"application/rss+xml\"; title=\"%s RSS\"",
                     g_config.title[0] ? g_config.title : "Fly Board");
            cwist_http_header_add(&res->headers, "Link", rss_link);
        }
    }

    if (req->method == CWIST_HTTP_OPTIONS) {
        res->status_code = CWIST_HTTP_NO_CONTENT;
        cwist_sstring_assign(res->body, "");
        maybe_trim_heap();
        return;
    }

    const char *m = cwist_http_method_to_string(req->method);
    const char *p = (req->path && req->path->data) ? req->path->data : "?";
    CWIST_LOG_DEBUG("%s %s", m ? m : "?", p ? p : "?");

    /* The router only registers GET routes, so a bare HEAD (crawlers, health
     * checks, PageSpeed probes) used to 404.  Route it as GET, then restore
     * the method afterwards so the HEAD post-processing below still strips
     * the body.  The marker header lets handlers skip GET side effects such
     * as view counting. */
    bool head_rewrite = (req->method == CWIST_HTTP_HEAD);
    if (head_rewrite) {
        req->method = CWIST_HTTP_GET;
        cwist_http_header_add(&req->headers, "X-Fly-Head-Rewrite", "1");
    }

    next(req, res);

    if (head_rewrite) req->method = CWIST_HTTP_HEAD;

    strip_html_binary_prefix(res);

    /* Post-processing: ensure HEAD responses never carry a body. Keep the
       Content-Length header so the client knows the GET representation size. */
    if (req->method == CWIST_HTTP_HEAD) {
        cwist_sstring_assign(res->body, "");
        if (res->use_file_stream) {
            if (res->file_stream_fd >= 0) close(res->file_stream_fd);
            res->file_stream_fd = -1;
            res->use_file_stream = false;
            res->file_stream_auto_close = false;
        }
    }

    /* For missing image assets, return a valid 1x1 transparent PNG body
       instead of a text/plain 404. This avoids Firefox MIME/decoding mismatch
       logs and lets the page render a clean broken-image placeholder. */
    if (res->status_code == CWIST_HTTP_NOT_FOUND && req->method == CWIST_HTTP_GET) {
        const char *path2 = (req->path && req->path->data) ? req->path->data : "";
        bool is_image_path = (strncmp(path2, "/assets/img/", 12) == 0) ||
                             (strncmp(path2, "/assets/uploads/", 16) == 0) ||
                             (strncmp(path2, "/assets/profile/", 16) == 0);
        if (!is_image_path) {
            const char *accept = cwist_http_header_get(req->headers, "Accept");
            is_image_path = accept && strstr(accept, "image/") != NULL;
        }
        if (is_image_path) {
            static const unsigned char empty_png[67] = {
                0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A,0x00,0x00,0x00,0x0D,0x49,0x48,0x44,0x52,
                0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01,0x08,0x06,0x00,0x00,0x00,0x1F,0x15,0xC4,
                0x89,0x00,0x00,0x00,0x0A,0x49,0x44,0x41,0x54,0x78,0x9C,0x63,0x60,0x00,0x00,0x00,
                0x02,0x00,0x01,0x73,0x75,0x01,0x18,0x00,0x00,0x00,0x00,0x49,0x45,0x4E,0x44,0xAE,
                0x42,0x60,0x82
            };
            cwist_http_header_node **h = &res->headers;
            while (*h) {
                if (strcmp((*h)->key->data, "Content-Type") == 0 ||
                    strcmp((*h)->key->data, "Content-Length") == 0 ||
                    strcmp((*h)->key->data, "Cache-Control") == 0) {
                    cwist_http_header_node *r = *h;
                    *h = (*h)->next;
                    cwist_sstring_destroy(r->key);
                    cwist_sstring_destroy(r->value);
                    /* Directly free the header node */
                    cwist_free(r);
                } else {
                    h = &(*h)->next;
                }
            }
            cwist_http_header_add(&res->headers, "Content-Type", "image/png");
            cwist_http_header_add(&res->headers, "Cache-Control", "no-store, no-cache, must-revalidate");
            char len_buf[8];
            snprintf(len_buf, sizeof(len_buf), "%zu", sizeof(empty_png));
            cwist_http_header_add(&res->headers, "Content-Length", len_buf);
            cwist_sstring_assign(res->body, "");
            cwist_sstring_append_len(res->body, (const char *)empty_png, sizeof(empty_png));
        }
    }

    /* Static assets may be served with compression; tell caches to vary on Accept-Encoding. */
    if (is_static_asset) {
        cwist_http_header_add(&res->headers, "Vary", "Accept-Encoding");
    }

    /* SEO / crawler directives: index normal pages and static assets, noindex errors. */
    if (res->status_code >= 400) {
        cwist_http_header_add(&res->headers, "X-Robots-Tag", "noindex, nofollow");
    } else {
        cwist_http_header_add(&res->headers, "X-Robots-Tag", "all");
    }

    maybe_trim_heap();
}

void handler_sw_js(cwist_http_request *req, cwist_http_response *res) {
    if (!send_cached_file_response(req, res, "public/sw.js",
                                   "application/javascript; charset=utf-8",
                                   "no-cache", NULL)) {
        res->status_code = CWIST_HTTP_NOT_FOUND;
    }
}

/* Serve static JS/CSS through send_cached_file_response so high-RTT clients
 * get pre-compressed (zstd/br/gzip) immutable assets over HTTP/2 and HTTP/3.
 * CWIST's built-in static directory helper bypasses the compression cache. */
static bool safe_static_filename(const char *name) {
    if (!name || !name[0]) return false;
    if (name[0] == '.') return false;
    if (strstr(name, "..")) return false;
    if (strchr(name, '/')) return false;
    return true;
}

void handler_static_js(cwist_http_request *req, cwist_http_response *res) {
    const char *filename = cwist_query_map_get(req->path_params, "filename");
    if (!safe_static_filename(filename)) {
        res->status_code = CWIST_HTTP_NOT_FOUND;
        return;
    }
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "public/js/%s", filename);
    if (!send_cached_file_response(req, res, path,
                                   "application/javascript; charset=utf-8",
                                   "public, max-age=31536000, immutable", NULL)) {
        res->status_code = CWIST_HTTP_NOT_FOUND;
    }
}

void handler_static_css(cwist_http_request *req, cwist_http_response *res) {
    const char *filename = cwist_query_map_get(req->path_params, "filename");
    if (!safe_static_filename(filename)) {
        res->status_code = CWIST_HTTP_NOT_FOUND;
        return;
    }
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "public/css/%s", filename);
    if (!send_cached_file_response(req, res, path,
                                   "text/css; charset=utf-8",
                                   "public, max-age=31536000, immutable", NULL)) {
        res->status_code = CWIST_HTTP_NOT_FOUND;
    }
}

void handler_tasfa_stream_placeholder(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    res->status_code = CWIST_HTTP_NO_CONTENT;
    cwist_http_header_add(&res->headers, "Cache-Control", "no-store");
    cwist_http_header_add(&res->headers, "Content-Type", "application/octet-stream");
    cwist_sstring_assign(res->body, "");
}

/* CWIST does not route HEAD to GET handlers, so static asset HEAD requests
   (used by some caches and Firefox pre-checks) would otherwise return 404.
   Dispatch them to the matching GET handler and strip the body below. */
void handler_not_found(cwist_http_request *req, cwist_http_response *res, cwist_http_status_t status) {
    (void)status;
    /* cwist matches routes by exact method and only runs the middleware chain
     * after a match, so HEAD on any GET route lands here.  Re-dispatch once
     * as GET through the normal router (the marker headers guard against
     * recursion and let handlers skip GET side effects like view counting),
     * then strip the body per HEAD semantics.  This replaces the previous
     * hardcoded list of asset paths that needed individual HEAD shims. */
    if (req->method == CWIST_HTTP_HEAD &&
        !cwist_http_header_get(req->headers, "X-Fly-Head-Redispatch")) {
        cwist_http_header_add(&req->headers, "X-Fly-Head-Redispatch", "1");
        cwist_http_header_add(&req->headers, "X-Fly-Head-Rewrite", "1");
        req->method = CWIST_HTTP_GET;
        cwist_app_dispatch(req->app, req, res);
        req->method = CWIST_HTTP_HEAD;
        cwist_sstring_assign(res->body, "");
        if (res->use_file_stream) {
            if (res->file_stream_fd >= 0) close(res->file_stream_fd);
            res->file_stream_fd = -1;
            res->use_file_stream = false;
            res->file_stream_auto_close = false;
        }
        return;
    }

    res->status_code = CWIST_HTTP_NOT_FOUND;
    cwist_sstring_assign(res->status_text, "Not Found");
    cwist_http_header_add(&res->headers, "Cache-Control", "no-store, no-cache, must-revalidate");
    /* Error handlers may run outside the global middleware, so ensure the full
       security header set and CSP are applied to HTML 404 responses as well. */
    replace_csp(res);
    apply_security_headers(res, false);
    cwist_sstring_assign(res->body, "Not found");
}

/* render_file_detail declared in render.h */

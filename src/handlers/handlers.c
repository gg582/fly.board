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
#include "../config/config.h"
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
#include <sqlite3.h>

bool is_dark(cwist_http_request *req) {
    cwist_http_header_node *curr = req->headers;
    while (curr) {
        if (curr->key && curr->key->data && strcasecmp(curr->key->data, "Cookie") == 0) {
            const char *cookie_val = curr->value ? curr->value->data : NULL;
            if (cookie_val && strstr(cookie_val, "theme=dark") != NULL) {
                return true;
            }
        }
        curr = curr->next;
    }
    return false;
}

void redirect(cwist_http_response *res, const char *url) {
    res->status_code = (cwist_http_status_t)302;
    cwist_http_header_add(&res->headers, "Location", url);
    cwist_sstring_assign(res->body, "Redirecting...");
}

/* Extract scheme://host[:port] from root_url (e.g. https://example.com:8443/).
   Returns a pointer to a static buffer; caller must copy if needed. */
static const char *site_origin(void) {
    static char buf[256];
    if (buf[0]) return buf;
    const char *url = g_config.root_url;
    if (!url || !url[0]) url = "https://localhost";
    const char *p = strstr(url, "://");
    if (!p) { snprintf(buf, sizeof(buf), "%s", url); return buf; }
    p += 3;
    const char *end = p;
    while (*end && *end != '/' && *end != '?' && *end != '#') end++;
    size_t len = (size_t)(end - url);
    if (len >= sizeof(buf)) len = sizeof(buf) - 1;
    memcpy(buf, url, len);
    buf[len] = '\0';
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

char *get_profile_pic(cwist_db *db, int uid, const char *role) {
    if (uid <= 0) {
        if (role && strcmp(role, "admin") == 0) {
            render_set_nav_profile("Admin", "@admin");
            if (g_config.blog_logo[0]) {
                char *buf = (char *)malloc(512);
                snprintf(buf, 512, "/assets/img/%s", g_config.blog_logo);
                return buf;
            }
            return strdup("/assets/img/logo.png");
        }
        render_set_nav_profile(NULL, NULL);
        return NULL;
    }
    cJSON *user = db_user_get_by_id(db, uid);
    if (!user) {
        if (role && strcmp(role, "admin") == 0) {
            render_set_nav_profile("Admin", "@admin");
            if (g_config.blog_logo[0]) {
                char *buf = (char *)malloc(512);
                snprintf(buf, 512, "/assets/img/%s", g_config.blog_logo);
                return buf;
            }
            return strdup("/assets/img/logo.png");
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

    cJSON *pp = cJSON_GetObjectItem(user, "profile_pic");
    char *res = NULL;
    if (pp && pp->type == cJSON_String && pp->valuestring[0]) {
        res = strdup(pp->valuestring);
    } else if (role && strcmp(role, "admin") == 0) {
        if (g_config.blog_logo[0]) {
            res = (char *)malloc(512);
            snprintf(res, 512, "/assets/img/%s", g_config.blog_logo);
        } else {
            res = strdup("/assets/img/logo.png");
        }
    }
    cJSON_Delete(user);
    return res;
}

void send_html_res(cwist_http_response *res, cwist_sstring *html) {
    cwist_http_header_add(&res->headers, "Content-Type", "text/html; charset=utf-8");
    cwist_http_header_add(&res->headers, "Cache-Control", "no-cache, private");
    if (html) {
        cwist_sstring_assign(res->body, html->data);
        cwist_sstring_destroy(html);
    } else {
        res->status_code = CWIST_HTTP_INTERNAL_ERROR;
        cwist_sstring_assign(res->body, "render error");
    }
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
            cwist_free(to_remove);
        } else {
            cur = &(*cur)->next;
        }
    }
    char csp[1024];
    snprintf(csp, sizeof(csp),
        "default-src 'self' 'unsafe-inline' 'unsafe-eval' https://cdnjs.cloudflare.com https://cdn.jsdelivr.net https://cdn.plyr.io data: blob:; "
        "script-src 'self' 'unsafe-inline' 'unsafe-eval' https://cdnjs.cloudflare.com https://cdn.jsdelivr.net https://cdn.plyr.io data: blob:; "
        "style-src 'self' 'unsafe-inline' 'unsafe-eval' https://fonts.googleapis.com https://cdn.jsdelivr.net https://cdnjs.cloudflare.com https://cdn.plyr.io data: blob:; "
        "font-src 'self' https://fonts.gstatic.com https://cdn.jsdelivr.net data:; "
        "img-src 'self' blob: data: https:; "
        "media-src 'self' blob: data: https:; "
        "connect-src 'self' https:; "
        "frame-src 'self' https:; "
        "worker-src 'self' blob:; "
        "manifest-src 'self'; "
        "frame-ancestors 'none'; base-uri 'self'; form-action 'self'; object-src 'none'%s",
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

static int g_active_requests = 0;
static time_t g_last_trim_time = 0;

void global_middleware(cwist_http_request *req, cwist_http_response *res, cwist_handler_func next) {
    __sync_add_and_fetch(&g_active_requests, 1);
    /* Keep-alive is disabled process-wide to prevent request object/header
     * reuse issues in the framework from stripping Cookie headers on reused
     * connections. Each request gets a fresh connection. */
    req->keep_alive = false;
    res->keep_alive = false;
    cwist_http_header_add(&res->headers, "Connection", "close");

    if (g_config.use_http3 && env_flag_enabled("FLYBOARD_ADVERTISE_H3", true)) {
        char altsvc[128];
        int alt_ma = env_int_clamped("FLYBOARD_ALT_SVC_MAX_AGE", 300, 0, 86400);
        if (g_config.use_http2) {
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
            cwist_free(to_remove);
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

    /* Preconnect to common third-party origins so fonts/scripts load faster.
       These origins are already allowed by the CSP. */
    if (!is_static_asset) {
        cwist_http_header_add(&res->headers, "Link", "<https://fonts.googleapis.com>; rel=preconnect");
        cwist_http_header_add(&res->headers, "Link", "<https://fonts.gstatic.com>; rel=preconnect; crossorigin");
        cwist_http_header_add(&res->headers, "Link", "<https://cdnjs.cloudflare.com>; rel=preconnect");
        cwist_http_header_add(&res->headers, "Link", "<https://cdn.jsdelivr.net>; rel=preconnect");
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
        int active = __sync_sub_and_fetch(&g_active_requests, 1);
        if (active == 0) {
            time_t now = time(NULL);
            if (now - g_last_trim_time >= 5) {
                g_last_trim_time = now;
                malloc_trim(0);
            }
        }
        return;
    }

    const char *m = cwist_http_method_to_string(req->method);
    const char *p = (req->path && req->path->data) ? req->path->data : "?";
    CWIST_LOG_DEBUG("%s %s", m ? m : "?", p ? p : "?");
    next(req, res);

    /* Post-processing: ensure HEAD responses never carry a body. Keep the
       Content-Length header so the client knows the GET representation size. */
    if (req->method == CWIST_HTTP_HEAD) {
        cwist_sstring_assign(res->body, "");
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

    int active = __sync_sub_and_fetch(&g_active_requests, 1);
    if (active == 0) {
        time_t now = time(NULL);
        if (now - g_last_trim_time >= 5) {
            g_last_trim_time = now;
            malloc_trim(0);
        }
    }
}

void handler_sw_js(cwist_http_request *req, cwist_http_response *res) {
    if (!send_cached_file_response(req, res, "public/sw.js",
                                   "application/javascript; charset=utf-8",
                                   "no-cache", NULL)) {
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
    const char *path = (req->path && req->path->data) ? req->path->data : "";
    if (req->method == CWIST_HTTP_HEAD) {
        cwist_http_method_t original = req->method;
        req->method = CWIST_HTTP_GET;
        bool dispatched = false;

        if (strncmp(path, "/assets/img/", 12) == 0 && path[12]) {
            cwist_query_map_clear(req->path_params);
            cwist_query_map_set(req->path_params, "filename", path + 12);
            handler_asset_img(req, res);
            dispatched = true;
        } else if (strncmp(path, "/assets/uploads/", 16) == 0 && path[16]) {
            cwist_query_map_clear(req->path_params);
            cwist_query_map_set(req->path_params, "filename", path + 16);
            handler_asset_upload(req, res);
            dispatched = true;
        } else if (strncmp(path, "/assets/profile/", 16) == 0 && path[16]) {
            cwist_query_map_clear(req->path_params);
            cwist_query_map_set(req->path_params, "filename", path + 16);
            handler_asset_profile_upload(req, res);
            dispatched = true;
        } else if (strcmp(path, "/sw.js") == 0) {
            handler_sw_js(req, res);
            dispatched = true;
        } else if (strncmp(path, "/assets/images/", 15) == 0 && path[15]) {
            char full_path[PATH_MAX];
            snprintf(full_path, sizeof(full_path), "public/images/%s", path + 15);
            if (!send_cached_file_response(req, res, full_path, mime_type(path + 15), "public, max-age=86400, must-revalidate", NULL)) {
                res->status_code = CWIST_HTTP_NOT_FOUND;
                cwist_sstring_assign(res->status_text, "Not Found");
                cwist_sstring_assign(res->body, "Not found");
            }
            dispatched = true;
        } else if (strncmp(path, "/assets/js/", 11) == 0 && path[11]) {
            char full_path[PATH_MAX];
            snprintf(full_path, sizeof(full_path), "public/js/%s", path + 11);
            if (!send_cached_file_response(req, res, full_path, "application/javascript; charset=utf-8", "public, max-age=86400, must-revalidate", NULL)) {
                res->status_code = CWIST_HTTP_NOT_FOUND;
                cwist_sstring_assign(res->status_text, "Not Found");
                cwist_sstring_assign(res->body, "Not found");
            }
            dispatched = true;
        } else if (strncmp(path, "/assets/media/", 14) == 0 && path[14]) {
            char full_path[PATH_MAX];
            snprintf(full_path, sizeof(full_path), "public/media/%s", path + 14);
            if (!send_cached_file_response(req, res, full_path, mime_type(path + 14), "public, max-age=86400, must-revalidate", NULL)) {
                res->status_code = CWIST_HTTP_NOT_FOUND;
                cwist_sstring_assign(res->status_text, "Not Found");
                cwist_sstring_assign(res->body, "Not found");
            }
            dispatched = true;
        }

        req->method = original;
        if (dispatched) {
            cwist_sstring_assign(res->body, "");
            return;
        }
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

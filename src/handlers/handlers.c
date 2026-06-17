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
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <limits.h>
#include <time.h>
#include <sqlite3.h>

bool is_dark(cwist_http_request *req) {
    const char *cookie = cwist_http_header_get(req->headers, "Cookie");
    return cookie && strstr(cookie, "theme=dark") != NULL;
}

void redirect(cwist_http_response *res, const char *url) {
    res->status_code = (cwist_http_status_t)302;
    cwist_http_header_add(&res->headers, "Location", url);
    cwist_sstring_assign(res->body, "Redirecting...");
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

void global_middleware(cwist_http_request *req, cwist_http_response *res, cwist_handler_func next) {
    res->keep_alive = req->keep_alive;

    if (g_config.use_http3) {
        char altsvc[128];
        // Advertise HTTP/3 and HTTP/2 fallback. h3 is prioritized.
        if (g_config.use_http2) {
            snprintf(altsvc, sizeof(altsvc), "h3=\":%d\"; ma=86400, h2=\":%d\"; ma=86400", g_config.port, g_config.port);
        } else {
            snprintf(altsvc, sizeof(altsvc), "h3=\":%d\"; ma=86400", g_config.port);
        }
        cwist_http_header_add(&res->headers, "Alt-Svc", altsvc);
    }

    const char *origin = cwist_http_header_get(req->headers, "Origin");
    if (origin && origin[0]) {
        /* Echo the actual Origin so credentialed cross-origin requests work.
           Firefox (and other spec-compliant browsers) reject the combination
           of Access-Control-Allow-Origin: * + Access-Control-Allow-Credentials: true. */
        cwist_http_header_add(&res->headers, "Access-Control-Allow-Origin", origin);
        cwist_http_header_add(&res->headers, "Vary", "Origin");
        cwist_http_header_add(&res->headers, "Access-Control-Allow-Credentials", "true");
    } else {
        /* No Origin header usually means a same-origin request. Use a wildcard
           for simple non-credentialed access, but do NOT send Allow-Credentials
           because wildcard + credentials is invalid in Firefox. */
        cwist_http_header_add(&res->headers, "Access-Control-Allow-Origin", "*");
    }
    cwist_http_header_add(&res->headers, "Access-Control-Allow-Methods", "GET, POST, PUT, PATCH, DELETE, OPTIONS, HEAD");
    cwist_http_header_add(&res->headers, "Access-Control-Allow-Headers", "*");
    cwist_http_header_add(&res->headers, "Access-Control-Expose-Headers", "Content-Length, Content-Type, X-Request-Id");
    cwist_http_header_add(&res->headers, "Access-Control-Max-Age", "86400");

    const char *path = (req->path && req->path->data) ? req->path->data : "";
    bool is_static_asset = (strncmp(path, "/assets/", 8) == 0) || strcmp(path, "/sw.js") == 0;

    /* Remove any existing Content-Security-Policy header added by the framework
       so that our policy (which allows blob: URLs for img/media) is the only one sent. */
    cwist_http_header_node **cur = &res->headers;
    while (*cur) {
        if (strcmp((*cur)->key->data, "Content-Security-Policy") == 0) {
            cwist_http_header_node *to_remove = *cur;
            *cur = (*cur)->next;
            cwist_sstring_destroy(to_remove->key);
            cwist_sstring_destroy(to_remove->value);
            free(to_remove);
        } else {
            cur = &(*cur)->next;
        }
    }

    /* Static assets (images, js, css, media) do not need a CSP. Adding one can
       confuse Firefox when it evaluates img-src for cached/subresource loads. */
    if (!is_static_asset) {
        cwist_http_header_add(&res->headers, "Content-Security-Policy",
            "default-src 'self' 'unsafe-inline' 'unsafe-eval' https://cdnjs.cloudflare.com https://cdn.jsdelivr.net https://cdn.plyr.io data: blob:; "
            "script-src 'self' 'unsafe-inline' 'unsafe-eval' https://cdnjs.cloudflare.com https://cdn.jsdelivr.net https://cdn.plyr.io data: blob:; "
            "style-src 'self' 'unsafe-inline' 'unsafe-eval' https://fonts.googleapis.com https://cdn.jsdelivr.net https://cdnjs.cloudflare.com https://cdn.plyr.io data: blob:; "
            "font-src 'self' https://fonts.gstatic.com https://cdn.jsdelivr.net data:; "
            "img-src 'self' blob: data: https:; "
            "media-src 'self' blob: data: https:; "
            "connect-src 'self' https:; "
            "frame-ancestors 'none'; base-uri 'self'; form-action 'self'; object-src 'none';");
    }

    if (req->method == CWIST_HTTP_OPTIONS) {
        res->status_code = CWIST_HTTP_NO_CONTENT;
        cwist_sstring_assign(res->body, "");
        return;
    }

    const char *m = cwist_http_method_to_string(req->method);
    const char *p = (req->path && req->path->data) ? req->path->data : "?";
    CWIST_LOG_DEBUG("%s %s", m ? m : "?", p ? p : "?");
    next(req, res);
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

/* render_file_detail declared in render.h */

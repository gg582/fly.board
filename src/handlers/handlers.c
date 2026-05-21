#define _POSIX_C_SOURCE 200809L
#include "handlers.h"
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
            if (g_config.blog_logo[0]) {
                char *buf = (char *)malloc(512);
                snprintf(buf, 512, "/assets/img/%s", g_config.blog_logo);
                return buf;
            }
            return strdup("/assets/img/logo.png");
        }
        return NULL;
    }
    cJSON *user = db_user_get_by_id(db, uid);
    if (!user) {
        if (role && strcmp(role, "admin") == 0) {
            if (g_config.blog_logo[0]) {
                char *buf = (char *)malloc(512);
                snprintf(buf, 512, "/assets/img/%s", g_config.blog_logo);
                return buf;
            }
            return strdup("/assets/img/logo.png");
        }
        return NULL;
    }
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
    char altsvc[128];
    // Advertise HTTP/3 and HTTP/2 fallback. h3 is prioritized.
    snprintf(altsvc, sizeof(altsvc), "h3=\":%d\"; ma=86400, h2=\":%d\"; ma=86400", g_config.port, g_config.port);
    cwist_http_header_add(&res->headers, "Alt-Svc", altsvc);

    const char *origin = cwist_http_header_get(req->headers, "Origin");
    if (origin && origin[0]) {
        cwist_http_header_add(&res->headers, "Access-Control-Allow-Origin", origin);
        cwist_http_header_add(&res->headers, "Vary", "Origin");
    } else {
        cwist_http_header_add(&res->headers, "Access-Control-Allow-Origin", "*");
    }
    cwist_http_header_add(&res->headers, "Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
    cwist_http_header_add(&res->headers, "Access-Control-Allow-Headers", "Content-Type, X-Requested-With, Authorization");
    cwist_http_header_add(&res->headers, "Access-Control-Max-Age", "86400");

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
    cwist_http_header_add(&res->headers, "Content-Type", "application/javascript; charset=utf-8");
    cwist_http_header_add(&res->headers, "Cache-Control", "no-cache");

    FILE *fp = fopen("public/sw.js", "rb");
    if (!fp) {
        res->status_code = CWIST_HTTP_NOT_FOUND;
        return;
    }

    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    char *buf = (char *)cwist_alloc(sz + 1);
    if (!buf) {
        fclose(fp);
        res->status_code = CWIST_HTTP_INTERNAL_ERROR;
        return;
    }

    fread(buf, 1, sz, fp);
    buf[sz] = '\0';
    fclose(fp);

    cwist_sstring_assign(res->body, buf);
    cwist_free(buf);
}

/* render_file_detail declared in render.h */


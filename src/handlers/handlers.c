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
        if (role && strcmp(role, "admin") == 0) return strdup("/img/logo.png");
        return NULL;
    }
    cJSON *user = db_user_get_by_id(db, uid);
    if (!user) {
        if (role && strcmp(role, "admin") == 0) return strdup("/img/logo.png");
        return NULL;
    }
    cJSON *pp = cJSON_GetObjectItem(user, "profile_pic");
    char *res = NULL;
    if (pp && pp->type == cJSON_String && pp->valuestring[0]) {
        res = strdup(pp->valuestring);
    } else if (role && strcmp(role, "admin") == 0) {
        res = strdup("/img/logo.png");
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
    char alt_svc[128];
    snprintf(alt_svc, sizeof(alt_svc), "h3=\":%d\"; ma=2592000, h3-29=\":%d\"; ma=2592000", g_config.port, g_config.port);
    cwist_http_header_add(&res->headers, "Alt-Svc", alt_svc);

    next(req, res);
}

/* render_file_detail declared in render.h */


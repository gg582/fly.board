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

static bool is_dark(cwist_http_request *req) {
    const char *cookie = cwist_http_header_get(req->headers, "Cookie");
    return cookie && strstr(cookie, "theme=dark") != NULL;
}

static void redirect(cwist_http_response *res, const char *url) {
    res->status_code = (cwist_http_status_t)302;
    cwist_http_header_add(&res->headers, "Location", url);
    cwist_sstring_assign(res->body, "Redirecting...");
}

static char *get_profile_pic(cwist_db *db, int uid, const char *role) {
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

static void send_html_res(cwist_http_response *res, cwist_sstring *html) {
    cwist_http_header_add(&res->headers, "Content-Type", "text/html; charset=utf-8");
    if (html) {
        cwist_sstring_assign(res->body, html->data);
        cwist_sstring_destroy(html);
    } else {
        res->status_code = CWIST_HTTP_INTERNAL_ERROR;
        cwist_sstring_assign(res->body, "render error");
    }
}

static int json_int(cJSON *obj, const char *key, int def) {
    if (!obj) return def;
    cJSON *item = cJSON_GetObjectItem(obj, key);
    if (!item) return def;
    if (cJSON_IsNumber(item)) return item->valueint;
    if (cJSON_IsString(item) && item->valuestring) return atoi(item->valuestring);
    return def;
}

static cJSON *board_by_route_key(cwist_db *db, const char *key) {
    if (!key || !key[0]) return NULL;
    errno = 0;
    char *end;
    long id = strtol(key, &end, 10);
    if (errno == 0 && *end == '\0' && id > 0 && id <= INT_MAX) return db_board_get_by_id(db, (int)id);
    return db_board_get_by_slug(db, key);
}

static char *sql_esc(const char *src) {
    size_t len = strlen(src);
    char *out = (char *)cwist_alloc(len * 2 + 1);
    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        if (src[i] == '\'') out[j++] = '\'';
        out[j++] = src[i];
    }
    out[j] = '\0';
    return out;
}

/* render_file_detail declared in render.h */

/* ---- Home ---- */
void handler_home(cwist_http_request *req, cwist_http_response *res) {
    bool dark = is_dark(req);
    int uid = 0;
    char role[32] = {0};
    auth_is_logged_in(req, &uid, role, sizeof(role));
    char *pp = get_profile_pic(req->db, uid, role);
    cJSON *posts = db_post_recent(req->db, 12);
    cJSON *boards = db_board_list(req->db);
    cwist_sstring *page = render_post_list(posts, boards, dark, role, 1, 1, NULL, NULL, pp);
    if (posts) cJSON_Delete(posts);
    if (boards) cJSON_Delete(boards);
    send_html_res(res, page);
    free(pp);
}

/* ---- Theme JSON ---- */
void handler_theme_json(cwist_http_request *req, cwist_http_response *res) {
    const char *mode = cwist_query_map_get(req->query_params, "mode");
    bool dark = mode && strcmp(mode, "dark") == 0;
    char *json = theme_build_json(dark);
    cwist_http_header_add(&res->headers, "Content-Type", "application/json; charset=utf-8");
    if (json) {
        cwist_sstring_assign(res->body, json);
        free(json);
    }
}

/* ---- Auth ---- */
void handler_login_get(cwist_http_request *req, cwist_http_response *res) {
    send_html_res(res, render_login(is_dark(req), NULL));
}

void handler_login_post(cwist_http_request *req, cwist_http_response *res) {
    bool dark = is_dark(req);
    form_kv_t *kv = parse_urlencoded(req->body->data);
    const char *username = form_kv_get(kv, "username");
    const char *password = form_kv_get(kv, "password");
    if (!username || !password) {
        send_html_res(res, render_login(dark, "Missing fields"));
        form_kv_free(kv);
        return;
    }
    /* Admin login via admin.settings */
    if (auth_admin_check(username, password)) {
        char *token = auth_jwt_issue(1, username, "admin");
        if (token) {
            char cookie[2048];
            snprintf(cookie, sizeof(cookie), "%s=%s; Path=/; Max-Age=604800; HttpOnly; SameSite=Lax", SESSION_COOKIE_NAME, token);
            cwist_http_header_add(&res->headers, "Set-Cookie", cookie);
            cwist_free(token);
        }
        form_kv_free(kv);
        redirect(res, "/");
        return;
    }
    char *u = sql_esc(username);
    cJSON *user = db_user_get_by_username(req->db, u);
    cwist_free(u);
    if (!user) {
        send_html_res(res, render_login(dark, "Invalid credentials"));
        form_kv_free(kv);
        return;
    }
    cJSON *hash = cJSON_GetObjectItem(user, "password_hash");
    if (!auth_verify_password(password, hash->valuestring)) {
        cJSON_Delete(user);
        send_html_res(res, render_login(dark, "Invalid credentials"));
        form_kv_free(kv);
        return;
    }
    int user_id = json_int(user, "id", 0);
    cJSON *uname = cJSON_GetObjectItem(user, "username");
    cJSON *role = cJSON_GetObjectItem(user, "role");
    char *token = auth_jwt_issue(user_id, uname->valuestring, role->valuestring);
    if (token) {
        char cookie[2048];
        snprintf(cookie, sizeof(cookie), "%s=%s; Path=/; Max-Age=604800; HttpOnly; SameSite=Lax", SESSION_COOKIE_NAME, token);
        cwist_http_header_add(&res->headers, "Set-Cookie", cookie);
        cwist_free(token);
    }
    cJSON_Delete(user);
    form_kv_free(kv);
    redirect(res, "/");
}

void handler_logout(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    char cookie[256];
    snprintf(cookie, sizeof(cookie), "%s=; Path=/; Max-Age=0; HttpOnly; SameSite=Lax", SESSION_COOKIE_NAME);
    cwist_http_header_add(&res->headers, "Set-Cookie", cookie);
    redirect(res, "/");
}

void handler_register_get(cwist_http_request *req, cwist_http_response *res) {
    send_html_res(res, render_register(is_dark(req), NULL));
}

void handler_register_post(cwist_http_request *req, cwist_http_response *res) {
    bool dark = is_dark(req);
    form_kv_t *kv = parse_urlencoded(req->body->data);
    const char *username = form_kv_get(kv, "username");
    const char *email = form_kv_get(kv, "email");
    const char *password = form_kv_get(kv, "password");
    if (!username || !email || !password || strlen(password) < 6) {
        send_html_res(res, render_register(dark, "Invalid input (password min 6 chars)"));
        form_kv_free(kv);
        return;
    }
    char hash[256];
    if (!auth_hash_password(password, hash, sizeof(hash))) {
        send_html_res(res, render_register(dark, "Server error"));
        form_kv_free(kv);
        return;
    }
    char *u = sql_esc(username);
    char *e = sql_esc(email);
    char *h = sql_esc(hash);
    bool ok = db_user_create(req->db, u, e, h);
    cwist_free(u); cwist_free(e); cwist_free(h);
    form_kv_free(kv);
    if (!ok) {
        send_html_res(res, render_register(dark, "Username or email already exists"));
        return;
    }
    redirect(res, "/login");
}

void handler_unregister_post(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0;
    char role[32] = {0};
    if (!auth_require_login(req, res, &uid, role, sizeof(role))) return;
    form_kv_t *kv = parse_urlencoded(req->body->data);
    const char *id_str = form_kv_get(kv, "id");
    if (id_str) {
        int target = atoi(id_str);
        if (target == uid || strcmp(role, "admin") == 0) {
            const char *cascade = form_kv_get(kv, "cascade");
            if (cascade && atoi(cascade) == 1) {
                db_user_delete_with_cascade(req->db, target, true);
            } else {
                db_user_delete_with_cascade(req->db, target, false);
            }
            if (target == uid) {
                handler_logout(req, res);
                form_kv_free(kv);
                return;
            }
        }
    }
    form_kv_free(kv);
    redirect(res, "/admin/users");
}

/* ---- Boards ---- */
void handler_profile_get(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0;
    char role[32] = {0};
    if (!auth_require_login(req, res, &uid, role, sizeof(role))) return;
    cJSON *user = db_user_get_by_id(req->db, uid);
    if (!user) {
        redirect(res, "/login");
        return;
    }
    char *pp = get_profile_pic(req->db, uid, role);
    send_html_res(res, render_profile(user, is_dark(req), role, pp, true));
    cJSON_Delete(user);
    free(pp);
}

void handler_profile_post(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0;
    char role[32] = {0};
    if (!auth_require_login(req, res, &uid, role, sizeof(role))) return;
    redirect(res, "/account/settings");
}

void handler_account_settings_get(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0;
    char role[32] = {0};
    if (!auth_require_login(req, res, &uid, role, sizeof(role))) return;
    cJSON *user = db_user_get_by_id(req->db, uid);
    if (!user) {
        redirect(res, "/login");
        return;
    }
    char *pp = get_profile_pic(req->db, uid, role);
    send_html_res(res, render_account_settings(user, is_dark(req), pp, NULL));
    cJSON_Delete(user);
    free(pp);
}

void handler_account_settings_post(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0;
    char role[32] = {0};
    if (!auth_require_login(req, res, &uid, role, sizeof(role))) return;

    const char *ctype = cwist_http_header_get(req->headers, "Content-Type");
    char *nickname = NULL, *bio = NULL;
    char *profile_pic_url = NULL;

    if (ctype && strstr(ctype, "multipart/form-data")) {
        const char *bnd = strstr(ctype, "boundary=");
        if (bnd) {
            bnd += 9;
            if (*bnd == '"') bnd++;
            size_t bnd_len = strcspn(bnd, "\"\r\n; ");
            char *boundary = (char *)cwist_alloc(bnd_len + 1);
            memcpy(boundary, bnd, bnd_len);
            boundary[bnd_len] = '\0';
            form_field_t *fields = multipart_parse(req->body->data, req->body->size, boundary);
            cwist_free(boundary);
            form_field_t *f;
            if ((f = form_find(fields, "nickname"))) {
                nickname = (char *)cwist_alloc(f->len + 1);
                memcpy(nickname, f->data, f->len);
                nickname[f->len] = '\0';
            }
            if ((f = form_find(fields, "bio"))) {
                bio = (char *)cwist_alloc(f->len + 1);
                memcpy(bio, f->data, f->len);
                bio[f->len] = '\0';
            }
            f = form_find(fields, "profile_pic");
            if (f && f->filename && f->filename[0] && f->data && f->file_size > 0) {
                const char *mt = mime_type(f->filename);
                if (mt && strncmp(mt, "image/", 6) == 0) {
                    const char *data_path = f->data;
                    profile_pic_url = (char *)cwist_alloc(512);
                    if (strncmp(data_path, "public/", 7) == 0) {
                        snprintf(profile_pic_url, 512, "/assets/%s", data_path + 7);
                    } else {
                        snprintf(profile_pic_url, 512, "%s", data_path);
                    }
                }
            }
            multipart_free(fields);
        }
    } else {
        form_kv_t *kv = parse_urlencoded(req->body->data);
        const char *n = form_kv_get(kv, "nickname");
        const char *b = form_kv_get(kv, "bio");
        nickname = (char *)cwist_alloc(strlen(n ? n : "") + 1);
        strcpy(nickname, n ? n : "");
        bio = (char *)cwist_alloc(strlen(b ? b : "") + 1);
        strcpy(bio, b ? b : "");
        form_kv_free(kv);
    }

    if (!nickname || !bio) {
        cJSON *user = db_user_get_by_id(req->db, uid);
        char *pp = get_profile_pic(req->db, uid, role);
        send_html_res(res, render_account_settings(user, is_dark(req), pp, "Invalid form data"));
        if (user) cJSON_Delete(user);
        free(pp);
        cwist_free(nickname); cwist_free(bio); cwist_free(profile_pic_url);
        return;
    }

    char *n_escaped = sql_esc(nickname);
    char *b_escaped = sql_esc(bio);

    cJSON *user = db_user_get_by_id(req->db, uid);
    if (user) {
        cJSON *pp_obj = cJSON_GetObjectItem(user, "profile_pic");
        const char *existing_pic = (pp_obj && pp_obj->type == cJSON_String) ? pp_obj->valuestring : "";
        db_user_update_profile(req->db, uid, n_escaped, b_escaped, profile_pic_url ? profile_pic_url : existing_pic);
        cJSON_Delete(user);
    } else {
        db_user_update_profile(req->db, uid, n_escaped, b_escaped, profile_pic_url ? profile_pic_url : "");
    }

    cwist_free(n_escaped); cwist_free(b_escaped);
    cwist_free(nickname); cwist_free(bio); cwist_free(profile_pic_url);
    redirect(res, "/profile");
}

void handler_password_change_get(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0;
    char role[32] = {0};
    if (!auth_require_login(req, res, &uid, role, sizeof(role))) return;
    char *pp = get_profile_pic(req->db, uid, role);
    send_html_res(res, render_password_change(is_dark(req), NULL));
    free(pp);
}

void handler_password_change_post(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0;
    char role[32] = {0};
    if (!auth_require_login(req, res, &uid, role, sizeof(role))) return;
    bool dark = is_dark(req);

    form_kv_t *kv = parse_urlencoded(req->body->data);
    const char *current = form_kv_get(kv, "current_password");
    const char *new_pw = form_kv_get(kv, "new_password");
    const char *confirm = form_kv_get(kv, "confirm_password");

    if (!current || !new_pw || !confirm || strlen(new_pw) < 6) {
        send_html_res(res, render_password_change(dark, "Invalid input (password min 6 chars)"));
        form_kv_free(kv);
        return;
    }
    if (strcmp(new_pw, confirm) != 0) {
        send_html_res(res, render_password_change(dark, "New passwords do not match"));
        form_kv_free(kv);
        return;
    }

    cJSON *user = db_user_get_by_id(req->db, uid);
    if (!user) {
        send_html_res(res, render_password_change(dark, "User not found"));
        form_kv_free(kv);
        return;
    }

    cJSON *hash = cJSON_GetObjectItem(user, "password_hash");
    if (!hash || !hash->valuestring || !auth_verify_password(current, hash->valuestring)) {
        cJSON_Delete(user);
        send_html_res(res, render_password_change(dark, "Current password is incorrect"));
        form_kv_free(kv);
        return;
    }

    char new_hash[256];
    if (!auth_hash_password(new_pw, new_hash, sizeof(new_hash))) {
        cJSON_Delete(user);
        send_html_res(res, render_password_change(dark, "Server error"));
        form_kv_free(kv);
        return;
    }

    char *h = sql_esc(new_hash);
    db_user_update_password(req->db, uid, h);
    cwist_free(h);
    cJSON_Delete(user);
    form_kv_free(kv);

    redirect(res, "/profile");
}

void handler_user_profile_get(cwist_http_request *req, cwist_http_response *res) {
    bool dark = is_dark(req);
    int viewer_uid = 0;
    char viewer_role[32] = {0};
    auth_is_logged_in(req, &viewer_uid, viewer_role, sizeof(viewer_role));

    const char *id_str = cwist_query_map_get(req->path_params, "id");
    if (!id_str) { redirect(res, "/"); return; }
    int target_uid = atoi(id_str);
    if (target_uid <= 0) { redirect(res, "/"); return; }

    cJSON *user = db_user_get_by_id(req->db, target_uid);
    if (!user) {
        res->status_code = CWIST_HTTP_NOT_FOUND;
        cwist_sstring_assign(res->body, "User not found");
        return;
    }

    char *pp = get_profile_pic(req->db, viewer_uid, viewer_role);
    bool is_own = (viewer_uid == target_uid);
    send_html_res(res, render_profile(user, dark, viewer_role, pp, is_own));
    cJSON_Delete(user);
    free(pp);
}

void handler_board_list(cwist_http_request *req, cwist_http_response *res) {
    bool dark = is_dark(req);
    int uid = 0;
    char role[32] = {0};
    auth_is_logged_in(req, &uid, role, sizeof(role));
    char *pp = get_profile_pic(req->db, uid, role);
    cJSON *boards = db_board_list(req->db);
    if (boards) {
        int n = cJSON_GetArraySize(boards);
        for (int i = 0; i < n; i++) {
            cJSON *bo = cJSON_GetArrayItem(boards, i);
            cJSON *bid = cJSON_GetObjectItem(bo, "id");
            if (bid) {
                cJSON *posts = db_post_recent_by_board(req->db, bid->valueint, 5);
                if (posts) {
                    cJSON_AddItemToObject(bo, "posts", posts);
                }
            }
        }
    }
    cwist_sstring *page = render_board_list(boards, dark, role, pp);
    if (boards) cJSON_Delete(boards);
    send_html_res(res, page);
    free(pp);
}

void handler_board_new_get(cwist_http_request *req, cwist_http_response *res) {
    if (!auth_require_admin(req, res)) return;
    int uid = 0; char role[32] = {0};
    auth_is_logged_in(req, &uid, role, sizeof(role));
    char *pp = get_profile_pic(req->db, uid, role);
    send_html_res(res, render_board_form(NULL, is_dark(req), NULL, pp));
    free(pp);
}

void handler_board_new_post(cwist_http_request *req, cwist_http_response *res) {
    if (!auth_require_admin(req, res)) return;
    form_kv_t *kv = parse_urlencoded(req->body->data);
    const char *name = form_kv_get(kv, "name");
    const char *slug = form_kv_get(kv, "slug");
    const char *desc = form_kv_get(kv, "description");
    const char *ao = form_kv_get(kv, "admin_only");
    if (!name || !slug) { redirect(res, "/board/new"); form_kv_free(kv); return; }
    char *n = sql_esc(name); char *s = sql_esc(slug); char *d = sql_esc(desc ? desc : "");
    db_board_create(req->db, n, s, d, ao != NULL, 0, 0, 0);
    cwist_free(n); cwist_free(s); cwist_free(d);
    form_kv_free(kv);
    redirect(res, "/boards");
}

void handler_board_edit_get(cwist_http_request *req, cwist_http_response *res) {
    if (!auth_require_admin(req, res)) return;
    const char *id_str = cwist_query_map_get(req->path_params, "id");
    if (!id_str) { redirect(res, "/boards"); return; }
    cJSON *board = board_by_route_key(req->db, id_str);
    if (!board) { redirect(res, "/boards"); return; }
    int uid = 0; char role[32] = {0};
    auth_is_logged_in(req, &uid, role, sizeof(role));
    char *pp = get_profile_pic(req->db, uid, role);
    cwist_sstring *page = render_board_form(board, is_dark(req), NULL, pp);
    cJSON_Delete(board);
    send_html_res(res, page);
    free(pp);
}

void handler_board_edit_post(cwist_http_request *req, cwist_http_response *res) {
    if (!auth_require_admin(req, res)) return;
    form_kv_t *kv = parse_urlencoded(req->body->data);
    const char *id_str = form_kv_get(kv, "id");
    const char *name = form_kv_get(kv, "name");
    const char *slug = form_kv_get(kv, "slug");
    const char *desc = form_kv_get(kv, "description");
    const char *ao = form_kv_get(kv, "admin_only");

    int uid = 0; char role[32] = {0};
    auth_is_logged_in(req, &uid, role, sizeof(role));
    char *pp = get_profile_pic(req->db, uid, role);

    const char *error = NULL;
    cJSON *board = NULL;
    int bid = 0;

    if (id_str) {
        bid = atoi(id_str);
        board = db_board_get_by_id(req->db, bid);
    }

    if (!id_str || !name || !slug) {
        error = "All fields are required.";
    } else {
        size_t name_len = strlen(name);
        size_t slug_len = strlen(slug);
        if (name_len == 0 || slug_len == 0) error = "Name and slug cannot be empty.";
        else if (name_len > 80) error = "Name is too long (max 80 characters).";
        else if (slug_len > 80) error = "Slug is too long (max 80 characters).";
        else {
            for (const char *p = slug; *p && !error; p++) {
                if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '_')) {
                    error = "Slug may only contain lowercase letters, numbers, hyphens and underscores.";
                }
            }
        }
    }

    if (!error && !board) {
        error = "Board not found.";
    }

    if (!error) {
        cJSON *existing = db_board_get_by_slug(req->db, slug);
        if (existing) {
            cJSON *eid = cJSON_GetObjectItem(existing, "id");
            if (!eid || eid->valueint != bid) {
                error = "A board with this slug already exists.";
            }
            cJSON_Delete(existing);
        }
    }

    if (error) {
        cwist_sstring *page = render_board_form(board, is_dark(req), error, pp);
        if (board) cJSON_Delete(board);
        send_html_res(res, page);
        free(pp);
        form_kv_free(kv);
        return;
    }

    char *n = sql_esc(name); char *s = sql_esc(slug); char *d = sql_esc(desc ? desc : "");
    if (!db_board_update(req->db, bid, n, s, d, ao != NULL, 0, 0, 0)) {
        cwist_sstring *page = render_board_form(board, is_dark(req), "Failed to update board.", pp);
        send_html_res(res, page);
        cJSON_Delete(board);
        cwist_free(n); cwist_free(s); cwist_free(d);
        free(pp);
        form_kv_free(kv);
        return;
    }

    cJSON *slug_obj = cJSON_GetObjectItem(board, "slug");
    char redirect_url[128];
    if (slug_obj && slug_obj->valuestring) {
        snprintf(redirect_url, sizeof(redirect_url), "/board/%s", slug_obj->valuestring);
    } else {
        strcpy(redirect_url, "/boards");
    }
    cJSON_Delete(board);
    cwist_free(n); cwist_free(s); cwist_free(d);
    free(pp);
    form_kv_free(kv);
    redirect(res, redirect_url);
}

void handler_board_delete(cwist_http_request *req, cwist_http_response *res) {
    if (!auth_require_admin(req, res)) return;
    const char *id_str = cwist_query_map_get(req->path_params, "id");
    if (id_str) db_board_delete(req->db, atoi(id_str));
    redirect(res, "/boards");
}

void handler_board_perms_get(cwist_http_request *req, cwist_http_response *res) {
    if (!auth_require_admin(req, res)) return;
    const char *id_str = cwist_query_map_get(req->path_params, "id");
    if (!id_str) { redirect(res, "/boards"); return; }
    cJSON *board = board_by_route_key(req->db, id_str);
    if (!board) { redirect(res, "/boards"); return; }
    int bid = json_int(board, "id", 0);
    if (bid <= 0) { cJSON_Delete(board); redirect(res, "/boards"); return; }
    cJSON *perms = db_board_perm_list(req->db, bid);
    cJSON *users = db_user_list(req->db);
    int uid = 0; char role[32] = {0};
    auth_is_logged_in(req, &uid, role, sizeof(role));
    char *pp = get_profile_pic(req->db, uid, role);
    const char *msg = cwist_query_map_get(req->query_params, "msg");
    cwist_sstring *page = render_board_perms(board, perms, users, is_dark(req), msg, pp);
    cJSON_Delete(board);
    if (perms) cJSON_Delete(perms);
    if (users) cJSON_Delete(users);
    send_html_res(res, page);
    free(pp);
}

void handler_board_perms_post(cwist_http_request *req, cwist_http_response *res) {
    if (!auth_require_admin(req, res)) return;
    form_kv_t *kv = parse_urlencoded(req->body->data);
    const char *bid = form_kv_get(kv, "board_id");
    const char *uid_str = form_kv_get(kv, "user_id");
    const char *msg = "error";
    if (bid && uid_str) {
        int board_id = atoi(bid);
        int user_id = atoi(uid_str);
        if (board_id > 0 && user_id > 0) {
            if (db_board_perm_grant(req->db, board_id, user_id)) {
                msg = "granted";
            } else {
                msg = "exists";
            }
        }
    }
    form_kv_free(kv);
    if (!bid) { redirect(res, "/boards"); return; }
    char url[128];
    snprintf(url, sizeof(url), "/board/%s/perms?msg=%s", bid, msg);
    redirect(res, url);
}

void handler_board_perms_revoke_post(cwist_http_request *req, cwist_http_response *res) {
    if (!auth_require_admin(req, res)) return;
    form_kv_t *kv = parse_urlencoded(req->body->data);
    const char *bid = form_kv_get(kv, "board_id");
    const char *uid_str = form_kv_get(kv, "user_id");
    const char *msg = "error";
    if (bid && uid_str) {
        int board_id = atoi(bid);
        int user_id = atoi(uid_str);
        if (board_id > 0 && user_id > 0 && db_board_perm_revoke(req->db, board_id, user_id)) {
            msg = "revoked";
        }
    }
    form_kv_free(kv);
    if (!bid) { redirect(res, "/boards"); return; }
    char url[128];
    snprintf(url, sizeof(url), "/board/%s/perms?msg=%s", bid, msg);
    redirect(res, url);
}

/* ---- Posts ---- */
void handler_post_list(cwist_http_request *req, cwist_http_response *res) {
    const char *slug = cwist_query_map_get(req->path_params, "slug");
    bool dark = is_dark(req);
    int uid = 0;
    char role[32] = {0};
    auth_is_logged_in(req, &uid, role, sizeof(role));
    cJSON *boards = db_board_list(req->db);
    cJSON *posts = NULL;
    int page = 1, total_pages = 1;
    const char *page_str = cwist_query_map_get(req->query_params, "page");
    if (page_str) page = atoi(page_str);
    if (page < 1) page = 1;
    int per_page = 20;
    const char *search = cwist_query_map_get(req->query_params, "search");
    if (slug) {
        cJSON *board = db_board_get_by_slug(req->db, slug);
        if (board) {
            int bid = json_int(board, "id", 0);
            int ao = json_int(board, "admin_only", 0);
            bool can = true;
            if (ao) {
                can = db_board_can_user_access(req->db, bid, uid, strcmp(role, "admin") == 0);
            }
            if (!can) {
                res->status_code = CWIST_HTTP_FORBIDDEN;
                cwist_sstring_assign(res->body, "Forbidden");
                cJSON_Delete(board);
                if (boards) cJSON_Delete(boards);
                return;
            }
            int total = db_post_count_search(req->db, bid, search);
            total_pages = (total + per_page - 1) / per_page;
            if (total_pages < 1) total_pages = 1;
            if (page > total_pages) page = total_pages;
            posts = db_post_list_search(req->db, bid, search, per_page, (page - 1) * per_page);
            cJSON_Delete(board);
        }
    } else {
        int total = db_post_count_search(req->db, 0, search);
        total_pages = (total + per_page - 1) / per_page;
        if (total_pages < 1) total_pages = 1;
        if (page > total_pages) page = total_pages;
        posts = db_post_list_search(req->db, 0, search, per_page, (page - 1) * per_page);
    }
    char *pp = get_profile_pic(req->db, uid, role);
    cwist_sstring *page_html = render_post_list(posts, boards, dark, role, page, total_pages, slug, search, pp);
    if (posts) cJSON_Delete(posts);
    if (boards) cJSON_Delete(boards);
    send_html_res(res, page_html);
    free(pp);
}

void handler_post_get(cwist_http_request *req, cwist_http_response *res) {
    const char *slug = cwist_query_map_get(req->path_params, "slug");
    if (!slug) { redirect(res, "/"); return; }
    bool dark = is_dark(req);
    int uid = 0;
    char role[32] = {0};
    auth_is_logged_in(req, &uid, role, sizeof(role));
    cJSON *post = db_post_get_by_slug(req->db, slug);
    if (!post) { res->status_code = CWIST_HTTP_NOT_FOUND; cwist_sstring_assign(res->body, "Not found"); return; }
    cJSON *pid = cJSON_GetObjectItem(post, "id");
    db_post_increment_view(req->db, pid->valueint);
    cJSON *files = db_file_list_by_post(req->db, pid->valueint);
    cJSON *comments = db_comment_list_by_target(req->db, "post", pid->valueint);
    bool verified = false;
    cJSON *sig_json = cJSON_GetObjectItem(post, "pqc_signature");
    if (sig_json && sig_json->valuestring && sig_json->valuestring[0]) {
        cJSON *t = cJSON_GetObjectItem(post, "title");
        cJSON *c = cJSON_GetObjectItem(post, "content");
        if (t && c && t->valuestring && c->valuestring) {
            size_t mlen = strlen(t->valuestring) + 1 + strlen(c->valuestring) + 1;
            char *msg = (char *)cwist_alloc(mlen);
            snprintf(msg, mlen, "%s\n%s", t->valuestring, c->valuestring);
            verified = fly_crypto_verify((const uint8_t *)msg, strlen(msg), sig_json->valuestring);
            cwist_free(msg);
        }
    }
    pid = cJSON_GetObjectItem(post, "id");
    cJSON *vote_counts = db_post_vote_counts(req->db, pid->valueint);
    int vote_up = 0, vote_down = 0, user_vote = 0;
    if (vote_counts) {
        cJSON *vu = cJSON_GetObjectItem(vote_counts, "up");
        cJSON *vd = cJSON_GetObjectItem(vote_counts, "down");
        if (vu && vu->type == cJSON_Number) vote_up = (int)vu->valuedouble;
        if (vd && vd->type == cJSON_Number) vote_down = (int)vd->valuedouble;
        cJSON_Delete(vote_counts);
    }
    if (uid > 0) user_vote = db_post_user_vote(req->db, pid->valueint, uid);
    char *pp = get_profile_pic(req->db, uid, role);
    cwist_sstring *page = render_post_detail(post, files, comments, dark, role, verified, vote_up, vote_down, user_vote, pp);
    cJSON_Delete(post);
    if (files) cJSON_Delete(files);
    if (comments) cJSON_Delete(comments);
    send_html_res(res, page);
    free(pp);
}

void handler_post_new_get(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0;
    char role[32] = {0};
    if (!auth_require_login(req, res, &uid, role, sizeof(role))) return;
    char *pp = get_profile_pic(req->db, uid, role);
    cJSON *boards = db_board_list(req->db);
    cwist_sstring *page = render_post_editor(boards, NULL, is_dark(req), role, NULL, pp);
    if (boards) cJSON_Delete(boards);
    send_html_res(res, page);
    free(pp);
}

void handler_post_new_post(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0;
    char role[32] = {0};
    if (!auth_require_login(req, res, &uid, role, sizeof(role))) return;

    const char *ctype = cwist_http_header_get(req->headers, "Content-Type");
    char *title = NULL, *content = NULL, *summary = NULL, *board_id_str = NULL;
    form_field_t *files = NULL;

    FLY_LOG_DEBUG("ctype=%s body_len=%zu", ctype ? ctype : "NULL", req->body->size);
    FLY_LOG_DEBUG("body first 80 bytes: %.80s", req->body->data);
    if (ctype && strstr(ctype, "multipart/form-data")) {
        const char *bnd = strstr(ctype, "boundary=");
        if (bnd) {
            bnd += 9;
            if (*bnd == '"') bnd++;
            size_t bnd_len = strcspn(bnd, "\"\r\n; ");
            char *boundary = (char *)cwist_alloc(bnd_len + 1);
            memcpy(boundary, bnd, bnd_len);
            boundary[bnd_len] = '\0';
            FLY_LOG_DEBUG("boundary=%s", boundary);
            files = multipart_parse(req->body->data, req->body->size, boundary);
            cwist_free(boundary);
            for (form_field_t *ff = files; ff; ff = ff->next) {
                FLY_LOG_DEBUG("field name=%s len=%zu data=%.20s", ff->name, ff->len, ff->data ? ff->data : "NULL");
            }
            form_field_t *f;
            if ((f = form_find(files, "title"))) title = (char *)cwist_alloc(f->len+1), memcpy(title, f->data, f->len), title[f->len]=0;
            if ((f = form_find(files, "content"))) content = (char *)cwist_alloc(f->len+1), memcpy(content, f->data, f->len), content[f->len]=0;
            if ((f = form_find(files, "summary"))) summary = (char *)cwist_alloc(f->len+1), memcpy(summary, f->data, f->len), summary[f->len]=0;
            if ((f = form_find(files, "board_id"))) board_id_str = (char *)cwist_alloc(f->len+1), memcpy(board_id_str, f->data, f->len), board_id_str[f->len]=0;
            FLY_LOG_DEBUG("multipart parsed: title=%s content_len=%zu board_id=%s", title ? title : "NULL", content ? strlen(content) : 0, board_id_str ? board_id_str : "NULL");
        } else {
            FLY_LOG_DEBUG("boundary not found in ctype");
        }
    } else {
        form_kv_t *kv = parse_urlencoded(req->body->data);
        title = (char *)cwist_alloc(strlen(form_kv_get(kv, "title") ? form_kv_get(kv, "title") : "")+1);
        strcpy(title, form_kv_get(kv, "title") ? form_kv_get(kv, "title") : "");
        content = (char *)cwist_alloc(strlen(form_kv_get(kv, "content") ? form_kv_get(kv, "content") : "")+1);
        strcpy(content, form_kv_get(kv, "content") ? form_kv_get(kv, "content") : "");
        summary = (char *)cwist_alloc(strlen(form_kv_get(kv, "summary") ? form_kv_get(kv, "summary") : "")+1);
        strcpy(summary, form_kv_get(kv, "summary") ? form_kv_get(kv, "summary") : "");
        board_id_str = (char *)cwist_alloc(strlen(form_kv_get(kv, "board_id") ? form_kv_get(kv, "board_id") : "0")+1);
        strcpy(board_id_str, form_kv_get(kv, "board_id") ? form_kv_get(kv, "board_id") : "0");
        form_kv_free(kv);
    }

    if (!title || !content || !title[0] || !content[0]) {
        cJSON *boards = db_board_list(req->db);
        char *pp = get_profile_pic(req->db, uid, role);
        cwist_sstring *page = render_post_editor(boards, NULL, is_dark(req), role, "Title and content required", pp);
        if (boards) cJSON_Delete(boards);
        send_html_res(res, page);
        free(pp);
        cwist_free(title); cwist_free(content); cwist_free(summary); cwist_free(board_id_str);
        multipart_free(files);
        return;
    }

    int board_id = board_id_str ? atoi(board_id_str) : 0;
    char *t = sql_esc(title);
    char *sl = generate_slug(title);
    char *c = sql_esc(content);
    char *sm = sql_esc(summary ? summary : "");

    /* PQC sign: title + "\n" + content */
    size_t msg_len = (title ? strlen(title) : 0) + 1 + (content ? strlen(content) : 0);
    char *msg = (char *)cwist_alloc(msg_len + 1);
    snprintf(msg, msg_len + 1, "%s\n%s", title ? title : "", content ? content : "");
    char *sig_b64 = NULL;
    fly_crypto_sign((const uint8_t *)msg, strlen(msg), &sig_b64);
    cwist_free(msg);

    bool created = false;
    int slug_idx = 0;
    while (!created && slug_idx < 100) {
        char *final_slug = NULL;
        if (slug_idx == 0) {
            final_slug = strdup(sl);
        } else {
            final_slug = (char *)cwist_alloc(strlen(sl) + 16);
            snprintf(final_slug, strlen(sl) + 16, "%s%d", sl, slug_idx);
        }

        cJSON *existing = db_post_get_by_slug(req->db, final_slug);
        if (existing) {
            cJSON_Delete(existing);
            slug_idx++;
            cwist_free(final_slug);
        } else {
            created = db_post_create(req->db, board_id, uid, t, final_slug, c, sm, sig_b64 ? sig_b64 : "", 0, 0, "");
            
            /* The final_slug isn't strictly needed later but we update 'sl' to point to the created slug so publish_post uses the right slug. */
            if (created) {
                 cwist_free(sl);
                 sl = final_slug;
            } else {
                 cwist_free(final_slug);
                 break;
            }
        }
    }
    if (sig_b64) cwist_free(sig_b64);

    /* Publish post metadata to NATS for distributed subscribers */
    fly_nats_publish_post(title, sl, summary ? summary : "");

    /* Handle attachments */
    if (files) {
        int post_id = (int)sqlite3_last_insert_rowid(req->db->conn);
        for (form_field_t *f = files; f; f = f->next) {
            if (f->filename && f->filename[0] != '\0' && f->data) {
                char fpath[512];
                snprintf(fpath, sizeof(fpath), "public/uploads/%s", f->filename);
                file_write(fpath, f->data, f->file_size);
                db_file_create_volume(req->db, post_id, uid, f->filename, mime_type(f->filename), fpath, f->file_size);
            }
        }
    }

    cwist_free(t); cwist_free(sl); cwist_free(c); cwist_free(sm);
    cwist_free(title); cwist_free(content); cwist_free(summary); cwist_free(board_id_str);
    multipart_free(files);
    redirect(res, "/");
}

void handler_post_edit_get(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0;
    char role[32] = {0};
    if (!auth_require_login(req, res, &uid, role, sizeof(role))) return;
    const char *id_str = cwist_query_map_get(req->path_params, "id");
    if (!id_str) { redirect(res, "/"); return; }
    cJSON *post = db_post_get_by_id(req->db, atoi(id_str));
    if (!post) { redirect(res, "/"); return; }
    cJSON *boards = db_board_list(req->db);
    char *pp = get_profile_pic(req->db, uid, role);
    cwist_sstring *page = render_post_editor(boards, post, is_dark(req), role, NULL, pp);
    cJSON_Delete(post);
    if (boards) cJSON_Delete(boards);
    send_html_res(res, page);
    free(pp);
}

void handler_post_edit_post(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0;
    char role[32] = {0};
    if (!auth_require_login(req, res, &uid, role, sizeof(role))) return;
    form_kv_t *kv = parse_urlencoded(req->body->data);
    const char *id_str = form_kv_get(kv, "id");
    const char *title = form_kv_get(kv, "title");
    const char *content = form_kv_get(kv, "content");
    const char *summary = form_kv_get(kv, "summary");
    if (!id_str || !title || !content) { redirect(res, "/"); form_kv_free(kv); return; }
    char *t = sql_esc(title); char *c = sql_esc(content); char *s = sql_esc(summary ? summary : "");
    size_t msg_len2 = (title ? strlen(title) : 0) + 1 + (content ? strlen(content) : 0);
    char *msg2 = (char *)cwist_alloc(msg_len2 + 1);
    snprintf(msg2, msg_len2 + 1, "%s\n%s", title ? title : "", content ? content : "");
    char *sig_b642 = NULL;
    fly_crypto_sign((const uint8_t *)msg2, strlen(msg2), &sig_b642);
    cwist_free(msg2);
    db_post_update(req->db, atoi(id_str), t, c, s, sig_b642 ? sig_b642 : "", 0, 0, "");
    if (sig_b642) cwist_free(sig_b642);
    cwist_free(t); cwist_free(c); cwist_free(s);
    form_kv_free(kv);
    redirect(res, "/");
}

void handler_post_delete(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0;
    char role[32] = {0};
    if (!auth_require_login(req, res, &uid, role, sizeof(role))) return;
    const char *id_str = cwist_query_map_get(req->path_params, "id");
    if (id_str) db_post_delete(req->db, atoi(id_str));
    redirect(res, "/");
}

/* ---- Files ---- */
void handler_file_repo(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0;
    char role[32] = {0};
    auth_is_logged_in(req, &uid, role, sizeof(role));
    char sql[] = "SELECT id, filename, mime_type, file_path, size, created_at FROM files ORDER BY id DESC LIMIT 200";
    cJSON *files = NULL;
    cwist_db_query(req->db, sql, &files);
    char *pp = get_profile_pic(req->db, uid, role);
    cwist_sstring *page = render_file_repo(files, is_dark(req), pp);
    if (files) cJSON_Delete(files);
    send_html_res(res, page);
    free(pp);
}

void handler_file_upload(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0;
    char role[32] = {0};
    if (!auth_require_login(req, res, &uid, role, sizeof(role))) return;
    const char *ctype = cwist_http_header_get(req->headers, "Content-Type");
    if (!ctype || !strstr(ctype, "multipart/form-data")) { redirect(res, "/files"); return; }
    const char *bnd = strstr(ctype, "boundary=");
    if (!bnd) { redirect(res, "/files"); return; }
    bnd += 9;
    form_field_t *fields = multipart_parse(req->body->data, req->body->size, bnd);
    form_field_t *f = form_find(fields, "file");
    if (f && f->filename && f->filename[0] != '\0' && f->data) {
        char fpath[512];
        snprintf(fpath, sizeof(fpath), "public/uploads/%s", f->filename);
        file_write(fpath, f->data, f->file_size);
        db_file_create_volume(req->db, 0, uid, f->filename, mime_type(f->filename), fpath, f->file_size);
    }
    multipart_free(fields);
    redirect(res, "/files");
}

void handler_file_detail_get(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0;
    char role[32] = {0};
    auth_is_logged_in(req, &uid, role, sizeof(role));
    const char *id_str = cwist_query_map_get(req->path_params, "id");
    if (!id_str) { res->status_code = CWIST_HTTP_NOT_FOUND; cwist_sstring_assign(res->body, "Not found"); return; }
    cJSON *file = db_file_get(req->db, atoi(id_str));
    if (!file) { res->status_code = CWIST_HTTP_NOT_FOUND; cwist_sstring_assign(res->body, "Not found"); return; }
    cJSON *comments = db_comment_list_by_target(req->db, "file", atoi(id_str));
    char *pp = get_profile_pic(req->db, uid, role);
    cwist_sstring *page = render_file_detail(file, comments, is_dark(req), role, pp);
    cJSON_Delete(file);
    if (comments) cJSON_Delete(comments);
    send_html_res(res, page);
    free(pp);
}

void handler_file_download(cwist_http_request *req, cwist_http_response *res) {
    const char *id_str = cwist_query_map_get(req->path_params, "id");
    if (!id_str) { res->status_code = CWIST_HTTP_NOT_FOUND; cwist_sstring_assign(res->body, "Not found"); return; }
    cJSON *file = db_file_get(req->db, atoi(id_str));
    if (!file) { res->status_code = CWIST_HTTP_NOT_FOUND; cwist_sstring_assign(res->body, "Not found"); return; }
    cJSON *fname = cJSON_GetObjectItem(file, "filename");
    cJSON *mtype = cJSON_GetObjectItem(file, "mime_type");
    cJSON *fpath = cJSON_GetObjectItem(file, "file_path");

    char cdisp[512];
    snprintf(cdisp, sizeof(cdisp), "attachment; filename=\"%s\"", fname->valuestring);
    cwist_http_header_add(&res->headers, "Content-Disposition", cdisp);

    if (fpath && fpath->valuestring[0]) {
        size_t sz = 0;
        cwist_error_t err = cwist_http_response_send_file(res, fpath->valuestring, mtype && mtype->valuestring[0] ? mtype->valuestring : "application/octet-stream", &sz);
        if (err.error.err_i32 != 0) {
            res->status_code = CWIST_HTTP_NOT_FOUND;
        }
    } else {
        res->status_code = CWIST_HTTP_NOT_FOUND;
    }
    cJSON_Delete(file);
}

void handler_file_delete(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0;
    char role[32] = {0};
    if (!auth_require_login(req, res, &uid, role, sizeof(role))) return;
    form_kv_t *kv = parse_urlencoded(req->body->data);
    const char *id_str = form_kv_get(kv, "id");
    if (id_str) {
        cJSON *f = db_file_get(req->db, atoi(id_str));
        if (f) {
            cJSON *stype = cJSON_GetObjectItem(f, "storage_type");
            cJSON *fpath = cJSON_GetObjectItem(f, "file_path");
            if (strcmp(stype->valuestring, "volume") == 0 && fpath && fpath->valuestring[0]) {
                unlink(fpath->valuestring);
            }
            cJSON_Delete(f);
        }
        db_file_delete(req->db, atoi(id_str));
    }
    form_kv_free(kv);
    redirect(res, "/files");
}

/* ---- Comments ---- */
void handler_comment_new_post(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0;
    char role[32] = {0};
    if (!auth_require_login(req, res, &uid, role, sizeof(role))) return;
    form_kv_t *kv = parse_urlencoded(req->body->data);
    const char *target_type = form_kv_get(kv, "target_type");
    const char *target_id_str = form_kv_get(kv, "target_id");
    const char *parent_id_str = form_kv_get(kv, "parent_id");
    const char *content = form_kv_get(kv, "content");
    const char *referer = cwist_http_header_get(req->headers, "Referer");
    if (target_type && target_id_str && content && content[0]) {
        int target_id = atoi(target_id_str);
        int parent_id = parent_id_str ? atoi(parent_id_str) : 0;
        db_comment_create(req->db, target_type, target_id, uid, parent_id, content);
    }
    form_kv_free(kv);
    redirect(res, referer && referer[0] ? referer : "/");
}

void handler_comment_edit_post(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0;
    char role[32] = {0};
    if (!auth_require_login(req, res, &uid, role, sizeof(role))) return;
    form_kv_t *kv = parse_urlencoded(req->body->data);
    const char *id_str = form_kv_get(kv, "id");
    const char *content = form_kv_get(kv, "content");
    const char *referer = cwist_http_header_get(req->headers, "Referer");
    if (id_str && content && content[0]) {
        db_comment_update(req->db, atoi(id_str), uid, content);
    }
    form_kv_free(kv);
    redirect(res, referer && referer[0] ? referer : "/");
}

void handler_comment_delete_get(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0;
    char role[32] = {0};
    if (!auth_require_login(req, res, &uid, role, sizeof(role))) return;
    const char *id_str = cwist_query_map_get(req->path_params, "id");
    const char *referer = cwist_http_header_get(req->headers, "Referer");
    if (id_str) {
        db_comment_delete(req->db, atoi(id_str), uid);
    }
    redirect(res, referer && referer[0] ? referer : "/");
}

/* ---- Admin ---- */
void handler_admin_users(cwist_http_request *req, cwist_http_response *res) {
    if (!auth_require_admin(req, res)) return;
    int uid = 0; char role[32] = {0};
    auth_is_logged_in(req, &uid, role, sizeof(role));
    cJSON *users = db_user_list(req->db);
    char *pp = get_profile_pic(req->db, uid, role);
    cwist_sstring *page = render_user_admin(users, is_dark(req), pp);
    if (users) cJSON_Delete(users);
    send_html_res(res, page);
    free(pp);
}

void handler_admin_user_role(cwist_http_request *req, cwist_http_response *res) {
    if (!auth_require_admin(req, res)) return;
    form_kv_t *kv = parse_urlencoded(req->body->data);
    const char *id_str = form_kv_get(kv, "id");
    const char *role = form_kv_get(kv, "role");
    if (id_str && role) db_user_update_role(req->db, atoi(id_str), role);
    form_kv_free(kv);
    redirect(res, "/admin/users");
}

/* ---- API ---- */
void handler_api_preview(cwist_http_request *req, cwist_http_response *res) {
    cwist_sstring *html = render_markdown_to_html(req->body->data);
    if (html) {
        cwist_http_header_add(&res->headers, "Content-Type", "text/html; charset=utf-8");
        cwist_sstring_assign(res->body, html->data);
        cwist_sstring_destroy(html);
    } else {
        res->status_code = CWIST_HTTP_INTERNAL_ERROR;
        cwist_sstring_assign(res->body, "render error");
    }
}

void handler_uploads_static(cwist_http_request *req, cwist_http_response *res) {
    const char *filename = cwist_query_map_get(req->path_params, "filename");
    if (!filename || !filename[0] || strchr(filename, '/') || strchr(filename, '\\')) {
        res->status_code = CWIST_HTTP_NOT_FOUND;
        cwist_sstring_assign(res->body, "Not found");
        return;
    }
    char path[512];
    snprintf(path, sizeof(path), "public/uploads/%s", filename);
    size_t sz = 0;
    char *data = file_read(path, &sz);
    if (data) {
        cwist_http_header_add(&res->headers, "Content-Type", mime_type(filename));
        cwist_sstring_assign_len(res->body, data, sz);
        cwist_free(data);
    } else {
        res->status_code = CWIST_HTTP_NOT_FOUND;
        cwist_sstring_assign(res->body, "Not found");
    }
}

void handler_api_upload(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0;
    char role[32] = {0};
    if (!auth_require_login(req, res, &uid, role, sizeof(role))) return;
    const char *ctype = cwist_http_header_get(req->headers, "Content-Type");
    if (!ctype || !strstr(ctype, "multipart/form-data")) {
        res->status_code = CWIST_HTTP_BAD_REQUEST;
        cwist_sstring_assign(res->body, "{\"ok\":false,\"error\":\"multipart required\"}");
        return;
    }
    const char *bnd = strstr(ctype, "boundary=");
    if (!bnd) {
        res->status_code = CWIST_HTTP_BAD_REQUEST;
        cwist_sstring_assign(res->body, "{\"ok\":false,\"error\":\"boundary missing\"}");
        return;
    }
    bnd += 9;
    if (*bnd == '\"') bnd++;
    size_t bnd_len = strcspn(bnd, "\"\r\n; ");
    char *boundary = (char *)cwist_alloc(bnd_len + 1);
    memcpy(boundary, bnd, bnd_len);
    boundary[bnd_len] = '\0';
    FLY_LOG_DEBUG("[UPLOAD] body size=%zu first_bytes=%.80s", req->body->size, req->body->data ? req->body->data : "(null)");
    form_field_t *fields = multipart_parse(req->body->data, req->body->size, boundary);
    cwist_free(boundary);
    form_field_t *f = form_find(fields, "file");
    FLY_LOG_DEBUG("[UPLOAD] after parse f=%p filename=%s data=%s file_size=%zu", (void*)f, f?f->filename:"(null)", f&&f->data?f->data:"(null)", f?f->file_size:0);
    cJSON *obj = cJSON_CreateObject();
    if (f && f->filename && f->data) {
        cJSON_AddBoolToObject(obj, "ok", true);
        cJSON_AddStringToObject(obj, "filename", f->filename);
        cJSON_AddStringToObject(obj, "mime_type", mime_type(f->filename));
        const char *url = f->data;
        if (strncmp(url, "public/uploads/", 15) == 0) url += 7;
        cJSON_AddStringToObject(obj, "url", url);
        cJSON_AddNumberToObject(obj, "size", (double)f->file_size);
    } else {
        cJSON_AddBoolToObject(obj, "ok", false);
        cJSON_AddStringToObject(obj, "error", "no file");
    }
    multipart_free(fields);
    char *json = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    cwist_http_header_add(&res->headers, "Content-Type", "application/json");
    cwist_sstring_assign(res->body, json ? json : "{}");
    if (json) free(json);
}

/* ---- Progressive Themes ---- */
void handler_themes_json(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    char *json = theme_build_all_json();
    cwist_http_header_add(&res->headers, "Content-Type", "application/json; charset=utf-8");
    cwist_http_header_add(&res->headers, "Cache-Control", "public, max-age=3600");
    if (json) {
        cwist_sstring_assign(res->body, json);
        free(json);
    }
}

/* ---- RSS Feed with footprint optimization ---- */
static char *rfc822_time(const char *iso) {
    static char buf[64];
    struct tm tm = {0};
    sscanf(iso, "%d-%d-%d %d:%d:%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour, &tm.tm_min, &tm.tm_sec);
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", &tm);
    return buf;
}

void handler_rss_xml(cwist_http_request *req, cwist_http_response *res) {
    cJSON *posts = db_post_list_search(req->db, 0, NULL, 20, 0);
    const char *last_modified = "";
    char etag_buf[128] = {0};
    if (posts && cJSON_GetArraySize(posts) > 0) {
        cJSON *first = cJSON_GetArrayItem(posts, 0);
        cJSON *upd = cJSON_GetObjectItem(first, "updated_at");
        if (upd && upd->valuestring) last_modified = upd->valuestring;
        snprintf(etag_buf, sizeof(etag_buf), "\"fly-%s\"", last_modified);
    }

    const char *if_none = cwist_http_header_get(req->headers, "If-None-Match");
    const char *if_mod = cwist_http_header_get(req->headers, "If-Modified-Since");
    if ((if_none && strcmp(if_none, etag_buf) == 0) || (if_mod && strcmp(if_mod, last_modified) == 0)) {
        res->status_code = 304;
        if (posts) cJSON_Delete(posts);
        return;
    }

    cwist_sstring *rss = cwist_sstring_create();
    cwist_sstring_append(rss, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<rss version=\"2.0\">\n<channel>\n");
    cwist_sstring_append(rss, "<title>"); cwist_sstring_append_escaped(rss, g_config.title); cwist_sstring_append(rss, "</title>\n");
    cwist_sstring_append(rss, "<link>/</link>\n");
    cwist_sstring_append(rss, "<description>"); cwist_sstring_append_escaped(rss, g_config.subtitle); cwist_sstring_append(rss, "</description>\n");
    cwist_sstring_append(rss, "<language>ko</language>\n");
    if (last_modified[0]) {
        cwist_sstring_append(rss, "<lastBuildDate>"); cwist_sstring_append(rss, rfc822_time(last_modified)); cwist_sstring_append(rss, "</lastBuildDate>\n");
    }

    if (posts) {
        int n = cJSON_GetArraySize(posts);
        for (int i = 0; i < n; i++) {
            cJSON *p = cJSON_GetArrayItem(posts, i);
            cJSON *slug = cJSON_GetObjectItem(p, "slug");
            cJSON *title = cJSON_GetObjectItem(p, "title");
            cJSON *summary = cJSON_GetObjectItem(p, "summary");
            cJSON *date = cJSON_GetObjectItem(p, "created_at");
            cwist_sstring_append(rss, "<item>\n");
            cwist_sstring_append(rss, "<title>"); cwist_sstring_append_escaped(rss, title ? title->valuestring : ""); cwist_sstring_append(rss, "</title>\n");
            cwist_sstring_append(rss, "<link>/post/"); cwist_sstring_append(rss, slug->valuestring); cwist_sstring_append(rss, "</link>\n");
            cwist_sstring_append(rss, "<guid>/post/"); cwist_sstring_append(rss, slug->valuestring); cwist_sstring_append(rss, "</guid>\n");
            cwist_sstring_append(rss, "<description>"); cwist_sstring_append_escaped(rss, summary && summary->valuestring ? summary->valuestring : ""); cwist_sstring_append(rss, "</description>\n");
            if (date && date->valuestring) {
                cwist_sstring_append(rss, "<pubDate>"); cwist_sstring_append(rss, rfc822_time(date->valuestring)); cwist_sstring_append(rss, "</pubDate>\n");
            }
            cwist_sstring_append(rss, "</item>\n");
        }
    }
    cwist_sstring_append(rss, "</channel>\n</rss>");
    if (posts) cJSON_Delete(posts);

    cwist_http_header_add(&res->headers, "Content-Type", "application/rss+xml; charset=utf-8");
    cwist_http_header_add(&res->headers, "Cache-Control", "public, max-age=60");
    if (etag_buf[0]) cwist_http_header_add(&res->headers, "ETag", etag_buf);
    if (last_modified[0]) cwist_http_header_add(&res->headers, "Last-Modified", last_modified);
    cwist_sstring_assign(res->body, rss->data);
    cwist_sstring_destroy(rss);
}

/* ---- Post vote ---- */
void handler_post_vote(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0;
    char role[32] = {0};
    if (!auth_require_login(req, res, &uid, role, sizeof(role))) return;
    form_kv_t *kv = parse_urlencoded(req->body->data);
    const char *post_id_str = form_kv_get(kv, "post_id");
    const char *vote_type_str = form_kv_get(kv, "vote_type");
    if (!post_id_str || !vote_type_str) {
        res->status_code = CWIST_HTTP_BAD_REQUEST;
        cwist_sstring_assign(res->body, "Missing parameters");
        form_kv_free(kv);
        return;
    }
    int post_id = atoi(post_id_str);
    int vote_type = atoi(vote_type_str);
    if (vote_type == 0) {
        db_post_vote_remove(req->db, post_id, uid);
    } else {
        db_post_vote(req->db, post_id, uid, vote_type);
    }
    cJSON *counts = db_post_vote_counts(req->db, post_id);
    int user_vote = db_post_user_vote(req->db, post_id, uid);
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddBoolToObject(obj, "ok", true);
    if (counts) {
        cJSON *up = cJSON_GetObjectItem(counts, "up");
        cJSON *down = cJSON_GetObjectItem(counts, "down");
        cJSON_AddNumberToObject(obj, "up", up && up->type == cJSON_Number ? up->valuedouble : 0);
        cJSON_AddNumberToObject(obj, "down", down && down->type == cJSON_Number ? down->valuedouble : 0);
        cJSON_Delete(counts);
    } else {
        cJSON_AddNumberToObject(obj, "up", 0);
        cJSON_AddNumberToObject(obj, "down", 0);
    }
    cJSON_AddNumberToObject(obj, "user_vote", user_vote);
    char *json = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    cwist_http_header_add(&res->headers, "Content-Type", "application/json");
    cwist_sstring_assign(res->body, json ? json : "{}");
    if (json) free(json);
    form_kv_free(kv);
}

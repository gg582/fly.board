#define _POSIX_C_SOURCE 200809L
#include "handlers_internal.h"
#include <ctype.h>
#include <stdbool.h>
#include <string.h>

static bool is_valid_sha512_hex(const char *str) {
    if (!str) return false;
    if (strlen(str) != 128) return false;
    for (int i = 0; i < 128; i++) {
        char c = str[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
            return false;
        }
    }
    return true;
}

/* Extract host (without port) from root_url like https://example.com:8443/ */
static void session_cookie_domain(char *out, size_t out_len) {
    out[0] = '\0';
    const char *url = g_config.root_url;
    if (!url || !url[0]) return;
    const char *p = strstr(url, "://");
    if (!p) p = url;
    else p += 3;
    const char *end = p;
    while (*end && *end != '/' && *end != '?' && *end != '#') end++;
    const char *port = strchr(p, ':');
    size_t len = port && port < end ? (size_t)(port - p) : (size_t)(end - p);
    if (len > 0 && len < out_len) {
        memcpy(out, p, len);
        out[len] = '\0';
    }
}

static void set_session_cookie(cwist_http_response *res, const char *token, int max_age) {
    char domain[256] = {0};
    session_cookie_domain(domain, sizeof(domain));
    const char *secure_attr = g_config.use_tls ? "; Secure" : "";
    const char *domain_attr = domain[0] ? domain : NULL;
    char cookie[2048];

    /* Clear a possible stale host-only cookie so it cannot shadow the new one. */
    if (domain_attr && max_age > 0) {
        snprintf(cookie, sizeof(cookie), "%s=; Path=/; Max-Age=0; HttpOnly; SameSite=Lax%s",
                 SESSION_COOKIE_NAME, secure_attr);
        cwist_http_header_add(&res->headers, "Set-Cookie", cookie);
    }

    if (domain_attr) {
        snprintf(cookie, sizeof(cookie), "%s=%s; Path=/; Domain=%s; Max-Age=%d; HttpOnly; SameSite=Lax%s",
                 SESSION_COOKIE_NAME, token, domain_attr, max_age, secure_attr);
    } else {
        snprintf(cookie, sizeof(cookie), "%s=%s; Path=/; Max-Age=%d; HttpOnly; SameSite=Lax%s",
                 SESSION_COOKIE_NAME, token, max_age, secure_attr);
    }
    cwist_http_header_add(&res->headers, "Set-Cookie", cookie);

    /* On logout also clear the domain-scoped cookie. */
    if (max_age == 0 && domain_attr) {
        snprintf(cookie, sizeof(cookie), "%s=; Path=/; Domain=%s; Max-Age=0; HttpOnly; SameSite=Lax%s",
                 SESSION_COOKIE_NAME, domain_attr, secure_attr);
        cwist_http_header_add(&res->headers, "Set-Cookie", cookie);
    }
}

void handler_login_get(cwist_http_request *req, cwist_http_response *res) {
    send_html_res(res, render_login(is_dark(req), NULL, is_mobile_request(req)));
}

void handler_login_post(cwist_http_request *req, cwist_http_response *res) {
    bool dark = is_dark(req);
    cwist_query_map *kv = cwist_query_map_create();
    cwist_query_map_parse(kv, req->body->data);
    const char *username = cwist_query_map_get(kv, "username");
    const char *password = cwist_query_map_get(kv, "password");
    if (!username || !password) {
        CWIST_LOG_WARN("Login failed: missing fields");
        send_html_res(res, render_login(dark, "Missing fields", is_mobile_request(req)));
        cwist_query_map_destroy(kv);
        return;
    }
    if (!is_valid_sha512_hex(password)) {
        CWIST_LOG_WARN("Login failed: invalid password format");
        send_html_res(res, render_login(dark, "Invalid credentials", is_mobile_request(req)));
        cwist_query_map_destroy(kv);
        return;
    }
    /* Admin login via admin.settings */
    if (auth_admin_check(username, password)) {
        CWIST_LOG_INFO("Admin login success: username='%s'", username);
        char *token = auth_jwt_issue(1, username, "admin");
        if (token) {
            set_session_cookie(res, token, 604800);
            cwist_free(token);
        }
        cwist_query_map_destroy(kv);
        redirect(res, "/");
        return;
    }
    cJSON *user = db_user_get_by_username(req->db, username);
    if (!user) {
        CWIST_LOG_WARN("Login failed: invalid credentials for username='%s'", username);
        send_html_res(res, render_login(dark, "Invalid credentials", is_mobile_request(req)));
        cwist_query_map_destroy(kv);
        return;
    }
    cJSON *hash = cJSON_GetObjectItem(user, "password_hash");
    if (!auth_verify_password(password, hash->valuestring)) {
        CWIST_LOG_WARN("Login failed: wrong password for username='%s'", username);
        cJSON_Delete(user);
        send_html_res(res, render_login(dark, "Invalid credentials", is_mobile_request(req)));
        cwist_query_map_destroy(kv);
        return;
    }
    CWIST_LOG_INFO("User login success: username='%s'", username);
    int user_id = json_int(user, "id", 0);
    cJSON *uname = cJSON_GetObjectItem(user, "username");
    cJSON *role = cJSON_GetObjectItem(user, "role");
    char *token = auth_jwt_issue(user_id, uname->valuestring, role->valuestring);
    if (token) {
        set_session_cookie(res, token, 604800);
        cwist_free(token);
    }
    cJSON_Delete(user);
    cwist_query_map_destroy(kv);
    redirect(res, "/");
}

void handler_logout(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    set_session_cookie(res, "", 0);
    redirect(res, "/");
}

void handler_register_get(cwist_http_request *req, cwist_http_response *res) {
    cJSON *legal = legal_load_docs();
    send_html_res(res, render_register(is_dark(req), NULL, is_mobile_request(req), legal));
    if (legal) cJSON_Delete(legal);
}

void handler_register_post(cwist_http_request *req, cwist_http_response *res) {
    bool dark = is_dark(req);
    cwist_query_map *kv = cwist_query_map_create();
    cwist_query_map_parse(kv, req->body->data);
    const char *username = cwist_query_map_get(kv, "username");
    const char *email = cwist_query_map_get(kv, "email");
    const char *password = cwist_query_map_get(kv, "password");
    if (!username || !email || !password || strlen(password) < 6) {
        CWIST_LOG_WARN("Registration failed: invalid input username='%s'", username ? username : "NULL");
        cJSON *legal = legal_load_docs();
        send_html_res(res, render_register(dark, "Invalid input (password min 6 chars)", is_mobile_request(req), legal));
        if (legal) cJSON_Delete(legal);
        cwist_query_map_destroy(kv);
        return;
    }
    if (!is_valid_sha512_hex(password)) {
        CWIST_LOG_WARN("Registration failed: invalid password format username='%s'", username ? username : "NULL");
        cJSON *legal = legal_load_docs();
        send_html_res(res, render_register(dark, "Invalid password format", is_mobile_request(req), legal));
        if (legal) cJSON_Delete(legal);
        cwist_query_map_destroy(kv);
        return;
    }

    cJSON *legal = legal_load_docs();
    if (legal) {
        bool all_required = true;
        cJSON *doc;
        cJSON_ArrayForEach(doc, legal) {
            cJSON *req_j = cJSON_GetObjectItem(doc, "required");
            if (!req_j || !req_j->valueint) continue;
            cJSON *name_j = cJSON_GetObjectItem(doc, "name");
            if (!name_j || !name_j->valuestring) continue;
            char field_name[256];
            snprintf(field_name, sizeof(field_name), "legal_%s", name_j->valuestring);
            const char *val = cwist_query_map_get(kv, field_name);
            if (!val || strcmp(val, "on") != 0) {
                all_required = false;
                break;
            }
        }
        if (!all_required) {
            send_html_res(res, render_register(dark, "You must agree to all required terms", is_mobile_request(req), legal));
            cJSON_Delete(legal);
            cwist_query_map_destroy(kv);
            return;
        }
    }

    char hash[256];
    if (!auth_hash_password(password, hash, sizeof(hash))) {
        CWIST_LOG_ERROR("Registration failed: password hash error username='%s'", username);
        send_html_res(res, render_register(dark, "Server error", is_mobile_request(req), legal));
        if (legal) cJSON_Delete(legal);
        cwist_query_map_destroy(kv);
        return;
    }
    bool ok = db_user_create(req->db, username, email, hash);
    if (!ok) {
        CWIST_LOG_WARN("Registration failed: username or email exists username='%s' email='%s'", username, email);
        send_html_res(res, render_register(dark, "Username or email already exists", is_mobile_request(req), legal));
        if (legal) cJSON_Delete(legal);
        cwist_query_map_destroy(kv);
        return;
    }
    if (legal) cJSON_Delete(legal);
    cwist_query_map_destroy(kv);
    CWIST_LOG_INFO("User registered: username='%s' email='%s'", username, email);
    redirect(res, "/login");
}

void handler_unregister_post(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0;
    char role[32] = {0};
    if (!auth_require_login(req, res, &uid, role, sizeof(role))) return;
    cwist_query_map *kv = cwist_query_map_create();
    cwist_query_map_parse(kv, req->body->data);
    const char *id_str = cwist_query_map_get(kv, "id");
    if (id_str) {
        int target = atoi(id_str);
        if (target > 0 && (target == uid || strcmp(role, "admin") == 0)) {
            const char *cascade = cwist_query_map_get(kv, "cascade");
            bool cascade_del = cascade && atoi(cascade) == 1;
            db_user_delete_with_cascade(req->db, target, cascade_del);
            CWIST_LOG_INFO("User unregistered: target_uid=%d by_uid=%d cascade=%d", target, uid, cascade_del);
            if (target == uid) {
                handler_logout(req, res);
                cwist_query_map_destroy(kv);
                return;
            }
        } else {
            CWIST_LOG_WARN("User unregister forbidden: target_uid=%s by_uid=%d role=%s", id_str, uid, role);
        }
    }
    cwist_query_map_destroy(kv);
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
    send_html_res(res, render_profile(user, is_dark(req), role, pp, true, is_mobile_request(req)));
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

    int target_uid = uid;
    const char *id_str = cwist_query_map_get(req->query_params, "id");
    if (id_str && strcmp(role, "admin") == 0) {
        target_uid = atoi(id_str);
    }

    if (target_uid <= 0) {
        res->status_code = CWIST_HTTP_NOT_FOUND;
        cwist_sstring_assign(res->body, "User not found");
        return;
    }

    cJSON *user = db_user_get_by_id(req->db, target_uid);
    if (!user) {
        if (target_uid == uid) redirect(res, "/login");
        else {
            res->status_code = CWIST_HTTP_NOT_FOUND;
            cwist_sstring_assign(res->body, "User not found");
        }
        return;
    }
    char *pp = get_profile_pic(req->db, uid, role);
    send_html_res(res, render_account_settings(user, is_dark(req), role, pp, NULL, is_mobile_request(req)));
    cJSON_Delete(user);
    free(pp);
}

static const char *profile_ext_from_mime(const char *mime, const char *filename) {
    if (mime) {
        if (strcmp(mime, "image/png") == 0) return ".png";
        if (strcmp(mime, "image/jpeg") == 0) return ".jpg";
        if (strcmp(mime, "image/gif") == 0) return ".gif";
        if (strcmp(mime, "image/webp") == 0) return ".webp";
        if (strcmp(mime, "image/svg+xml") == 0) return ".svg";
    }
    const char *dot = filename ? strrchr(filename, '.') : NULL;
    if (!dot || strlen(dot) > 10) return ".img";
    for (const char *p = dot + 1; *p; ++p) {
        if (!isalnum((unsigned char)*p)) return ".img";
    }
    return dot;
}

static bool persist_profile_pic_upload(form_field_t *f, int target_uid, char **out_url) {
    if (!f || !f->filename || !f->filename[0] || !f->data || !f->data[0] || f->file_size == 0 || !out_url) {
        return false;
    }

    char detected_mime[128] = {0};
    const char *mt = mime_type(f->filename);
    if (mime_type_from_data(f->data, detected_mime, sizeof(detected_mime))) {
        mt = detected_mime;
    }
    if (!mt || strncmp(mt, "image/", 6) != 0) {
        unlink(f->data);
        return false;
    }
    if (!dir_ensure("public/profile")) {
        return false;
    }

    const char *ext = profile_ext_from_mime(mt, f->filename);
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        ts.tv_sec = time(NULL);
        ts.tv_nsec = 0;
    }
    char stored_name[256];
    snprintf(stored_name, sizeof(stored_name), "profile_%d_%lld_%ld_%zu%s",
             target_uid > 0 ? target_uid : 0,
             (long long)ts.tv_sec,
             ts.tv_nsec,
             f->file_size,
             ext);

    char target_path[PATH_MAX];
    int written = snprintf(target_path, sizeof(target_path), "public/profile/%s", stored_name);
    if (written < 0 || written >= (int)sizeof(target_path)) return false;

    if (rename(f->data, target_path) != 0) {
        size_t len = 0;
        char *data = file_read(f->data, &len);
        if (!data) return false;
        bool ok = file_write(target_path, data, len);
        cwist_free(data);
        if (!ok) return false;
        unlink(f->data);
    }

    *out_url = (char *)cwist_alloc(512);
    snprintf(*out_url, 512, "/assets/profile/%s", stored_name);
    return true;
}

void handler_account_settings_post(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0;
    char role[32] = {0};
    if (!auth_require_login(req, res, &uid, role, sizeof(role))) return;

    const char *ctype = cwist_http_header_get(req->headers, "Content-Type");
    char *nickname = NULL, *bio = NULL;
    char *profile_pic_url = NULL;
    int target_uid = uid;

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
            if ((f = form_find(fields, "id")) && strcmp(role, "admin") == 0) {
                int parsed = atoi(f->data);
                if (parsed > 0) target_uid = parsed;
            }
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
                persist_profile_pic_upload(f, target_uid, &profile_pic_url);
            }
            multipart_free(fields);
        }
    } else {
        cwist_query_map *kv = cwist_query_map_create();
        cwist_query_map_parse(kv, req->body->data);
        const char *id_str = cwist_query_map_get(kv, "id");
        if (id_str && strcmp(role, "admin") == 0) {
            int parsed = atoi(id_str);
            if (parsed > 0) target_uid = parsed;
        }
        const char *n = cwist_query_map_get(kv, "nickname");
        const char *b = cwist_query_map_get(kv, "bio");
        nickname = (char *)cwist_alloc(strlen(n ? n : "") + 1);
        strcpy(nickname, n ? n : "");
        bio = (char *)cwist_alloc(strlen(b ? b : "") + 1);
        strcpy(bio, b ? b : "");
        cwist_query_map_destroy(kv);
    }

    if (!nickname || !bio) {
        cJSON *user = db_user_get_by_id(req->db, target_uid);
        char *pp = get_profile_pic(req->db, uid, role);
        send_html_res(res, render_account_settings(user, is_dark(req), role, pp, "Invalid form data", is_mobile_request(req)));
        if (user) cJSON_Delete(user);
        free(pp);
        cwist_free(nickname); cwist_free(bio); cwist_free(profile_pic_url);
        return;
    }

    cJSON *user = db_user_get_by_id(req->db, target_uid);
    if (user) {
        cJSON *pp_obj = cJSON_GetObjectItem(user, "profile_pic");
        const char *existing_pic = (pp_obj && pp_obj->type == cJSON_String) ? pp_obj->valuestring : "";
        db_user_update_profile(req->db, target_uid, nickname, bio, profile_pic_url ? profile_pic_url : existing_pic);
        cJSON_Delete(user);
    } else if (target_uid > 0) {
        db_user_update_profile(req->db, target_uid, nickname, bio, profile_pic_url ? profile_pic_url : "");
    }
    cwist_free(nickname); cwist_free(bio); cwist_free(profile_pic_url);
    
    if (target_uid != uid) {
        redirect(res, "/admin/users");
    } else {
        redirect(res, "/profile");
    }
}

void handler_password_change_get(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0;
    char role[32] = {0};
    if (!auth_require_login(req, res, &uid, role, sizeof(role))) return;
    char *pp = get_profile_pic(req->db, uid, role);
    send_html_res(res, render_password_change(is_dark(req), role, pp, NULL, is_mobile_request(req)));
    free(pp);
}

void handler_password_change_post(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0;
    char role[32] = {0};
    if (!auth_require_login(req, res, &uid, role, sizeof(role))) return;
    bool dark = is_dark(req);
    char *pp = get_profile_pic(req->db, uid, role);

    cwist_query_map *kv = cwist_query_map_create();
    cwist_query_map_parse(kv, req->body->data);
    const char *current = cwist_query_map_get(kv, "current_password");
    const char *new_pw = cwist_query_map_get(kv, "new_password");
    const char *confirm = cwist_query_map_get(kv, "confirm_password");

    if (!current || !new_pw || !confirm || strlen(new_pw) < 6) {
        CWIST_LOG_WARN("Password change failed: invalid input uid=%d", uid);
        send_html_res(res, render_password_change(dark, role, pp, "Invalid input (password min 6 chars)", is_mobile_request(req)));
        free(pp);
        cwist_query_map_destroy(kv);
        return;
    }
    if (!is_valid_sha512_hex(current) || !is_valid_sha512_hex(new_pw) || !is_valid_sha512_hex(confirm)) {
        CWIST_LOG_WARN("Password change failed: invalid password format uid=%d", uid);
        send_html_res(res, render_password_change(dark, role, pp, "Invalid password format", is_mobile_request(req)));
        free(pp);
        cwist_query_map_destroy(kv);
        return;
    }
    if (strcmp(new_pw, confirm) != 0) {
        CWIST_LOG_WARN("Password change failed: new passwords do not match uid=%d", uid);
        send_html_res(res, render_password_change(dark, role, pp, "New passwords do not match", is_mobile_request(req)));
        free(pp);
        cwist_query_map_destroy(kv);
        return;
    }

    cJSON *user = db_user_get_by_id(req->db, uid);
    if (!user) {
        send_html_res(res, render_password_change(dark, role, pp, "User not found", is_mobile_request(req)));
        free(pp);
        cwist_query_map_destroy(kv);
        return;
    }

    cJSON *hash = cJSON_GetObjectItem(user, "password_hash");
    if (!hash || !hash->valuestring || !auth_verify_password(current, hash->valuestring)) {
        CWIST_LOG_WARN("Password change failed: current password incorrect uid=%d", uid);
        cJSON_Delete(user);
        send_html_res(res, render_password_change(dark, role, pp, "Current password is incorrect", is_mobile_request(req)));
        free(pp);
        cwist_query_map_destroy(kv);
        return;
    }

    char new_hash[256];
    if (!auth_hash_password(new_pw, new_hash, sizeof(new_hash))) {
        cJSON_Delete(user);
        send_html_res(res, render_password_change(dark, role, pp, "Server error", is_mobile_request(req)));
        free(pp);
        cwist_query_map_destroy(kv);
        return;
    }

    db_user_update_password(req->db, uid, new_hash);
    CWIST_LOG_INFO("Password changed: uid=%d", uid);
    cJSON_Delete(user);
    cwist_query_map_destroy(kv);
    free(pp);

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
    send_html_res(res, render_profile(user, dark, viewer_role, pp, is_own, is_mobile_request(req)));
    cJSON_Delete(user);
    free(pp);
}

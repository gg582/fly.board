#define _POSIX_C_SOURCE 200809L
#include "handlers_internal.h"

void handler_login_get(cwist_http_request *req, cwist_http_response *res) {
    send_html_res(res, render_login(is_dark(req), NULL));
}

void handler_login_post(cwist_http_request *req, cwist_http_response *res) {
    bool dark = is_dark(req);
    cwist_query_map *kv = cwist_query_map_create();
    cwist_query_map_parse(kv, req->body->data);
    const char *username = cwist_query_map_get(kv, "username");
    const char *password = cwist_query_map_get(kv, "password");
    if (!username || !password) {
        CWIST_LOG_WARN("Login failed: missing fields");
        send_html_res(res, render_login(dark, "Missing fields"));
        cwist_query_map_destroy(kv);
        return;
    }
    /* Admin login via admin.settings */
    if (auth_admin_check(username, password)) {
        CWIST_LOG_INFO("Admin login success: username='%s'", username);
        char *token = auth_jwt_issue(1, username, "admin");
        if (token) {
            char cookie[2048];
            snprintf(cookie, sizeof(cookie), "%s=%s; Path=/; Max-Age=604800; HttpOnly; SameSite=Lax", SESSION_COOKIE_NAME, token);
            cwist_http_header_add(&res->headers, "Set-Cookie", cookie);
            cwist_free(token);
        }
        cwist_query_map_destroy(kv);
        redirect(res, "/");
        return;
    }
    cJSON *user = db_user_get_by_username(req->db, username);
    if (!user) {
        CWIST_LOG_WARN("Login failed: invalid credentials for username='%s'", username);
        send_html_res(res, render_login(dark, "Invalid credentials"));
        cwist_query_map_destroy(kv);
        return;
    }
    cJSON *hash = cJSON_GetObjectItem(user, "password_hash");
    if (!auth_verify_password(password, hash->valuestring)) {
        CWIST_LOG_WARN("Login failed: wrong password for username='%s'", username);
        cJSON_Delete(user);
        send_html_res(res, render_login(dark, "Invalid credentials"));
        cwist_query_map_destroy(kv);
        return;
    }
    CWIST_LOG_INFO("User login success: username='%s'", username);
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
    cwist_query_map_destroy(kv);
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
    cwist_query_map *kv = cwist_query_map_create();
    cwist_query_map_parse(kv, req->body->data);
    const char *username = cwist_query_map_get(kv, "username");
    const char *email = cwist_query_map_get(kv, "email");
    const char *password = cwist_query_map_get(kv, "password");
    if (!username || !email || !password || strlen(password) < 6) {
        CWIST_LOG_WARN("Registration failed: invalid input username='%s'", username ? username : "NULL");
        send_html_res(res, render_register(dark, "Invalid input (password min 6 chars)"));
        cwist_query_map_destroy(kv);
        return;
    }
    char hash[256];
    if (!auth_hash_password(password, hash, sizeof(hash))) {
        CWIST_LOG_ERROR("Registration failed: password hash error username='%s'", username);
        send_html_res(res, render_register(dark, "Server error"));
        cwist_query_map_destroy(kv);
        return;
    }
    bool ok = db_user_create(req->db, username, email, hash);
    cwist_query_map_destroy(kv);
    if (!ok) {
        CWIST_LOG_WARN("Registration failed: username or email exists username='%s' email='%s'", username, email);
        send_html_res(res, render_register(dark, "Username or email already exists"));
        return;
    }
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
        if (target == uid || strcmp(role, "admin") == 0) {
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

    int target_uid = uid;
    const char *id_str = cwist_query_map_get(req->query_params, "id");
    if (id_str && strcmp(role, "admin") == 0) {
        target_uid = atoi(id_str);
    }

    if (target_uid <= 0 && strcmp(role, "admin") == 0 && target_uid == uid) {
        redirect(res, "/admin/users");
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
    send_html_res(res, render_account_settings(user, is_dark(req), role, pp, NULL));
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
                target_uid = atoi(f->data);
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
                const char *mt = mime_type(f->filename);
                if (mt && strncmp(mt, "image/", 6) == 0) {
                    const char *data_path = f->data;
                    profile_pic_url = (char *)cwist_alloc(512);
                    if (strncmp(data_path, "public/uploads/", 15) == 0) {
                        snprintf(profile_pic_url, 512, "/assets/profile/%s", data_path + 15);
                    } else if (strncmp(data_path, "public/", 7) == 0) {
                        snprintf(profile_pic_url, 512, "/assets/%s", data_path + 7);
                    } else {
                        // If it's already an absolute or relative URL, keep it
                        snprintf(profile_pic_url, 512, "%s", data_path);
                    }
                }
            }
            multipart_free(fields);
        }
    } else {
        cwist_query_map *kv = cwist_query_map_create();
        cwist_query_map_parse(kv, req->body->data);
        const char *id_str = cwist_query_map_get(kv, "id");
        if (id_str && strcmp(role, "admin") == 0) {
            target_uid = atoi(id_str);
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
        send_html_res(res, render_account_settings(user, is_dark(req), role, pp, "Invalid form data"));
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
    send_html_res(res, render_password_change(is_dark(req), role, pp, NULL));
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
        send_html_res(res, render_password_change(dark, role, pp, "Invalid input (password min 6 chars)"));
        free(pp);
        cwist_query_map_destroy(kv);
        return;
    }
    if (strcmp(new_pw, confirm) != 0) {
        CWIST_LOG_WARN("Password change failed: new passwords do not match uid=%d", uid);
        send_html_res(res, render_password_change(dark, role, pp, "New passwords do not match"));
        free(pp);
        cwist_query_map_destroy(kv);
        return;
    }

    cJSON *user = db_user_get_by_id(req->db, uid);
    if (!user) {
        send_html_res(res, render_password_change(dark, role, pp, "User not found"));
        free(pp);
        cwist_query_map_destroy(kv);
        return;
    }

    cJSON *hash = cJSON_GetObjectItem(user, "password_hash");
    if (!hash || !hash->valuestring || !auth_verify_password(current, hash->valuestring)) {
        CWIST_LOG_WARN("Password change failed: current password incorrect uid=%d", uid);
        cJSON_Delete(user);
        send_html_res(res, render_password_change(dark, role, pp, "Current password is incorrect"));
        free(pp);
        cwist_query_map_destroy(kv);
        return;
    }

    char new_hash[256];
    if (!auth_hash_password(new_pw, new_hash, sizeof(new_hash))) {
        cJSON_Delete(user);
        send_html_res(res, render_password_change(dark, role, pp, "Server error"));
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
    send_html_res(res, render_profile(user, dark, viewer_role, pp, is_own));
    cJSON_Delete(user);
    free(pp);
}

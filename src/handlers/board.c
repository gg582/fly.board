#define _POSIX_C_SOURCE 200809L
#include "handlers_internal.h"

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

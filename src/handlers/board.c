#define _POSIX_C_SOURCE 200809L
#include "handlers_internal.h"
#include "cwist/board_tree.h"

static int board_depth(int board_id, cJSON *tree) {
    int depth = 0;
    int current = board_id;
    while (current > 0 && tree) {
        int n = cJSON_GetArraySize(tree);
        int parent = 0;
        for (int i = 0; i < n; i++) {
            cJSON *node = cJSON_GetArrayItem(tree, i);
            if (json_int(node, "board_id", 0) == current) {
                parent = json_int(node, "parent_board_id", 0);
                break;
            }
        }
        if (parent <= 0) break;
        depth++;
        if (parent == current) break;
        current = parent;
    }
    return depth;
}

static void append_boards_flat(cJSON *out, cJSON *boards, cJSON *tree, int parent_id, int max_depth) {
    if (!boards || !out) return;
    int n = cJSON_GetArraySize(boards);
    for (int i = 0; i < n; i++) {
        cJSON *bo = cJSON_GetArrayItem(boards, i);
        int bid = json_int(bo, "id", 0);
        int pid = 0;
        if (tree) {
            int tn = cJSON_GetArraySize(tree);
            for (int j = 0; j < tn; j++) {
                cJSON *node = cJSON_GetArrayItem(tree, j);
                if (json_int(node, "board_id", 0) == bid) {
                    pid = json_int(node, "parent_board_id", 0);
                    break;
                }
            }
        }
        if (pid != parent_id) continue;
        int depth = board_depth(bid, tree);
        if (depth > max_depth) continue;
        cJSON *copy = cJSON_Duplicate(bo, 1);
        if (copy) {
            cJSON_AddNumberToObject(copy, "depth", depth);
            cJSON_AddItemToArray(out, copy);
        }
        append_boards_flat(out, boards, tree, bid, max_depth);
    }
}

void handler_board_list(cwist_http_request *req, cwist_http_response *res) {
    bool dark = is_dark(req);
    int uid = 0;
    char role[32] = {0};
    auth_is_logged_in(req, &uid, role, sizeof(role));
    char *pp = get_profile_pic(req->db, uid, role);
    cJSON *boards = db_board_list(req->db);
    cJSON *tree = db_board_tree_get_all();
    if (boards) {
        int n = cJSON_GetArraySize(boards);
        for (int i = 0; i < n; i++) {
            cJSON *bo = cJSON_GetArrayItem(boards, i);
            cJSON *bid = cJSON_GetObjectItem(bo, "id");
            if (bid) {
                cJSON *posts = db_post_recent_by_board(req->db, json_int(bo, "id", 0), 2);
                if (posts) {
                    cJSON_AddItemToObject(bo, "posts", posts);
                }
            }
        }
    }
    cJSON *ordered = cJSON_CreateArray();
    append_boards_flat(ordered, boards, tree, 0, 2);
    cwist_sstring *page = render_board_list(ordered, dark, role, pp, is_mobile_request(req));
    cJSON_Delete(ordered);
    if (boards) cJSON_Delete(boards);
    if (tree) cJSON_Delete(tree);
    send_html_res(res, page);
    free(pp);
}

void handler_board_new_get(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0; char role[32] = {0};
    if (!auth_require_login(req, res, &uid, role, sizeof(role))) return;
    char *pp = get_profile_pic(req->db, uid, role);
    cJSON *boards = db_board_list(req->db);
    send_html_res(res, render_board_form(NULL, boards, is_dark(req), NULL, pp, is_mobile_request(req), role));
    if (boards) cJSON_Delete(boards);
    free(pp);
}

void handler_board_new_post(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0; char role[32] = {0};
    if (!auth_require_login(req, res, &uid, role, sizeof(role))) return;
    cwist_query_map *kv = cwist_query_map_create(); cwist_query_map_parse(kv, req->body->data);
    const char *name = cwist_query_map_get(kv, "name");
    const char *slug = cwist_query_map_get(kv, "slug");
    const char *desc = cwist_query_map_get(kv, "description");
    const char *ao = cwist_query_map_get(kv, "admin_only");
    const char *parent_id_str = cwist_query_map_get(kv, "parent_id");
    const char *error = NULL;
    if (!name || !slug) {
        error = "Name and slug are required.";
        CWIST_LOG_WARN("Board creation failed: missing name or slug");
    } else {
        size_t name_len = strlen(name);
        size_t slug_len = strlen(slug);
        if (name_len == 0 || slug_len == 0) error = "Name and slug cannot be empty.";
        else if (name_len > 80) { error = "Name is too long (max 80 characters)."; CWIST_LOG_WARN("Board creation POST: name too long"); }
        else if (slug_len > 80) { error = "Slug is too long (max 80 characters)."; CWIST_LOG_WARN("Board creation POST: slug too long"); }
        else {
            for (const char *p = slug; *p && !error; p++) {
                if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '_')) {
                    error = "Slug may only contain lowercase letters, numbers, hyphens and underscores.";
                    CWIST_LOG_WARN("Board creation POST: invalid slug='%s'", slug);
                }
            }
        }
    }

    if (error) {
        int uid = 0; char role[32] = {0};
        auth_is_logged_in(req, &uid, role, sizeof(role));
        char *pp = get_profile_pic(req->db, uid, role);
        cJSON *boards = db_board_list(req->db);
        cwist_sstring *page = render_board_form(NULL, boards, is_dark(req), error, pp, is_mobile_request(req), role);
        if (boards) cJSON_Delete(boards);
        send_html_res(res, page);
        free(pp);
        cwist_query_map_destroy(kv);
        return;
    }

    bool is_admin = (role[0] && strcmp(role, "admin") == 0);
    if (db_board_create(req->db, name, slug, desc ? desc : "", is_admin && (ao != NULL), 0, 0, 0)) {
        cJSON *created = db_board_get_by_slug(req->db, slug);
        if (created) {
            int bid = json_int(created, "id", 0);
            int parent_id = parent_id_str ? atoi(parent_id_str) : 0;
            if (bid > 0 && parent_id > 0) {
                db_board_tree_set_parent(bid, parent_id);
            } else if (bid > 0) {
                db_board_tree_set_parent(bid, 0);
            }
            cJSON_Delete(created);
        }
        CWIST_LOG_INFO("Board created: name='%s' slug='%s'", name, slug);
    } else {
        CWIST_LOG_ERROR("Board creation failed: name='%s' slug='%s'", name, slug);
    }
    cwist_query_map_destroy(kv);
    const char *referer = cwist_http_header_get(req->headers, "Referer");
    redirect(res, referer && referer[0] ? referer : "/boards");
}

void handler_board_edit_get(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0; char role[32] = {0};
    if (!auth_require_login(req, res, &uid, role, sizeof(role))) return;
    const char *id_str = cwist_query_map_get(req->path_params, "id");
    if (!id_str) { redirect(res, "/boards"); return; }
    cJSON *board = board_by_route_key(req->db, id_str);
    if (!board) { CWIST_LOG_WARN("Board edit GET: board not found id=%s", id_str); redirect(res, "/boards"); return; }
    bool is_admin = (role[0] && strcmp(role, "admin") == 0);
    int bid = json_int(board, "id", 0);
    if (!is_admin && json_int(board, "user_id", 0) != uid && json_int(board, "user_id", 0) != 0) {
        redirect(res, "/boards");
        cJSON_Delete(board);
        return;
    }
    char *pp = get_profile_pic(req->db, uid, role);
    cJSON *all_boards = db_board_list(req->db);
    int parent_id = db_board_tree_get_parent(bid);
    cJSON_AddNumberToObject(board, "parent_id", parent_id);
    cwist_sstring *page = render_board_form(board, all_boards, is_dark(req), NULL, pp, is_mobile_request(req), role);
    if (all_boards) cJSON_Delete(all_boards);
    cJSON_Delete(board);
    send_html_res(res, page);
    free(pp);
}

void handler_board_edit_post(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0; char role[32] = {0};
    if (!auth_require_login(req, res, &uid, role, sizeof(role))) return;
    cwist_query_map *kv = cwist_query_map_create(); cwist_query_map_parse(kv, req->body->data);
    const char *id_str = cwist_query_map_get(kv, "id");
    const char *name = cwist_query_map_get(kv, "name");
    const char *slug = cwist_query_map_get(kv, "slug");
    const char *desc = cwist_query_map_get(kv, "description");
    const char *ao = cwist_query_map_get(kv, "admin_only");

    bool is_admin = (role[0] && strcmp(role, "admin") == 0);
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
        CWIST_LOG_WARN("Board edit POST: missing fields id=%s", id_str ? id_str : "NULL");
    } else {
        size_t name_len = strlen(name);
        size_t slug_len = strlen(slug);
        if (name_len == 0 || slug_len == 0) error = "Name and slug cannot be empty.";
        else if (name_len > 80) { error = "Name is too long (max 80 characters)."; CWIST_LOG_WARN("Board edit POST: name too long"); }
        else if (slug_len > 80) { error = "Slug is too long (max 80 characters)."; CWIST_LOG_WARN("Board edit POST: slug too long"); }
        else {
            for (const char *p = slug; *p && !error; p++) {
                if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '_')) {
                    error = "Slug may only contain lowercase letters, numbers, hyphens and underscores.";
                    CWIST_LOG_WARN("Board edit POST: invalid slug='%s'", slug);
                }
            }
        }
    }

    if (!error && !board) {
        error = "Board not found.";
        CWIST_LOG_WARN("Board edit POST: board not found id=%s", id_str);
    }

    if (!error) {
        cJSON *existing = db_board_get_by_slug(req->db, slug);
        if (existing) {
            cJSON *eid = cJSON_GetObjectItem(existing, "id");
            if (!eid || eid->valueint != bid) {
                error = "A board with this slug already exists.";
                CWIST_LOG_WARN("Board edit POST: slug conflict slug='%s' bid=%d", slug, bid);
            }
            cJSON_Delete(existing);
        }
    }

    if (error) {
        cwist_sstring *page = render_board_form(board, NULL, is_dark(req), error, pp, is_mobile_request(req), role);
        if (board) cJSON_Delete(board);
        send_html_res(res, page);
        free(pp);
        cwist_query_map_destroy(kv);
        return;
    }

    int parent_id = 0;
    const char *parent_id_str = cwist_query_map_get(kv, "parent_id");
    if (parent_id_str) parent_id = atoi(parent_id_str);
    if (parent_id > 0 && parent_id == bid) parent_id = 0;

    bool admin_only_val = is_admin ? (ao != NULL) : json_int(board, "admin_only", 0);
    if (!db_board_update(req->db, bid, name, slug, desc ? desc : "", admin_only_val, 0, 0, 0)) {
        CWIST_LOG_ERROR("Board update failed: bid=%d", bid);
        cwist_sstring *page = render_board_form(board, NULL, is_dark(req), "Failed to update board.", pp, is_mobile_request(req), role);
        send_html_res(res, page);
        cJSON_Delete(board);
        free(pp);
        cwist_query_map_destroy(kv);
        return;
    }
    db_board_tree_set_parent(bid, parent_id);
    CWIST_LOG_INFO("Board updated: bid=%d name='%s' slug='%s' parent=%d", bid, name, slug, parent_id);

    cJSON *slug_obj = cJSON_GetObjectItem(board, "slug");
    char redirect_url[128];
    if (slug_obj && slug_obj->valuestring) {
        snprintf(redirect_url, sizeof(redirect_url), "/board/%s", slug_obj->valuestring);
    } else {
        strcpy(redirect_url, "/boards");
    }
    cJSON_Delete(board);
    free(pp);
    cwist_query_map_destroy(kv);
    const char *referer = cwist_http_header_get(req->headers, "Referer");
    redirect(res, referer && referer[0] ? referer : redirect_url);
}

void handler_board_delete(cwist_http_request *req, cwist_http_response *res) {
    if (!auth_require_admin(req, res)) return;
    const char *id_str = cwist_query_map_get(req->path_params, "id");
    if (id_str) {
        int bid = atoi(id_str);
        db_board_tree_promote_children(bid);
        db_board_delete(req->db, bid);
        db_board_tree_remove(bid);
        CWIST_LOG_INFO("Board deleted: bid=%d", bid);
    }
    const char *referer = cwist_http_header_get(req->headers, "Referer");
    redirect(res, referer && referer[0] ? referer : "/boards");
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
    cwist_sstring *page = render_board_perms(board, perms, users, is_dark(req), msg, pp, is_mobile_request(req));
    cJSON_Delete(board);
    if (perms) cJSON_Delete(perms);
    if (users) cJSON_Delete(users);
    send_html_res(res, page);
    free(pp);
}

void handler_board_perms_post(cwist_http_request *req, cwist_http_response *res) {
    if (!auth_require_admin(req, res)) return;
    cwist_query_map *kv = cwist_query_map_create(); cwist_query_map_parse(kv, req->body->data);
    const char *bid = cwist_query_map_get(kv, "board_id");
    const char *uid_str = cwist_query_map_get(kv, "user_id");
    const char *msg = "error";
    if (bid && uid_str) {
        int board_id = atoi(bid);
        int user_id = atoi(uid_str);
        if (board_id > 0 && user_id > 0) {
            if (db_board_perm_grant(req->db, board_id, user_id)) {
                msg = "granted";
                CWIST_LOG_INFO("Board permission granted: board_id=%d user_id=%d", board_id, user_id);
            } else {
                msg = "exists";
                CWIST_LOG_WARN("Board permission already exists: board_id=%d user_id=%d", board_id, user_id);
            }
        }
    }
    cwist_query_map_destroy(kv);
    if (!bid) { redirect(res, "/boards"); return; }
    char url[128];
    snprintf(url, sizeof(url), "/board/%s/perms?msg=%s", bid, msg);
    redirect(res, url);
}

void handler_board_perms_revoke_post(cwist_http_request *req, cwist_http_response *res) {
    if (!auth_require_admin(req, res)) return;
    cwist_query_map *kv = cwist_query_map_create(); cwist_query_map_parse(kv, req->body->data);
    const char *bid = cwist_query_map_get(kv, "board_id");
    const char *uid_str = cwist_query_map_get(kv, "user_id");
    const char *msg = "error";
    if (bid && uid_str) {
        int board_id = atoi(bid);
        int user_id = atoi(uid_str);
        if (board_id > 0 && user_id > 0 && db_board_perm_revoke(req->db, board_id, user_id)) {
            msg = "revoked";
            CWIST_LOG_INFO("Board permission revoked: board_id=%d user_id=%d", board_id, user_id);
        }
    }
    cwist_query_map_destroy(kv);
    if (!bid) { redirect(res, "/boards"); return; }
    char url[128];
    snprintf(url, sizeof(url), "/board/%s/perms?msg=%s", bid, msg);
    redirect(res, url);
}

/* ---- Posts ---- */

#define _POSIX_C_SOURCE 200809L
#include "handlers_internal.h"
#include "cwist/board_tree.h"
#include "../db/db_internal.h"
#include <pthread.h>
#include <stdlib.h>

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

void append_boards_flat(cJSON *out, cJSON *boards, cJSON *tree, int parent_id, int max_depth) {
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
    bool mobile = is_mobile_request(req);

    char key[256];
    page_cache_key_board_list(key, sizeof(key), dark, mobile, role, uid);
    const char *cached = NULL;
    size_t cached_len = 0;
    uint32_t ttl = 0;
    if (page_cache_get(key, &cached, &cached_len, &ttl)) {
        send_cached_html_res(res, cached, cached_len, ttl);
        page_cache_release(key);
        return;
    }

    bool leader = false;
    cwist_sstring *shared = reqshare_wait_or_start(key, &leader);
    if (!leader) {
        send_html_res(res, shared);
        return;
    }

    char *pp = get_profile_pic(req->db, uid, role);
    cJSON *boards = db_board_list(req->db);
    cJSON *tree = db_board_tree_get_all();
    if (boards) {
        cJSON *all_recent_posts = db_post_recent_by_boards_batch(req->db, 2);
        int post_count = all_recent_posts ? cJSON_GetArraySize(all_recent_posts) : 0;
        int n = cJSON_GetArraySize(boards);
        for (int i = 0; i < n; i++) {
            cJSON *bo = cJSON_GetArrayItem(boards, i);
            int bid = json_int(bo, "id", 0);
            if (bid > 0) {
                cJSON *board_posts = cJSON_CreateArray();
                for (int p = 0; p < post_count; p++) {
                    cJSON *post = cJSON_GetArrayItem(all_recent_posts, p);
                    if (json_int(post, "board_id", 0) == bid) {
                        cJSON_AddItemToArray(board_posts, cJSON_Duplicate(post, 1));
                    }
                }
                cJSON_AddItemToObject(bo, "posts", board_posts);
            }
        }
        if (all_recent_posts) cJSON_Delete(all_recent_posts);
    }
    cJSON *ordered = cJSON_CreateArray();
    append_boards_flat(ordered, boards, tree, 0, 2);
    cwist_sstring *page = render_board_list(ordered, dark, role, pp, mobile);
    cJSON_Delete(ordered);
    if (boards) cJSON_Delete(boards);
    if (tree) cJSON_Delete(tree);
    if (page) {
        page_cache_set(key, page->data, page->size, 60);
        reqshare_finish(key, page);
    } else {
        reqshare_finish(key, NULL);
    }
    send_html_res(res, page);
    free(pp);
}

void handler_board_new_get(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0; char role[32] = {0};
    if (!auth_require_login(req, res, &uid, role, sizeof(role))) return;
    char *pp = get_profile_pic(req->db, uid, role);
    cJSON *boards = db_board_list(req->db);
    cJSON *tree = db_board_tree_get_all();
    cJSON *ordered = cJSON_CreateArray();
    append_boards_flat(ordered, boards, tree, 0, 4);
    send_html_res(res, render_board_form(NULL, ordered, is_dark(req), NULL, pp, is_mobile_request(req), role));
    if (ordered) cJSON_Delete(ordered);
    if (tree) cJSON_Delete(tree);
    if (boards) cJSON_Delete(boards);
    free(pp);
}

/* Language and translation pairing from raw form values. */
static void apply_board_lang_strings(cwist_db *db, int bid, const char *lang, const char *pair) {
    if (bid <= 0) return;
    if (lang && !i18n_lang_valid(lang)) lang = NULL;
    int pair_id = pair ? atoi(pair) : 0;
    if (pair_id > 0) {
        cJSON *other = db_board_get_by_id(db, pair_id);
        db_i18n_set(db, "board", bid, lang, other ? pair_id : 0);
        if (other) cJSON_Delete(other);
    } else {
        db_i18n_set(db, "board", bid, lang, pair ? -1 : 0);
    }
}

/* Language and translation pairing from the board form (edit handler). */
static void apply_board_lang(cwist_db *db, int bid, cwist_query_map *kv) {
    apply_board_lang_strings(db, bid, cwist_query_map_get(kv, "lang"),
                             cwist_query_map_get(kv, "translation_of"));
}

/* "lang" and "translation_of" on a board, for the edit form. */
static void attach_board_lang(cwist_db *db, cJSON *board) {
    int bid = json_int(board, "id", 0);
    cJSON *i18n = db_i18n_get(db, "board", bid);
    cJSON *lang = i18n ? cJSON_GetObjectItem(i18n, "lang") : NULL;
    cJSON_AddStringToObject(board, "lang", cJSON_IsString(lang) ? lang->valuestring : "");
    if (i18n) cJSON_Delete(i18n);
    cJSON *sib = db_i18n_siblings(db, "board", bid, false);
    cJSON_AddNumberToObject(board, "translation_of", cJSON_GetArraySize(sib) > 0 ? json_int(cJSON_GetArrayItem(sib, 0), "id", 0) : 0);
    if (sib) cJSON_Delete(sib);
}

/* Fire-and-forget continuation of board creation. The HTTP response (a
 * redirect) no longer waits for the INSERT plus tree/i18n/cache follow-up,
 * so a slow or contended database write cannot stall the request path. Runs
 * detached with its own short-lived cwist_db handle: fly_db_conn() and
 * board_tree_db_conn() open per-thread connections, so nothing here touches
 * request memory after the handler returns. */
typedef struct {
    char db_path[512];
    char *name;
    char *slug;
    char *desc;
    bool admin_only;
    int parent_id;
    char *lang; /* NULL when absent */
    char *pair; /* NULL when absent */
} board_create_job;

static void board_create_job_free(board_create_job *j) {
    if (!j) return;
    free(j->name);
    free(j->slug);
    free(j->desc);
    free(j->lang);
    free(j->pair);
    free(j);
}

static void *board_create_bg_thread(void *arg) {
    board_create_job *j = arg;
    cwist_db local = {0};
    if (sqlite3_open(j->db_path, &local.conn) != SQLITE_OK) {
        sqlite3_close(local.conn);
        CWIST_LOG_ERROR("Async board creation: cannot open %s", j->db_path);
        board_create_job_free(j);
        return NULL;
    }
    bool ok = db_board_create(&local, j->name, j->slug, j->desc, j->admin_only, 0, 0, 0);
    if (ok) {
        cJSON *created = db_board_get_by_slug(&local, j->slug);
        if (created) {
            int bid = json_int(created, "id", 0);
            if (bid > 0) {
                db_board_tree_set_parent(bid, j->parent_id > 0 ? j->parent_id : 0);
                apply_board_lang_strings(&local, bid, j->lang, j->pair);
            }
            cJSON_Delete(created);
        }
        CWIST_LOG_INFO("Board created (async): name='%s' slug='%s'", j->name, j->slug);
        page_cache_invalidate_board(j->slug);
    } else {
        CWIST_LOG_ERROR("Async board creation failed: name='%s' slug='%s'", j->name, j->slug);
    }
    sqlite3_close(local.conn);
    board_create_job_free(j);
    return NULL;
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
            if (!error) {
                cJSON *existing = db_board_get_by_slug(req->db, slug);
                if (existing) {
                    error = "A board with this slug already exists.";
                    CWIST_LOG_WARN("Board creation POST: slug conflict slug='%s'", slug);
                    cJSON_Delete(existing);
                }
            }
        }
    }

    if (error) {
        int uid = 0; char role[32] = {0};
        auth_is_logged_in(req, &uid, role, sizeof(role));
        char *pp = get_profile_pic(req->db, uid, role);
        cJSON *boards = db_board_list(req->db);
        cJSON *tree = db_board_tree_get_all();
        cJSON *ordered = cJSON_CreateArray();
        append_boards_flat(ordered, boards, tree, 0, 4);
        cwist_sstring *page = render_board_form(NULL, ordered, is_dark(req), error, pp, is_mobile_request(req), role);
        if (ordered) cJSON_Delete(ordered);
        if (tree) cJSON_Delete(tree);
        if (boards) cJSON_Delete(boards);
        send_html_res(res, page);
        free(pp);
        cwist_query_map_destroy(kv);
        return;
    }

    bool is_admin = (role[0] && strcmp(role, "admin") == 0);
    /* Queue the create (+ tree/i18n/cache follow-up) on a detached thread and
     * answer with the redirect immediately; only fall back to the inline path
     * when the job cannot be spawned. The board shows up on /boards a moment
     * after the redirect; a background failure is logged, not shown. */
    board_create_job *job = calloc(1, sizeof(*job));
    bool queued = false;
    if (job) {
        const char *db_path = sqlite3_db_filename(fly_db_conn(req->db), "main");
        snprintf(job->db_path, sizeof(job->db_path), "%s", db_path ? db_path : "data/blog.db");
        job->name = strdup(name);
        job->slug = strdup(slug);
        job->desc = strdup(desc ? desc : "");
        job->admin_only = is_admin && (ao != NULL);
        job->parent_id = parent_id_str ? atoi(parent_id_str) : 0;
        const char *lang = cwist_query_map_get(kv, "lang");
        const char *pair = cwist_query_map_get(kv, "translation_of");
        job->lang = (lang && lang[0]) ? strdup(lang) : NULL;
        job->pair = (pair && pair[0]) ? strdup(pair) : NULL;
        pthread_t t;
        if (job->name && job->slug && job->desc &&
            pthread_create(&t, NULL, board_create_bg_thread, job) == 0) {
            pthread_detach(t);
            queued = true;
        } else {
            board_create_job_free(job);
        }
    }
    if (!queued) {
        if (db_board_create(req->db, name, slug, desc ? desc : "", is_admin && (ao != NULL), 0, 0, 0)) {
            cJSON *created = db_board_get_by_slug(req->db, slug);
            if (created) {
                int bid = json_int(created, "id", 0);
                int parent_id = parent_id_str ? atoi(parent_id_str) : 0;
                if (bid > 0) db_board_tree_set_parent(bid, parent_id > 0 ? parent_id : 0);
                const char *lang = cwist_query_map_get(kv, "lang");
                const char *pair = cwist_query_map_get(kv, "translation_of");
                apply_board_lang_strings(req->db, bid, lang, pair);
                cJSON_Delete(created);
            }
            CWIST_LOG_INFO("Board created: name='%s' slug='%s'", name, slug);
            page_cache_invalidate_board(slug);
        } else {
            CWIST_LOG_ERROR("Board creation failed: name='%s' slug='%s'", name, slug);
        }
    }
    cwist_query_map_destroy(kv);
    const char *referer = cwist_http_header_get(req->headers, "Referer");
    redirect_referer_safe(res, referer, "/boards");
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
    cJSON *tree = db_board_tree_get_all();
    cJSON *ordered = cJSON_CreateArray();
    append_boards_flat(ordered, all_boards, tree, 0, 4);
    int parent_id = db_board_tree_get_parent(bid);
    cJSON_AddNumberToObject(board, "parent_id", parent_id);
    attach_board_lang(req->db, board);
    cwist_sstring *page = render_board_form(board, ordered, is_dark(req), NULL, pp, is_mobile_request(req), role);
    if (ordered) cJSON_Delete(ordered);
    if (tree) cJSON_Delete(tree);
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

    cJSON *old_slug_obj = cJSON_GetObjectItem(board, "slug");
    char *old_slug = (old_slug_obj && cJSON_IsString(old_slug_obj) && old_slug_obj->valuestring)
        ? strdup(old_slug_obj->valuestring) : NULL;

    bool admin_only_val = is_admin ? (ao != NULL) : json_int(board, "admin_only", 0);
    if (!db_board_update(req->db, bid, name, slug, desc ? desc : "", admin_only_val, 0, 0, 0)) {
        CWIST_LOG_ERROR("Board update failed: bid=%d", bid);
        cwist_sstring *page = render_board_form(board, NULL, is_dark(req), "Failed to update board.", pp, is_mobile_request(req), role);
        send_html_res(res, page);
        cJSON_Delete(board);
        free(old_slug);
        free(pp);
        cwist_query_map_destroy(kv);
        return;
    }
    db_board_tree_set_parent(bid, parent_id);
    apply_board_lang(req->db, bid, kv);
    CWIST_LOG_INFO("Board updated: bid=%d name='%s' slug='%s' parent=%d", bid, name, slug, parent_id);

    page_cache_invalidate_board(slug);
    if (old_slug && (!slug || strcmp(old_slug, slug) != 0)) {
        page_cache_invalidate_board(old_slug);
    }
    free(old_slug);

    char redirect_url[128];
    if (slug && slug[0]) {
        snprintf(redirect_url, sizeof(redirect_url), "/board/%s", slug);
    } else {
        strcpy(redirect_url, "/boards");
    }
    cJSON_Delete(board);
    free(pp);
    cwist_query_map_destroy(kv);
    const char *referer = cwist_http_header_get(req->headers, "Referer");
    redirect_referer_safe(res, referer, redirect_url);
}

void handler_board_delete(cwist_http_request *req, cwist_http_response *res) {
    if (!auth_require_admin(req, res)) return;
    const char *id_str = cwist_query_map_get(req->path_params, "id");
    if (id_str && req->method != CWIST_HTTP_POST) {
        /* GET shows a confirmation page; the page's form POSTs the delete
         * (one-click GET deletes are a CSRF hole under SameSite=Lax). */
        char action[96];
        snprintf(action, sizeof(action), "/board/%s/delete", id_str);
        send_delete_confirm_page(req, res, action, "Delete this board? Posts in it are kept.", NULL);
        return;
    }
    if (id_str) {
        int bid = atoi(id_str);
        cJSON *board = db_board_get_by_id(req->db, bid);
        if (board) {
            cJSON *slug_obj = cJSON_GetObjectItem(board, "slug");
            if (slug_obj && cJSON_IsString(slug_obj) && slug_obj->valuestring) {
                page_cache_invalidate_board(slug_obj->valuestring);
            } else {
                page_cache_invalidate_all();
            }
            cJSON_Delete(board);
        }
        db_board_tree_promote_children(bid);
        db_board_delete(req->db, bid);
        db_board_tree_remove(bid);
        CWIST_LOG_INFO("Board deleted: bid=%d", bid);
    }
    const char *referer = cwist_http_header_get(req->headers, "Referer");
    redirect_referer_safe(res, referer, "/boards");
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
                cJSON *board = db_board_get_by_id(req->db, board_id);
                if (board) {
                    cJSON *slug = cJSON_GetObjectItem(board, "slug");
                    if (slug && cJSON_IsString(slug) && slug->valuestring) {
                        page_cache_invalidate_board(slug->valuestring);
                    }
                    cJSON_Delete(board);
                }
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
            cJSON *board = db_board_get_by_id(req->db, board_id);
            if (board) {
                cJSON *slug = cJSON_GetObjectItem(board, "slug");
                if (slug && cJSON_IsString(slug) && slug->valuestring) {
                    page_cache_invalidate_board(slug->valuestring);
                }
                cJSON_Delete(board);
            }
        }
    }
    cwist_query_map_destroy(kv);
    if (!bid) { redirect(res, "/boards"); return; }
    char url[128];
    snprintf(url, sizeof(url), "/board/%s/perms?msg=%s", bid, msg);
    redirect(res, url);
}

/* ---- Posts ---- */

#define _POSIX_C_SOURCE 200809L
#include "handlers_internal.h"
#include <openssl/rand.h>

static bool random_hex_local(char *out, size_t byte_len) {
    unsigned char bytes[64];
    if (!out || byte_len == 0 || byte_len > sizeof(bytes)) return false;
    if (RAND_bytes(bytes, (int)byte_len) != 1) return false;
    for (size_t i = 0; i < byte_len; i++) snprintf(out + (i * 2), 3, "%02x", bytes[i]);
    out[byte_len * 2] = '\0';
    return true;
}

static void attach_media_meta_to_post(cwist_db *db, const char *media_meta_json, int post_id, int uid, const char *role) {
    if (!db || !media_meta_json || !media_meta_json[0] || post_id <= 0) return;
    cJSON *arr = cJSON_Parse(media_meta_json);
    if (!arr || !cJSON_IsArray(arr)) {
        if (arr) cJSON_Delete(arr);
        return;
    }
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, arr) {
        if (!cJSON_IsObject(item)) continue;
        int fid = json_int(item, "fid", 0);
        const char *mode = cJSON_GetObjectItem(item, "mode") && cJSON_IsString(cJSON_GetObjectItem(item, "mode"))
            ? cJSON_GetObjectItem(item, "mode")->valuestring : "attachment";
        const char *delete_pin = cJSON_GetObjectItem(item, "delete_pin") && cJSON_IsString(cJSON_GetObjectItem(item, "delete_pin"))
            ? cJSON_GetObjectItem(item, "delete_pin")->valuestring : "";
        if (fid <= 0) continue;
        cJSON *file = db_file_get(db, fid);
        if (!file) continue;
        int file_uid = json_int(file, "user_id", 0);
        int existing_post_id = json_int(file, "post_id", 0);
        cJSON *pin_hash = cJSON_GetObjectItem(file, "delete_pin_hash");
        bool pin_ok = delete_pin && delete_pin[0] && pin_hash && pin_hash->valuestring && pin_hash->valuestring[0] &&
                      auth_verify_password(delete_pin, pin_hash->valuestring);
        bool owner_ok = (uid > 0 && file_uid == uid) || (role && strcmp(role, "admin") == 0);
        if ((existing_post_id == 0 || existing_post_id == post_id) && (owner_ok || pin_ok)) {
            db_file_attach_to_post(db, fid, post_id, strcmp(mode, "inline") == 0);
        }
        cJSON_Delete(file);
    }
    cJSON_Delete(arr);
}

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
    const char *search_type = cwist_query_map_get(req->query_params, "search_type");
    bool empty_search = search && !search[0];

    if (slug) {
        cJSON *board = db_board_get_by_slug(req->db, slug);
        if (!board) {
            CWIST_LOG_WARN("Post list: board not found slug='%s'", slug);
            res->status_code = CWIST_HTTP_NOT_FOUND;
            cwist_sstring_assign(res->body, "Board not found");
            if (boards) cJSON_Delete(boards);
            return;
        }
        int bid = json_int(board, "id", 0);
        if (!empty_search) {
            int total = db_post_count_search(req->db, bid, search, search_type);
            total_pages = (total + per_page - 1) / per_page;
            if (total_pages < 1) total_pages = 1;
            if (page > total_pages) page = total_pages;
            posts = db_post_list_search(req->db, bid, search, search_type, per_page, (page - 1) * per_page);
        }
        cJSON_Delete(board);
    } else {
        if (!empty_search) {
            int total = db_post_count_search(req->db, 0, search, search_type);
            total_pages = (total + per_page - 1) / per_page;
            if (total_pages < 1) total_pages = 1;
            if (page > total_pages) page = total_pages;
            posts = db_post_list_search(req->db, 0, search, search_type, per_page, (page - 1) * per_page);
        }
    }

    char *pp = get_profile_pic(req->db, uid, role);
    cwist_sstring *page_html = render_post_list(posts, boards, dark, role, page, total_pages, slug, search, search_type, pp, uid);
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
    int post_id = json_int(post, "id", 0);
    if (post_id > 0) db_post_increment_view(req->db, post_id);
    cJSON *files = db_file_list_by_post(req->db, post_id);
    cJSON *comments = db_comment_list_by_target(req->db, "post", post_id);
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
    cJSON *vote_counts = db_post_vote_counts(req->db, post_id);
    int vote_up = 0, vote_down = 0, user_vote = 0;
    if (vote_counts) {
        cJSON *vu = cJSON_GetObjectItem(vote_counts, "up");
        cJSON *vd = cJSON_GetObjectItem(vote_counts, "down");
        if (vu && vu->type == cJSON_Number) vote_up = (int)vu->valuedouble;
        if (vd && vd->type == cJSON_Number) vote_down = (int)vd->valuedouble;
        cJSON_Delete(vote_counts);
    }
    if (uid > 0) user_vote = db_post_user_vote(req->db, post_id, uid);
    char *pp = get_profile_pic(req->db, uid, role);
    const char *ephemeral_delete_pin = cwist_query_map_get(req->query_params, "delete_pin");
    cwist_sstring *page = render_post_detail(post, files, comments, dark, role, verified, vote_up, vote_down, user_vote, pp, uid, ephemeral_delete_pin);
    cJSON_Delete(post);
    if (files) cJSON_Delete(files);
    if (comments) cJSON_Delete(comments);
    send_html_res(res, page);
    free(pp);
}

void handler_post_new_get(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0;
    char role[32] = {0};
    auth_is_logged_in(req, &uid, role, sizeof(role));
    char *pp = get_profile_pic(req->db, uid, role);
    cJSON *boards = db_board_list(req->db);
    cwist_sstring *page = render_post_editor(boards, NULL, NULL, is_dark(req), role, NULL, pp);
    if (boards) cJSON_Delete(boards);
    send_html_res(res, page);
    free(pp);
}

void handler_post_new_post(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0;
    char role[32] = {0};
    auth_is_logged_in(req, &uid, role, sizeof(role));

    const char *ctype = cwist_http_header_get(req->headers, "Content-Type");
    char *title = NULL, *content = NULL, *summary = NULL, *board_id_str = NULL, *media_meta = NULL;
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
            if ((f = form_find(files, "media_meta"))) media_meta = (char *)cwist_alloc(f->len+1), memcpy(media_meta, f->data, f->len), media_meta[f->len]=0;
            FLY_LOG_DEBUG("multipart parsed: title=%s content_len=%zu board_id=%s", title ? title : "NULL", content ? strlen(content) : 0, board_id_str ? board_id_str : "NULL");
        } else {
            FLY_LOG_DEBUG("boundary not found in ctype");
        }
    } else {
        cwist_query_map *kv = cwist_query_map_create(); cwist_query_map_parse(kv, req->body->data);
        title = (char *)cwist_alloc(strlen(cwist_query_map_get(kv, "title") ? cwist_query_map_get(kv, "title") : "")+1);
        strcpy(title, cwist_query_map_get(kv, "title") ? cwist_query_map_get(kv, "title") : "");
        content = (char *)cwist_alloc(strlen(cwist_query_map_get(kv, "content") ? cwist_query_map_get(kv, "content") : "")+1);
        strcpy(content, cwist_query_map_get(kv, "content") ? cwist_query_map_get(kv, "content") : "");
        summary = (char *)cwist_alloc(strlen(cwist_query_map_get(kv, "summary") ? cwist_query_map_get(kv, "summary") : "")+1);
        strcpy(summary, cwist_query_map_get(kv, "summary") ? cwist_query_map_get(kv, "summary") : "");
        board_id_str = (char *)cwist_alloc(strlen(cwist_query_map_get(kv, "board_id") ? cwist_query_map_get(kv, "board_id") : "0")+1);
        strcpy(board_id_str, cwist_query_map_get(kv, "board_id") ? cwist_query_map_get(kv, "board_id") : "0");
        media_meta = (char *)cwist_alloc(strlen(cwist_query_map_get(kv, "media_meta") ? cwist_query_map_get(kv, "media_meta") : "[]")+1);
        strcpy(media_meta, cwist_query_map_get(kv, "media_meta") ? cwist_query_map_get(kv, "media_meta") : "[]");
        cwist_query_map_destroy(kv);
    }

    if (!title || !content || !title[0] || !content[0]) {
        CWIST_LOG_WARN("Post creation failed: missing title or content uid=%d", uid);
        cJSON *boards = db_board_list(req->db);
        char *pp = get_profile_pic(req->db, uid, role);
        cwist_sstring *page = render_post_editor(boards, NULL, NULL, is_dark(req), role, "Title and content required", pp);
        if (boards) cJSON_Delete(boards);
        send_html_res(res, page);
        free(pp);
        cwist_free(title); cwist_free(content); cwist_free(summary); cwist_free(board_id_str);
        multipart_free(files);
        return;
    }

    int board_id = board_id_str ? atoi(board_id_str) : 0;
    char *sl = generate_slug(title);

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
            int created_id = db_post_create(req->db, board_id, uid, title, final_slug, content, summary ? summary : "", sig_b64 ? sig_b64 : "", 0, 0, "");
            created = created_id > 0;

            /* The final_slug isn't strictly needed later but we update 'sl' to point to the created slug so publish_post uses the right slug. */
            if (created) {
                 if (uid == 0) {
                     char delete_pin[13];
                     char delete_pin_hash[512];
                     delete_pin[0] = '\0';
                     if (random_hex_local(delete_pin, 6) && auth_hash_password(delete_pin, delete_pin_hash, sizeof(delete_pin_hash))) {
                         (void)db_post_set_delete_pin_hash(req->db, created_id, delete_pin_hash);
                         char redirect_with_pin[1024];
                         snprintf(redirect_with_pin, sizeof(redirect_with_pin), "/post/%s?delete_pin=%s", final_slug, delete_pin);
                         cwist_free(sl);
                         sl = final_slug;
                         if (sig_b64) cwist_free(sig_b64);
                         fly_nats_publish_post(title, sl, summary ? summary : "");
                         attach_media_meta_to_post(req->db, media_meta, created_id, uid, role);
                         cwist_free(title); cwist_free(content); cwist_free(summary); cwist_free(board_id_str); cwist_free(media_meta);
                         multipart_free(files);
                         redirect(res, redirect_with_pin);
                         return;
                     }
                 }
                 cwist_free(sl);
                 sl = final_slug;
            } else {
                 cwist_free(final_slug);
                 break;
            }
        }
    }
    if (created) {
        CWIST_LOG_INFO("Post created: uid=%d slug='%s' board_id=%d", uid, sl, board_id);
    } else {
        CWIST_LOG_ERROR("Post creation failed: uid=%d board_id=%d", uid, board_id);
    }
    if (sig_b64) cwist_free(sig_b64);

    /* Publish post metadata to NATS for distributed subscribers */
    fly_nats_publish_post(title, sl, summary ? summary : "");

    /* Link orphaned uploads to this post */
    int post_id = (int)sqlite3_last_insert_rowid(req->db->conn);
    attach_media_meta_to_post(req->db, media_meta, post_id, uid, role);

    cwist_free(sl);
    cwist_free(title); cwist_free(content); cwist_free(summary); cwist_free(board_id_str); cwist_free(media_meta);
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
    if (!is_author_or_admin(post, uid, role)) {
        res->status_code = CWIST_HTTP_FORBIDDEN;
        cwist_sstring_assign(res->body, "Forbidden");
        cJSON_Delete(post);
        return;
    }
    cJSON *boards = db_board_list(req->db);
    char *pp = get_profile_pic(req->db, uid, role);
    int post_id_val = json_int(post, "id", 0);
    cJSON *files = db_file_list_by_post(req->db, post_id_val);
    cwist_sstring *page = render_post_editor(boards, post, files, is_dark(req), role, NULL, pp);
    cJSON_Delete(post);
    if (files) cJSON_Delete(files);
    if (boards) cJSON_Delete(boards);
    send_html_res(res, page);
    free(pp);
}

void handler_post_edit_post(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0;
    char role[32] = {0};
    if (!auth_require_login(req, res, &uid, role, sizeof(role))) return;

    const char *ctype = cwist_http_header_get(req->headers, "Content-Type");
    char *title = NULL, *content = NULL, *summary = NULL, *id_str = NULL, *board_id_str = NULL, *media_meta = NULL;
    form_field_t *files = NULL;

    const char *path_id = cwist_query_map_get(req->path_params, "id");
    if (path_id) {
        id_str = (char *)cwist_alloc(strlen(path_id)+1);
        strcpy(id_str, path_id);
    }

    if (ctype && strstr(ctype, "multipart/form-data")) {
        const char *bnd = strstr(ctype, "boundary=");
        if (bnd) {
            bnd += 9;
            if (*bnd == '"') bnd++;
            size_t bnd_len = strcspn(bnd, "\"\r\n; ");
            char *boundary = (char *)cwist_alloc(bnd_len + 1);
            memcpy(boundary, bnd, bnd_len);
            boundary[bnd_len] = '\0';
            files = multipart_parse(req->body->data, req->body->size, boundary);
            cwist_free(boundary);
            form_field_t *f;

            if ((f = form_find(files, "title"))) title = (char *)cwist_alloc(f->len+1), memcpy(title, f->data, f->len), title[f->len]=0;
            if ((f = form_find(files, "content"))) content = (char *)cwist_alloc(f->len+1), memcpy(content, f->data, f->len), content[f->len]=0;
            if ((f = form_find(files, "summary"))) summary = (char *)cwist_alloc(f->len+1), memcpy(summary, f->data, f->len), summary[f->len]=0;
            if ((f = form_find(files, "board_id"))) board_id_str = (char *)cwist_alloc(f->len+1), memcpy(board_id_str, f->data, f->len), board_id_str[f->len]=0;
            if ((f = form_find(files, "media_meta"))) media_meta = (char *)cwist_alloc(f->len+1), memcpy(media_meta, f->data, f->len), media_meta[f->len]=0;
        }
    } else {
        cwist_query_map *kv = cwist_query_map_create(); cwist_query_map_parse(kv, req->body->data);

        title = (char *)cwist_alloc(strlen(cwist_query_map_get(kv, "title") ? cwist_query_map_get(kv, "title") : "")+1);
        strcpy(title, cwist_query_map_get(kv, "title") ? cwist_query_map_get(kv, "title") : "");
        content = (char *)cwist_alloc(strlen(cwist_query_map_get(kv, "content") ? cwist_query_map_get(kv, "content") : "")+1);
        strcpy(content, cwist_query_map_get(kv, "content") ? cwist_query_map_get(kv, "content") : "");
        summary = (char *)cwist_alloc(strlen(cwist_query_map_get(kv, "summary") ? cwist_query_map_get(kv, "summary") : "")+1);
        strcpy(summary, cwist_query_map_get(kv, "summary") ? cwist_query_map_get(kv, "summary") : "");
        board_id_str = (char *)cwist_alloc(strlen(cwist_query_map_get(kv, "board_id") ? cwist_query_map_get(kv, "board_id") : "0")+1);
        strcpy(board_id_str, cwist_query_map_get(kv, "board_id") ? cwist_query_map_get(kv, "board_id") : "0");
        media_meta = (char *)cwist_alloc(strlen(cwist_query_map_get(kv, "media_meta") ? cwist_query_map_get(kv, "media_meta") : "[]")+1);
        strcpy(media_meta, cwist_query_map_get(kv, "media_meta") ? cwist_query_map_get(kv, "media_meta") : "[]");
        cwist_query_map_destroy(kv);
    }

    if (!id_str || !title || !content || !title[0] || !content[0]) {
        cwist_free(title); cwist_free(content); cwist_free(summary); cwist_free(id_str);
        multipart_free(files);
        redirect(res, "/");
        return;
    }

    cJSON *post = db_post_get_by_id(req->db, atoi(id_str));
    if (!post) {
        CWIST_LOG_WARN("Post edit failed: post not found id=%s uid=%d", id_str, uid);
        cwist_free(title); cwist_free(content); cwist_free(summary); cwist_free(id_str);
        multipart_free(files);
        redirect(res, "/");
        return;
    }
    if (!is_author_or_admin(post, uid, role)) {
        CWIST_LOG_WARN("Post edit forbidden: id=%s uid=%d role=%s", id_str, uid, role);
        res->status_code = CWIST_HTTP_FORBIDDEN;
        cwist_sstring_assign(res->body, "Forbidden");
        cJSON_Delete(post);
        cwist_free(title); cwist_free(content); cwist_free(summary); cwist_free(id_str);
        multipart_free(files);
        return;
    }
    cJSON_Delete(post);

    int board_id = board_id_str ? atoi(board_id_str) : 0;
    size_t msg_len2 = (title ? strlen(title) : 0) + 1 + (content ? strlen(content) : 0);
    char *msg2 = (char *)cwist_alloc(msg_len2 + 1);
    snprintf(msg2, msg_len2 + 1, "%s\n%s", title ? title : "", content ? content : "");
    char *sig_b642 = NULL;
    fly_crypto_sign((const uint8_t *)msg2, strlen(msg2), &sig_b642);
    cwist_free(msg2);
    if (db_post_update(req->db, atoi(id_str), board_id, title, content, summary ? summary : "", sig_b642 ? sig_b642 : "", 0, 0, "")) {
        CWIST_LOG_INFO("Post updated: id=%s uid=%d board_id=%d", id_str, uid, board_id);
    } else {
        CWIST_LOG_ERROR("Post update failed: id=%s uid=%d", id_str, uid);
    }
    if (sig_b642) cwist_free(sig_b642);

    attach_media_meta_to_post(req->db, media_meta, atoi(id_str), uid, role);

    cwist_free(title); cwist_free(content); cwist_free(summary); cwist_free(id_str); cwist_free(board_id_str); cwist_free(media_meta);
    multipart_free(files);
    redirect(res, "/");
}
void handler_post_delete(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0;
    char role[32] = {0};
    auth_is_logged_in(req, &uid, role, sizeof(role));
    const char *id_str = cwist_query_map_get(req->path_params, "id");
    const char *delete_pin = cwist_query_map_get(req->query_params, "delete_pin");
    if (!id_str) { redirect(res, "/"); return; }
    cJSON *post = db_post_get_by_id(req->db, atoi(id_str));
    if (!post) { CWIST_LOG_WARN("Post delete failed: not found id=%s uid=%d", id_str, uid); redirect(res, "/"); return; }
    cJSON *pin_hash = cJSON_GetObjectItem(post, "delete_pin_hash");
    bool pin_ok = delete_pin && pin_hash && pin_hash->valuestring && pin_hash->valuestring[0] &&
                  auth_verify_password(delete_pin, pin_hash->valuestring);
    if (!is_author_or_admin(post, uid, role) && !pin_ok) {
        CWIST_LOG_WARN("Post delete forbidden: id=%s uid=%d role=%s", id_str, uid, role);
        res->status_code = CWIST_HTTP_FORBIDDEN;
        cwist_sstring_assign(res->body, "Forbidden");
        cJSON_Delete(post);
        return;
    }
    cJSON_Delete(post);
    db_post_delete(req->db, atoi(id_str));
    CWIST_LOG_INFO("Post deleted: id=%s uid=%d", id_str, uid);
    redirect(res, "/");
}

/* ---- Files ---- */

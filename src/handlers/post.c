#define _POSIX_C_SOURCE 200809L
#include "handlers_internal.h"

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
            bool can = true;
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
    cwist_sstring *page_html = render_post_list(posts, boards, dark, role, page, total_pages, slug, search, pp, uid);
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
    cwist_sstring *page = render_post_detail(post, files, comments, dark, role, verified, vote_up, vote_down, user_vote, pp, uid);
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
    cwist_sstring *page = render_post_editor(boards, NULL, NULL, is_dark(req), role, NULL, pp);
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
            created = db_post_create(req->db, board_id, uid, title, final_slug, content, summary ? summary : "", sig_b64 ? sig_b64 : "", 0, 0, "");
            
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

    /* Link orphaned uploads to this post */
    int post_id = (int)sqlite3_last_insert_rowid(req->db->conn);
    char orphan_sql[256];
    snprintf(orphan_sql, sizeof(orphan_sql), "UPDATE files SET post_id=%d WHERE post_id=0 AND user_id=%d", post_id, uid);
    db_exec_sql(req->db, orphan_sql);

    /* Handle attachments */
    if (files) {
        int post_id = (int)sqlite3_last_insert_rowid(req->db->conn);
        for (form_field_t *f = files; f; f = f->next) {
            if (f->filename && f->filename[0] != '\0' && f->data && f->data[0] != '\0') {
                db_file_create_volume(req->db, post_id, uid, f->filename, mime_type(f->filename), f->data, f->file_size);
            }
        }
    }

    cwist_free(sl);
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
    char *title = NULL, *content = NULL, *summary = NULL, *id_str = NULL, *board_id_str = NULL;
    form_field_t *files = NULL;

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
            if ((f = form_find(files, "id"))) id_str = (char *)cwist_alloc(f->len+1), memcpy(id_str, f->data, f->len), id_str[f->len]=0;
            if ((f = form_find(files, "title"))) title = (char *)cwist_alloc(f->len+1), memcpy(title, f->data, f->len), title[f->len]=0;
            if ((f = form_find(files, "content"))) content = (char *)cwist_alloc(f->len+1), memcpy(content, f->data, f->len), content[f->len]=0;
            if ((f = form_find(files, "summary"))) summary = (char *)cwist_alloc(f->len+1), memcpy(summary, f->data, f->len), summary[f->len]=0;
            if ((f = form_find(files, "board_id"))) board_id_str = (char *)cwist_alloc(f->len+1), memcpy(board_id_str, f->data, f->len), board_id_str[f->len]=0;
        }
    } else {
        form_kv_t *kv = parse_urlencoded(req->body->data);
        id_str = (char *)cwist_alloc(strlen(form_kv_get(kv, "id") ? form_kv_get(kv, "id") : "")+1);
        strcpy(id_str, form_kv_get(kv, "id") ? form_kv_get(kv, "id") : "");
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

    if (!id_str || !title || !content || !title[0] || !content[0]) {
        cwist_free(title); cwist_free(content); cwist_free(summary); cwist_free(id_str);
        multipart_free(files);
        redirect(res, "/");
        return;
    }

    cJSON *post = db_post_get_by_id(req->db, atoi(id_str));
    if (!post) {
        cwist_free(title); cwist_free(content); cwist_free(summary); cwist_free(id_str);
        multipart_free(files);
        redirect(res, "/");
        return;
    }
    if (!is_author_or_admin(post, uid, role)) {
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
    db_post_update(req->db, atoi(id_str), board_id, title, content, summary ? summary : "", sig_b642 ? sig_b642 : "", 0, 0, "");
    if (sig_b642) cwist_free(sig_b642);

    /* Handle new attachments during edit */
    if (files) {
        for (form_field_t *f = files; f; f = f->next) {
            if (f->filename && f->filename[0] != '\0' && f->data && f->data[0] != '\0') {
                db_file_create_volume(req->db, atoi(id_str), uid, f->filename, mime_type(f->filename), f->data, f->file_size);
            }
        }
    }

    cwist_free(title); cwist_free(content); cwist_free(summary); cwist_free(id_str); cwist_free(board_id_str);
    multipart_free(files);
    redirect(res, "/");
}
void handler_post_delete(cwist_http_request *req, cwist_http_response *res) {
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
    cJSON_Delete(post);
    db_post_delete(req->db, atoi(id_str));
    redirect(res, "/");
}

/* ---- Files ---- */

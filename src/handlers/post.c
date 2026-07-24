#define _POSIX_C_SOURCE 200809L
#include "handlers_internal.h"
#include "db/sql_escape.h"
#include <openssl/rand.h>

static bool random_hex_local(char *out, size_t byte_len) {
    unsigned char bytes[64];
    if (!out || byte_len == 0 || byte_len > sizeof(bytes)) return false;
    if (RAND_bytes(bytes, (int)byte_len) != 1) return false;
    for (size_t i = 0; i < byte_len; i++) snprintf(out + (i * 2), 3, "%02x", bytes[i]);
    out[byte_len * 2] = '\0';
    return true;
}

#include <ctype.h>

static char *cwist_strdup_local(const char *src) {
    if (!src) return NULL;
    size_t len = strlen(src);
    char *copy = (char *)cwist_alloc(len + 1);
    if (!copy) return NULL;
    memcpy(copy, src, len + 1);
    return copy;
}

static void rewrite_content_legacy_urls(cwist_db *db, char **content) {
    if (!db || !content || !*content || !**content) return;
    const char *prefix = "/file/download/";
    size_t prefix_len = strlen(prefix);
    cwist_sstring *out = cwist_sstring_create();
    if (!out) return;
    const char *p = *content;
    while (*p) {
        const char *found = strstr(p, prefix);
        if (!found) {
            cwist_sstring_append(out, p);
            break;
        }
        cwist_sstring_append_len(out, p, found - p);
        const char *num_start = found + prefix_len;
        if (!isdigit((unsigned char)*num_start)) {
            cwist_sstring_append_len(out, found, prefix_len);
            p = num_start;
            continue;
        }
        int fid = 0;
        const char *num_end = num_start;
        while (isdigit((unsigned char)*num_end)) {
            if (fid > (INT_MAX / 10) || (fid == INT_MAX / 10 && (*num_end - '0') > (INT_MAX % 10))) {
                fid = 0; break;
            }
            fid = fid * 10 + (*num_end - '0');
            num_end++;
        }
        if (fid <= 0) {
            cwist_sstring_append_len(out, found, num_end - found);
            p = num_end;
            continue;
        }
        cJSON *file = db_file_get(db, fid);
        if (file) {
            cJSON_Delete(file);
        }
        cwist_sstring_append_len(out, found, num_end - found);
        p = num_end;
    }
    char *rewritten = cwist_strdup_local(out->data);
    if (rewritten) {
        cwist_free(*content);
        *content = rewritten;
    }
    cwist_sstring_destroy(out);
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
    bool mobile = is_mobile_request(req);
    cJSON *posts = NULL;
    int page = 1, total_pages = 1;
    const char *page_str = cwist_query_map_get(req->query_params, "page");
    if (page_str) page = atoi(page_str);
    if (page < 1) page = 1;
    int per_page = 20;
    const char *search = cwist_query_map_get(req->query_params, "search");
    const char *search_type = cwist_query_map_get(req->query_params, "search_type");
    bool empty_search = search && !search[0];

    char key[512];
    page_cache_key_board(key, sizeof(key), slug ? slug : "", page, dark, mobile, role, uid, search, search_type);
    const char *cached = NULL;
    size_t cached_len = 0;
    uint32_t ttl = 0;
    if (page_cache_get(key, &cached, &cached_len, &ttl)) {
        send_cached_html_res(res, cached, cached_len, ttl);
        page_cache_release(key);
        return;
    }

    int bid = 0;
    if (slug) {
        cJSON *board = db_board_get_by_slug(req->db, slug);
        if (!board) {
            CWIST_LOG_WARN("Post list: board not found slug='%s'", slug);
            res->status_code = CWIST_HTTP_NOT_FOUND;
            cwist_sstring_assign(res->body, "Board not found");
            return;
        }
        bid = json_int(board, "id", 0);
        cJSON_Delete(board);
    }

    bool leader = false;
    cwist_sstring *shared = reqshare_wait_or_start(key, &leader);
    if (!leader) {
        send_html_res(res, shared);
        return;
    }

    cJSON *children = NULL;
    if (!empty_search) {
        int total = db_post_count_search(req->db, bid, search, search_type);
        total_pages = (total + per_page - 1) / per_page;
        if (total_pages < 1) total_pages = 1;
        if (page > total_pages) page = total_pages;
        posts = db_post_list_search(req->db, bid, search, search_type, per_page, (page - 1) * per_page);
    }
    if (bid > 0) {
        cJSON *child_ids = db_board_tree_get_children(bid);
        if (child_ids && cJSON_GetArraySize(child_ids) > 0) {
            children = cJSON_CreateArray();
            int n = cJSON_GetArraySize(child_ids);
            for (int i = 0; i < n; i++) {
                cJSON *id_item = cJSON_GetArrayItem(child_ids, i);
                int cid = id_item->valueint;
                cJSON *cboard = db_board_get_by_id(req->db, cid);
                if (cboard) {
                    cJSON *cslug = cJSON_GetObjectItem(cboard, "slug");
                    cJSON *cname = cJSON_GetObjectItem(cboard, "name");
                    if (cslug && cslug->valuestring && cname && cname->valuestring) {
                        cJSON *obj = cJSON_CreateObject();
                        cJSON_AddStringToObject(obj, "slug", cslug->valuestring);
                        cJSON_AddStringToObject(obj, "name", cname->valuestring);
                        cJSON_AddItemToArray(children, obj);
                    }
                    cJSON_Delete(cboard);
                }
            }
        }
        if (child_ids) cJSON_Delete(child_ids);
    }

    char *pp = get_profile_pic(req->db, uid, role);
    cwist_sstring *page_html = render_post_list(posts, NULL, dark, role, page, total_pages, slug, search, search_type, pp, uid, mobile, children);
    if (posts) cJSON_Delete(posts);
    if (children) cJSON_Delete(children);
    if (page_html) {
        page_cache_set(key, page_html->data, page_html->size, 60);
        reqshare_finish(key, page_html);
    } else {
        reqshare_finish(key, NULL);
    }
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
    bool mobile = is_mobile_request(req);

    char key[512];
    page_cache_key_post(key, sizeof(key), slug, dark, mobile, role, uid);
    const char *cached = NULL;
    size_t cached_len = 0;
    uint32_t ttl = 0;
    if (page_cache_get(key, &cached, &cached_len, &ttl)) {
        send_cached_html_res(res, cached, cached_len, ttl);
        page_cache_release(key);
        return;
    }

    cJSON *post = db_post_get_by_slug(req->db, slug);
    if (!post) {
        int retry = 0;
        const char *retry_str = cwist_query_map_get(req->query_params, "retry");
        if (retry_str) retry = atoi(retry_str);
        if (retry < 3) {
            char refresh_url[1024];
            const char *delete_pin = cwist_query_map_get(req->query_params, "delete_pin");
            if (delete_pin) {
                snprintf(refresh_url, sizeof(refresh_url), "/post/%s?delete_pin=%s&retry=%d", slug, delete_pin, retry + 1);
            } else {
                snprintf(refresh_url, sizeof(refresh_url), "/post/%s?retry=%d", slug, retry + 1);
            }
            res->status_code = CWIST_HTTP_OK;
            cwist_http_header_add(&res->headers, "Content-Type", "text/html; charset=utf-8");
            char body[2048];
            snprintf(body, sizeof(body), 
                "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><meta http-equiv=\"refresh\" content=\"1;url=%s\"><title>Loading post...</title></head>"
                "<body style=\"font-family:sans-serif;text-align:center;padding:50px;\">"
                "<p>Routing to post, please wait... (%d/3)</p>"
                "<script>setTimeout(function(){window.location.replace('%s');}, 1500);</script>"
                "</body></html>", refresh_url, retry + 1, refresh_url);
            cwist_sstring_assign(res->body, body);
            return;
        }
        res->status_code = CWIST_HTTP_NOT_FOUND;
        cwist_sstring_assign(res->body, "Not found");
        return;
    }

    bool leader = false;
    cwist_sstring *shared = reqshare_wait_or_start(key, &leader);
    if (!leader) {
        cJSON_Delete(post);
        send_html_res(res, shared);
        return;
    }

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
    int author_id = json_int(post, "user_id", 0);
    char *author_pp = NULL;
    if (author_id > 0) {
        cJSON *author_user = db_user_get_by_id(req->db, author_id);
        if (author_user) {
            cJSON *pic = cJSON_GetObjectItem(author_user, "profile_pic");
            if (pic && pic->valuestring && pic->valuestring[0]) {
                author_pp = strdup(pic->valuestring);
            }
            cJSON_Delete(author_user);
        }
    }
    const char *ephemeral_delete_pin = cwist_query_map_get(req->query_params, "delete_pin");
    cwist_sstring *page = render_post_detail(post, files, comments, dark, role, verified, vote_up, vote_down, user_vote, pp, author_pp, uid, ephemeral_delete_pin, mobile);
    if (page) {
        page_cache_set(key, page->data, page->size, 60);
        reqshare_finish(key, page);
    } else {
        reqshare_finish(key, NULL);
    }
    if (author_pp) free(author_pp);
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
    cwist_sstring *page = render_post_editor(boards, NULL, NULL, is_dark(req), role, NULL, pp, is_mobile_request(req));
    if (boards) cJSON_Delete(boards);
    send_html_res(res, page);
    free(pp);
}

void handler_post_new_post(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0;
    char role[32] = {0};
    bool logged_in = auth_is_logged_in(req, &uid, role, sizeof(role));

    /* If the browser sent a session cookie but we could not verify it, do not
     * silently fall back to anonymous posting. The auth layer already logged
     * the precise failure reason. */
    if (!logged_in && auth_has_session_cookie(req)) {
        res->status_code = CWIST_HTTP_UNAUTHORIZED;
        cwist_sstring_assign(res->body, "Unauthorized. Session lost during post creation. Please <a href='/login'>login</a>.");
        return;
    }

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
            if (title) { char *unescaped = sql_unescape(title); cwist_free(title); title = unescaped; }
            if ((f = form_find(files, "content"))) content = (char *)cwist_alloc(f->len+1), memcpy(content, f->data, f->len), content[f->len]=0;
            if ((f = form_find(files, "summary"))) summary = (char *)cwist_alloc(f->len+1), memcpy(summary, f->data, f->len), summary[f->len]=0;
            if (summary) { char *unescaped = sql_unescape(summary); cwist_free(summary); summary = unescaped; }
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
        if (title) { char *unescaped = sql_unescape(title); cwist_free(title); title = unescaped; }
        content = (char *)cwist_alloc(strlen(cwist_query_map_get(kv, "content") ? cwist_query_map_get(kv, "content") : "")+1);
        strcpy(content, cwist_query_map_get(kv, "content") ? cwist_query_map_get(kv, "content") : "");
        summary = (char *)cwist_alloc(strlen(cwist_query_map_get(kv, "summary") ? cwist_query_map_get(kv, "summary") : "")+1);
        strcpy(summary, cwist_query_map_get(kv, "summary") ? cwist_query_map_get(kv, "summary") : "");
        if (summary) { char *unescaped = sql_unescape(summary); cwist_free(summary); summary = unescaped; }
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
        cwist_sstring *page = render_post_editor(boards, NULL, NULL, is_dark(req), role, "Title and content required", pp, is_mobile_request(req));
        if (boards) cJSON_Delete(boards);
        send_html_res(res, page);
        free(pp);
        cwist_free(title); cwist_free(content); cwist_free(summary); cwist_free(board_id_str); cwist_free(media_meta);
        multipart_free(files);
        return;
    }

    int board_id = board_id_str ? atoi(board_id_str) : 0;
    rewrite_content_legacy_urls(req->db, &content);
    char *sl = generate_slug(title);

    /* PQC sign: title + "\n" + content */
    size_t msg_len = (title ? strlen(title) : 0) + 1 + (content ? strlen(content) : 0);
    char *msg = (char *)cwist_alloc(msg_len + 1);
    snprintf(msg, msg_len + 1, "%s\n%s", title ? title : "", content ? content : "");
    char *sig_b64 = NULL;
    fly_crypto_sign((const uint8_t *)msg, strlen(msg), &sig_b64);
    cwist_free(msg);

    int created_id = 0;
    char *created_slug = NULL;
    created_id = db_post_create_with_auto_slug(req->db, board_id, uid, title, sl, content, summary ? summary : "", sig_b64 ? sig_b64 : "", 0, 0, "", &created_slug);
    if (!created_slug) {
        CWIST_LOG_ERROR("Post creation failed: uid=%d board_id=%d", uid, board_id);
        res->status_code = CWIST_HTTP_INTERNAL_ERROR;
        cwist_sstring_assign(res->body, "Post creation failed");
        if (sig_b64) cwist_free(sig_b64);
        cwist_free(sl);
        cwist_free(title); cwist_free(content); cwist_free(summary); cwist_free(board_id_str); cwist_free(media_meta);
        multipart_free(files);
        return;
    }
    CWIST_LOG_INFO("Post created: uid=%d slug='%s' board_id=%d", uid, created_slug, board_id);
    if (sig_b64) cwist_free(sig_b64);

    if (uid == 0) {
        char delete_pin[13];
        char delete_pin_hash[512];
        delete_pin[0] = '\0';
        if (random_hex_local(delete_pin, 6) && auth_hash_password(delete_pin, delete_pin_hash, sizeof(delete_pin_hash))) {
            (void)db_post_set_delete_pin_hash(req->db, created_id, delete_pin_hash);
            char redirect_with_pin[1024];
            snprintf(redirect_with_pin, sizeof(redirect_with_pin), "/post/%s?delete_pin=%s", created_slug, delete_pin);
            fly_nats_publish_post(title, created_slug, summary ? summary : "");
            attach_media_meta_to_post(req->db, media_meta, created_id, uid, role);
            cwist_free(sl);
            cwist_free(created_slug);
            cwist_free(title); cwist_free(content); cwist_free(summary); cwist_free(board_id_str); cwist_free(media_meta);
            multipart_free(files);
            redirect(res, redirect_with_pin);
            return;
        }
    }

    /* Publish post metadata to NATS for distributed subscribers */
    fly_nats_publish_post(title, created_slug, summary ? summary : "");

    attach_media_meta_to_post(req->db, media_meta, created_id, uid, role);

    /* New posts appear on home and board listings, so clear those caches. */
    page_cache_invalidate_all();

    cwist_free(sl);
    cwist_free(created_slug);
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
    cwist_sstring *page = render_post_editor(boards, post, files, is_dark(req), role, NULL, pp, is_mobile_request(req));
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

    /* Deduplicate concurrent Edit requests for the same post.
     * If the user clicked Submit multiple times (or the browser retried),
     * only the first request executes the DB write.  Subsequent in-flight
     * duplicates receive 409 and are silently redirected by the browser. */
    char wl_key[64];
    snprintf(wl_key, sizeof(wl_key), "edit:post:%s", id_str ? id_str : "");
    if (!reqshare_write_lock_try(wl_key)) {
        CWIST_LOG_WARN("Post edit deduplicated (concurrent duplicate): id=%s uid=%d", id_str ? id_str : "", uid);
        cwist_free(id_str);
        res->status_code = (cwist_http_status_t)409;
        cwist_sstring_assign(res->body, "Duplicate request – the post is already being saved.");
        return;
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
            if (title) { char *unescaped = sql_unescape(title); cwist_free(title); title = unescaped; }
            if ((f = form_find(files, "content"))) content = (char *)cwist_alloc(f->len+1), memcpy(content, f->data, f->len), content[f->len]=0;
            if ((f = form_find(files, "summary"))) summary = (char *)cwist_alloc(f->len+1), memcpy(summary, f->data, f->len), summary[f->len]=0;
            if (summary) { char *unescaped = sql_unescape(summary); cwist_free(summary); summary = unescaped; }
            if ((f = form_find(files, "board_id"))) board_id_str = (char *)cwist_alloc(f->len+1), memcpy(board_id_str, f->data, f->len), board_id_str[f->len]=0;
            if ((f = form_find(files, "media_meta"))) media_meta = (char *)cwist_alloc(f->len+1), memcpy(media_meta, f->data, f->len), media_meta[f->len]=0;
        }
    } else {
        cwist_query_map *kv = cwist_query_map_create(); cwist_query_map_parse(kv, req->body->data);

        title = (char *)cwist_alloc(strlen(cwist_query_map_get(kv, "title") ? cwist_query_map_get(kv, "title") : "")+1);
        strcpy(title, cwist_query_map_get(kv, "title") ? cwist_query_map_get(kv, "title") : "");
        if (title) { char *unescaped = sql_unescape(title); cwist_free(title); title = unescaped; }
        content = (char *)cwist_alloc(strlen(cwist_query_map_get(kv, "content") ? cwist_query_map_get(kv, "content") : "")+1);
        strcpy(content, cwist_query_map_get(kv, "content") ? cwist_query_map_get(kv, "content") : "");
        summary = (char *)cwist_alloc(strlen(cwist_query_map_get(kv, "summary") ? cwist_query_map_get(kv, "summary") : "")+1);
        strcpy(summary, cwist_query_map_get(kv, "summary") ? cwist_query_map_get(kv, "summary") : "");
        if (summary) { char *unescaped = sql_unescape(summary); cwist_free(summary); summary = unescaped; }
        board_id_str = (char *)cwist_alloc(strlen(cwist_query_map_get(kv, "board_id") ? cwist_query_map_get(kv, "board_id") : "0")+1);
        strcpy(board_id_str, cwist_query_map_get(kv, "board_id") ? cwist_query_map_get(kv, "board_id") : "0");
        media_meta = (char *)cwist_alloc(strlen(cwist_query_map_get(kv, "media_meta") ? cwist_query_map_get(kv, "media_meta") : "[]")+1);
        strcpy(media_meta, cwist_query_map_get(kv, "media_meta") ? cwist_query_map_get(kv, "media_meta") : "[]");
        cwist_query_map_destroy(kv);
    }

    if (!id_str || !title || !content || !title[0] || !content[0]) {
        reqshare_write_lock_release(wl_key);
        cwist_free(title); cwist_free(content); cwist_free(summary); cwist_free(id_str); cwist_free(board_id_str); cwist_free(media_meta);
        multipart_free(files);
        redirect(res, "/");
        return;
    }

    cJSON *post = db_post_get_by_id(req->db, atoi(id_str));
    if (!post) {
        CWIST_LOG_WARN("Post edit failed: post not found id=%s uid=%d", id_str, uid);
        reqshare_write_lock_release(wl_key);
        cwist_free(title); cwist_free(content); cwist_free(summary); cwist_free(id_str); cwist_free(board_id_str); cwist_free(media_meta);
        multipart_free(files);
        redirect(res, "/");
        return;
    }
    if (!is_author_or_admin(post, uid, role)) {
        CWIST_LOG_WARN("Post edit forbidden: id=%s uid=%d role=%s", id_str, uid, role);
        res->status_code = CWIST_HTTP_FORBIDDEN;
        cwist_sstring_assign(res->body, "Forbidden");
        cJSON_Delete(post);
        reqshare_write_lock_release(wl_key);
        cwist_free(title); cwist_free(content); cwist_free(summary); cwist_free(id_str); cwist_free(board_id_str); cwist_free(media_meta);
        multipart_free(files);
        return;
    }
    cJSON_Delete(post);

    int board_id = board_id_str ? atoi(board_id_str) : 0;
    rewrite_content_legacy_urls(req->db, &content);
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

    cJSON *updated_post = db_post_get_by_id(req->db, atoi(id_str));
    if (updated_post) {
        cJSON *slug_item = cJSON_GetObjectItem(updated_post, "slug");
        if (slug_item && cJSON_IsString(slug_item) && slug_item->valuestring) {
            page_cache_invalidate_post(slug_item->valuestring);
        } else {
            page_cache_invalidate_all();
        }
        cJSON_Delete(updated_post);
    } else {
        page_cache_invalidate_all();
    }

    reqshare_write_lock_release(wl_key);
    cwist_free(title); cwist_free(content); cwist_free(summary); cwist_free(id_str); cwist_free(board_id_str); cwist_free(media_meta);
    multipart_free(files);
    redirect(res, "/");
}
void handler_post_delete(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0;
    char role[32] = {0};
    bool logged_in = auth_is_logged_in(req, &uid, role, sizeof(role));
    const char *id_str = cwist_query_map_get(req->path_params, "id");
    const char *delete_pin = cwist_query_map_get(req->query_params, "delete_pin");

    /* A request that carried a session cookie but failed verification is a
     * logged-in flow that lost auth, not an anonymous delete attempt. */
    if (!logged_in && auth_has_session_cookie(req)) {
        res->status_code = CWIST_HTTP_UNAUTHORIZED;
        cwist_sstring_assign(res->body, "Unauthorized. Session lost. Please <a href='/login'>login</a>.");
        return;
    }

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
    cJSON *slug_item = cJSON_GetObjectItem(post, "slug");
    char *deleted_slug = (slug_item && cJSON_IsString(slug_item) && slug_item->valuestring) ? strdup(slug_item->valuestring) : NULL;
    cJSON_Delete(post);
    int post_id = atoi(id_str);

    /* Delete files and the post record in one main-DB transaction so a crash
     * in the middle cannot leave a post pointing to deleted files. */
    bool tx_ok = db_transaction_begin(req->db);
    db_file_delete_by_post(req->db, post_id);
    bool post_deleted = db_post_delete(req->db, post_id);
    if (tx_ok) {
        if (post_deleted) {
            if (!db_transaction_commit(req->db)) {
                db_transaction_rollback(req->db);
                post_deleted = false;
            }
        } else {
            db_transaction_rollback(req->db);
        }
    }

    if (post_deleted) {
        /* Comments live in a separate database; best-effort cleanup after the
         * main transaction commits. */
        db_comment_delete_by_target("post", post_id);
        if (deleted_slug) {
            page_cache_invalidate_post(deleted_slug);
            free(deleted_slug);
        } else {
            page_cache_invalidate_all();
        }
        CWIST_LOG_INFO("Post deleted: id=%s uid=%d", id_str, uid);
        redirect(res, "/");
    } else {
        CWIST_LOG_ERROR("Post delete failed: id=%s uid=%d", id_str, uid);
        res->status_code = CWIST_HTTP_INTERNAL_ERROR;
        cwist_sstring_assign(res->body, "Failed to delete post");
        free(deleted_slug);
    }
}

/* ---- Files ---- */

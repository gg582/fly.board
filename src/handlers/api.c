#define _POSIX_C_SOURCE 200809L
#include "handlers_internal.h"
#include "cwist/board_tree.h"
#include <curl/curl.h>

#define TRANSLATION_MAX_REQUEST (100U * 1024U)
#define TRANSLATION_MAX_RESPONSE (256U * 1024U)

typedef struct {
    cwist_sstring *body;
    bool overflow;
} translation_response_buffer;

static size_t translation_write_callback(void *data, size_t size, size_t count, void *userdata) {
    translation_response_buffer *buffer = userdata;
    size_t bytes = size * count;
    if (buffer->body->size + bytes > TRANSLATION_MAX_RESPONSE) {
        buffer->overflow = true;
        return 0;
    }
    cwist_sstring_append_len(buffer->body, data, bytes);
    return bytes;
}

static void translation_json_error(cwist_http_response *res, int status, const char *message) {
    cJSON *object = cJSON_CreateObject();
    cJSON_AddBoolToObject(object, "ok", false);
    cJSON_AddStringToObject(object, "error", message);
    char *json = cJSON_PrintUnformatted(object);
    cJSON_Delete(object);
    res->status_code = status;
    cwist_http_header_add(&res->headers, "Content-Type", "application/json; charset=utf-8");
    cwist_http_header_add(&res->headers, "Cache-Control", "no-store");
    cwist_sstring_assign(res->body, json ? json : "{\"ok\":false}");
    free(json);
}

static char *translate_text_via_api(CURL *curl, const char *text, const char *source, const char *target) {
    if (!text || !text[0]) return strdup("");
    char *escaped = curl_easy_escape(curl, text, 0);
    if (!escaped) return NULL;

    const char *src = (source && strcmp(source, "auto") != 0 && strlen(source) > 0) ? source : "auto";
    const char *tgt = (target && strlen(target) > 0) ? target : "ko";

    char url[2048];
    snprintf(url, sizeof(url),
             "https://translate.googleapis.com/translate_a/single?client=gtx&sl=%s&tl=%s&dt=t&q=%s",
             src, tgt, escaped);
    curl_free(escaped);

    translation_response_buffer buffer = {cwist_sstring_create(), false};
    struct curl_slist *headers = curl_slist_append(NULL, "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64)");

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 4000L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 15000L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, translation_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);

    CURLcode result = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);

    char *translated = NULL;
    if (result == CURLE_OK && !buffer.overflow && status >= 200 && status < 300 && buffer.body->size > 0) {
        cJSON *root = cJSON_ParseWithLength(buffer.body->data, buffer.body->size);
        if (root && cJSON_IsArray(root)) {
            cJSON *sentences = cJSON_GetArrayItem(root, 0);
            if (sentences && cJSON_IsArray(sentences)) {
                cwist_sstring *comb = cwist_sstring_create();
                int scount = cJSON_GetArraySize(sentences);
                for (int i = 0; i < scount; i++) {
                    cJSON *sent = cJSON_GetArrayItem(sentences, i);
                    if (sent && cJSON_IsArray(sent)) {
                        cJSON *seg = cJSON_GetArrayItem(sent, 0);
                        if (seg && seg->valuestring) {
                            cwist_sstring_append(comb, seg->valuestring);
                        }
                    }
                }
                translated = strdup(comb->data ? comb->data : "");
                cwist_sstring_destroy(comb);
            }
            cJSON_Delete(root);
        }
    }
    cwist_sstring_destroy(buffer.body);
    return translated;
}

void handler_api_translate(cwist_http_request *req, cwist_http_response *res) {
    if (!req->body || !req->body->data || req->body->size < 2 || req->body->size > TRANSLATION_MAX_REQUEST) {
        translation_json_error(res, CWIST_HTTP_BAD_REQUEST, "invalid translation request");
        return;
    }

    cJSON *input = cJSON_ParseWithLength(req->body->data, req->body->size);
    if (!input) {
        translation_json_error(res, CWIST_HTTP_BAD_REQUEST, "invalid json");
        return;
    }

    cJSON *source = cJSON_GetObjectItemCaseSensitive(input, "source");
    cJSON *target = cJSON_GetObjectItemCaseSensitive(input, "target");
    cJSON *chunks = cJSON_GetObjectItemCaseSensitive(input, "chunks");
    if (!cJSON_IsString(source) || !cJSON_IsString(target) || !cJSON_IsArray(chunks)) {
        cJSON_Delete(input);
        translation_json_error(res, CWIST_HTTP_BAD_REQUEST, "invalid translation payload");
        return;
    }

    int count = cJSON_GetArraySize(chunks);
    if (count < 1 || count > 64) {
        cJSON_Delete(input);
        translation_json_error(res, CWIST_HTTP_BAD_REQUEST, "invalid chunk count");
        return;
    }

    CURL *curl = curl_easy_init();
    if (!curl) {
        cJSON_Delete(input);
        translation_json_error(res, CWIST_HTTP_SERVICE_UNAVAILABLE, "curl init failed");
        return;
    }

    cJSON *out_array = cJSON_CreateArray();
    const char *src_str = source->valuestring;
    const char *tgt_str = target->valuestring;

    for (int i = 0; i < count; i++) {
        cJSON *chunk = cJSON_GetArrayItem(chunks, i);
        if (!chunk || !chunk->valuestring) {
            cJSON_AddItemToArray(out_array, cJSON_CreateString(""));
            continue;
        }
        char *trans = translate_text_via_api(curl, chunk->valuestring, src_str, tgt_str);
        if (trans) {
            cJSON_AddItemToArray(out_array, cJSON_CreateString(trans));
            free(trans);
        } else {
            cJSON_AddItemToArray(out_array, cJSON_CreateString(chunk->valuestring));
        }
    }

    curl_easy_cleanup(curl);
    cJSON_Delete(input);

    cJSON *resp_obj = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp_obj, "ok", true);
    cJSON_AddItemToObject(resp_obj, "parts", out_array);
    char *json = cJSON_PrintUnformatted(resp_obj);
    cJSON_Delete(resp_obj);

    cwist_http_header_add(&res->headers, "Content-Type", "application/json; charset=utf-8");
    cwist_http_header_add(&res->headers, "Cache-Control", "no-store");
    cwist_sstring_assign(res->body, json ? json : "{\"ok\":true,\"parts\":[]}");
    if (json) free(json);
}

void handler_api_preview(cwist_http_request *req, cwist_http_response *res) {
    cwist_sstring *html = render_markdown_to_html(req->body->data);
    if (html) {
        cwist_http_header_add(&res->headers, "Content-Type", "text/html; charset=utf-8");
        cwist_http_header_add(&res->headers, "Cache-Control", "no-cache, private");
        cwist_sstring *wrapped = cwist_sstring_create();
        cwist_sstring_append(wrapped, "<div class='markdown-body'>");
        cwist_sstring_append_sstring(wrapped, html);
        cwist_sstring_append(wrapped, "</div>");
        cwist_sstring_assign(res->body, wrapped->data);
        cwist_sstring_destroy(wrapped);
        cwist_sstring_destroy(html);
    } else {
        res->status_code = CWIST_HTTP_INTERNAL_ERROR;
        cwist_sstring_assign(res->body, "render error");
    }
}

void handler_api_upload(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0;
    char role[32] = {0};
    auth_is_logged_in(req, &uid, role, sizeof(role));
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
    const char *post_id_str = cwist_query_map_get(req->query_params, "post_id");
    int post_id = post_id_str ? atoi(post_id_str) : 0;
    int media_quality_score = media_quality_score_from_link(
        cwist_query_map_get(req->query_params, "media_quality_score"),
        cwist_query_map_get(req->query_params, "link_effective_type"),
        cwist_query_map_get(req->query_params, "link_downlink_mbps"),
        cwist_query_map_get(req->query_params, "link_rtt_ms"),
        cwist_query_map_get(req->query_params, "link_retry_events"),
        cwist_query_map_get(req->query_params, "link_timeout_events"),
        cwist_query_map_get(req->query_params, "link_save_data")
    );

    upload_result_t result = {0};
    bool ok = process_file_upload(req->db, f, uid, post_id, media_quality_score, &result);

    cJSON *obj = cJSON_CreateObject();
    cJSON_AddBoolToObject(obj, "ok", ok);
    if (ok) {
        cJSON_AddNumberToObject(obj, "file_id", result.file_id);
        cJSON_AddStringToObject(obj, "filename", result.filename);
        cJSON_AddStringToObject(obj, "mime_type", result.mime_type);
        cJSON_AddStringToObject(obj, "url", result.url);
        cJSON_AddStringToObject(obj, "blob_url", result.url);
        cJSON_AddStringToObject(obj, "html", result.html);
        cJSON_AddNumberToObject(obj, "size", (double)result.file_size);
        CWIST_LOG_INFO("API upload success: uid=%d post_id=%d filename='%s' size=%zu mime=%s", uid, post_id, result.filename, result.file_size, result.mime_type);
    } else {
        cJSON_AddStringToObject(obj, "error", result.error);
        CWIST_LOG_WARN("API upload failed: %s uid=%d", result.error, uid);
    }
    multipart_free(fields);
    char *json = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    cwist_http_header_add(&res->headers, "Content-Type", "application/json");
    cwist_http_header_add(&res->headers, "Cache-Control", "no-cache, private");
    cwist_sstring_assign(res->body, json ? json : "{}");
    if (json) free(json);
}

void handler_api_boards_json(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0;
    char role[32] = {0};
    auth_is_logged_in(req, &uid, role, sizeof(role));
    bool is_admin = strcmp(role, "admin") == 0;

    cJSON *boards = db_board_list(req->db);
    cJSON *roots = db_board_tree_get_roots();
    cJSON *out = cJSON_CreateArray();
    if (boards && out) {
        int n = cJSON_GetArraySize(boards);
        for (int i = 0; i < n; i++) {
            cJSON *bo = cJSON_GetArrayItem(boards, i);
            if (!bo) continue;
            int bid = json_int(bo, "id", 0);
            if (bid <= 0 || !db_board_can_user_access(req->db, bid, uid, is_admin)) continue;
            /* dropdown shows only root-level boards (1-depth) */
            bool is_root = true;
            if (roots) {
                is_root = false;
                int rn = cJSON_GetArraySize(roots);
                for (int j = 0; j < rn; j++) {
                    cJSON *r = cJSON_GetArrayItem(roots, j);
                    if (r && r->valueint == bid) { is_root = true; break; }
                }
            }
            if (!is_root) continue;
            cJSON *slug = cJSON_GetObjectItem(bo, "slug");
            cJSON *name = cJSON_GetObjectItem(bo, "name");
            if (!slug || !slug->valuestring || !slug->valuestring[0] ||
                !name || !name->valuestring || !name->valuestring[0]) continue;
            cJSON *item = cJSON_CreateObject();
            if (!item) continue;
            cJSON_AddStringToObject(item, "slug", slug->valuestring);
            cJSON_AddStringToObject(item, "name", name->valuestring);
            cJSON *post_count = cJSON_GetObjectItem(bo, "post_count");
            cJSON_AddNumberToObject(item, "post_count", post_count ? post_count->valuedouble : 0);
            cJSON_AddItemToArray(out, item);
        }
    }
    if (roots) cJSON_Delete(roots);

    char *json = out ? cJSON_PrintUnformatted(out) : NULL;
    cwist_http_header_add(&res->headers, "Content-Type", "application/json; charset=utf-8");
    cwist_http_header_add(&res->headers, "Cache-Control", "no-cache, private");
    cwist_sstring_assign(res->body, json ? json : "[]");

    if (json) free(json);
    if (out) cJSON_Delete(out);
    if (boards) cJSON_Delete(boards);
}

/* ---- Progressive Themes ---- */
void handler_themes_json(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    char *json = theme_build_all_json();
    cwist_http_header_add(&res->headers, "Content-Type", "application/json; charset=utf-8");
    cwist_http_header_add(&res->headers, "Cache-Control", "no-store, no-cache, must-revalidate, private");
    if (json) {
        cwist_sstring_assign(res->body, json);
        free(json);
    }
}

/* ---- RSS Feed ---- */
static char *rfc822_time(const char *iso) {
    static char buf[64];
    struct tm tm = {0};
    sscanf(iso, "%d-%d-%d %d:%d:%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour, &tm.tm_min, &tm.tm_sec);
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", &tm);
    return buf;
}

static void append_rss_link(cwist_sstring *rss, const char *root, const char *path) {
    if (root && root[0]) {
        cwist_sstring_append(rss, root);
        size_t len = strlen(root);
        if (len > 0 && root[len - 1] == '/' && path[0] == '/') {
            path++;
        }
    }
    cwist_sstring_append(rss, path);
}

void handler_rss_xml(cwist_http_request *req, cwist_http_response *res) {
    cJSON *posts = db_post_list_search(req->db, 0, NULL, NULL, 20, 0);
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
    cwist_sstring_append(rss, "<link>"); append_rss_link(rss, g_config.root_url, "/"); cwist_sstring_append(rss, "</link>\n");
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
            cwist_sstring_append(rss, "<link>"); append_rss_link(rss, g_config.root_url, "/post/"); cwist_sstring_append(rss, slug->valuestring); cwist_sstring_append(rss, "</link>\n");
            cwist_sstring_append(rss, "<guid>"); append_rss_link(rss, g_config.root_url, "/post/"); cwist_sstring_append(rss, slug->valuestring); cwist_sstring_append(rss, "</guid>\n");
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
    cwist_http_header_add(&res->headers, "Cache-Control", "no-cache, private");
    if (etag_buf[0]) cwist_http_header_add(&res->headers, "ETag", etag_buf);
    if (last_modified[0]) cwist_http_header_add(&res->headers, "Last-Modified", last_modified);
    cwist_sstring_assign(res->body, rss->data);
    cwist_sstring_destroy(rss);
}

/* ---- Sitemap / robots ---- */
static void append_sitemap_url(cwist_sstring *sm, const char *path, const char *lastmod) {
    cwist_sstring_append(sm, "<url>\n<loc>");
    append_rss_link(sm, g_config.root_url, path);
    cwist_sstring_append(sm, "</loc>\n");
    if (lastmod && strlen(lastmod) >= 10) {
        cwist_sstring_append(sm, "<lastmod>");
        cwist_sstring_append_len(sm, lastmod, 10); /* YYYY-MM-DD */
        cwist_sstring_append(sm, "</lastmod>\n");
    }
    cwist_sstring_append(sm, "</url>\n");
}

void handler_sitemap_xml(cwist_http_request *req, cwist_http_response *res) {
    cJSON *posts = db_post_list_search(req->db, 0, NULL, NULL, 5000, 0);
    cwist_sstring *sm = cwist_sstring_create();
    cwist_sstring_append(sm, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
    cwist_sstring_append(sm, "<urlset xmlns=\"http://www.sitemaps.org/schemas/sitemap/0.9\">\n");

    static const char *const static_paths[] = {"/", "/boards", "/files"};
    for (size_t i = 0; i < sizeof(static_paths) / sizeof(static_paths[0]); i++) {
        append_sitemap_url(sm, static_paths[i], NULL);
    }

    if (posts) {
        int n = cJSON_GetArraySize(posts);
        for (int i = 0; i < n; i++) {
            cJSON *p = cJSON_GetArrayItem(posts, i);
            cJSON *secret = cJSON_GetObjectItem(p, "is_secret");
            if (secret && secret->valueint) continue;
            cJSON *slug = cJSON_GetObjectItem(p, "slug");
            if (!slug || !slug->valuestring || !slug->valuestring[0]) continue;
            cwist_sstring *path = cwist_sstring_create();
            cwist_sstring_append(path, "/post/");
            cwist_sstring_append_escaped(path, slug->valuestring);
            cJSON *upd = cJSON_GetObjectItem(p, "updated_at");
            cJSON *crt = cJSON_GetObjectItem(p, "created_at");
            const char *lastmod = (upd && upd->valuestring && upd->valuestring[0]) ? upd->valuestring
                                : (crt && crt->valuestring) ? crt->valuestring : NULL;
            append_sitemap_url(sm, path->data, lastmod);
            cwist_sstring_destroy(path);
        }
        cJSON_Delete(posts);
    }
    cwist_sstring_append(sm, "</urlset>");

    cwist_http_header_add(&res->headers, "Content-Type", "application/xml; charset=utf-8");
    cwist_http_header_add(&res->headers, "Cache-Control", "public, max-age=3600");
    cwist_sstring_assign(res->body, sm->data);
    cwist_sstring_destroy(sm);
}

void handler_robots_txt(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cwist_sstring *robots = cwist_sstring_create();
    cwist_sstring_append(robots, "User-agent: *\nAllow: /\n");
    cwist_sstring_append(robots, "Sitemap: ");
    append_rss_link(robots, g_config.root_url, "/sitemap.xml");
    cwist_sstring_append(robots, "\n");

    cwist_http_header_add(&res->headers, "Content-Type", "text/plain; charset=utf-8");
    cwist_http_header_add(&res->headers, "Cache-Control", "public, max-age=3600");
    cwist_sstring_assign(res->body, robots->data);
    cwist_sstring_destroy(robots);
}

/* ---- My Files ---- */
void handler_api_my_files(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0;
    char role[32] = {0};
    if (!auth_require_login(req, res, &uid, role, sizeof(role))) return;
    cJSON *files = db_file_list_by_user(req->db, uid, 200);
    char *json = files ? cJSON_PrintUnformatted(files) : NULL;
    cwist_http_header_add(&res->headers, "Content-Type", "application/json; charset=utf-8");
    cwist_http_header_add(&res->headers, "Cache-Control", "no-cache, private");
    cwist_sstring_assign(res->body, json ? json : "[]");
    if (json) free(json);
    if (files) cJSON_Delete(files);
}

/* ---- Post vote ---- */
void handler_post_vote(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0;
    char role[32] = {0};
    bool logged_in = auth_require_login(req, res, &uid, role, sizeof(role));
    if (!logged_in) {
        res->status_code = CWIST_HTTP_OK;
        cwist_sstring_assign(res->body, "");
        uid = 0;
    }
    cwist_query_map *kv = cwist_query_map_create(); cwist_query_map_parse(kv, req->body->data);
    const char *post_id_str = cwist_query_map_get(kv, "post_id");
    const char *vote_type_str = cwist_query_map_get(kv, "vote_type");
    if (!post_id_str || !vote_type_str) {
        res->status_code = CWIST_HTTP_BAD_REQUEST;
        cwist_sstring_assign(res->body, "Missing parameters");
        cwist_query_map_destroy(kv);
        return;
    }
    int post_id = atoi(post_id_str);
    int vote_type = atoi(vote_type_str);
    if (logged_in) {
        if (vote_type == 0) {
            db_post_vote_remove(req->db, post_id, uid);
            CWIST_LOG_INFO("Vote removed: post_id=%d uid=%d", post_id, uid);
        } else {
            db_post_vote(req->db, post_id, uid, vote_type);
            CWIST_LOG_INFO("Vote cast: post_id=%d uid=%d vote_type=%d", post_id, uid, vote_type);
        }
    } else {
        if (vote_type != 0) {
            db_post_vote_anon(req->db, post_id, vote_type);
            CWIST_LOG_INFO("Anon vote cast: post_id=%d vote_type=%d", post_id, vote_type);
        }
    }
    cJSON *counts = db_post_vote_counts(req->db, post_id);
    int user_vote = logged_in ? db_post_user_vote(req->db, post_id, uid) : 0;
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
    cwist_http_header_add(&res->headers, "Cache-Control", "no-cache, private");
    cwist_sstring_assign(res->body, json ? json : "{}");
    if (json) free(json);
    cwist_query_map_destroy(kv);
}

/* ---- Prometheus / OpenTelemetry Metrics Endpoint ---- */
void handler_api_metrics(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cwist_sstring *m = cwist_sstring_create();
    int post_count = db_post_count(req->db, 0);

    cwist_sstring_append(m, "# HELP flyboard_posts_total Total number of posts in database.\n");
    cwist_sstring_append(m, "# TYPE flyboard_posts_total counter\n");
    char pbuf[64];
    snprintf(pbuf, sizeof(pbuf), "flyboard_posts_total %d\n\n", post_count);
    cwist_sstring_append(m, pbuf);

    cwist_sstring_append(m, "# HELP flyboard_up_status Operational status of flyboard process.\n");
    cwist_sstring_append(m, "# TYPE flyboard_up_status gauge\n");
    cwist_sstring_append(m, "flyboard_up_status 1\n");

    cwist_http_header_add(&res->headers, "Content-Type", "text/plain; version=0.0.4; charset=utf-8");
    cwist_http_header_add(&res->headers, "Cache-Control", "no-cache, private");
    cwist_sstring_assign(res->body, m->data);
    cwist_sstring_destroy(m);
}

#define _POSIX_C_SOURCE 200809L
#include "handlers_internal.h"
#include "../utils/media_preview.h"
#include <ctype.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <time.h>

/* Production-grade cache control:
   - Static user content (images/uploads/profile): public caches allowed, 1-day freshness,
     must revalidate after that. ETag is always provided for validation.
   - Generic downloads: same policy. */ 
#define IMAGE_CACHE_CONTROL "public, max-age=86400, must-revalidate"
#define FILE_CACHE_CONTROL  "public, max-age=86400, must-revalidate"

static void send_upload_not_found(cwist_http_response *res) {
    res->status_code = CWIST_HTTP_NOT_FOUND;
    cwist_sstring_assign(res->status_text, "Not Found");
    cwist_sstring_assign(res->body, "Not found");
}

static void http_date(time_t t, char *out, size_t out_len) {
    struct tm tm_buf;
    if (!gmtime_r(&t, &tm_buf)) {
        out[0] = '\0';
        return;
    }
    strftime(out, out_len, "%a, %d %b %Y %H:%M:%S GMT", &tm_buf);
}

static void file_etag(const struct stat *st, char *out, size_t out_len) {
    snprintf(out, out_len, "\"%llx-%llx\"",
             (unsigned long long)st->st_size,
             (unsigned long long)st->st_mtime);
}

static bool request_cache_fresh(cwist_http_request *req, const char *etag, const char *last_modified) {
    const char *if_none = cwist_http_header_get(req->headers, "If-None-Match");
    if (if_none && etag && strstr(if_none, etag)) return true;

    const char *if_mod = cwist_http_header_get(req->headers, "If-Modified-Since");
    if (if_mod && last_modified && strcmp(if_mod, last_modified) == 0) return true;

    return false;
}

bool send_cached_file_response(cwist_http_request *req, cwist_http_response *res,
                               const char *path, const char *mime,
                               const char *cache_control, bool *not_modified) {
    struct stat st;
    if (not_modified) *not_modified = false;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0) return false;

    char etag[64];
    char last_modified[64];
    file_etag(&st, etag, sizeof(etag));
    http_date(st.st_mtime, last_modified, sizeof(last_modified));

    cwist_http_header_add(&res->headers, "Content-Type", mime ? mime : "application/octet-stream");
    cwist_http_header_add(&res->headers, "Cache-Control", cache_control ? cache_control : FILE_CACHE_CONTROL);
    cwist_http_header_add(&res->headers, "ETag", etag);
    cwist_http_header_add(&res->headers, "Accept-Ranges", "bytes");
    if (last_modified[0]) cwist_http_header_add(&res->headers, "Last-Modified", last_modified);



    if (request_cache_fresh(req, etag, last_modified)) {
        res->status_code = (cwist_http_status_t)304;
        cwist_sstring_assign(res->status_text, "Not Modified");
        cwist_sstring_assign(res->body, "");
        if (not_modified) *not_modified = true;
        return true;
    }

    off_t range_start = 0;
    off_t range_end = st.st_size - 1;
    bool is_range = false;

    const char *range_hdr = cwist_http_header_get(req->headers, "Range");
    if (range_hdr && strncmp(range_hdr, "bytes=", 6) == 0) {
        const char *p = range_hdr + 6;
        while (*p && isspace((unsigned char)*p)) p++;

        if (*p == '-') {
            p++;
            char *end_ptr;
            long long val = strtoll(p, &end_ptr, 10);
            if (end_ptr != p && val > 0) {
                if (val > (long long)st.st_size) {
                    val = (long long)st.st_size;
                }
                range_start = st.st_size - val;
                range_end = st.st_size - 1;
                is_range = true;
            }
        } else {
            char *end_ptr;
            long long start_val = strtoll(p, &end_ptr, 10);
            if (end_ptr != p && start_val >= 0) {
                p = end_ptr;
                while (*p && isspace((unsigned char)*p)) p++;
                if (*p == '-') {
                    p++;
                    while (*p && isspace((unsigned char)*p)) p++;
                    if (*p == '\0') {
                        if (start_val < (long long)st.st_size) {
                            range_start = start_val;
                            range_end = st.st_size - 1;
                            is_range = true;
                        }
                    } else {
                        char *end_ptr2;
                        long long end_val = strtoll(p, &end_ptr2, 10);
                        if (end_ptr2 != p && end_val >= start_val) {
                            range_start = start_val;
                            range_end = (end_val < (long long)st.st_size) ? end_val : (st.st_size - 1);
                            is_range = true;
                        }
                    }
                }
            }
        }

        if (!is_range) {
            res->status_code = (cwist_http_status_t)416;
            cwist_sstring_assign(res->status_text, "Range Not Satisfiable");
            char content_range[128];
            snprintf(content_range, sizeof(content_range), "bytes */%lld", (long long)st.st_size);
            cwist_http_header_add(&res->headers, "Content-Range", content_range);
            cwist_http_header_add(&res->headers, "Content-Length", "0");
            cwist_sstring_assign(res->body, "");
            if (not_modified) *not_modified = false;
            return true;
        }
    }

    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;

    size_t sz = (size_t)st.st_size;
    if (sz == 0) {
        close(fd);
        res->status_code = CWIST_HTTP_OK;
        cwist_sstring_assign(res->status_text, "OK");
        cwist_http_header_add(&res->headers, "Content-Length", "0");
        cwist_sstring_assign(res->body, "");
        return true;
    }

    size_t response_len = is_range ? (size_t)(range_end - range_start + 1) : sz;
    off_t response_offset = is_range ? range_start : 0;

    /* CWIST's HTTPS-over-HTTP/1.1 path stringifies the response and ignores
       res->use_file_stream content, so the body must be materialized here.
       HTTP/2 and HTTP/3 have their own send paths that honor file_stream. */
    bool is_http11_tls = g_config.use_tls && req->stream_id == 0 &&
        req->version && req->version->data &&
        strncmp(req->version->data, "HTTP/1.", 7) == 0;

    if (is_http11_tls) {
        char *buf = (char *)cwist_alloc(response_len);
        if (!buf) {
            close(fd);
            res->status_code = CWIST_HTTP_SERVICE_UNAVAILABLE;
            cwist_sstring_assign(res->status_text, "Service Unavailable");
            cwist_http_header_add(&res->headers, "Content-Type", "text/plain");
            cwist_sstring_assign(res->body, "Asset too large for HTTP/1.1 TLS transport; use HTTP/2 or HTTP/3.");
            return true;
        }
        size_t total = 0;
        while (total < response_len) {
            ssize_t rc = pread(fd, buf + total, response_len - total,
                               response_offset + (off_t)total);
            if (rc <= 0) break;
            total += (size_t)rc;
        }
        close(fd);
        if (total != response_len) { cwist_free(buf); return false; }

        res->use_file_stream = false;
        cwist_sstring_assign(res->body, "");
        cwist_sstring_append_len(res->body, buf, total);
        cwist_free(buf);

        res->status_code = is_range ? (cwist_http_status_t)206 : CWIST_HTTP_OK;
        cwist_sstring_assign(res->status_text, is_range ? "Partial Content" : "OK");
        if (is_range) {
            char content_range[128];
            snprintf(content_range, sizeof(content_range), "bytes %lld-%lld/%lld",
                     (long long)range_start, (long long)range_end, (long long)st.st_size);
            cwist_http_header_add(&res->headers, "Content-Range", content_range);
        }
        char len_buf[32];
        snprintf(len_buf, sizeof(len_buf), "%zu", response_len);
        cwist_http_header_add(&res->headers, "Content-Length", len_buf);
        return true;
    }

    res->use_file_stream = true;
    res->file_stream_fd = fd;
    res->file_stream_auto_close = true;
    cwist_sstring_assign(res->body, "");

    if (is_range) {
        res->status_code = (cwist_http_status_t)206;
        cwist_sstring_assign(res->status_text, "Partial Content");
        char content_range[128];
        snprintf(content_range, sizeof(content_range), "bytes %lld-%lld/%lld",
                 (long long)range_start, (long long)range_end, (long long)st.st_size);
        cwist_http_header_add(&res->headers, "Content-Range", content_range);

        res->file_stream_len = response_len;
        res->file_stream_offset = range_start;
    } else {
        res->status_code = CWIST_HTTP_OK;
        cwist_sstring_assign(res->status_text, "OK");
        res->file_stream_len = sz;
        res->file_stream_offset = 0;
    }

    /* Firefox is strict about Content-Length matching the actual body length
       for range responses; ensure the header is explicit and correct. */
    char len_buf[32];
    snprintf(len_buf, sizeof(len_buf), "%zu", response_len);
    cwist_http_header_add(&res->headers, "Content-Length", len_buf);

    return true;
}

static bool client_accepts_webp(cwist_http_request *req) {
    const char *accept = cwist_http_header_get(req->headers, "Accept");
    return accept && strstr(accept, "image/webp") != NULL;
}

static void build_static_asset_thumb_path(const char *scope, const char *filename,
                                          int w, int h, char *out, size_t out_len) {
    char scope_fname[512] = {0};
    snprintf(scope_fname, sizeof(scope_fname), "%s_", scope);
    char *p = scope_fname + strlen(scope_fname);
    for (size_t i = 0; filename[i] && (size_t)(p - scope_fname) < sizeof(scope_fname) - 1; i++) {
        *p++ = (filename[i] == '/') ? '_' : filename[i];
    }
    *p = '\0';
    snprintf(out, out_len, "public/uploads/.thumbs/asset_%s_%dx%d.webp", scope_fname, w, h);
}

/* Serve a static image asset (img or profile scope) as a webp file with
   aggressive compression. Falls back to the original file when the client does
   not accept webp or when conversion fails. */
static bool send_static_asset_webp_response(cwist_http_request *req, cwist_http_response *res,
                                            const char *scope, const char *path, const char *filename,
                                            int default_w, int default_h) {
    const char *orig_mime = mime_type(filename);
    bool is_image = orig_mime && strncmp(orig_mime, "image/", 6) == 0;
    if (!is_image || !client_accepts_webp(req)) {
        return send_cached_file_response(req, res, path, orig_mime, IMAGE_CACHE_CONTROL, NULL);
    }

    int w = default_w;
    int h = default_h;
    const char *w_str = cwist_query_map_get(req->query_params, "w");
    const char *h_str = cwist_query_map_get(req->query_params, "h");
    if (w_str && h_str) {
        int qw = atoi(w_str);
        int qh = atoi(h_str);
        if (qw > 0 && qh > 0) {
            if (qw > 3072) qw = 3072;
            if (qh > 3072) qh = 3072;
            w = qw;
            h = qh;
        }
    }

    char thumb_path[PATH_MAX];
    build_static_asset_thumb_path(scope, filename, w, h, thumb_path, sizeof(thumb_path));
    struct stat st;
    if (stat(thumb_path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
        if (!generate_static_asset_webp(path, thumb_path, w, h)) {
            return send_cached_file_response(req, res, path, orig_mime, IMAGE_CACHE_CONTROL, NULL);
        }
    }
    return send_cached_file_response(req, res, thumb_path, "image/webp", IMAGE_CACHE_CONTROL, NULL);
}

static char *decode_upload_path_segment(const char *src) {
    size_t len = strlen(src);
    char *out = (char *)cwist_alloc(len + 1);
    if (!out) return NULL;
    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        if (src[i] == '%' && i + 2 < len && isxdigit((unsigned char)src[i + 1]) && isxdigit((unsigned char)src[i + 2])) {
            unsigned int val = 0;
            sscanf(src + i + 1, "%2x", &val);
            out[j++] = (char)val;
            i += 2;
        } else {
            out[j++] = src[i];
        }
    }
    out[j] = '\0';
    return out;
}

static bool is_safe_upload_name(const char *name) {
    if (!name || !name[0]) return false;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return false;
    return strchr(name, '/') == NULL && strchr(name, '\\') == NULL;
}

static bool is_safe_upload_preview_name(const char *name) {
    if (!name) return false;
    if (strncmp(name, ".thumbs/", 8) == 0) return is_safe_upload_name(name + 8);
    if (strncmp(name, ".previews/", 10) == 0) return is_safe_upload_name(name + 10);
    return false;
}

bool is_profile_pic_asset(cwist_db *db, const char *name) {
    if (!db || !name || !name[0]) return false;
    char profile_url[512];
    int written_profile = snprintf(profile_url, sizeof(profile_url), "/assets/profile/%s", name);
    if (written_profile < 0 || written_profile >= (int)sizeof(profile_url)) return false;

    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT 1 FROM users WHERE profile_pic=? LIMIT 1";
    if (sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, profile_url, -1, SQLITE_STATIC);
    bool found = sqlite3_step(stmt) == SQLITE_ROW;
    sqlite3_finalize(stmt);
    return found;
}

void handler_asset_img(cwist_http_request *req, cwist_http_response *res) {
    const char *encoded = cwist_query_map_get(req->path_params, "filename");
    if (!encoded || !encoded[0]) { send_upload_not_found(res); return; }

    char *decoded = decode_upload_path_segment(encoded);
    if (!decoded) {
        res->status_code = CWIST_HTTP_INTERNAL_ERROR;
        cwist_sstring_assign(res->body, "decode error");
        return;
    }
    if (!is_safe_upload_name(decoded)) {
        cwist_free(decoded);
        send_upload_not_found(res);
        return;
    }

    char path[PATH_MAX];
    int written = snprintf(path, sizeof(path), "public/img/%s", decoded);
    if (written < 0 || written >= (int)sizeof(path)) {
        cwist_free(decoded);
        send_upload_not_found(res);
        return;
    }

    if (!send_static_asset_webp_response(req, res, "img", path, decoded, 1920, 1920)) {
        cwist_free(decoded);
        send_upload_not_found(res);
        return;
    }

    cwist_free(decoded);
}

void handler_asset_upload(cwist_http_request *req, cwist_http_response *res) {
    const char *encoded = cwist_query_map_get(req->path_params, "filename");
    if (!encoded || !encoded[0]) { send_upload_not_found(res); return; }

    char *decoded = decode_upload_path_segment(encoded);
    if (!decoded) {
        res->status_code = CWIST_HTTP_INTERNAL_ERROR;
        cwist_sstring_assign(res->body, "decode error");
        return;
    }
    bool top_level_upload = is_safe_upload_name(decoded);
    bool derived_upload = is_safe_upload_preview_name(decoded);
    if (!top_level_upload && !derived_upload) {
        cwist_free(decoded);
        send_upload_not_found(res);
        return;
    }

    char path[PATH_MAX];
    int written = snprintf(path, sizeof(path), "public/uploads/%s", decoded);
    if (written < 0 || written >= (int)sizeof(path)) {
        cwist_free(decoded);
        send_upload_not_found(res);
        return;
    }

    char detected_mime[128] = {0};
    const char *response_mime = mime_type(decoded);
    if (mime_type_from_data(path, detected_mime, sizeof(detected_mime))) {
        response_mime = detected_mime;
    }

    if (top_level_upload && (!is_profile_pic_asset(req->db, decoded) || strncmp(response_mime, "image/", 6) != 0)) {
        cwist_free(decoded);
        res->status_code = CWIST_HTTP_FORBIDDEN;
        cwist_sstring_assign(res->body, "Direct asset fetch disabled; use the reliable transfer path.");
        return;
    }

    if (!send_cached_file_response(req, res, path, response_mime, IMAGE_CACHE_CONTROL, NULL)) {
        cwist_free(decoded);
        send_upload_not_found(res);
        return;
    }
    cwist_free(decoded);
}


void handler_asset_profile_upload(cwist_http_request *req, cwist_http_response *res) {
    const char *encoded = cwist_query_map_get(req->path_params, "filename");
    if (!encoded || !encoded[0]) { send_upload_not_found(res); return; }

    char *decoded = decode_upload_path_segment(encoded);
    if (!decoded) {
        res->status_code = CWIST_HTTP_INTERNAL_ERROR;
        cwist_sstring_assign(res->body, "decode error");
        return;
    }
    if (!is_safe_upload_name(decoded)) {
        cwist_free(decoded);
        send_upload_not_found(res);
        return;
    }

    if (!is_profile_pic_asset(req->db, decoded)) {
        cwist_free(decoded);
        res->status_code = CWIST_HTTP_FORBIDDEN;
        cwist_sstring_assign(res->body, "Not a profile picture asset.");
        return;
    }

    char path[PATH_MAX];
    int written = snprintf(path, sizeof(path), "public/profile/%s", decoded);
    if (written < 0 || written >= (int)sizeof(path)) {
        cwist_free(decoded);
        send_upload_not_found(res);
        return;
    }
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        written = snprintf(path, sizeof(path), "public/uploads/%s", decoded);
        if (written < 0 || written >= (int)sizeof(path)) {
            cwist_free(decoded);
            send_upload_not_found(res);
            return;
        }
    }

    if (!send_static_asset_webp_response(req, res, "profile", path, decoded, 512, 512)) {
        cwist_free(decoded);
        send_upload_not_found(res);
        return;
    }
    cwist_free(decoded);
}

void handler_file_repo(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0;
    char role[32] = {0};
    auth_is_logged_in(req, &uid, role, sizeof(role));
    char sql[] =
        "SELECT a.id, a.user_id, a.filename, a.mime_type, a.file_path, a.size, a.created_at, a.thumb_path, a.preview_path "
        "FROM files a "
        "LEFT JOIN files b ON (a.post_id = b.post_id OR (a.post_id IS NULL AND b.post_id IS NULL)) "
        "AND a.filename = b.filename AND a.id < b.id "
        "WHERE (a.post_id = 0 OR a.post_id IS NULL) AND b.id IS NULL AND (a.file_path IS NULL OR a.file_path NOT LIKE '%/.thumbs/%') "
        "ORDER BY a.id DESC LIMIT 200";
    cJSON *files = NULL;
    cwist_db_query(req->db, sql, &files);
    char *pp = get_profile_pic(req->db, uid, role);
    cwist_sstring *page = render_file_repo(files, is_dark(req), role, uid, pp, is_mobile_request(req));
    if (files) cJSON_Delete(files);
    send_html_res(res, page);
    free(pp);
}

void handler_file_preview(cwist_http_request *req, cwist_http_response *res) {
    const char *id_str = cwist_query_map_get(req->path_params, "id");
    int id = id_str ? atoi(id_str) : 0;
    if (id <= 0) {
        send_upload_not_found(res);
        return;
    }

    cJSON *file = db_file_get(req->db, id);
    if (!file) {
        send_upload_not_found(res);
        return;
    }

    cJSON *jpath = cJSON_GetObjectItem(file, "file_path");
    cJSON *jfilename = cJSON_GetObjectItem(file, "filename");
    cJSON *jmime = cJSON_GetObjectItem(file, "mime_type");
    const char *path = (jpath && jpath->type == cJSON_String && jpath->valuestring) ? jpath->valuestring : "";
    const char *filename = (jfilename && jfilename->type == cJSON_String && jfilename->valuestring) ? jfilename->valuestring : "";
    const char *mime = (jmime && jmime->type == cJSON_String && jmime->valuestring) ? jmime->valuestring : "";
    char detected_mime[128] = {0};
    if (!mime[0] || strcmp(mime, "application/octet-stream") == 0) {
        if (mime_type_from_data(path, detected_mime, sizeof(detected_mime))) {
            mime = detected_mime;
        } else {
            mime = mime_type(filename);
        }
    }
    if (strncmp(mime, "image/", 6) != 0) {
        cJSON_Delete(file);
        send_upload_not_found(res);
        return;
    }

    int w = 1280;
    int h = 1280;
    const char *w_str = cwist_query_map_get(req->query_params, "w");
    const char *h_str = cwist_query_map_get(req->query_params, "h");
    if (w_str) w = atoi(w_str);
    if (h_str) h = atoi(h_str);
    if (w <= 0) w = 1280;
    if (h <= 0) h = 1280;
    if (w >= h) {
        if (w < 1080) w = 1080;
        if (h < 720) h = 720;
    } else {
        if (w < 720) w = 720;
        if (h < 1080) h = 1080;
    }
    if (w > 3072) w = 3072;
    if (h > 3072) h = 3072;

    char preview_path[PATH_MAX] = {0};
    snprintf(preview_path, sizeof(preview_path), "public/uploads/.thumbs/%d_%dx%d.webp", id, w, h);
    struct stat st;
    if (stat(preview_path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
        if (!generate_image_thumb(path, preview_path, w, h)) {
            snprintf(preview_path, sizeof(preview_path), "%s", path);
        }
    }

    if (!send_cached_file_response(req, res, preview_path, "image/webp", IMAGE_CACHE_CONTROL, NULL)) {
        send_upload_not_found(res);
    }
    cJSON_Delete(file);
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
    cwist_sstring *page = render_file_detail(file, comments, is_dark(req), role, pp, uid, is_mobile_request(req));
    cJSON_Delete(file);
    if (comments) cJSON_Delete(comments);
    send_html_res(res, page);
    free(pp);
}

void handler_file_download(cwist_http_request *req, cwist_http_response *res) {
    const char *id_str = cwist_query_map_get(req->path_params, "id");
    if (!id_str) { send_upload_not_found(res); return; }

    cJSON *file = db_file_get(req->db, atoi(id_str));
    if (!file) { send_upload_not_found(res); return; }

    cJSON *jpath = cJSON_GetObjectItem(file, "file_path");
    cJSON *jfilename = cJSON_GetObjectItem(file, "filename");
    cJSON *jmime = cJSON_GetObjectItem(file, "mime_type");
    cJSON *jthumb = cJSON_GetObjectItem(file, "thumb_path");
    cJSON *jpreview = cJSON_GetObjectItem(file, "preview_path");
    char resolved_path[PATH_MAX];
    snprintf(resolved_path, sizeof(resolved_path), "%s", (jpath && jpath->type == cJSON_String && jpath->valuestring) ? jpath->valuestring : "");
    const char *filename = (jfilename && jfilename->type == cJSON_String && jfilename->valuestring) ? jfilename->valuestring : "download";
    const char *mime = (jmime && jmime->type == cJSON_String && jmime->valuestring) ? jmime->valuestring : "application/octet-stream";
    const char *thumb_path = (jthumb && jthumb->type == cJSON_String && jthumb->valuestring) ? jthumb->valuestring : "";
    const char *preview_path = (jpreview && jpreview->type == cJSON_String && jpreview->valuestring) ? jpreview->valuestring : "";
    bool is_image = strncmp(mime, "image/", 6) == 0;
    bool is_media = strncmp(mime, "video/", 6) == 0 || strncmp(mime, "audio/", 6) == 0;
    if (!is_image && !is_media && (!mime[0] || strcmp(mime, "application/octet-stream") == 0)) {
        const char *name_mime = mime_type(filename);
        if (name_mime) {
            if (strncmp(name_mime, "image/", 6) == 0) {
                mime = name_mime;
                is_image = true;
            } else if (strncmp(name_mime, "video/", 6) == 0 || strncmp(name_mime, "audio/", 6) == 0) {
                mime = name_mime;
                is_media = true;
            }
        }
    }

    struct stat st;
    if (stat(resolved_path, &st) != 0 || st.st_size <= 0) {
        cJSON_Delete(file);
        send_upload_not_found(res);
        return;
    }

    char detected_mime[128] = {0};
    if (mime_type_from_data(resolved_path, detected_mime, sizeof(detected_mime))) {
        mime = detected_mime;
        is_image = strncmp(mime, "image/", 6) == 0;
        is_media = strncmp(mime, "video/", 6) == 0 || strncmp(mime, "audio/", 6) == 0;
    }

    if (is_image) {
        const char *w_str = cwist_query_map_get(req->query_params, "w");
        const char *h_str = cwist_query_map_get(req->query_params, "h");
        if (w_str && h_str) {
            int w = atoi(w_str);
            int h = atoi(h_str);
            if (w > 0 && h > 0) {
                if (w >= h) {
                    if (w < 1080) w = 1080;
                    if (h < 720) h = 720;
                } else {
                    if (w < 720) w = 720;
                    if (h < 1080) h = 1080;
                }
                if (w > 3072) w = 3072;
                if (h > 3072) h = 3072;

                char thumb_img_path[PATH_MAX];
                snprintf(thumb_img_path, sizeof(thumb_img_path), "public/uploads/.thumbs/%d_%dx%d.webp", atoi(id_str), w, h);
                struct stat pst;
                if (stat(thumb_img_path, &pst) != 0 || !S_ISREG(pst.st_mode) || pst.st_size <= 0) {
                    if (generate_image_thumb(resolved_path, thumb_img_path, w, h)) {
                        snprintf(resolved_path, sizeof(resolved_path), "%s", thumb_img_path);
                        mime = "image/webp";
                    }
                } else {
                    snprintf(resolved_path, sizeof(resolved_path), "%s", thumb_img_path);
                    mime = "image/webp";
                }
            }
        }
    }

    const char *path = resolved_path;

    bool wants_preview = false;
    const char *preview_q = cwist_query_map_get(req->query_params, "preview");
    if (preview_q && (strcmp(preview_q, "1") == 0 || strcmp(preview_q, "true") == 0)) {
        wants_preview = true;
    }
    char generated_preview[PATH_MAX] = {0};
    if (wants_preview && strncmp(mime, "video/", 6) == 0) {
        struct stat pst;
        if (preview_path[0] && strncmp(preview_path, "public/uploads/", 15) == 0 &&
            stat(preview_path, &pst) == 0 && S_ISREG(pst.st_mode) && pst.st_size > 0) {
            path = preview_path;
        } else {
            snprintf(generated_preview, sizeof(generated_preview), "public/uploads/.previews/%d.mp4", atoi(id_str));
            if (!generate_video_preview(path, generated_preview, 1080)) {
                cJSON_Delete(file);
                send_upload_not_found(res);
                return;
            }
            db_file_set_preview_paths(req->db, atoi(id_str), thumb_path, generated_preview);
            path = generated_preview;
        }
        filename = "preview.mp4";
        mime = "video/mp4";
        is_image = false;
        is_media = true;
    }

    if (g_config.use_tasfa && !is_image) {
        bool has_valid_session = false;
        const char *session_id = cwist_http_header_get(req->headers, "X-TASFA-Session-ID");
        const char *session_token = cwist_http_header_get(req->headers, "X-TASFA-Session-Token");
        if (!session_id || !session_token) {
            session_id = cwist_query_map_get(req->query_params, "session_id");
            session_token = cwist_query_map_get(req->query_params, "session_token");
        }
        if (session_id && session_token) {
            cJSON *meta = load_download_session_cached(session_id);
            if (meta) {
                cJSON *tok = cJSON_GetObjectItem(meta, "session_token");
                if (tok && cJSON_IsString(tok) && tok->valuestring && secure_str_eq(session_token, tok->valuestring)) {
                    has_valid_session = true;
                }
                cJSON_Delete(meta);
            }
        }
        if (!has_valid_session) {
            cJSON_Delete(file);
            res->status_code = CWIST_HTTP_FORBIDDEN;
            cwist_sstring_assign(res->body, "Direct download disabled; use the reliable transfer path.");
            return;
        }
    }

    char disp[512];
    snprintf(disp, sizeof(disp), "%s; filename=\"%s\"", (is_image || is_media) ? "inline" : "attachment", filename);
    cwist_http_header_add(&res->headers, "Content-Disposition", disp);

    db_file_increment_download(req->db, atoi(id_str));

    bool not_modified = false;
    bool ok = send_cached_file_response(req, res, path, mime, is_image ? IMAGE_CACHE_CONTROL : FILE_CACHE_CONTROL, &not_modified);
    cJSON_Delete(file);
    if (!ok) {
        send_upload_not_found(res);
        return;
    }
}

void handler_file_delete(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0;
    char role[32] = {0};
    auth_is_logged_in(req, &uid, role, sizeof(role));
    cwist_query_map *kv = cwist_query_map_create(); cwist_query_map_parse(kv, req->body->data);
    const char *id_str = cwist_query_map_get(kv, "id");
    const char *delete_pin = cwist_query_map_get(kv, "delete_pin");
    if (id_str) {
        cJSON *f = db_file_get(req->db, atoi(id_str));
        if (f) {
            int file_uid = json_int(f, "user_id", 0);
            cJSON *pin_hash = cJSON_GetObjectItem(f, "delete_pin_hash");
            bool pin_ok = delete_pin && pin_hash && pin_hash->valuestring && pin_hash->valuestring[0] &&
                          auth_verify_password(delete_pin, pin_hash->valuestring);
            bool can_delete = (role[0] && strcmp(role, "admin") == 0) || (file_uid > 0 && file_uid == uid) || pin_ok;
            if (can_delete) {
                cJSON *fpath = cJSON_GetObjectItem(f, "file_path");
                cJSON *thumb_path = cJSON_GetObjectItem(f, "thumb_path");
                cJSON *preview_path = cJSON_GetObjectItem(f, "preview_path");
                if (fpath && fpath->valuestring && fpath->valuestring[0]) {
                    unlink(fpath->valuestring);
                }
                if (thumb_path && thumb_path->valuestring && thumb_path->valuestring[0]) {
                    unlink(thumb_path->valuestring);
                }
                if (preview_path && preview_path->valuestring && preview_path->valuestring[0]) {
                    if (!fpath || !fpath->valuestring || strcmp(preview_path->valuestring, fpath->valuestring) != 0) {
                        unlink(preview_path->valuestring);
                    }
                }
                int fid = atoi(id_str);
                if (db_file_delete(req->db, fid)) {
                    CWIST_LOG_INFO("File deleted: fid=%d by_uid=%d", fid, uid);
                } else {
                    CWIST_LOG_ERROR("File delete failed: fid=%d by_uid=%d", fid, uid);
                }
            } else {
                CWIST_LOG_WARN("File delete forbidden: fid=%s by_uid=%d role=%s", id_str, uid, role);
            }
            cJSON_Delete(f);
        }
    }
    cwist_query_map_destroy(kv);
    redirect(res, "/files");
}

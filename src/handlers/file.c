#define _POSIX_C_SOURCE 200809L
#include "handlers_internal.h"
#include <ctype.h>
#include <sys/stat.h>

static void send_upload_not_found(cwist_http_response *res) {
    res->status_code = CWIST_HTTP_NOT_FOUND;
    cwist_sstring_assign(res->body, "Not found");
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

    size_t sz = 0;
    char *data = file_read(path, &sz);
    if (!data) {
        cwist_free(decoded);
        send_upload_not_found(res);
        return;
    }

    cwist_http_header_add(&res->headers, "Content-Type", mime_type(decoded));
    if (strcmp(decoded, "logo.png") == 0 || (g_config.blog_logo[0] && strcmp(decoded, g_config.blog_logo) == 0)) {
        cwist_http_header_add(&res->headers, "Cache-Control", "public, max-age=31536000, immutable");
    } else {
        cwist_http_header_add(&res->headers, "Cache-Control", "public, max-age=86400");
    }
    char slen[32];
    snprintf(slen, sizeof(slen), "%zu", sz);
    cwist_http_header_add(&res->headers, "Content-Length", slen);
    cwist_sstring_assign(res->body, "");
    cwist_sstring_append_len(res->body, data, sz);

    cwist_free(data);
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
    if (!is_safe_upload_name(decoded)) {
        cwist_free(decoded);
        send_upload_not_found(res);
        return;
    }

    /* Allow direct access to thumbnails and audio previews */
    if (strncmp(decoded, ".thumbs/", 8) == 0 || strncmp(decoded, ".previews/", 10) == 0) {
        char path[PATH_MAX];
        int written = snprintf(path, sizeof(path), "public/uploads/%s", decoded);
        if (written < 0 || written >= (int)sizeof(path)) {
            cwist_free(decoded);
            send_upload_not_found(res);
            return;
        }
        size_t sz = 0;
        char *data = file_read(path, &sz);
        if (!data) {
            cwist_free(decoded);
            send_upload_not_found(res);
            return;
        }
        cwist_http_header_add(&res->headers, "Content-Type", mime_type(decoded));
        cwist_http_header_add(&res->headers, "Cache-Control", "public, max-age=86400");
        char slen[32];
        snprintf(slen, sizeof(slen), "%zu", sz);
        cwist_http_header_add(&res->headers, "Content-Length", slen);
        cwist_sstring_assign(res->body, "");
        cwist_sstring_append_len(res->body, data, sz);
        cwist_free(data);
        cwist_free(decoded);
        return;
    }

    cwist_free(decoded);
    res->status_code = CWIST_HTTP_FORBIDDEN;
    cwist_sstring_assign(res->body, "Direct asset fetch disabled; use the reliable transfer path.");
}

void handler_file_repo(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0;
    char role[32] = {0};
    auth_is_logged_in(req, &uid, role, sizeof(role));
    char sql[] =
        "SELECT a.id, a.user_id, a.filename, a.mime_type, a.file_path, a.size, a.created_at "
        "FROM files a "
        "LEFT JOIN files b ON (a.post_id = b.post_id OR (a.post_id IS NULL AND b.post_id IS NULL)) "
        "AND a.filename = b.filename AND a.id < b.id "
        "WHERE (a.post_id = 0 OR a.post_id IS NULL) AND b.id IS NULL "
        "ORDER BY a.id DESC LIMIT 200";
    cJSON *files = NULL;
    cwist_db_query(req->db, sql, &files);
    char *pp = get_profile_pic(req->db, uid, role);
    cwist_sstring *page = render_file_repo(files, is_dark(req), role, uid, pp);
    if (files) cJSON_Delete(files);
    send_html_res(res, page);
    free(pp);
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
    cwist_sstring *page = render_file_detail(file, comments, is_dark(req), role, pp, uid);
    cJSON_Delete(file);
    if (comments) cJSON_Delete(comments);
    send_html_res(res, page);
    free(pp);
}

void handler_file_download(cwist_http_request *req, cwist_http_response *res) {
    if (g_config.use_tasfa) {
        res->status_code = CWIST_HTTP_FORBIDDEN;
        cwist_sstring_assign(res->body, "Direct download disabled; use the reliable transfer path.");
        return;
    }

    const char *id_str = cwist_query_map_get(req->path_params, "id");
    if (!id_str) { send_upload_not_found(res); return; }

    cJSON *file = db_file_get(req->db, atoi(id_str));
    if (!file) { send_upload_not_found(res); return; }

    cJSON *jpath = cJSON_GetObjectItem(file, "file_path");
    cJSON *jfilename = cJSON_GetObjectItem(file, "filename");
    cJSON *jmime = cJSON_GetObjectItem(file, "mime_type");
    const char *path = (jpath && jpath->type == cJSON_String && jpath->valuestring) ? jpath->valuestring : "";
    const char *filename = (jfilename && jfilename->type == cJSON_String && jfilename->valuestring) ? jfilename->valuestring : "download";
    const char *mime = (jmime && jmime->type == cJSON_String && jmime->valuestring) ? jmime->valuestring : "application/octet-stream";

    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 0) {
        cJSON_Delete(file);
        send_upload_not_found(res);
        return;
    }

    size_t sz = 0;
    char *data = file_read(path, &sz);
    if (!data) {
        cJSON_Delete(file);
        send_upload_not_found(res);
        return;
    }

    char disp[512];
    snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", filename);
    cwist_http_header_add(&res->headers, "Content-Type", mime);
    cwist_http_header_add(&res->headers, "Content-Disposition", disp);
    cwist_http_header_add(&res->headers, "Cache-Control", "public, max-age=86400");
    char slen[32];
    snprintf(slen, sizeof(slen), "%zu", sz);
    cwist_http_header_add(&res->headers, "Content-Length", slen);
    cwist_sstring_assign(res->body, "");
    cwist_sstring_append_len(res->body, data, sz);

    db_file_increment_download(req->db, atoi(id_str));

    cwist_free(data);
    cJSON_Delete(file);
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

#define _POSIX_C_SOURCE 200809L
#include "handlers_internal.h"

void handler_file_repo(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0;
    char role[32] = {0};
    auth_is_logged_in(req, &uid, role, sizeof(role));
    char sql[] = "SELECT id, filename, mime_type, file_path, size, created_at FROM files WHERE post_id=0 OR post_id IS NULL ORDER BY id DESC LIMIT 200";
    cJSON *files = NULL;
    cwist_db_query(req->db, sql, &files);
    char *pp = get_profile_pic(req->db, uid, role);
    cwist_sstring *page = render_file_repo(files, is_dark(req), pp);
    if (files) cJSON_Delete(files);
    send_html_res(res, page);
    free(pp);
}

void handler_file_upload(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0;
    char role[32] = {0};
    if (!auth_require_login(req, res, &uid, role, sizeof(role))) return;
    const char *ctype = cwist_http_header_get(req->headers, "Content-Type");
    if (!ctype || !strstr(ctype, "multipart/form-data")) { redirect(res, "/files"); return; }
    const char *bnd = strstr(ctype, "boundary=");
    if (!bnd) { redirect(res, "/files"); return; }
    bnd += 9;
    form_field_t *fields = multipart_parse(req->body->data, req->body->size, bnd);
    form_field_t *f = form_find(fields, "file");
    if (f && f->filename && f->filename[0] != '\0' && f->data && f->data[0] != '\0') {
        db_file_create_volume(req->db, 0, uid, f->filename, mime_type(f->filename), f->data, f->file_size);
    }
    multipart_free(fields);
    redirect(res, "/files");
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
    const char *id_str = cwist_query_map_get(req->path_params, "id");
    if (!id_str) { res->status_code = CWIST_HTTP_NOT_FOUND; cwist_sstring_assign(res->body, "Not found"); return; }
    cJSON *file = db_file_get(req->db, atoi(id_str));
    if (!file) { res->status_code = CWIST_HTTP_NOT_FOUND; cwist_sstring_assign(res->body, "Not found"); return; }
    cJSON *fname = cJSON_GetObjectItem(file, "filename");
    cJSON *mtype = cJSON_GetObjectItem(file, "mime_type");
    cJSON *fpath = cJSON_GetObjectItem(file, "file_path");

    char cdisp[512];
    snprintf(cdisp, sizeof(cdisp), "attachment; filename=\"%s\"", fname->valuestring);
    cwist_http_header_add(&res->headers, "Content-Disposition", cdisp);

    if (fpath && fpath->valuestring[0]) {
        size_t sz = 0;
        cwist_error_t err = cwist_http_response_send_file(res, fpath->valuestring, mtype && mtype->valuestring[0] ? mtype->valuestring : "application/octet-stream", &sz);
        if (err.error.err_i32 != 0) {
            res->status_code = CWIST_HTTP_NOT_FOUND;
        }
    } else {
        res->status_code = CWIST_HTTP_NOT_FOUND;
    }
    cJSON_Delete(file);
}

void handler_file_delete(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0;
    char role[32] = {0};
    if (!auth_require_login(req, res, &uid, role, sizeof(role))) return;
    form_kv_t *kv = parse_urlencoded(req->body->data);
    const char *id_str = form_kv_get(kv, "id");
    if (id_str) {
        cJSON *f = db_file_get(req->db, atoi(id_str));
        if (f) {
            cJSON *stype = cJSON_GetObjectItem(f, "storage_type");
            cJSON *fpath = cJSON_GetObjectItem(f, "file_path");
            if (strcmp(stype->valuestring, "volume") == 0 && fpath && fpath->valuestring[0]) {
                unlink(fpath->valuestring);
            }
            cJSON_Delete(f);
        }
        db_file_delete(req->db, atoi(id_str));
    }
    form_kv_free(kv);
    redirect(res, "/files");
}

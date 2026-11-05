#define _POSIX_C_SOURCE 200809L
#include "handlers_internal.h"
#include "utils/privacy.h"

static void privacy_json(cwist_http_response *res, int status, cJSON *body) {
    char *text = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!text) {
        res->status_code = CWIST_HTTP_INTERNAL_ERROR;
        cwist_sstring_assign(res->body, "Could not create response");
        return;
    }
    res->status_code = (cwist_http_status_t)status;
    cwist_http_header_add(&res->headers, "Content-Type", "application/json");
    cwist_http_header_add(&res->headers, "Cache-Control", "private, no-store");
    cwist_sstring_assign(res->body, text);
    cJSON_free(text);
}

void handler_account_export_post(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0;
    char role[32] = {0};
    if (!auth_require_login(req, res, &uid, role, sizeof(role))) return;
    char id[PRIVACY_EXPORT_ID_LEN + 1];
    if (!privacy_export_start(uid, id)) {
        privacy_json(res, CWIST_HTTP_SERVICE_UNAVAILABLE,
                     cJSON_CreateString("Export could not be queued"));
        return;
    }
    char location[192];
    snprintf(location, sizeof(location), "/account/settings?export=%s", id);
    redirect(res, location);
}

void handler_account_export_status(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0;
    char role[32] = {0};
    if (!auth_require_login(req, res, &uid, role, sizeof(role))) return;
    const char *id = cwist_query_map_get(req->path_params, "id");
    cJSON *status = privacy_export_status(uid, id);
    if (!status) {
        res->status_code = CWIST_HTTP_NOT_FOUND;
        cwist_http_header_add(&res->headers, "Cache-Control", "private, no-store");
        cwist_sstring_assign(res->body, "Export not found");
        return;
    }
    privacy_json(res, CWIST_HTTP_OK, status);
}

void handler_account_export_download(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0;
    char role[32] = {0};
    if (!auth_require_login(req, res, &uid, role, sizeof(role))) return;
    const char *id = cwist_query_map_get(req->path_params, "id");
    cJSON *status = privacy_export_status(uid, id);
    if (!status) {
        res->status_code = CWIST_HTTP_NOT_FOUND;
        cwist_sstring_assign(res->body, "Export not found");
        return;
    }
    const char *state = cJSON_GetStringValue(cJSON_GetObjectItem(status, "state"));
    bool ready = state && strcmp(state, "ready") == 0;
    cJSON_Delete(status);
    char path[PATH_MAX];
    if (!ready) {
        res->status_code = CWIST_HTTP_CONFLICT;
        cwist_sstring_assign(res->body, "Export is not ready");
        return;
    }
    if (!privacy_export_download_path(uid, id, path, sizeof(path)) ||
        !send_cached_file_response(req, res, path, "application/zstd", "private, no-store", NULL)) {
        res->status_code = CWIST_HTTP_NOT_FOUND;
        cwist_sstring_assign(res->body, "Export not found");
        return;
    }
    cwist_http_header_add(&res->headers, "Content-Disposition",
                          "attachment; filename=flyboard-personal-data.tar.zst");
}

#define _POSIX_C_SOURCE 200809L
#include "handlers_internal.h"

void handler_admin_users(cwist_http_request *req, cwist_http_response *res) {
    if (!auth_require_admin(req, res)) return;
    int uid = 0; char role[32] = {0};
    auth_is_logged_in(req, &uid, role, sizeof(role));
    cJSON *users = db_user_list(req->db);
    char *pp = get_profile_pic(req->db, uid, role);
    cwist_sstring *page = render_user_admin(users, is_dark(req), pp);
    if (users) cJSON_Delete(users);
    send_html_res(res, page);
    free(pp);
}

void handler_admin_user_role(cwist_http_request *req, cwist_http_response *res) {
    if (!auth_require_admin(req, res)) return;
    form_kv_t *kv = parse_urlencoded(req->body->data);
    const char *id_str = form_kv_get(kv, "id");
    const char *role = form_kv_get(kv, "role");
    if (id_str && role) db_user_update_role(req->db, atoi(id_str), role);
    form_kv_free(kv);
    redirect(res, "/admin/users");
}

/* ---- API ---- */

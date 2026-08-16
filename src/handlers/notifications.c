#define _POSIX_C_SOURCE 200809L
#include "handlers_internal.h"

void handler_notifications_get(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0;
    char role[32] = {0};
    if (!auth_require_login(req, res, &uid, role, sizeof(role))) return;
    cJSON *notifs = db_notification_list(req->db, uid, 100);
    char *pp = get_profile_pic(req->db, uid, role);
    send_html_res(res, render_notifications_page(notifs, is_dark(req), role, pp, is_mobile_request(req)));
    free(pp);
    if (notifs) cJSON_Delete(notifs);
    db_notification_mark_all_read(req->db, uid);
}

#define _POSIX_C_SOURCE 200809L
#include "render.h"
#include <cwist/core/sstring/sstring.h>
#include <stdio.h>

cwist_sstring *render_user_admin(cJSON *users, bool dark, const char *profile_pic, bool is_mobile) {
    cwist_sstring *b = cwist_sstring_create();
    cwist_sstring_assign(b, "<div class='hero'><h1>User Admin</h1></div>");
    cwist_sstring_append(b, "<div class='card admin-user-card'><div class='table-scroll'><table class='admin-user-table' style='width:100%;border-collapse:collapse'>");
    cwist_sstring_append(b, "<thead><tr><th style='text-align:left;padding:8px'>User</th><th style='text-align:left;padding:8px'>Email</th><th class='admin-role-col' style='text-align:left;padding:8px'>Role</th><th class='admin-action-col' style='text-align:left;padding:8px'>Action</th></tr></thead><tbody>");
    if (users) {
        int n = cJSON_GetArraySize(users);
        for (int i = 0; i < n; i++) {
            cJSON *u = cJSON_GetArrayItem(users, i);
            cJSON *uid = cJSON_GetObjectItem(u, "id");
            cJSON *uname = cJSON_GetObjectItem(u, "username");
            cJSON *email = cJSON_GetObjectItem(u, "email");
            cJSON *role = cJSON_GetObjectItem(u, "role");
            cwist_sstring_append(b, "<tr><td style='padding:8px'>");
            cwist_sstring_append_escaped(b, uname->valuestring);
            cwist_sstring_append(b, "</td><td style='padding:8px'>");
            cwist_sstring_append_escaped(b, email->valuestring);
            cwist_sstring_append(b, "</td><td class='admin-role-cell' style='padding:8px'>");
            cwist_sstring_append_escaped(b, role->valuestring);
            cwist_sstring_append(b, "</td><td class='admin-action-cell' style='padding:8px'>");
            cwist_sstring_append(b, "<form class='admin-role-form' action='/admin/user/role' method='post'>");
            cwist_sstring_append(b, "<input type='hidden' name='id' value='");
            char uid_buf[32];
            snprintf(uid_buf, sizeof(uid_buf), "%d", uid->valueint);
            cwist_sstring_append(b, uid_buf);
            cwist_sstring_append(b, "'>");
            cwist_sstring_append(b, "<select class='admin-role-select' name='role'><option value='user'>user</option><option value='admin'>admin</option></select>");
            cwist_sstring_append(b, "<button type='submit' class='btn' style='font-size:12px;padding:4px 10px;margin-left:6px'>Set</button></form>");
            cwist_sstring_append(b, " <a href='/account/settings?id=");
            cwist_sstring_append(b, uid_buf);
            cwist_sstring_append(b, "' class='btn btn-outline' style='font-size:12px;padding:4px 10px;text-decoration:none'>Edit</a>");
            cwist_sstring_append(b, " <form class='admin-delete-form' action='/unregister' method='post' style='display:inline'>");
            cwist_sstring_append(b, "<input type='hidden' name='id' value='");
            cwist_sstring_append(b, uid_buf);
            cwist_sstring_append(b, "'>");
            cwist_sstring_append(b, "<button type='submit' class='btn btn-outline' style='font-size:12px;padding:4px 10px' onclick='return confirm(\"Delete?\")'>Delete</button></form>");
            cwist_sstring_append(b, "</td></tr>");
        }
    }
    cwist_sstring_append(b, "</tbody></table></div></div>");
    cwist_sstring_append(b, "<div class='card' style='margin-top:20px'><h3 style='margin-top:0'>File Admin</h3>");
    cwist_sstring_append(b, "<form action='/admin/files/drop' method='post' onsubmit='return confirm(&quot;Drop ALL files? This cannot be undone.&quot;)'>");
    cwist_sstring_append(b, "<button type='submit' class='btn btn-outline' style='color:#c00;border-color:#c00'>Drop All Files</button></form></div>");
    cwist_sstring *page = render_page("User Admin", b->data, dark, "admin", profile_pic, is_mobile);
    cwist_sstring_destroy(b);
    return page;
}

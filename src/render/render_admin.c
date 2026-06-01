#define _POSIX_C_SOURCE 200809L
#include "render.h"
#include "render_internal.h"
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

cwist_sstring *render_admin_dashboard(bool dark, const char *profile_pic, bool is_mobile) {
    cwist_sstring *b = cwist_sstring_create();
    cwist_sstring_assign(b, "<div class='hero'><h1>Dashboard</h1></div>");
    cwist_sstring_append(b, "<div class='board-list stagger'>");
    cwist_sstring_append(b, "<section class='board-line fade-in'><div class='board-line-head'><h2 class='board-line-title'>Users</h2></div>");
    cwist_sstring_append(b, "<p class='board-card-desc'>Manage user accounts and roles.</p>");
    cwist_sstring_append(b, "<a href='/admin/users' class='btn'>Go to Users</a></section>");
    cwist_sstring_append(b, "<section class='board-line fade-in' style='animation-delay:0.05s'><div class='board-line-head'><h2 class='board-line-title'>Manage Boards</h2></div>");
    cwist_sstring_append(b, "<p class='board-card-desc'>Organize board hierarchy and parent-child relationships.</p>");
    cwist_sstring_append(b, "<a href='/admin/boards' class='btn'>Go to Manage Boards</a></section>");
    cwist_sstring_append(b, "<section class='board-line fade-in' style='animation-delay:0.10s'><div class='board-line-head'><h2 class='board-line-title'>File Admin</h2></div>");
    cwist_sstring_append(b, "<p class='board-card-desc'>Drop all uploaded files. This action cannot be undone.</p>");
    cwist_sstring_append(b, "<form action='/admin/files/drop' method='post' onsubmit='return confirm(&quot;Drop ALL files? This cannot be undone.&quot;)'>");
    cwist_sstring_append(b, "<button type='submit' class='btn btn-outline' style='color:#c00;border-color:#c00'>Drop All Files</button></form></section>");
    cwist_sstring_append(b, "</div>");
    cwist_sstring *page = render_page("Dashboard", b->data, dark, "admin", profile_pic, is_mobile);
    cwist_sstring_destroy(b);
    return page;
}

cwist_sstring *render_admin_boards(cJSON *boards, cJSON *tree, bool dark, const char *profile_pic, bool is_mobile) {
    cwist_sstring *b = cwist_sstring_create();
    cwist_sstring_assign(b, "<div class='hero'><h1>Manage Boards</h1></div>");
    cwist_sstring_append(b, "<div class='card' style='overflow-x:auto'><table style='width:100%;border-collapse:collapse'>");
    cwist_sstring_append(b, "<thead><tr><th style='text-align:left;padding:8px'>ID</th><th style='text-align:left;padding:8px'>Name</th><th style='text-align:left;padding:8px'>Slug</th><th style='text-align:left;padding:8px'>Parent</th><th style='text-align:left;padding:8px'>Action</th></tr></thead><tbody>");
    if (boards) {
        int n = cJSON_GetArraySize(boards);
        for (int i = 0; i < n; i++) {
            cJSON *bo = cJSON_GetArrayItem(boards, i);
            int bid = json_int(bo, "id", 0);
            cJSON *name = cJSON_GetObjectItem(bo, "name");
            cJSON *slug = cJSON_GetObjectItem(bo, "slug");
            int parent_id = 0;
            const char *parent_name = "None";
            if (tree) {
                int tn = cJSON_GetArraySize(tree);
                for (int j = 0; j < tn; j++) {
                    cJSON *node = cJSON_GetArrayItem(tree, j);
                    if (json_int(node, "board_id", 0) == bid) {
                        parent_id = json_int(node, "parent_board_id", 0);
                        break;
                    }
                }
            }
            if (parent_id > 0 && boards) {
                int pn = cJSON_GetArraySize(boards);
                for (int k = 0; k < pn; k++) {
                    cJSON *pb = cJSON_GetArrayItem(boards, k);
                    if (json_int(pb, "id", 0) == parent_id) {
                        cJSON *pname = cJSON_GetObjectItem(pb, "name");
                        if (pname && pname->valuestring) parent_name = pname->valuestring;
                        break;
                    }
                }
            }
            char bid_buf[32];
            snprintf(bid_buf, sizeof(bid_buf), "%d", bid);
            cwist_sstring_append(b, "<tr><td style='padding:8px'>");
            cwist_sstring_append(b, bid_buf);
            cwist_sstring_append(b, "</td><td style='padding:8px'>");
            cwist_sstring_append_escaped(b, name && name->valuestring ? name->valuestring : "");
            cwist_sstring_append(b, "</td><td style='padding:8px'>");
            cwist_sstring_append_escaped(b, slug && slug->valuestring ? slug->valuestring : "");
            cwist_sstring_append(b, "</td><td style='padding:8px'>");
            cwist_sstring_append_escaped(b, parent_name);
            cwist_sstring_append(b, "</td><td style='padding:8px'>");
            cwist_sstring_append(b, "<form action='/admin/board/tree' method='post' style='display:flex;gap:6px;align-items:center'>");
            cwist_sstring_append(b, "<input type='hidden' name='board_id' value='");
            cwist_sstring_append(b, bid_buf);
            cwist_sstring_append(b, "'><select name='parent_id' style='font-size:13px'>");
            cwist_sstring_append(b, "<option value='0' ");
            if (parent_id == 0) cwist_sstring_append(b, "selected");
            cwist_sstring_append(b, ">(None)</option>");
            if (boards) {
                int bn = cJSON_GetArraySize(boards);
                for (int j = 0; j < bn; j++) {
                    cJSON *pbo = cJSON_GetArrayItem(boards, j);
                    int pbid = json_int(pbo, "id", 0);
                    if (pbid <= 0 || pbid == bid) continue;
                    cJSON *pname = cJSON_GetObjectItem(pbo, "name");
                    char pbid_buf[32];
                    snprintf(pbid_buf, sizeof(pbid_buf), "%d", pbid);
                    cwist_sstring_append(b, "<option value='");
                    cwist_sstring_append(b, pbid_buf);
                    cwist_sstring_append(b, "' ");
                    if (pbid == parent_id) cwist_sstring_append(b, "selected");
                    cwist_sstring_append(b, ">");
                    if (pname && pname->valuestring) cwist_sstring_append_escaped(b, pname->valuestring);
                    cwist_sstring_append(b, "</option>");
                }
            }
            cwist_sstring_append(b, "</select><button type='submit' class='btn' style='font-size:12px;padding:4px 10px'>Save</button></form>");
            cwist_sstring_append(b, "</td></tr>");
        }
    }
    cwist_sstring_append(b, "</tbody></table></div>");
    cwist_sstring *page = render_page("Manage Boards", b->data, dark, "admin", profile_pic, is_mobile);
    cwist_sstring_destroy(b);
    return page;
}

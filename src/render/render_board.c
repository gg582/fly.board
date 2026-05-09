#define _POSIX_C_SOURCE 200809L
#include "render.h"
#include "render_internal.h"
#include <cwist/core/sstring/sstring.h>
#include <string.h>
#include <stdio.h>

cwist_sstring *render_board_list(cJSON *boards, bool dark, const char *user_role, const char *profile_pic) {
    cwist_sstring *b = cwist_sstring_create();
    cwist_sstring_assign(b, "<div class='hero'><img class='hero-logo' src='/img/logo.png' alt='Logo'><h1>");
    cwist_sstring_append_escaped(b, g_config.title);
    cwist_sstring_append(b, "</h1><p>");
    cwist_sstring_append_escaped(b, g_config.subtitle);
    cwist_sstring_append(b, "</p></div>");
    if (user_role && strcmp(user_role, "admin") == 0) {
        cwist_sstring_append(b, "<a href='/board/new' class='btn' style='margin-bottom:18px'>New Board</a>");
    }
    if (boards) {
        int n = cJSON_GetArraySize(boards);
        for (int i = 0; i < n; i++) {
            cJSON *bo = cJSON_GetArrayItem(boards, i);
            cJSON *slug = cJSON_GetObjectItem(bo, "slug");
            cJSON *name = cJSON_GetObjectItem(bo, "name");
            cJSON *desc = cJSON_GetObjectItem(bo, "description");
            cwist_sstring_append(b, "<div class='board-section card' style='margin-bottom:18px'>");
            cwist_sstring_append(b, "<div style='display:flex;justify-content:space-between;align-items:center;flex-wrap:wrap;gap:8px'>");
            cwist_sstring_append(b, "<a href='/board/");
            cwist_sstring_append(b, slug->valuestring);
            cwist_sstring_append(b, "'><h3 style='margin:0'>");
            cwist_sstring_append_escaped(b, name->valuestring);
            cwist_sstring_append(b, "</h3></a>");
            if (user_role && strcmp(user_role, "admin") == 0) {
                cJSON *bid = cJSON_GetObjectItem(bo, "id");
                char bid_buf[32];
                snprintf(bid_buf, sizeof(bid_buf), "%d", bid->valueint);
                cwist_sstring_append(b, "<div style='display:flex;gap:8px'>");
                cwist_sstring_append(b, "<a href='/board/");
                cwist_sstring_append(b, bid_buf);
                cwist_sstring_append(b, "/delete' class='btn btn-outline' style='font-size:12px;padding:4px 10px' onclick='return confirm(\"Delete this board? This action cannot be undone.\")'>Delete</a>");
                cwist_sstring_append(b, "</div>");
            }
            cwist_sstring_append(b, "</div>");
            cwist_sstring_append(b, "<p style='color:var(--muted);font-size:14px;margin-top:8px'>");
            cwist_sstring_append_escaped(b, desc && desc->valuestring[0] ? desc->valuestring : "");
            cwist_sstring_append(b, "</p>");

            cJSON *posts = cJSON_GetObjectItem(bo, "posts");
            if (posts && cJSON_GetArraySize(posts) > 0) {
                cwist_sstring_append(b, "<ul style='margin:12px 0 0;padding-left:18px'>");
                int pn = cJSON_GetArraySize(posts);
                for (int j = 0; j < pn; j++) {
                    cJSON *p = cJSON_GetArrayItem(posts, j);
                    cJSON *pslug = cJSON_GetObjectItem(p, "slug");
                    cJSON *ptitle = cJSON_GetObjectItem(p, "title");
                    cJSON *pdate = cJSON_GetObjectItem(p, "created_at");
                    cwist_sstring_append(b, "<li style='margin-bottom:6px'>");
                    cwist_sstring_append(b, "<a href='/post/");
                    cwist_sstring_append(b, pslug->valuestring);
                    cwist_sstring_append(b, "'>");
                    cwist_sstring_append_escaped(b, ptitle->valuestring);
                    cwist_sstring_append(b, "</a>");
                    if (pdate && pdate->valuestring) {
                        cwist_sstring_append(b, " <span style='color:var(--muted);font-size:12px'>");
                        cwist_sstring_append_escaped(b, pdate->valuestring);
                        cwist_sstring_append(b, "</span>");
                    }
                    cwist_sstring_append(b, "</li>");
                }
                cwist_sstring_append(b, "</ul>");
            } else {
                cwist_sstring_append(b, "<p style='color:var(--muted);font-size:13px;margin-top:10px'>No posts yet.</p>");
            }
            cwist_sstring_append(b, "</div>");
        }
    } else {
        cwist_sstring_append(b, "<p style='color:var(--muted)'>No boards available.</p>");
    }
    cwist_sstring *page = render_page("Boards", b->data, dark, user_role, profile_pic);
    cwist_sstring_destroy(b);
    return page;
}

cwist_sstring *render_board_form(cJSON *board, bool dark, const char *error, const char *profile_pic) {
    cwist_sstring *fields = cwist_sstring_create();
    cwist_sstring_assign(fields, "<label>Name</label><input name='name' value='");
    if (board) {
        cJSON *n = cJSON_GetObjectItem(board, "name");
        cwist_sstring_append_escaped(fields, n->valuestring);
    }
    cwist_sstring_append(fields, "' required>");
    cwist_sstring_append(fields, "<label>Slug</label><input name='slug' value='");
    if (board) {
        cJSON *s = cJSON_GetObjectItem(board, "slug");
        cwist_sstring_append_escaped(fields, s->valuestring);
    }
    cwist_sstring_append(fields, "' required>");
    cwist_sstring_append(fields, "<label>Description</label><input name='description' value='");
    if (board) {
        cJSON *d = cJSON_GetObjectItem(board, "description");
        cwist_sstring_append_escaped(fields, d && d->valuestring[0] ? d->valuestring : "");
    }
    cwist_sstring_append(fields, "'>");
    cwist_sstring_append(fields, "<label><input type='checkbox' name='admin_only' value='1' ");
    if (board) {
        cJSON *ao = cJSON_GetObjectItem(board, "admin_only");
        if (ao && ao->valueint) cwist_sstring_append(fields, "checked");
    }
    cwist_sstring_append(fields, "> Admin-only board</label>");
    if (board) {
        cJSON *bid = cJSON_GetObjectItem(board, "id");
        char bid_buf[32];
        snprintf(bid_buf, sizeof(bid_buf), "%d", bid->valueint);
        cwist_sstring_append(fields, "<input type='hidden' name='id' value='");
        cwist_sstring_append(fields, bid_buf);
        cwist_sstring_append(fields, "'>");
    }
    cwist_sstring *body = build_form(board ? "Edit Board" : "New Board", board ? "/board/edit" : "/board/new", "post", fields->data, "Save", error, dark);
    cwist_sstring *page = render_page(board ? "Edit Board" : "New Board", body->data, dark, "admin", profile_pic);
    cwist_sstring_destroy(fields);
    cwist_sstring_destroy(body);
    return page;
}

cwist_sstring *render_board_perms(cJSON *board, cJSON *perms, cJSON *users, bool dark, const char *msg, const char *profile_pic) {
    cwist_sstring *b = cwist_sstring_create();
    cJSON *bname = cJSON_GetObjectItem(board, "name");
    cwist_sstring_assign(b, "<div class='hero'><h1>Permissions: ");
    cwist_sstring_append_escaped(b, bname ? bname->valuestring : "Unknown");
    cwist_sstring_append(b, "</h1></div>");

    cwist_sstring_append(b, "<div class='card' style='max-width:520px;margin:0 auto'>");

    if (msg && msg[0]) {
        cwist_sstring_append(b, "<div class='alert'>");
        if (strcmp(msg, "granted") == 0) cwist_sstring_append(b, "Permission granted.");
        else if (strcmp(msg, "revoked") == 0) cwist_sstring_append(b, "Permission revoked.");
        else if (strcmp(msg, "exists") == 0) cwist_sstring_append(b, "User already has permission.");
        else cwist_sstring_append(b, "Operation failed.");
        cwist_sstring_append(b, "</div>");
    }

    cwist_sstring_append(b, "<form action='/board/perms' method='post'>");
    cJSON *bid = cJSON_GetObjectItem(board, "id");
    char bid_buf[32];
    snprintf(bid_buf, sizeof(bid_buf), "%d", bid ? bid->valueint : 0);
    cwist_sstring_append(b, "<input type='hidden' name='board_id' value='");
    cwist_sstring_append(b, bid_buf);
    cwist_sstring_append(b, "'>");
    cwist_sstring_append(b, "<label>Grant user</label><select name='user_id'>");
    if (users) {
        int n = cJSON_GetArraySize(users);
        for (int i = 0; i < n; i++) {
            cJSON *u = cJSON_GetArrayItem(users, i);
            if (!u) continue;
            cJSON *uid = cJSON_GetObjectItem(u, "id");
            cJSON *uname = cJSON_GetObjectItem(u, "username");
            if (!uid || !uname) continue;

            bool has_perm = false;
            if (perms) {
                int pn = cJSON_GetArraySize(perms);
                for (int j = 0; j < pn; j++) {
                    cJSON *pu = cJSON_GetArrayItem(perms, j);
                    cJSON *puid = cJSON_GetObjectItem(pu, "id");
                    if (puid && puid->valueint == uid->valueint) {
                        has_perm = true;
                        break;
                    }
                }
            }
            if (has_perm) continue;

            char uid_buf[32];
            snprintf(uid_buf, sizeof(uid_buf), "%d", uid->valueint);
            cwist_sstring_append(b, "<option value='");
            cwist_sstring_append(b, uid_buf);
            cwist_sstring_append(b, "'>");
            cwist_sstring_append_escaped(b, uname->valuestring);
            cwist_sstring_append(b, "</option>");
        }
    }
    cwist_sstring_append(b, "</select>");
    cwist_sstring_append(b, "<button type='submit' class='btn' style='margin-top:8px'>Grant</button>");
    cwist_sstring_append(b, "</form>");

    cwist_sstring_append(b, "<h3 style='margin-top:24px'>Allowed users</h3>");
    if (perms && cJSON_GetArraySize(perms) > 0) {
        cwist_sstring_append(b, "<ul>");
        int n = cJSON_GetArraySize(perms);
        for (int i = 0; i < n; i++) {
            cJSON *u = cJSON_GetArrayItem(perms, i);
            if (!u) continue;
            cJSON *uid = cJSON_GetObjectItem(u, "id");
            cJSON *uname = cJSON_GetObjectItem(u, "username");
            cwist_sstring_append(b, "<li>");
            cwist_sstring_append_escaped(b, uname ? uname->valuestring : "?");
            cwist_sstring_append(b, " <form style='display:inline' action='/board/perms/revoke' method='post'>");
            cwist_sstring_append(b, "<input type='hidden' name='board_id' value='");
            cwist_sstring_append(b, bid_buf);
            cwist_sstring_append(b, "'>");
            cwist_sstring_append(b, "<input type='hidden' name='user_id' value='");
            char uid_buf[32];
            snprintf(uid_buf, sizeof(uid_buf), "%d", uid ? uid->valueint : 0);
            cwist_sstring_append(b, uid_buf);
            cwist_sstring_append(b, "'>");
            cwist_sstring_append(b, "<button type='submit' class='btn btn-outline' style='font-size:12px;padding:4px 10px'>Revoke</button></form></li>");
        }
        cwist_sstring_append(b, "</ul>");
    } else {
        cwist_sstring_append(b, "<p style='color:var(--muted);font-size:14px'>No users have been granted access yet.</p>");
    }
    cwist_sstring_append(b, "</div>");

    cwist_sstring *page = render_page("Board Permissions", b->data, dark, "admin", profile_pic);
    cwist_sstring_destroy(b);
    return page;
}

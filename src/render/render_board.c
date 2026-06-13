#define _POSIX_C_SOURCE 200809L
#include "render.h"
#include "render_internal.h"
#include "config/config.h"
#include "utils/utils.h"
#include "db/sql_escape.h"
#include "cwist/image_contrast.h"
#include <cwist/core/sstring/sstring.h>
#include <cwist/core/mem/alloc.h>
#include <string.h>
#include <stdio.h>

cwist_sstring *render_board_list(cJSON *boards, bool dark, const char *user_role, const char *profile_pic, bool is_mobile) {
    cwist_sstring *b = cwist_sstring_create();
    int has_boards_bg = g_config.boards_img[0];
    char shell_style[768] = {0};
    char text_style[256] = {0};
    char logo_filter[128] = {0};
    char overlay_style[256] = {0};
    char img_url[512] = {0};
    if (has_boards_bg) {
        char img_path[512];
        snprintf(img_path, sizeof(img_path), "public/img/%s", g_config.boards_img);
        snprintf(img_url, sizeof(img_url), "/assets/img/%s", g_config.boards_img);
        get_image_text_style(img_path, img_url, shell_style, sizeof(shell_style),
                             text_style, sizeof(text_style),
                             logo_filter, sizeof(logo_filter),
                             overlay_style, sizeof(overlay_style));
        cwist_sstring_append(b, "<div style=\"");
        cwist_sstring_append(b, shell_style);
        cwist_sstring_append(b, ";");
        cwist_sstring_append(b, text_style);
        cwist_sstring_append(b, "\">");
        cwist_sstring_append(b, "<img class='hero-bg' data-tasfa-skip='1' fetchpriority='high' src='");
        cwist_sstring_append(b, img_url);
        cwist_sstring_append(b, "' alt='' style='position:absolute;inset:0;width:100%;height:100%;object-fit:cover;object-position:center;z-index:0'>");
        if (overlay_style[0]) {
            cwist_sstring_append(b, "<div style=\"position:absolute;inset:0;z-index:1;");
            cwist_sstring_append(b, overlay_style);
            cwist_sstring_append(b, "\"></div>");
        }
    }
    cwist_sstring_append(b, "<div class='hero' ");
    if (has_boards_bg) cwist_sstring_append(b, "style='position:relative;z-index:2;background:none;' ");
    cwist_sstring_append(b, "><img class='hero-logo' data-tasfa-skip='1' src='/assets/img/");
    if (g_config.blog_logo[0]) cwist_sstring_append_escaped(b, g_config.blog_logo);
    else cwist_sstring_append(b, "logo.png");
    cwist_sstring_append(b, "' alt='Logo'");
    if (has_boards_bg) {
        cwist_sstring_append(b, " style='filter:");
        cwist_sstring_append(b, logo_filter);
        cwist_sstring_append(b, "'");
    }
    cwist_sstring_append(b, " fetchpriority='high'><h1>");
    cwist_sstring_append_escaped(b, g_config.title);
    cwist_sstring_append(b, "</h1><p");
    if (has_boards_bg) cwist_sstring_append(b, " style='opacity:0.85'");
    cwist_sstring_append(b, ">");
    cwist_sstring_append_escaped(b, g_config.subtitle);
    cwist_sstring_append(b, "</p></div>");
    if (has_boards_bg) {
        cwist_sstring_append(b, "</div>");
    }
    if (user_role && user_role[0]) {
        cwist_sstring_append(b, "<div style='text-align:center;margin-bottom:24px'><a href='/board/new' class='btn'>New Board</a></div>");
    }
    if (boards) {
        int n = cJSON_GetArraySize(boards);
        cwist_sstring_append(b, "<div class='board-list stagger'>");
        for (int i = 0; i < n; i++) {
            cJSON *bo = cJSON_GetArrayItem(boards, i);
            cJSON *slug = cJSON_GetObjectItem(bo, "slug");
            cJSON *name = cJSON_GetObjectItem(bo, "name");
            cJSON *desc = cJSON_GetObjectItem(bo, "description");
            char delay_buf[32];
            snprintf(delay_buf, sizeof(delay_buf), "%.2fs", i * 0.05);
            cJSON *score = cJSON_GetObjectItem(bo, "score");
            bool is_hot = (score && score->valuedouble > 0.0 && i < 3);
            int depth = json_int(bo, "depth", 0);
            cwist_sstring_append(b, "<section class='board-line ");
            if (is_hot) cwist_sstring_append(b, "board-line-hot ");
            cwist_sstring_append(b, "fade-in' style='animation-delay:");
            cwist_sstring_append(b, delay_buf);
            if (depth > 0) {
                char depth_style[64];
                snprintf(depth_style, sizeof(depth_style), ";margin-left:%dpx", depth * 28);
                cwist_sstring_append(b, depth_style);
            }
            cwist_sstring_append(b, "'>");
            cwist_sstring_append(b, "<div class='board-line-head'>");
            cwist_sstring_append(b, "<a href='/board/");
            cwist_sstring_append(b, slug->valuestring);
            cwist_sstring_append(b, "' style='text-decoration:none'><h2 class='board-line-title'>");
            cwist_sstring_append_escaped(b, name->valuestring);
            cwist_sstring_append(b, "</h2></a>");
            if (user_role && strcmp(user_role, "admin") == 0) {
                char bid_buf[32];
                snprintf(bid_buf, sizeof(bid_buf), "%d", json_int(bo, "id", 0));
                cwist_sstring_append(b, "<a href='/board/");
                cwist_sstring_append(b, bid_buf);
                cwist_sstring_append(b, "/delete' class='btn btn-outline' style='font-size:12px;padding:4px 10px' data-confirm='Delete this board? This action cannot be undone.'>Delete</a>");
            }
            cwist_sstring_append(b, "</div>");
            cwist_sstring_append(b, "<p class='board-card-desc'>");
            cwist_sstring_append_escaped(b, desc && desc->valuestring && desc->valuestring[0] ? desc->valuestring : "");
            cwist_sstring_append(b, "</p>");
            cwist_sstring_append(b, "<div class='board-point-row'>");
            cwist_sstring_append(b, "<span class='board-point board-point-primary'><span>POINT</span>");
            char score_buf[32];
            snprintf(score_buf, sizeof(score_buf), "%.0f", score ? score->valuedouble : 0.0);
            cwist_sstring_append(b, score_buf);
            cwist_sstring_append(b, "</span>");
            cwist_sstring_append(b, "<span class='board-point'><span>POSTS</span>");
            char posts_buf[32];
            snprintf(posts_buf, sizeof(posts_buf), "%d", json_int(bo, "post_count", 0));
            cwist_sstring_append(b, posts_buf);
            cwist_sstring_append(b, "</span>");
            if (json_int(bo, "admin_only", 0)) {
                cwist_sstring_append(b, "<span class='board-point'><span>ACCESS</span>ADMIN</span>");
            } else {
                cwist_sstring_append(b, "<span class='board-point'><span>ACCESS</span>PUBLIC</span>");
            }
            cwist_sstring_append(b, "</div>");

            cJSON *posts = cJSON_GetObjectItem(bo, "posts");
            if (posts && cJSON_GetArraySize(posts) > 0) {
                cwist_sstring_append(b, "<ul class='board-post-list stagger'>");
                int pn = cJSON_GetArraySize(posts);
                for (int j = 0; j < pn; j++) {
                    cJSON *p = cJSON_GetArrayItem(posts, j);
                    cJSON *pslug = cJSON_GetObjectItem(p, "slug");
                    cJSON *ptitle = cJSON_GetObjectItem(p, "title");
                    cJSON *pdate = cJSON_GetObjectItem(p, "created_at");
                    cJSON *psummary = cJSON_GetObjectItem(p, "summary");
                    cJSON *pcontent = cJSON_GetObjectItem(p, "content");
                    cJSON *pauthor = cJSON_GetObjectItem(p, "author_name");
                    cwist_sstring_append(b, "<li class='board-post-item'>");
                    cwist_sstring_append(b, "<a class='board-post-title' href='/post/");
                    cwist_sstring_append(b, pslug->valuestring);
                    cwist_sstring_append(b, "'>");
                    cwist_sstring_append_escaped(b, ptitle->valuestring);
                    cwist_sstring_append(b, "</a>");

                    const char *summary_text = NULL;
                    if (psummary && psummary->valuestring && psummary->valuestring[0]) {
                        summary_text = psummary->valuestring;
                    } else if (pcontent && pcontent->valuestring && pcontent->valuestring[0]) {
                        summary_text = pcontent->valuestring;
                    }
                    if (summary_text) {
                        cwist_sstring_append(b, "<p class='board-post-summary'>");
                        size_t sum_len = strlen(summary_text);
                        if (sum_len > 120) {
                            size_t trunc = utf8_truncate_len(summary_text, 120);
                            char *tmp = (char *)cwist_alloc(trunc + 1);
                            memcpy(tmp, summary_text, trunc);
                            tmp[trunc] = '\0';
                            char *tmp_escaped = sql_escape(tmp);
                            cwist_sstring_append(b, tmp_escaped);
                            cwist_free(tmp_escaped);
                            cwist_free(tmp);
                            cwist_sstring_append(b, "…");
                        } else {
                            char *tmp_escaped = sql_escape(summary_text);
                            cwist_sstring_append(b, tmp_escaped);
                            cwist_free(tmp_escaped);
                        }
                        cwist_sstring_append(b, "</p>");
                    }

                    cwist_sstring_append(b, "<div class='board-post-meta'>");
                    if (pauthor && pauthor->valuestring && pauthor->valuestring[0]) {
                        cwist_sstring_append(b, "<span class='post-badge'>");
                        cwist_sstring_append_escaped(b, pauthor->valuestring);
                        cwist_sstring_append(b, "</span>");
                    }
                    if (pdate && pdate->valuestring) {
                        cwist_sstring_append(b, "<span class='post-badge'>");
                        cwist_sstring_append_escaped(b, pdate->valuestring);
                        cwist_sstring_append(b, "</span>");
                    }
                    cwist_sstring_append(b, "</div>");
                    cwist_sstring_append(b, "</li>");
                }
                cwist_sstring_append(b, "</ul>");
            } else if (depth > 0) {
                cwist_sstring_append(b, "<p class='board-card-empty'>No posts yet.</p>");
            }
            cwist_sstring_append(b, "</section>");
        }
        cwist_sstring_append(b, "</div>");
    } else {
        cwist_sstring_append(b, "<p style='color:var(--muted)'>No boards available.</p>");
    }
    cwist_sstring *page = render_page("Boards", b->data, dark, user_role, profile_pic, is_mobile);
    cwist_sstring_destroy(b);
    return page;
}

cwist_sstring *render_board_form(cJSON *board, cJSON *all_boards, bool dark, const char *error, const char *profile_pic, bool is_mobile, const char *user_role) {
    cwist_sstring *fields = cwist_sstring_create();
    cwist_sstring_assign(fields, "<label>Name</label><input name='name' value='");
    if (board) {
        cJSON *n = cJSON_GetObjectItem(board, "name");
        cwist_sstring_append_escaped(fields, n->valuestring);
    }
    cwist_sstring_append(fields, "' required>");
    cwist_sstring_append(fields, "<label>Slug</label><input name='slug' pattern='[A-Za-z0-9_-]+' title='Only letters, numbers, hyphens and underscores are allowed' value='");
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
    cwist_sstring_append(fields, "<label>Parent Board</label><select name='parent_id'><option value='0'>(None - top level)</option>");
    int current_parent = board ? json_int(board, "parent_id", 0) : 0;
    if (all_boards) {
        int self_id = board ? json_int(board, "id", 0) : 0;
        int n = cJSON_GetArraySize(all_boards);
        for (int i = 0; i < n; i++) {
            cJSON *bo = cJSON_GetArrayItem(all_boards, i);
            int bid = json_int(bo, "id", 0);
            if (bid <= 0 || bid == self_id) continue;
            cJSON *bname = cJSON_GetObjectItem(bo, "name");
            char bid_buf[32];
            snprintf(bid_buf, sizeof(bid_buf), "%d", bid);
            cwist_sstring_append(fields, "<option value='");
            cwist_sstring_append(fields, bid_buf);
            cwist_sstring_append(fields, "'");
            if (bid == current_parent) cwist_sstring_append(fields, " selected");
            cwist_sstring_append(fields, ">");
            if (bname && bname->valuestring) cwist_sstring_append_escaped(fields, bname->valuestring);
            cwist_sstring_append(fields, "</option>");
        }
    }
    cwist_sstring_append(fields, "</select>");
    if (user_role && strcmp(user_role, "admin") == 0) {
        cwist_sstring_append(fields, "<label><input type='checkbox' name='admin_only' value='1' ");
        if (board) {
            if (json_int(board, "admin_only", 0)) cwist_sstring_append(fields, "checked");
        }
        cwist_sstring_append(fields, "> Admin-only board</label>");
    }
    if (board) {
        char bid_buf[32];
        snprintf(bid_buf, sizeof(bid_buf), "%d", json_int(board, "id", 0));
        cwist_sstring_append(fields, "<input type='hidden' name='id' value='");
        cwist_sstring_append(fields, bid_buf);
        cwist_sstring_append(fields, "'>");
    }
    cwist_sstring *body = build_form(board ? "Edit Board" : "New Board", board ? "/board/edit" : "/board/new", "post", fields->data, "Save", error, dark);
    cwist_sstring *page = render_page(board ? "Edit Board" : "New Board", body->data, dark, user_role && user_role[0] ? user_role : "user", profile_pic, is_mobile);
    cwist_sstring_destroy(fields);
    cwist_sstring_destroy(body);
    return page;
}

cwist_sstring *render_board_perms(cJSON *board, cJSON *perms, cJSON *users, bool dark, const char *msg, const char *profile_pic, bool is_mobile) {
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
    char bid_buf[32];
    snprintf(bid_buf, sizeof(bid_buf), "%d", json_int(board, "id", 0));
    cwist_sstring_append(b, "<input type='hidden' name='board_id' value='");
    cwist_sstring_append(b, bid_buf);
    cwist_sstring_append(b, "'>");
    cwist_sstring_append(b, "<label>Grant user</label><select name='user_id'>");
    if (users) {
        int n = cJSON_GetArraySize(users);
        for (int i = 0; i < n; i++) {
            cJSON *u = cJSON_GetArrayItem(users, i);
            if (!u) continue;
            cJSON *uname = cJSON_GetObjectItem(u, "username");
            if (!uname) continue;

            bool has_perm = false;
            if (perms) {
                int pn = cJSON_GetArraySize(perms);
                for (int j = 0; j < pn; j++) {
                    cJSON *pu = cJSON_GetArrayItem(perms, j);
                    if (json_int(pu, "id", 0) == json_int(u, "id", 0)) {
                        has_perm = true;
                        break;
                    }
                }
            }
            if (has_perm) continue;

            char uid_buf[32];
            snprintf(uid_buf, sizeof(uid_buf), "%d", json_int(u, "id", 0));
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
            cJSON *uname = cJSON_GetObjectItem(u, "username");
            cwist_sstring_append(b, "<li>");
            cwist_sstring_append_escaped(b, uname ? uname->valuestring : "?");
            cwist_sstring_append(b, " <form style='display:inline' action='/board/perms/revoke' method='post'>");
            cwist_sstring_append(b, "<input type='hidden' name='board_id' value='");
            cwist_sstring_append(b, bid_buf);
            cwist_sstring_append(b, "'>");
            cwist_sstring_append(b, "<input type='hidden' name='user_id' value='");
            char uid_buf[32];
            snprintf(uid_buf, sizeof(uid_buf), "%d", json_int(u, "id", 0));
            cwist_sstring_append(b, uid_buf);
            cwist_sstring_append(b, "'>");
            cwist_sstring_append(b, "<button type='submit' class='btn btn-outline' style='font-size:12px;padding:4px 10px'>Revoke</button>");
            cwist_sstring_append(b, "</form></li>");
        }
        cwist_sstring_append(b, "</ul>");
    } else {
        cwist_sstring_append(b, "<p style='color:var(--muted)'>No allowed users.</p>");
    }
    cwist_sstring_append(b, "</div>");
    cwist_sstring *page = render_page("Board Permissions", b->data, dark, "admin", profile_pic, is_mobile);
    cwist_sstring_destroy(b);
    return page;
}

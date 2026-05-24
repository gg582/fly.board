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
#include <ctype.h>

void render_comment_node(cwist_sstring *b, cJSON *comment, cJSON *all_comments, int depth, int current_user_id, const char *user_role, int target_id) {
    int cid = json_int(comment, "id", 0);
    int comment_user_id = json_int(comment, "user_id", 0);
    cJSON *content = cJSON_GetObjectItem(comment, "content");
    cJSON *username = cJSON_GetObjectItem(comment, "username");
    cJSON *date = cJSON_GetObjectItem(comment, "created_at");
    cJSON *deleted = cJSON_GetObjectItem(comment, "deleted");
    int margin = depth * 20;

    const char *uname = username && username->valuestring ? username->valuestring : "unknown";

    cwist_sstring_append(b, "<div class='comment-node' style='margin-left:");
    char mbuf[32]; snprintf(mbuf, sizeof(mbuf), "%d", margin);
    cwist_sstring_append(b, mbuf);
    cwist_sstring_append(b, "px;margin-top:12px'>");

    /* Header: avatar + meta */
    cwist_sstring_append(b, "<div class='comment-header'>");
    cwist_sstring_append(b, "<div class='comment-avatar'>");
    if (uname[0]) {
        char initial[8] = {0};
        initial[0] = uname[0];
        cwist_sstring_append_escaped(b, initial);
    }
    cwist_sstring_append(b, "</div>");
    cwist_sstring_append(b, "<div class='comment-meta'>");
    cwist_sstring_append_escaped(b, uname);
    cwist_sstring_append(b, " <span class='comment-date'>&middot; ");
    cwist_sstring_append_escaped(b, date && date->valuestring ? date->valuestring : "");
    cwist_sstring_append(b, "</span></div></div>");

    /* Body */
    cwist_sstring_append(b, "<div class='comment-body'>");
    if (deleted && deleted->valueint) {
        cwist_sstring_append(b, "<p style='color:var(--muted);font-style:italic;margin:0'>Deleted comment</p>");
    } else {
        cwist_sstring_append(b, "<p style='margin:0 0 8px'>");
        cwist_sstring_append_escaped(b, content && content->valuestring ? content->valuestring : "");
        cwist_sstring_append(b, "</p>");
    }
    cwist_sstring_append(b, "</div>");

    bool can_edit_comment = (current_user_id > 0 && comment_user_id == current_user_id) || (user_role && strcmp(user_role, "admin") == 0);
    char cid_buf[32]; snprintf(cid_buf, sizeof(cid_buf), "%d", cid);
    char post_id_buf[32]; snprintf(post_id_buf, sizeof(post_id_buf), "%d", target_id);

    if (can_edit_comment && !(deleted && deleted->valueint)) {
        cwist_sstring_append(b, "<div style='margin-top:6px;display:flex;gap:8px;flex-wrap:wrap'>");
        cwist_sstring_append(b, "<form action='/comment/edit' method='post' style='display:flex;gap:8px;align-items:center;flex-wrap:wrap'>");
        cwist_sstring_append(b, "<input type='hidden' name='id' value='");
        cwist_sstring_append(b, cid_buf);
        cwist_sstring_append(b, "'>");
        cwist_sstring_append(b, "<textarea name='content' rows='2' required style='width:240px;font-family:inherit;font-size:14px'>");
        cwist_sstring_append_escaped(b, content && content->valuestring ? content->valuestring : "");
        cwist_sstring_append(b, "</textarea>");
        cwist_sstring_append(b, "<button type='submit' class='btn' style='font-size:12px;padding:4px 10px'>Update</button>");
        cwist_sstring_append(b, "</form>");
        cwist_sstring_append(b, "<a href='/comment/");
        cwist_sstring_append(b, cid_buf);
        cwist_sstring_append(b, "/delete' class='btn btn-outline' style='font-size:12px;padding:4px 10px' onclick='return confirm(\"Delete?\")'>Delete</a>");
        cwist_sstring_append(b, "</div>");
    }

    if (user_role && user_role[0] && !(deleted && deleted->valueint)) {
        cwist_sstring_append(b, "<div style='margin-top:8px'>");
        cwist_sstring_append(b, "<button type='button' class='btn btn-outline' style='font-size:12px;padding:4px 10px' onclick=\"var el=document.getElementById('reply-");
        cwist_sstring_append(b, cid_buf);
        cwist_sstring_append(b, "');el.style.display=el.style.display==='none'?'block':'none';\">Reply</button>");
        cwist_sstring_append(b, "<div id='reply-");
        cwist_sstring_append(b, cid_buf);
        cwist_sstring_append(b, "' style='display:none;margin-top:8px'>");
        cwist_sstring_append(b, "<form action='/comment/new' method='post'>");
        cwist_sstring_append(b, "<input type='hidden' name='target_type' value='post'>");
        cwist_sstring_append(b, "<input type='hidden' name='target_id' value='");
        cwist_sstring_append(b, post_id_buf);
        cwist_sstring_append(b, "'>");
        cwist_sstring_append(b, "<input type='hidden' name='parent_id' value='");
        cwist_sstring_append(b, cid_buf);
        cwist_sstring_append(b, "'>");
        cwist_sstring_append(b, "<textarea name='content' rows='2' placeholder='Write a reply...' required style='width:100%;font-family:inherit;font-size:14px'></textarea>");
        cwist_sstring_append(b, "<div style='margin-top:6px'><button type='submit' class='btn' style='font-size:12px;padding:4px 10px'>Reply</button></div>");
        cwist_sstring_append(b, "</form></div></div>");
    }

    cwist_sstring_append(b, "</div>");

    /* Find children */
    if (all_comments && cid > 0) {
        int n = cJSON_GetArraySize(all_comments);
        for (int i = 0; i < n; i++) {
            cJSON *c = cJSON_GetArrayItem(all_comments, i);
            cJSON *parent = cJSON_GetObjectItem(c, "parent_id");
            if (parent && parent->valueint == cid) {
                render_comment_node(b, c, all_comments, depth + 1, current_user_id, user_role, target_id);
            }
        }
    }
}

cwist_sstring *render_post_list(cJSON *posts, cJSON *boards, bool dark, const char *user_role, int page, int total_pages, const char *board_slug, const char *search, const char *search_type, const char *profile_pic, int user_id) {
    cwist_sstring *b = cwist_sstring_create();
    int has_home_bg = g_config.home_img[0];
    char bg_style[768] = {0};
    char text_style[256] = {0};
    char logo_filter[128] = {0};
    if (!board_slug || board_slug[0] == '\0') {
        if (has_home_bg) {
            char img_path[512];
            char img_url[512];
            snprintf(img_path, sizeof(img_path), "public/img/%s", g_config.home_img);
            snprintf(img_url, sizeof(img_url), "/assets/img/%s", g_config.home_img);
            get_image_text_style(img_path, img_url, bg_style, sizeof(bg_style),
                                 text_style, sizeof(text_style),
                                 logo_filter, sizeof(logo_filter));
            cwist_sstring_append(b, "<div style=\"");
            cwist_sstring_append(b, bg_style);
            cwist_sstring_append(b, ";padding:40px 20px 20px;");
            cwist_sstring_append(b, text_style);
            cwist_sstring_append(b, "\">");
        }
        cwist_sstring_append(b, "<div class='hero' ");
        if (has_home_bg) cwist_sstring_append(b, "style='background:none;padding:0' ");
        cwist_sstring_append(b, "><img class='hero-logo' src='/assets/img/");
        if (g_config.blog_logo[0]) cwist_sstring_append_escaped(b, g_config.blog_logo);
        else cwist_sstring_append(b, "logo.png");
        cwist_sstring_append(b, "' alt='Logo' style='height:120px");
        if (has_home_bg) {
            cwist_sstring_append(b, ";filter:");
            cwist_sstring_append(b, logo_filter);
        }
        cwist_sstring_append(b, "' fetchpriority='high'>");
        if (!board_slug || !board_slug[0]) {
            cwist_sstring_append(b, "<h1>");
            cwist_sstring_append_escaped(b, g_config.title);
            cwist_sstring_append(b, "</h1><p");
            if (has_home_bg) cwist_sstring_append(b, " style='opacity:0.85'");
            cwist_sstring_append(b, ">");
            cwist_sstring_append_escaped(b, g_config.subtitle);
            cwist_sstring_append(b, "</p>");
        } else {
            cwist_sstring_append(b, "<h1>All Boards</h1>");
        }
        cwist_sstring_append(b, "</div>");

        if (has_home_bg) {
            cwist_sstring_append(b, "</div>");
            cwist_sstring_append(b, "<hr style='border:none;border-top:2px solid var(--border);margin:12px 0 24px'>");
        }
    }

    cwist_sstring_append(b, "<div style='margin-bottom:18px;text-align:center'><a href='/post/new' class='btn'>New Post</a></div>");

    /* Search */
    const char *search_label = "Global search";
    const char *search_placeholder = "Search all posts...";
    const char *board_name = NULL;

    if (board_slug && board_slug[0]) {
        search_label = "Board search";
        search_placeholder = "Search in this board...";
        if (boards) {
            int bn = cJSON_GetArraySize(boards);
            for (int i = 0; i < bn; i++) {
                cJSON *bo = cJSON_GetArrayItem(boards, i);
                cJSON *bs = cJSON_GetObjectItem(bo, "slug");
                if (bs && bs->valuestring && strcmp(bs->valuestring, board_slug) == 0) {
                    cJSON *bnm = cJSON_GetObjectItem(bo, "name");
                    if (bnm && bnm->valuestring) board_name = bnm->valuestring;
                    break;
                }
            }
        }
    }

    cwist_sstring_append(b, "<div style='max-width:720px;margin:0 auto 18px'>");
    cwist_sstring_append(b, "<div style='display:flex;align-items:center;gap:8px;margin-bottom:8px'>");
    cwist_sstring_append(b, "<span style='font-size:11px;font-weight:700;color:var(--accent);text-transform:uppercase;letter-spacing:0.08em'>");
    cwist_sstring_append(b, search_label);
    cwist_sstring_append(b, "</span>");
    if (board_name) {
        cwist_sstring_append(b, "<span class='tag' style='margin:0;font-size:11px;padding:3px 8px'>");
        cwist_sstring_append_escaped(b, board_name);
        cwist_sstring_append(b, "</span>");
    }
    cwist_sstring_append(b, "</div>");

    cwist_sstring_append(b, "<form action='");
    if (board_slug && board_slug[0]) {
        cwist_sstring_append(b, "/board/");
        cwist_sstring_append(b, board_slug);
    } else {
        cwist_sstring_append(b, "/search");
    }
    cwist_sstring_append(b, "' method='get'>");
    cwist_sstring_append(b, "<div style='display:flex;gap:8px'>");
    cwist_sstring_append(b, "<input type='text' name='search' placeholder='");
    cwist_sstring_append(b, search_placeholder);
    cwist_sstring_append(b, "' value='");
    if (search && search[0]) cwist_sstring_append_escaped(b, search);
    cwist_sstring_append(b, "' style='flex:1'>");
    cwist_sstring_append(b, "<button type='submit' class='btn'>Search</button>");
    cwist_sstring_append(b, "<button type='button' class='btn btn-outline adv-toggle-btn' onclick=\"var el=document.getElementById('adv-search');var btn=this;var open=el.style.display!=='none';el.style.display=open?'none':'block';btn.classList.toggle('open',!open);\">Advanced</button>");
    if (search && search[0]) {
        cwist_sstring_append(b, "<a href='");
        if (board_slug) { cwist_sstring_append(b, "/board/"); cwist_sstring_append(b, board_slug); }
        cwist_sstring_append(b, "' class='btn btn-outline'>Clear</a>");
    }
    cwist_sstring_append(b, "</div>");
    cwist_sstring_append(b, "<div id='adv-search' class='dropdown-panel' style='display:none;margin-top:8px'>");
    cwist_sstring_append(b, "<select name='search_type' style='padding:6px 10px;border-radius:0;border:1px solid var(--border);background:var(--panel);color:var(--fg);font-family:inherit'>");
    cwist_sstring_append(b, "<option value=''");
    if (!search_type || !search_type[0]) cwist_sstring_append(b, " selected");
    cwist_sstring_append(b, ">All (Title + Body)</option>");
    cwist_sstring_append(b, "<option value='title'");
    if (search_type && strcmp(search_type, "title") == 0) cwist_sstring_append(b, " selected");
    cwist_sstring_append(b, ">Title</option>");
    cwist_sstring_append(b, "<option value='body'");
    if (search_type && strcmp(search_type, "body") == 0) cwist_sstring_append(b, " selected");
    cwist_sstring_append(b, ">Post Body</option>");
    cwist_sstring_append(b, "<option value='board'");
    if (search_type && strcmp(search_type, "board") == 0) cwist_sstring_append(b, " selected");
    cwist_sstring_append(b, ">Board Name</option>");
    cwist_sstring_append(b, "</select></div>");
    cwist_sstring_append(b, "</form>");

    cwist_sstring_append(b, "<div class='post-list stagger");
    if (board_slug && board_slug[0]) {
        cwist_sstring_append(b, " board-typography-list");
    }
    cwist_sstring_append(b, "'>");

    int max_views = -1;
    int featured_idx = -1;
    if (posts) {
        int n_posts = cJSON_GetArraySize(posts);
        for (int i = 0; i < n_posts; i++) {
            cJSON *p = cJSON_GetArrayItem(posts, i);
            cJSON *pv = cJSON_GetObjectItem(p, "view_count");
            int v = pv ? pv->valueint : 0;
            if (v > max_views) {
                max_views = v;
                featured_idx = i;
            }
        }
    }

    if (posts) {
        int n = cJSON_GetArraySize(posts);
        for (int i = 0; i < n; i++) {
            cJSON *p = cJSON_GetArrayItem(posts, i);
            cJSON *slug = cJSON_GetObjectItem(p, "slug");
            cJSON *title = cJSON_GetObjectItem(p, "title");
            cJSON *summary = cJSON_GetObjectItem(p, "summary");
            cJSON *author = cJSON_GetObjectItem(p, "author_name");
            cJSON *date = cJSON_GetObjectItem(p, "created_at");
            cJSON *views = cJSON_GetObjectItem(p, "view_count");
            cwist_sstring_append(b, "<div class='post-row");
            cJSON *is_notice = cJSON_GetObjectItem(p, "is_notice");
            if (is_notice && is_notice->valueint) {
                cwist_sstring_append(b, " post-row-notice");
            }
            if (board_slug && board_slug[0]) {
                cwist_sstring_append(b, " post-row-typography");
            }
            if (i == featured_idx) {
                cwist_sstring_append(b, " featured");
            }
            cwist_sstring_append(b, "'>");
            cwist_sstring_append(b, "<div class='post-row-head'>");
            if (is_notice && is_notice->valueint) {
                cwist_sstring_append(b, "<span class='tag' style='background:var(--accent);color:#fff'>Notice</span>");
            }
            if (!board_slug || board_slug[0] == '\0') {
                cJSON *board_name = cJSON_GetObjectItem(p, "board_name");
                if (board_name && board_name->valuestring && board_name->valuestring[0]) {
                    cwist_sstring_append(b, "<span class='tag'>");
                    cwist_sstring_append_escaped(b, board_name->valuestring);
                    cwist_sstring_append(b, "</span>");
                }
            }
            cwist_sstring_append(b, "</div>");
            cwist_sstring_append(b, "<a class='post-row-title' href='/post/");
            cwist_sstring_append(b, slug->valuestring);
            cwist_sstring_append(b, "'>");
            cwist_sstring_append_escaped(b, title->valuestring);
            cwist_sstring_append(b, "</a>");
            if (summary && summary->valuestring && summary->valuestring[0]) {
                cwist_sstring_append(b, "<p class='post-row-summary'>");
                cwist_sstring_append_escaped(b, summary->valuestring);
                cwist_sstring_append(b, "</p>");
            }
            cwist_sstring_append(b, "<div class='post-row-meta'>");
            if (author && author->valuestring) {
                cJSON *author_id = cJSON_GetObjectItem(p, "user_id");
                if (author_id && author_id->valueint > 0) {
                    char uid_buf[32];
                    snprintf(uid_buf, sizeof(uid_buf), "%d", author_id->valueint);
                    cwist_sstring_append(b, "<span class='post-badge'><a href='/user/");
                    cwist_sstring_append(b, uid_buf);
                    cwist_sstring_append(b, "'>");
                    cwist_sstring_append_escaped(b, author->valuestring);
                    cwist_sstring_append(b, "</a></span>");
                } else {
                    cwist_sstring_append(b, "<span class='post-badge'>");
                    cwist_sstring_append_escaped(b, author->valuestring);
                    cwist_sstring_append(b, "</span>");
                }
            } else {
                cwist_sstring_append(b, "<span class='post-badge'>unknown</span>");
            }
            cwist_sstring_append(b, "<span class='post-badge'>Views ");
            char vbuf[32]; snprintf(vbuf, sizeof(vbuf), "%d", views ? views->valueint : 0);
            cwist_sstring_append(b, vbuf);
            cwist_sstring_append(b, " views</span>");
            cwist_sstring_append(b, "<span class='post-badge'>");
            cwist_sstring_append_escaped(b, date && date->valuestring ? date->valuestring : "");
            cwist_sstring_append(b, "</span>");
            bool can_edit = (user_id > 0 && json_int(p, "user_id", 0) == user_id) || (user_role && strcmp(user_role, "admin") == 0);
            if (can_edit) {
                cwist_sstring_append(b, "<div style='margin-top:8px;display:flex;gap:8px'>");
                char pid_buf[32];
                snprintf(pid_buf, sizeof(pid_buf), "%d", json_int(p, "id", 0));
                cwist_sstring_append(b, "<a href='/post/");
                cwist_sstring_append(b, pid_buf);
                cwist_sstring_append(b, "/edit' class='btn btn-outline' style='font-size:12px;padding:4px 10px'>Edit</a>");
                cwist_sstring_append(b, "<a href='/post/delete/");
                cwist_sstring_append(b, pid_buf);
                cwist_sstring_append(b, "' class='btn btn-outline' style='font-size:12px;padding:4px 10px' onclick='return confirm(\"Delete this post?\")'>Delete</a>");
                cwist_sstring_append(b, "</div>");
            }
            cwist_sstring_append(b, "</div>");
            cwist_sstring_append(b, "</div>");
        }
    } else {
        if (search && !search[0]) {
            cwist_sstring_append(b, "<p style='color:var(--muted);text-align:center;padding:40px 0'>No Search Keyword</p>");
        } else {
            cwist_sstring_append(b, "<p style='color:var(--muted);text-align:center;padding:40px 0'>No posts found.</p>");
        }
    }

    cwist_sstring_append(b, "</div>");

    /* Pagination */
    if (total_pages > 1) {
        cwist_sstring_append(b, "<div style='margin-top:18px;display:flex;gap:8px;justify-content:center;align-items:center'>");
        if (page > 1) {
            cwist_sstring_append(b, "<a class='btn btn-outline' href='");
            if (board_slug) {
                cwist_sstring_append(b, "/board/");
                cwist_sstring_append(b, board_slug);
            }
            if (search && search[0]) {
                cwist_sstring_append(b, "?search="); cwist_sstring_append_escaped(b, search);
                if (search_type && search_type[0]) {
                    cwist_sstring_append(b, "&search_type="); cwist_sstring_append_escaped(b, search_type);
                }
                cwist_sstring_append(b, "&page=");
            } else {
                cwist_sstring_append(b, "?page=");
            }
            char buf[16];
            snprintf(buf, sizeof(buf), "%d", page - 1);
            cwist_sstring_append(b, buf);
            cwist_sstring_append(b, "'>Prev</a>");
        }
        cwist_sstring_append(b, "<span style='padding:6px 12px;color:var(--muted)'>Page ");
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", page);
        cwist_sstring_append(b, buf);
        cwist_sstring_append(b, " of ");
        snprintf(buf, sizeof(buf), "%d", total_pages);
        cwist_sstring_append(b, buf);
        cwist_sstring_append(b, "</span>");
        if (page < total_pages) {
            cwist_sstring_append(b, "<a class='btn btn-outline' href='");
            if (board_slug) {
                cwist_sstring_append(b, "/board/");
                cwist_sstring_append(b, board_slug);
            }
            if (search && search[0]) {
                cwist_sstring_append(b, "?search="); cwist_sstring_append_escaped(b, search);
                if (search_type && search_type[0]) {
                    cwist_sstring_append(b, "&search_type="); cwist_sstring_append_escaped(b, search_type);
                }
                cwist_sstring_append(b, "&page=");
            } else {
                cwist_sstring_append(b, "?page=");
            }
            char buf2[16];
            snprintf(buf2, sizeof(buf2), "%d", page + 1);
            cwist_sstring_append(b, buf2);
            cwist_sstring_append(b, "'>Next</a>");
        }
        cwist_sstring_append(b, "</div>");
    }

    cwist_sstring *page_html = render_page("Posts", b->data, dark, user_role, profile_pic);
    cwist_sstring_destroy(b);
    return page_html;
}

cwist_sstring *render_post_detail(cJSON *post, cJSON *files, cJSON *comments, bool dark, const char *user_role, bool pqc_verified, int vote_up, int vote_down, int user_vote, const char *profile_pic, int user_id, const char *ephemeral_delete_pin) {
    cwist_sstring *b = cwist_sstring_create();
    cJSON *title = cJSON_GetObjectItem(post, "title");
    cJSON *content = cJSON_GetObjectItem(post, "content");
    cJSON *date = cJSON_GetObjectItem(post, "created_at");
    cJSON *author = cJSON_GetObjectItem(post, "author_name");
    int post_id_val = json_int(post, "id", 0);
    cJSON *view_count = cJSON_GetObjectItem(post, "view_count");
    char pid_buf[32]; snprintf(pid_buf, sizeof(pid_buf), "%d", post_id_val);

    cwist_sstring_append(b, "<article>");
    if (ephemeral_delete_pin && ephemeral_delete_pin[0]) {
        cwist_sstring_append(b, "<div class='card' style='margin-bottom:16px;border-color:var(--accent)'>");
        cwist_sstring_append(b, "<strong>Delete PIN</strong><div style='margin-top:6px;color:var(--muted)'>Save this now. It is only shown once for this anonymous post.</div><div style='margin-top:8px'><code>");
        cwist_sstring_append_escaped(b, ephemeral_delete_pin);
        cwist_sstring_append(b, "</code></div></div>");
    }
    cwist_sstring_append(b, "<div style='margin-bottom:10px'>");
    if (pqc_verified) {
        cwist_sstring_append(b, "<span style='color:var(--accent);font-size:13px;font-weight:700'>PQC Verified</span>");
    }
    cwist_sstring_append(b, "</div>");
    cwist_sstring_append(b, "<h1 style='margin-bottom:6px;font-size:2.25rem;font-weight:800;letter-spacing:-0.03em;line-height:1.2'>");
    cwist_sstring_append_escaped(b, title->valuestring);
    cwist_sstring_append(b, "</h1>");
    cwist_sstring_append(b, "<p style='color:var(--muted);font-size:14px'>by ");
    if (author && author->valuestring) {
        cJSON *author_id = cJSON_GetObjectItem(post, "user_id");
        if (author_id && author_id->valueint > 0) {
            char uid_buf[32];
            snprintf(uid_buf, sizeof(uid_buf), "%d", author_id->valueint);
            cwist_sstring_append(b, "<a href='/user/");
            cwist_sstring_append(b, uid_buf);
            cwist_sstring_append(b, "'>");
            cwist_sstring_append_escaped(b, author->valuestring);
            cwist_sstring_append(b, "</a>");
        } else {
            cwist_sstring_append_escaped(b, author->valuestring);
        }
    } else if (json_int(post, "user_id", 0) == 0) {
        cwist_sstring_append(b, "anonymous");
    } else {
        cwist_sstring_append(b, "unknown");
    }
    cwist_sstring_append(b, " &middot; ");
    cwist_sstring_append_escaped(b, date->valuestring);
    cwist_sstring_append(b, " &middot; Views: ");
    char vbuf[32]; snprintf(vbuf, sizeof(vbuf), "%d", view_count ? view_count->valueint : 0);
    cwist_sstring_append(b, vbuf);
    cwist_sstring_append(b, "</p>");

    /* Vote buttons */
    cwist_sstring_append(b, "<div style='margin:16px 0;display:flex;gap:10px;align-items:center'>");
    cwist_sstring_append(b, "<button id='vote-up' class='btn btn-outline vote-btn' style='padding:6px 12px;font-size:13px");
    if (user_vote == 1) cwist_sstring_append(b, ";border-color:var(--accent);color:var(--accent)");
    cwist_sstring_append(b, "'>&#9650; ");
    char vup[32]; snprintf(vup, sizeof(vup), "%d", vote_up);
    cwist_sstring_append(b, vup);
    cwist_sstring_append(b, "</button>");
    cwist_sstring_append(b, "<button id='vote-down' class='btn btn-outline vote-btn' style='padding:6px 12px;font-size:13px");
    if (user_vote == -1) cwist_sstring_append(b, ";border-color:var(--accent);color:var(--accent)");
    cwist_sstring_append(b, "'>&#9660; ");
    char vdown[32]; snprintf(vdown, sizeof(vdown), "%d", vote_down);
    cwist_sstring_append(b, vdown);
    cwist_sstring_append(b, "</button>");
    cwist_sstring_append(b, "<span id='vote-msg' style='color:var(--muted);font-size:13px'></span>");
    cwist_sstring_append(b, "</div>");
    cwist_sstring_append(b, "<script>");
    cwist_sstring_append(b, "(function(){");
    cwist_sstring_append(b, "var pid="); cwist_sstring_append(b, pid_buf); cwist_sstring_append(b, ";");
    cwist_sstring_append(b, "var uv=");
    char uv_buf[32]; snprintf(uv_buf, sizeof(uv_buf), "%d", user_vote);
    cwist_sstring_append(b, uv_buf); cwist_sstring_append(b, ";");
    cwist_sstring_append(b, "function updateVoteStyle(v){");
    cwist_sstring_append(b, "var up=document.getElementById('vote-up');var down=document.getElementById('vote-down');");
    cwist_sstring_append(b, "up.style.borderColor=v==1?'var(--accent)':'';up.style.color=v==1?'var(--accent)':'';");
    cwist_sstring_append(b, "down.style.borderColor=v==-1?'var(--accent)':'';down.style.color=v==-1?'var(--accent)':'';");
    cwist_sstring_append(b, "}");
    cwist_sstring_append(b, "updateVoteStyle(uv);");
    cwist_sstring_append(b, "function sendVote(vt){");
    cwist_sstring_append(b, "fetch('/post/vote',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'post_id='+pid+'&vote_type='+vt})");
    cwist_sstring_append(b, ".then(function(r){return r.json();}).then(function(d){");
    cwist_sstring_append(b, "if(d.ok){document.getElementById('vote-up').innerHTML='&#9650; '+d.up;document.getElementById('vote-down').innerHTML='&#9660; '+d.down;updateVoteStyle(d.user_vote);}");
    cwist_sstring_append(b, "});}");
    cwist_sstring_append(b, "document.getElementById('vote-up').addEventListener('click',function(){sendVote(1);});");
    cwist_sstring_append(b, "document.getElementById('vote-down').addEventListener('click',function(){sendVote(-1);});");
    cwist_sstring_append(b, "})();");
    cwist_sstring_append(b, "</script>");

    cwist_sstring *md_html = render_markdown_to_html(content->valuestring);
    cwist_sstring_append(b, "<div class='markdown-body'>");
    if (md_html) {
        cwist_sstring_append_sstring(b, md_html);
        cwist_sstring_destroy(md_html);
    }
    cwist_sstring_append(b, "</div>");

    /* Files */
    if (files && cJSON_GetArraySize(files) > 0) {
        int n = cJSON_GetArraySize(files);
        int valid_files = 0;
        for (int i = 0; i < n; i++) {
            cJSON *f = cJSON_GetArrayItem(files, i);
            cJSON *fname = cJSON_GetObjectItem(f, "filename");
            if (fname && fname->valuestring && fname->valuestring[0] != '\0') {
                valid_files++;
            }
        }
        if (valid_files > 0) {
            cwist_sstring_append(b, "<h3 style='margin-top:32px'>Attachments</h3>");
            cwist_sstring_append(b, "<div class='post-attachments' style='display:grid;gap:12px;grid-template-columns:repeat(auto-fill,minmax(220px,1fr));margin-top:12px'>");
            for (int i = 0; i < n; i++) {
                cJSON *f = cJSON_GetArrayItem(files, i);
                cJSON *fname = cJSON_GetObjectItem(f, "filename");
                if (!fname || !fname->valuestring || fname->valuestring[0] == '\0') continue;
                cJSON *fid = cJSON_GetObjectItem(f, "id");
                cJSON *stype = cJSON_GetObjectItem(f, "mime_type");
                cJSON *jthumb = cJSON_GetObjectItem(f, "thumb_path");
                const char *thumb_path = (jthumb && jthumb->valuestring && jthumb->valuestring[0]) ? jthumb->valuestring : "";
                char fid_buf2[32];
                snprintf(fid_buf2, sizeof(fid_buf2), "%d", fid->valueint);
                const char *mime = stype && stype->valuestring ? stype->valuestring : "";
                int is_image = (strncmp(mime, "image/", 6) == 0);
                int is_video = (strncmp(mime, "video/", 6) == 0);
                int is_audio = (strncmp(mime, "audio/", 6) == 0);

                cwist_sstring_append(b, "<div class='post-attachment-item' style='border:1px solid var(--glass-border);background:color-mix(in srgb,var(--glass-bg) 90%,transparent);padding:10px'>");

                if (is_image) {
                    if (thumb_path[0] && strncmp(thumb_path, "public/uploads/", 15) == 0) {
                        cwist_sstring_append(b, "<img src='/assets/uploads/");
                        cwist_sstring_append(b, thumb_path + strlen("public/uploads/"));
                        cwist_sstring_append(b, "' loading='lazy' decoding='async' style='max-width:100%;height:auto;display:block'>");
                    } else {
                        cwist_sstring_append(b, "<img data-tasfa-download='/file/download/");
                        cwist_sstring_append(b, fid_buf2);
                        cwist_sstring_append(b, "' style='max-width:100%;height:auto;display:block' src='data:image/gif;base64,R0lGODlhAQABAIAAAAAAAP///yH5BAEAAAAALAAAAAABAAEAAAIBRAA7'>");
                    }
                    cwist_sstring_append(b, "<div style='margin-top:8px;font-size:13px;word-break:break-all'>");
                    cwist_sstring_append_escaped(b, fname->valuestring);
                    cwist_sstring_append(b, "</div>");
                } else if (is_video) {
                    cwist_sstring_append(b, "<video data-tasfa-download='/file/download/");
                    cwist_sstring_append(b, fid_buf2);
                    cwist_sstring_append(b, "' style='max-width:100%;height:auto;display:block' muted playsinline preload='metadata' controls></video>");
                    cwist_sstring_append(b, "<div style='margin-top:8px;font-size:13px;word-break:break-all'>");
                    cwist_sstring_append_escaped(b, fname->valuestring);
                    cwist_sstring_append(b, "</div>");
                } else if (is_audio) {
                    cwist_sstring_append(b, "<audio data-tasfa-download='/file/download/");
                    cwist_sstring_append(b, fid_buf2);
                    cwist_sstring_append(b, "' style='width:100%' controls></audio>");
                    cwist_sstring_append(b, "<div style='margin-top:8px;font-size:13px;word-break:break-all'>");
                    cwist_sstring_append_escaped(b, fname->valuestring);
                    cwist_sstring_append(b, "</div>");
                } else {
                    cwist_sstring_append(b, "<a href='#' data-tasfa-download-link='/file/download/");
                    cwist_sstring_append(b, fid_buf2);
                    cwist_sstring_append(b, "' style='display:flex;align-items:center;gap:8px;color:var(--accent);font-weight:600;font-size:14px'>&#128193; ");
                    cwist_sstring_append_escaped(b, fname->valuestring);
                    cwist_sstring_append(b, "</a>");
                }
                cwist_sstring_append(b, "</div>");
            }
            cwist_sstring_append(b, "</div>");
        }
    }

    cwist_sstring_append(b, code_copy_script);
    cwist_sstring_append(b, "</article>");

    /* Actions */
    cwist_sstring_append(b, "<div style='margin-top:24px;display:flex;gap:10px;flex-wrap:wrap'>");
    cwist_sstring_append(b, "<a href='/' class='btn btn-outline'>Back</a>");
    bool can_edit = (user_id > 0 && json_int(post, "user_id", 0) == user_id) || (user_role && strcmp(user_role, "admin") == 0);
    if (can_edit) {
        cwist_sstring_append(b, "<a href='/post/");
        cwist_sstring_append(b, pid_buf);
        cwist_sstring_append(b, "/edit' class='btn'>Edit</a>");
        cwist_sstring_append(b, "<a href='/post/delete/");
        cwist_sstring_append(b, pid_buf);
        cwist_sstring_append(b, "' class='btn btn-outline' onclick='return confirm(\"Delete this post?\")'>Delete</a>");
    }
    if (json_int(post, "user_id", 0) == 0) {
        cwist_sstring_append(b, "<form action='/post/delete/");
        cwist_sstring_append(b, pid_buf);
        cwist_sstring_append(b, "' method='get' style='display:flex;gap:8px;align-items:center;flex-wrap:wrap'>");
        cwist_sstring_append(b, "<input type='text' name='delete_pin' placeholder='Delete PIN' required style='max-width:180px'>");
        cwist_sstring_append(b, "<button type='submit' class='btn btn-outline' onclick='return confirm(\"Delete this anonymous post?\")'>Delete With PIN</button></form>");
    }
    cwist_sstring_append(b, "</div>");

    /* Comments */
    cwist_sstring_append(b, "<div style='margin-top:40px'><h2>Comments</h2>");
    if (comments && cJSON_GetArraySize(comments) > 0) {
        int n = cJSON_GetArraySize(comments);
        for (int i = 0; i < n; i++) {
            cJSON *c = cJSON_GetArrayItem(comments, i);
            cJSON *parent_id = cJSON_GetObjectItem(c, "parent_id");
            if (!parent_id || parent_id->valueint == 0) {
                render_comment_node(b, c, comments, 0, user_id, user_role, post_id_val);
            }
        }
    } else {
        cwist_sstring_append(b, "<p style='color:var(--muted)'>No comments yet.</p>");
    }
    if (user_role && user_role[0]) {
        cwist_sstring_append(b, "<form action='/comment/new' method='post' style='margin-top:18px'>");
        cwist_sstring_append(b, "<input type='hidden' name='target_type' value='post'>");
        cwist_sstring_append(b, "<input type='hidden' name='target_id' value='");
        cwist_sstring_append(b, pid_buf);
        cwist_sstring_append(b, "'>");
        cwist_sstring_append(b, "<textarea name='content' rows='3' placeholder='Write a comment...' required></textarea>");
        cwist_sstring_append(b, "<div style='margin-top:8px'><button type='submit' class='btn'>Comment</button></div>");
        cwist_sstring_append(b, "</form>");
    }
    cwist_sstring_append(b, "</div>");

    cwist_sstring *page = render_page(title->valuestring, b->data, dark, user_role, profile_pic);
    cwist_sstring_destroy(b);
    return page;
}

cwist_sstring *render_post_editor(cJSON *boards, cJSON *post, cJSON *files, bool dark, const char *user_role, const char *error, const char *profile_pic) {
    cwist_sstring *b = cwist_sstring_create();
    cwist_sstring_assign(b, "<div class='card' style='margin:24px 0;'>");
    if (error && error[0]) {
        cwist_sstring_append(b, "<div class='alert'>");
        char *tmp_err = sql_escape(error);
        cwist_sstring_append(b, tmp_err);
        cwist_free(tmp_err);
        cwist_sstring_append(b, "</div>");
    }
    char action[64] = "/post/new";
    if (post) {
        cJSON *pid = cJSON_GetObjectItem(post, "id");
        snprintf(action, sizeof(action), "/post/%d/edit", pid->valueint);
    }
    cwist_sstring_append(b, "<form action='");
    cwist_sstring_append(b, action);
    cwist_sstring_append(b, "' method='post'>");

    cwist_sstring_append(b, "<input type='hidden' id='media-meta' name='media_meta' value='[]'>");
    cwist_sstring_append(b, "<label>Title</label><input id='post-title-input' name='title' value='");
    if (post) {
        cJSON *t = cJSON_GetObjectItem(post, "title");
        char *tmp_title = sql_escape(t->valuestring);
        cwist_sstring_append(b, tmp_title);
        cwist_free(tmp_title);
    }
    cwist_sstring_append(b, "' required>");

    cwist_sstring_append(b, "<label>Board</label><select name='board_id'>");
    if (boards) {
        int n = cJSON_GetArraySize(boards);
        for (int i = 0; i < n; i++) {
            cJSON *bo = cJSON_GetArrayItem(boards, i);
            cJSON *bname = cJSON_GetObjectItem(bo, "name");
            int bid_val = json_int(bo, "id", 0);
            char bid_buf[32];
            snprintf(bid_buf, sizeof(bid_buf), "%d", bid_val);
            int post_board_id = post ? json_int(post, "board_id", 0) : 0;
            cwist_sstring_append(b, "<option value='");
            cwist_sstring_append(b, bid_buf);
            cwist_sstring_append(b, "'");
            if (post_board_id > 0 && post_board_id == bid_val) cwist_sstring_append(b, " selected");
            cwist_sstring_append(b, ">");
            char *tmp_bname = sql_escape(bname->valuestring);
            cwist_sstring_append(b, tmp_bname);
            cwist_free(tmp_bname);
            cwist_sstring_append(b, "</option>");
        }
    }
    cwist_sstring_append(b, "</select>");

    cwist_sstring_append(b, "<label>Summary</label><input id='summary-input' name='summary' value='");
    if (post) {
        cJSON *s = cJSON_GetObjectItem(post, "summary");
        char *tmp_summary = sql_escape(s && s->valuestring[0] ? s->valuestring : "");
        cwist_sstring_append(b, tmp_summary);
        cwist_free(tmp_summary);
    }
    cwist_sstring_append(b, "'>");

    cwist_sstring_append(b, "<div style='display:flex;justify-content:space-between;gap:12px;align-items:center;flex-wrap:wrap;margin-top:16px'>");
    cwist_sstring_append(b, "<label style='margin:0'>Content (Markdown)</label>");
    cwist_sstring_append(b, "<div style='display:flex;gap:8px;align-items:center;flex-wrap:wrap'>");
    cwist_sstring_append(b, "<span id='editor-word-count' style='font-size:13px;color:var(--muted)'>0 words</span>");
    cwist_sstring_append(b, "<span id='editor-reading-time' style='font-size:13px;color:var(--muted)'>0 min read</span>");
    cwist_sstring_append(b, "<span id='editor-sync-status' style='font-size:13px;color:var(--muted)'>Editor ready</span>");
    cwist_sstring_append(b, "</div></div>");
    cwist_sstring_append(b, "<div style='display:flex;gap:8px;flex-wrap:wrap;margin-top:8px'>");
    cwist_sstring_append(b, "<button type='button' class='btn btn-outline editor-tool' data-md-wrap='**' data-md-placeholder='bold text'>Bold</button>");
    cwist_sstring_append(b, "<button type='button' class='btn btn-outline editor-tool' data-md-wrap='*' data-md-placeholder='italic text'>Italic</button>");
    cwist_sstring_append(b, "<button type='button' class='btn btn-outline editor-tool' data-md-prefix='- ' data-md-placeholder='list item'>List</button>");
    cwist_sstring_append(b, "<button type='button' class='btn btn-outline editor-tool' data-md-prefix='> ' data-md-placeholder='quote'>Quote</button>");
    cwist_sstring_append(b, "<button type='button' class='btn btn-outline editor-tool' data-md-block='```\\n\\n```'>Code Block</button>");
    cwist_sstring_append(b, "</div>");
    cwist_sstring_append(b, "<div style='display:flex;gap:8px;flex-wrap:wrap;margin-top:12px'>");
    cwist_sstring_append(b, "<button type='button' class='btn' data-editor-tab='write'>Write</button>");
    cwist_sstring_append(b, "<button type='button' class='btn btn-outline' data-editor-tab='preview'>Preview</button>");
    cwist_sstring_append(b, "</div>");
    cwist_sstring_append(b, "<div style='border:1px solid var(--border);border-radius:0;overflow:hidden;margin-top:10px'>");
    cwist_sstring_append(b, "<div data-editor-pane='write' class='is-active' style='display:block'>");
    cwist_sstring_append(b, "<textarea id='md-editor' name='content' rows='18' style='width:100%;min-height:500px;height:60vh;font-family:monospace;font-size:15px;border:none;border-radius:0;padding:16px;background:transparent;resize:vertical;outline:none;' required>");
    if (post) {
        cJSON *c = cJSON_GetObjectItem(post, "content");
        const char *content_str = c && c->valuestring ? c->valuestring : "";
        const char *p = content_str;
        while (*p) {
            const char *end = strstr(p, "</textarea>");
            if (end) {
                cwist_sstring_append_len(b, p, end - p);
                cwist_sstring_append(b, "&lt;/textarea&gt;");
                p = end + 11;
            } else {
                cwist_sstring_append(b, p);
                break;
            }
        }
    }
    cwist_sstring_append(b, "</textarea></div>");
    cwist_sstring_append(b, "<div data-editor-pane='preview' style='display:none;background:var(--panel)'>");
    cwist_sstring_append(b, "<div id='md-preview' style='padding:16px;min-height:500px;height:60vh;overflow:auto'>");
    cwist_sstring_append(b, "<p style='color:var(--muted)'>Preview will appear here...</p>");
    cwist_sstring_append(b, "</div></div></div>");

    cwist_sstring_append(b, "<details style='margin-top:16px'><summary style='cursor:pointer;font-weight:600;font-size:14px;color:var(--accent);user-select:none;'>Markdown Guide</summary>");
    cwist_sstring_append(b, "<pre style='font-size:12px;background:var(--code-bg);padding:12px;border-radius:0;overflow:auto;white-space:pre-wrap;word-break:break-word;margin-top:8px;'>");
    cwist_sstring_append(b, "# Heading\n");
    cwist_sstring_append(b, "## Subheading\n");
    cwist_sstring_append(b, "**bold**  *italic*\n");
    cwist_sstring_append(b, "`inline code`\n\n");
    cwist_sstring_append(b, "```c\n");
    cwist_sstring_append(b, "int main() {}\n");
    cwist_sstring_append(b, "```\n\n");
    cwist_sstring_append(b, "[link](url)\n");
    cwist_sstring_append(b, "&lt;img src=\"url\"&gt;\n");
    cwist_sstring_append(b, "- list item\n");
    cwist_sstring_append(b, "> quote\n");
    cwist_sstring_append(b, "</pre></details>");
    if (post && files && cJSON_GetArraySize(files) > 0) {
        cwist_sstring_append(b, "<div style='margin-top:18px'><h4>Existing Attachments</h4><div id='upload-preview' style='display:grid;gap:10px'>");
        int n = cJSON_GetArraySize(files);
        for (int i = 0; i < n; i++) {
            cJSON *f = cJSON_GetArrayItem(files, i);
            cJSON *fname = cJSON_GetObjectItem(f, "filename");
            if (!fname || !fname->valuestring) continue;
            char fid_buf[32]; snprintf(fid_buf, sizeof(fid_buf), "%d", json_int(f, "id", 0));
            cwist_sstring_append(b, "<div class='existing-media media-card' data-fid='");
            cwist_sstring_append(b, fid_buf);
            cwist_sstring_append(b, "' data-filename='");
            cwist_sstring_append_escaped(b, fname->valuestring);
            char asset_url[512] = {0};
            snprintf(asset_url, sizeof(asset_url), "/file/download/%s", fid_buf);
            cwist_sstring_append(b, "' data-url='");
            cwist_sstring_append(b, asset_url);
            cwist_sstring_append(b, "' data-mode='attachment'>");
            cwist_sstring_append(b, "<div class='media-thumb'><span style='font-size:22px'>FILE</span></div>");
            cwist_sstring_append(b, "<div class='media-info'><div class='media-name'>");
            cwist_sstring_append_escaped(b, fname->valuestring);
            cwist_sstring_append(b, "</div><div class='media-status'>Attached to this post</div><div class='media-progress-bar'><div class='media-progress-inner'></div></div></div>");
            cwist_sstring_append(b, "<div class='media-actions'>");
            cwist_sstring_append(b, "<button type='button' class='btn media-inline-btn'>Inline</button>");
            cwist_sstring_append(b, "<button type='button' class='btn btn-outline media-attachment-btn'>Attachment</button>");
            cwist_sstring_append(b, "<button type='button' class='btn btn-outline media-delete-btn'>Delete</button>");
            cwist_sstring_append(b, "</div></div>");
        }
        cwist_sstring_append(b, "</div></div>");
    } else {
        cwist_sstring_append(b, "<div id='upload-preview' style='margin-top:18px;display:grid;gap:10px'></div>");
    }
    cwist_sstring_append(b, "<div id='upload-dropzone' style='margin-top:18px;padding:18px;border:1px dashed var(--border);border-radius:0;background:var(--panel)'>");
    cwist_sstring_append(b, "<div style='font-weight:600'>Attach files through the streamed transfer path</div>");
    cwist_sstring_append(b, "<small style='color:var(--muted);display:block;margin-top:6px'>Files upload immediately, resume automatically, and are attached explicitly on save.</small>");
    cwist_sstring_append(b, "<input id='file-input' type='file' multiple style='display:none'><label for='file-input' class='btn' style='margin-top:12px;display:inline-block;cursor:pointer'>Select Files...</label>");
    cwist_sstring_append(b, "</div>");

    cwist_sstring_append(b, "<div style='margin-top:12px;display:flex;gap:10px'><button type='submit' class='btn'>Save</button>");
    cwist_sstring_append(b, "<a href='/' class='btn btn-outline'>Cancel</a></div>");
    cwist_sstring_append(b, "</form></div>");
    cwist_sstring_append(b, "<script>window.BLOG_USE_TASFA=true;</script>");
    cwist_sstring_append(b, "<script src='/assets/js/editor.js'></script>");

    cwist_sstring *page = render_page(post ? "Edit Post" : "New Post", b->data, dark, user_role, profile_pic);
    cwist_sstring_destroy(b);
    return page;
}

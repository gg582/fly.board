#define _POSIX_C_SOURCE 200809L
#include "render.h"
#include "config/config.h"
#include <cwist/core/sstring/sstring.h>
#include <string.h>
#include <stdio.h>

static void render_comment_node(cwist_sstring *b, cJSON *comment, cJSON *all_comments, int depth) {
    cJSON *cid = cJSON_GetObjectItem(comment, "id");
    cJSON *content = cJSON_GetObjectItem(comment, "content");
    cJSON *username = cJSON_GetObjectItem(comment, "username");
    cJSON *date = cJSON_GetObjectItem(comment, "created_at");
    cJSON *deleted = cJSON_GetObjectItem(comment, "deleted");
    int margin = depth * 20;
    cwist_sstring_append(b, "<div style='margin-left:");
    char mbuf[32]; snprintf(mbuf, sizeof(mbuf), "%d", margin);
    cwist_sstring_append(b, mbuf);
    cwist_sstring_append(b, "px;border-left:2px solid var(--border);padding-left:12px;margin-top:12px'>");
    cwist_sstring_append(b, "<div style='font-size:13px;color:var(--muted);margin-bottom:4px'>");
    cwist_sstring_append_escaped(b, username && username->valuestring ? username->valuestring : "unknown");
    cwist_sstring_append(b, " &middot; ");
    cwist_sstring_append_escaped(b, date && date->valuestring ? date->valuestring : "");
    cwist_sstring_append(b, "</div>");
    if (deleted && deleted->valueint) {
        cwist_sstring_append(b, "<p style='color:var(--muted);font-style:italic'>Deleted comment</p>");
    } else {
        cwist_sstring_append(b, "<p>");
        cwist_sstring_append_escaped(b, content && content->valuestring ? content->valuestring : "");
        cwist_sstring_append(b, "</p>");
    }
    cwist_sstring_append(b, "</div>");

    /* Find children */
    if (all_comments && cid) {
        int n = cJSON_GetArraySize(all_comments);
        for (int i = 0; i < n; i++) {
            cJSON *c = cJSON_GetArrayItem(all_comments, i);
            cJSON *parent = cJSON_GetObjectItem(c, "parent_id");
            if (parent && parent->valueint == cid->valueint) {
                render_comment_node(b, c, all_comments, depth + 1);
            }
        }
    }
}

cwist_sstring *render_post_list(cJSON *posts, cJSON *boards, bool dark, const char *user_role, int page, int total_pages, const char *board_slug, const char *search, const char *profile_pic) {
    cwist_sstring *b = cwist_sstring_create();
    if (!board_slug || board_slug[0] == '\0') {
        if (!board_slug) {
            cwist_sstring_assign(b, "<div class='hero'><img class='hero-logo' src='/img/logo.png' alt='Logo'><h1>");
            cwist_sstring_append_escaped(b, g_config.title);
            cwist_sstring_append(b, "</h1><p>");
            cwist_sstring_append_escaped(b, g_config.subtitle);
            cwist_sstring_append(b, "</p></div>");
        } else {
            cwist_sstring_assign(b, "<div class='hero'><h1>All Boards</h1></div>");
        }

        /* Board chips */
        if (boards && cJSON_GetArraySize(boards) > 0) {
            cwist_sstring_append(b, "<div style='margin-bottom:18px'>");
            int n = cJSON_GetArraySize(boards);
            for (int i = 0; i < n; i++) {
                cJSON *bo = cJSON_GetArrayItem(boards, i);
                cJSON *slug = cJSON_GetObjectItem(bo, "slug");
                cJSON *name = cJSON_GetObjectItem(bo, "name");
                cwist_sstring_append(b, "<a class='tag' href='/board/");
                cwist_sstring_append(b, slug->valuestring);
                cwist_sstring_append(b, "'>");
                cwist_sstring_append_escaped(b, name->valuestring);
                cwist_sstring_append(b, "</a>");
            }
            cwist_sstring_append(b, "</div>");
        }
    }

    if (user_role && user_role[0]) {
        cwist_sstring_append(b, "<div style='margin-bottom:18px'><a href='/post/new' class='btn'>New Post</a></div>");
    }

    /* Search */
    cwist_sstring_append(b, "<form method='get' style='margin-bottom:18px;display:flex;gap:8px;max-width:480px'>");
    cwist_sstring_append(b, "<input type='text' name='search' placeholder='Search posts...' value='");
    if (search && search[0]) cwist_sstring_append_escaped(b, search);
    cwist_sstring_append(b, "' style='flex:1'>");
    cwist_sstring_append(b, "<button type='submit' class='btn'>Search</button>");
    if (search && search[0]) {
        cwist_sstring_append(b, "<a href='");
        if (board_slug) { cwist_sstring_append(b, "/board/"); cwist_sstring_append(b, board_slug); }
        cwist_sstring_append(b, "' class='btn btn-outline'>Clear</a>");
    }
    cwist_sstring_append(b, "</form>");

    /* Modern card list view */
    cwist_sstring_append(b, "<div class='post-list'>");

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
            cwist_sstring_append(b, "<div class='post-row'>");
            cwist_sstring_append(b, "<div class='post-row-head'>");
            cJSON *is_notice = cJSON_GetObjectItem(p, "is_notice");
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
                    cwist_sstring_append(b, "<span class='post-badge'>&#128100; <a href='/user/");
                    cwist_sstring_append(b, uid_buf);
                    cwist_sstring_append(b, "'>");
                    cwist_sstring_append_escaped(b, author->valuestring);
                    cwist_sstring_append(b, "</a></span>");
                } else {
                    cwist_sstring_append(b, "<span class='post-badge'>&#128100; ");
                    cwist_sstring_append_escaped(b, author->valuestring);
                    cwist_sstring_append(b, "</span>");
                }
            } else {
                cwist_sstring_append(b, "<span class='post-badge'>&#128100; unknown</span>");
            }
            cwist_sstring_append(b, "<span class='post-badge'>&#128065; ");
            char vbuf[32]; snprintf(vbuf, sizeof(vbuf), "%d", views ? views->valueint : 0);
            cwist_sstring_append(b, vbuf);
            cwist_sstring_append(b, " views</span>");
            cwist_sstring_append(b, "<span class='post-badge'>&#128197; ");
            cwist_sstring_append_escaped(b, date && date->valuestring ? date->valuestring : "");
            cwist_sstring_append(b, "</span>");
            cwist_sstring_append(b, "</div>");
            cwist_sstring_append(b, "</div>");
        }
    } else {
        cwist_sstring_append(b, "<p style='color:var(--muted);text-align:center;padding:40px 0'>No posts found.</p>");
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
                cwist_sstring_append(b, "?search="); cwist_sstring_append_escaped(b, search); cwist_sstring_append(b, "&page=");
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
                cwist_sstring_append(b, "?search="); cwist_sstring_append_escaped(b, search); cwist_sstring_append(b, "&page=");
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

cwist_sstring *render_post_detail(cJSON *post, cJSON *files, cJSON *comments, bool dark, const char *user_role, bool pqc_verified, int vote_up, int vote_down, int user_vote, const char *profile_pic) {
    cwist_sstring *b = cwist_sstring_create();
    cJSON *title = cJSON_GetObjectItem(post, "title");
    cJSON *content = cJSON_GetObjectItem(post, "content");
    cJSON *date = cJSON_GetObjectItem(post, "created_at");
    cJSON *author = cJSON_GetObjectItem(post, "author_name");
    cJSON *pid = cJSON_GetObjectItem(post, "id");
    cJSON *view_count = cJSON_GetObjectItem(post, "view_count");
    char pid_buf[32]; snprintf(pid_buf, sizeof(pid_buf), "%d", pid ? pid->valueint : 0);

    cwist_sstring_append(b, "<article>");
    cwist_sstring_append(b, "<div style='margin-bottom:10px'>");
    if (pqc_verified) {
        cwist_sstring_append(b, "<span style='color:var(--accent);font-size:13px;font-weight:700'>&#128274; PQC Verified</span>");
    }
    cwist_sstring_append(b, "</div>");
    cwist_sstring_append(b, "<h1 style='margin-bottom:6px'>");
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
    cwist_sstring_append(b, "<button id='vote-up' class='btn btn-outline' style='padding:6px 12px;font-size:13px'>&#9650; ");
    char vup[32]; snprintf(vup, sizeof(vup), "%d", vote_up);
    cwist_sstring_append(b, vup);
    cwist_sstring_append(b, "</button>");
    cwist_sstring_append(b, "<button id='vote-down' class='btn btn-outline' style='padding:6px 12px;font-size:13px'>&#9660; ");
    char vdown[32]; snprintf(vdown, sizeof(vdown), "%d", vote_down);
    cwist_sstring_append(b, vdown);
    cwist_sstring_append(b, "</button>");
    cwist_sstring_append(b, "<span id='vote-msg' style='color:var(--muted);font-size:13px'></span>");
    cwist_sstring_append(b, "</div>");
    cwist_sstring_append(b, "<script>");
    cwist_sstring_append(b, "(function(){");
    cwist_sstring_append(b, "var pid="); cwist_sstring_append(b, pid_buf); cwist_sstring_append(b, ";");
    cwist_sstring_append(b, "function sendVote(vt){");
    cwist_sstring_append(b, "fetch('/post/vote',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'post_id='+pid+'&vote_type='+vt})");
    cwist_sstring_append(b, ".then(function(r){return r.json();}).then(function(d){");
    cwist_sstring_append(b, "if(d.ok){document.getElementById('vote-up').innerHTML='&#9650; '+d.up;document.getElementById('vote-down').innerHTML='&#9660; '+d.down;}");
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
            cwist_sstring_append(b, "<h3 style='margin-top:32px'>Attachments</h3><ul>");
            for (int i = 0; i < n; i++) {
                cJSON *f = cJSON_GetArrayItem(files, i);
                cJSON *fname = cJSON_GetObjectItem(f, "filename");
                if (!fname || !fname->valuestring || fname->valuestring[0] == '\0') continue;
                cJSON *fid = cJSON_GetObjectItem(f, "id");
                char fid_buf2[32];
                snprintf(fid_buf2, sizeof(fid_buf2), "%d", fid->valueint);
                cwist_sstring_append(b, "<li><a href='/file/");
                cwist_sstring_append(b, fid_buf2);
                cwist_sstring_append(b, "'>");
                cwist_sstring_append_escaped(b, fname->valuestring);
                cwist_sstring_append(b, "</a></li>");
            }
            cwist_sstring_append(b, "</ul>");
        }
    }

    cwist_sstring_append(b, "</article>");

    /* Actions */
    cwist_sstring_append(b, "<div style='margin-top:24px;display:flex;gap:10px;flex-wrap:wrap'>");
    cwist_sstring_append(b, "<a href='/' class='btn btn-outline'>Back</a>");
    if (user_role && user_role[0]) {
        cwist_sstring_append(b, "<a href='/post/");
        cwist_sstring_append(b, pid_buf);
        cwist_sstring_append(b, "/edit' class='btn'>Edit</a>");
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
                render_comment_node(b, c, comments, 0);
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

cwist_sstring *render_post_editor(cJSON *boards, cJSON *post, bool dark, const char *user_role, const char *error, const char *profile_pic) {
    cwist_sstring *b = cwist_sstring_create();
    cwist_sstring_assign(b, "<div class='card' style='max-width:720px;margin:0 auto'>");
    if (error && error[0]) {
        cwist_sstring_append(b, "<div class='alert'>");
        cwist_sstring_append_escaped(b, error);
        cwist_sstring_append(b, "</div>");
    }
    const char *action = post ? "/post/edit" : "/post/new";
    cwist_sstring_append(b, "<form action='");
    cwist_sstring_append(b, action);
    cwist_sstring_append(b, "' method='post' enctype='multipart/form-data'>");

    if (post) {
        cJSON *pid = cJSON_GetObjectItem(post, "id");
        char pid_buf[32];
        snprintf(pid_buf, sizeof(pid_buf), "%d", pid->valueint);
        cwist_sstring_append(b, "<input type='hidden' name='id' value='");
        cwist_sstring_append(b, pid_buf);
        cwist_sstring_append(b, "'>");
    }

    cwist_sstring_append(b, "<label>Title</label><input name='title' value='");
    if (post) {
        cJSON *t = cJSON_GetObjectItem(post, "title");
        cwist_sstring_append_escaped(b, t->valuestring);
    }
    cwist_sstring_append(b, "' required>");

    cwist_sstring_append(b, "<label>Board</label><select name='board_id'>");
    if (boards) {
        int n = cJSON_GetArraySize(boards);
        for (int i = 0; i < n; i++) {
            cJSON *bo = cJSON_GetArrayItem(boards, i);
            cJSON *bid = cJSON_GetObjectItem(bo, "id");
            cJSON *bname = cJSON_GetObjectItem(bo, "name");
            char bid_buf[32];
            snprintf(bid_buf, sizeof(bid_buf), "%d", bid->valueint);
            cwist_sstring_append(b, "<option value='");
            cwist_sstring_append(b, bid_buf);
            cwist_sstring_append(b, "'>");
            cwist_sstring_append_escaped(b, bname->valuestring);
            cwist_sstring_append(b, "</option>");
        }
    }
    cwist_sstring_append(b, "</select>");

    cwist_sstring_append(b, "<label>Summary</label><input name='summary' value='");
    if (post) {
        cJSON *s = cJSON_GetObjectItem(post, "summary");
        cwist_sstring_append_escaped(b, s && s->valuestring[0] ? s->valuestring : "");
    }
    cwist_sstring_append(b, "'>");

    cwist_sstring_append(b, "<label>Content (Markdown)</label>");
    cwist_sstring_append(b, "<div style='display:flex;gap:12px;align-items:flex-start;flex-wrap:wrap;'>");
    cwist_sstring_append(b, "<div style='flex:1;min-width:300px;'>");
    cwist_sstring_append(b, "<textarea id='md-editor' name='content' rows='18' style='width:100%;font-family:monospace;font-size:15px;' required>");
    if (post) {
        cJSON *c = cJSON_GetObjectItem(post, "content");
        cwist_sstring_append_escaped(b, c->valuestring);
    }
    cwist_sstring_append(b, "</textarea></div>");
    cwist_sstring_append(b, "<div style='flex:1;min-width:300px;' class='card'><div id='md-preview' style='padding:12px;min-height:360px;overflow:auto;'>");
    cwist_sstring_append(b, "<p style='color:var(--muted)'>Preview will appear here...</p>");
    cwist_sstring_append(b, "</div></div>");
    cwist_sstring_append(b, "<div style='width:260px;flex-shrink:0;' class='card'><div style='padding:12px;'>");
    cwist_sstring_append(b, "<h4 style='margin-top:0;font-size:14px'>Markdown Guide</h4>");
    cwist_sstring_append(b, "<pre style='font-size:12px;background:var(--code-bg);padding:8px;border-radius:6px;overflow:auto;white-space:pre-wrap;word-break:break-word;'>");
    cwist_sstring_append(b, "# Heading\n");
    cwist_sstring_append(b, "## Subheading\n");
    cwist_sstring_append(b, "**bold**  *italic*\n");
    cwist_sstring_append(b, "`inline code`\n\n");
    cwist_sstring_append(b, "```c\n");
    cwist_sstring_append(b, "int main() {}\n");
    cwist_sstring_append(b, "```\n\n");
    cwist_sstring_append(b, "[link](url)\n");
    cwist_sstring_append(b, "![img](url)\n");
    cwist_sstring_append(b, "- list item\n");
    cwist_sstring_append(b, "> quote\n");
    cwist_sstring_append(b, "</pre></div></div></div>");
    cwist_sstring_append(b, "<script>");
    cwist_sstring_append(b, "(function(){");
    cwist_sstring_append(b, "var ta=document.getElementById('md-editor');");
    cwist_sstring_append(b, "var preview=document.getElementById('md-preview');");
    cwist_sstring_append(b, "var timer;");
    cwist_sstring_append(b, "function update(){");
    cwist_sstring_append(b, "fetch('/api/preview',{method:'POST',headers:{'Content-Type':'text/plain'},body:ta.value})");
    cwist_sstring_append(b, ".then(function(r){return r.text();})");
    cwist_sstring_append(b, ".then(function(html){preview.innerHTML=html;});}");
    cwist_sstring_append(b, "ta.addEventListener('input',function(){clearTimeout(timer);timer=setTimeout(update,300);});");
    cwist_sstring_append(b, "if(ta.value)update();");
    cwist_sstring_append(b, "var form=ta.closest('form');");
    cwist_sstring_append(b, "if(form){");
    cwist_sstring_append(b, "form.addEventListener('submit',function(e){");
    cwist_sstring_append(b, "e.preventDefault();");
    cwist_sstring_append(b, "var fd=new FormData(form);");
    cwist_sstring_append(b, "fetch(form.action,{method:'POST',body:fd})");
    cwist_sstring_append(b, ".then(function(r){");
    cwist_sstring_append(b, "if(r.redirected||(r.status>=200&&r.status<400)){window.location.href=r.url||'/';}");
    cwist_sstring_append(b, "else{r.text().then(function(html){document.open();document.write(html);document.close();});}");
    cwist_sstring_append(b, "});");
    cwist_sstring_append(b, "});");
    cwist_sstring_append(b, "}");
    cwist_sstring_append(b, "ta.addEventListener('paste',function(e){");
    cwist_sstring_append(b, "var items=e.clipboardData.items;var files=[];");
    cwist_sstring_append(b, "for(var i=0;i<items.length;i++){if(items[i].kind==='file') files.push(items[i].getAsFile());}");
    cwist_sstring_append(b, "if(files.length===0) return;");
    cwist_sstring_append(b, "if(!confirm('Paste as media link?')) return;");
    cwist_sstring_append(b, "e.preventDefault();");
    cwist_sstring_append(b, "files.forEach(function(file){");
    cwist_sstring_append(b, "var fd=new FormData();fd.append('file',file);");
    cwist_sstring_append(b, "fetch('/api/upload',{method:'POST',body:fd})");
    cwist_sstring_append(b, ".then(function(r){return r.json();})");
    cwist_sstring_append(b, ".then(function(data){");
    cwist_sstring_append(b, "if(!data.ok) return;");
    cwist_sstring_append(b, "var url=data.url;var md='';");
    cwist_sstring_append(b, "if(data.mime_type && data.mime_type.indexOf('image/')===0){");
    cwist_sstring_append(b, "md='!['+data.filename+']('+url+')';");
    cwist_sstring_append(b, "}else if(data.mime_type && data.mime_type.indexOf('video/')===0){");
    cwist_sstring_append(b, "md='<video controls src=\"'+url+'\" style=\"max-width:100%\"></video>';");
    cwist_sstring_append(b, "}else if(data.mime_type && data.mime_type.indexOf('audio/')===0){");
    cwist_sstring_append(b, "md='<audio controls src=\"'+url+'\"></audio>';");
    cwist_sstring_append(b, "}else{");
    cwist_sstring_append(b, "md='['+data.filename+']('+url+')';");
    cwist_sstring_append(b, "}");
    cwist_sstring_append(b, "var start=ta.selectionStart;var end=ta.selectionEnd;var text=ta.value;");
    cwist_sstring_append(b, "ta.value=text.substring(0,start)+md+text.substring(end);");
    cwist_sstring_append(b, "ta.selectionStart=ta.selectionEnd=start+md.length;");
    cwist_sstring_append(b, "ta.dispatchEvent(new Event('input'));");
    cwist_sstring_append(b, "});");
    cwist_sstring_append(b, "});");
    cwist_sstring_append(b, "});");
    cwist_sstring_append(b, "})();");
    cwist_sstring_append(b, "</script>");

    cwist_sstring_append(b, "<label>Attachments</label><input type='file' name='attachments' multiple>");
    cwist_sstring_append(b, "<small style='color:var(--muted)'>Small files stored in DB; large files go to volume.</small>");

    cwist_sstring_append(b, "<div style='margin-top:12px;display:flex;gap:10px'><button type='submit' class='btn'>Save</button>");
    cwist_sstring_append(b, "<a href='/' class='btn btn-outline'>Cancel</a></div>");
    cwist_sstring_append(b, "</form></div>");

    cwist_sstring *page = render_page(post ? "Edit Post" : "New Post", b->data, dark, user_role, profile_pic);
    cwist_sstring_destroy(b);
    return page;
}

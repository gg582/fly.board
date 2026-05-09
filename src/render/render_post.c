#define _POSIX_C_SOURCE 200809L
#include "render.h"
#include <cwist/core/sstring/sstring.h>
#include <string.h>
#include <stdio.h>

cwist_sstring *render_post_list(cJSON *posts, cJSON *boards, bool dark, const char *user_role, int page, int total_pages, const char *board_slug, const char *profile_pic) {
    cwist_sstring *b = cwist_sstring_create();
    cwist_sstring_assign(b, "<div class='hero'><img class='hero-logo' src='/img/logo.png' alt='Logo'><h1>");
    cwist_sstring_append_escaped(b, g_config.title);
    cwist_sstring_append(b, "</h1><p>");
    cwist_sstring_append_escaped(b, g_config.subtitle);
    cwist_sstring_append(b, "</p></div>");

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

    if (user_role && user_role[0]) {
        cwist_sstring_append(b, "<div style='margin-bottom:18px'><a href='/post/new' class='btn'>New Post</a></div>");
    }

    /* Table view for scalability */
    cwist_sstring_append(b, "<div class='card' style='overflow-x:auto'><table style='width:100%;border-collapse:collapse'>");
    cwist_sstring_append(b, "<thead><tr style='border-bottom:2px solid var(--border)'>");
    cwist_sstring_append(b, "<th style='text-align:left;padding:10px'>Title</th>");
    cwist_sstring_append(b, "<th style='text-align:left;padding:10px'>Author</th>");
    cwist_sstring_append(b, "<th style='text-align:left;padding:10px'>Date</th>");
    cwist_sstring_append(b, "</tr></thead><tbody>");

    if (posts) {
        int n = cJSON_GetArraySize(posts);
        for (int i = 0; i < n; i++) {
            cJSON *p = cJSON_GetArrayItem(posts, i);
            cJSON *slug = cJSON_GetObjectItem(p, "slug");
            cJSON *title = cJSON_GetObjectItem(p, "title");
            cJSON *author = cJSON_GetObjectItem(p, "author_name");
            cJSON *date = cJSON_GetObjectItem(p, "created_at");
            cwist_sstring_append(b, "<tr style='border-bottom:1px solid var(--border)'>");
            cwist_sstring_append(b, "<td style='padding:10px'><a href='/post/");
            cwist_sstring_append(b, slug->valuestring);
            cwist_sstring_append(b, "'>");
            cwist_sstring_append_escaped(b, title->valuestring);
            cwist_sstring_append(b, "</a></td>");
            cwist_sstring_append(b, "<td style='padding:10px;color:var(--muted)'>");
            if (author && author->valuestring) {
                cJSON *author_id = cJSON_GetObjectItem(p, "user_id");
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
            cwist_sstring_append(b, "</td>");
            cwist_sstring_append(b, "<td style='padding:10px;color:var(--muted);font-size:13px'>");
            cwist_sstring_append_escaped(b, date->valuestring);
            cwist_sstring_append(b, "</td>");
            cwist_sstring_append(b, "</tr>");
        }
    }

    cwist_sstring_append(b, "</tbody></table></div>");

    /* Pagination */
    if (total_pages > 1) {
        cwist_sstring_append(b, "<div style='margin-top:18px;display:flex;gap:8px;justify-content:center;align-items:center'>");
        if (page > 1) {
            cwist_sstring_append(b, "<a class='btn btn-outline' href='");
            if (board_slug) {
                cwist_sstring_append(b, "/board/");
                cwist_sstring_append(b, board_slug);
            }
            cwist_sstring_append(b, "?page=");
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
            cwist_sstring_append(b, "?page=");
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

cwist_sstring *render_post_detail(cJSON *post, cJSON *files, cJSON *comments, bool dark, const char *user_role, bool pqc_verified, const char *profile_pic) {
    (void)comments;
    cwist_sstring *b = cwist_sstring_create();
    cJSON *title = cJSON_GetObjectItem(post, "title");
    cJSON *content = cJSON_GetObjectItem(post, "content");
    cJSON *date = cJSON_GetObjectItem(post, "created_at");
    cJSON *author = cJSON_GetObjectItem(post, "author_name");
    (void)post; /* board_id available if needed */

    cwist_sstring_append(b, "<article>");
    cwist_sstring_append(b, "<div style='margin-bottom:10px'>");
    if (pqc_verified) {
        cwist_sstring_append(b, "<span style='color:var(--accent);font-size:13px;font-weight:700'>&#128274; PQC Verified</span>");
    } else {
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
    cwist_sstring_append(b, "</p>");

    cwist_sstring *md_html = render_markdown_to_html(content->valuestring);
    cwist_sstring_append(b, "<div class='markdown-body'>");
    if (md_html) {
        cwist_sstring_append_sstring(b, md_html);
        cwist_sstring_destroy(md_html);
    }
    cwist_sstring_append(b, "</div>");

    /* Files */
    if (files && cJSON_GetArraySize(files) > 0) {
        cwist_sstring_append(b, "<h3 style='margin-top:32px'>Attachments</h3><ul>");
        int n = cJSON_GetArraySize(files);
        for (int i = 0; i < n; i++) {
            cJSON *f = cJSON_GetArrayItem(files, i);
            cJSON *fid = cJSON_GetObjectItem(f, "id");
            cJSON *fname = cJSON_GetObjectItem(f, "filename");
            char fid_buf[32];
            snprintf(fid_buf, sizeof(fid_buf), "%d", fid->valueint);
            cwist_sstring_append(b, "<li><a href='/file/");
            cwist_sstring_append(b, fid_buf);
            cwist_sstring_append(b, "'>");
            cwist_sstring_append_escaped(b, fname->valuestring);
            cwist_sstring_append(b, "</a></li>");
        }
        cwist_sstring_append(b, "</ul>");
    }

    cwist_sstring_append(b, "</article>");

    /* Actions */
    cwist_sstring_append(b, "<div style='margin-top:24px;display:flex;gap:10px;flex-wrap:wrap'>");
    cwist_sstring_append(b, "<a href='/' class='btn btn-outline'>Back</a>");
    if (user_role && user_role[0]) {
        cJSON *pid = cJSON_GetObjectItem(post, "id");
        char pid_buf[32];
        snprintf(pid_buf, sizeof(pid_buf), "%d", pid->valueint);
        cwist_sstring_append(b, "<a href='/post/");
        cwist_sstring_append(b, pid_buf);
        cwist_sstring_append(b, "/edit' class='btn'>Edit</a>");
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
    cwist_sstring_append(b, "<div style='display:flex;gap:12px;align-items:flex-start;'>");
    cwist_sstring_append(b, "<div style='flex:1;'>");
    cwist_sstring_append(b, "<textarea id='md-editor' name='content' rows='18' style='width:100%;font-family:monospace;font-size:15px;' required>");
    if (post) {
        cJSON *c = cJSON_GetObjectItem(post, "content");
        cwist_sstring_append_escaped(b, c->valuestring);
    }
    cwist_sstring_append(b, "</textarea></div>");
    cwist_sstring_append(b, "<div style='flex:1;' class='card'><div id='md-preview' style='padding:12px;min-height:360px;overflow:auto;'>");
    cwist_sstring_append(b, "<p style='color:var(--muted)'>Preview will appear here...</p>");
    cwist_sstring_append(b, "</div></div></div>");
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

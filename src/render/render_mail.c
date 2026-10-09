#define _POSIX_C_SOURCE 200809L
/**
 * @file render_mail.c
 * @brief Webmail pages rendered inside the standard render_page() shell.
 */
#include "render.h"
#include "render_internal.h"
#include "db/db.h"
#include <cwist/core/sstring/sstring.h>
#include <stdio.h>
#include <string.h>

static void append_mail_css(cwist_sstring *b) {
    cwist_sstring_append(b,
        "<style>"
        ".mail-tabs{display:flex;gap:8px;margin:16px 0;flex-wrap:wrap}"
        ".mail-tabs a{padding:6px 14px;border:1px solid var(--border,#8884);border-radius:6px;text-decoration:none}"
        ".mail-tabs a.active{font-weight:700;border-color:var(--accent,#48f)}"
        ".mail-badge{background:#c33;color:#fff;border-radius:10px;padding:0 7px;font-size:12px;margin-left:5px}"
        ".mail-table{width:100%;border-collapse:collapse;margin:12px 0}"
        ".mail-table td,.mail-table th{padding:8px 10px;border-bottom:1px solid var(--border,#8882);text-align:left}"
        ".mail-unread{font-weight:700}"
        ".mail-body{white-space:normal;margin:16px 0;line-height:1.5}"
        ".mail-form label{display:block;margin:10px 0 4px;font-weight:600}"
        ".mail-form input[type=text],.mail-form textarea{width:100%;padding:8px;border:1px solid var(--border,#8884);border-radius:6px;background:transparent;color:inherit}"
        /* Mobile: keep the mailbox readable on narrow screens - let long
         * addresses/subjects wrap, shrink the action buttons, and let the
         * body text break anywhere so no horizontal scroll appears. */
        "@media(max-width:640px){"
        ".mail-table td,.mail-table th{padding:6px 4px;font-size:13px}"
        ".mail-table td{overflow-wrap:anywhere;word-break:break-word}"
        ".mail-table td:last-child{white-space:nowrap}"
        ".mail-table .btn{padding:2px 5px;font-size:11px}"
        ".mail-body{overflow-wrap:anywhere;word-break:break-word}"
        "}"
        "</style>");
}

static void append_msg_banner(cwist_sstring *b, const char *msg) {
    if (!msg || !msg[0]) return;
    const char *text = msg;
    char buf[512];
    if (!strcmp(msg, "sent")) text = "Message sent.";
    else if (!strcmp(msg, "send_invalid")) text = "Missing fields: recipient, subject and body are required.";
    else if (!strcmp(msg, "send_no_local")) text = "No local recipients matched; nothing was delivered.";
    else if (!strcmp(msg, "send_partial")) text = "Message saved, but delivery to some external addresses failed:";
    else if (!strcmp(msg, "deleted")) text = "Message moved to Trash.";
    else if (!strcmp(msg, "trash_emptied")) text = "Trash emptied.";
    else if (!strncmp(msg, "broadcast_", 10)) {
        if (!strcmp(msg, "broadcast_invalid")) text = "Broadcast needs both a subject and a body.";
        else text = "Broadcast delivered.";
    } else {
        snprintf(buf, sizeof(buf), "%s", msg);
        text = buf;
    }
    cwist_sstring_append(b, "<div class='alert' style='margin:12px 0'>");
    cwist_sstring_append_escaped(b, text);
    if (!strcmp(msg, "send_partial")) { /* caller passes failed list via ?failed= */ }
    cwist_sstring_append(b, "</div>");
}

cwist_sstring *render_mail_list(cJSON *emails, const char *folder, int page, int total_pages,
                                int unread, const char *msg, bool dark, const char *user_role,
                                const char *profile_pic, bool is_mobile, const char *own_addr) {
    (void)is_mobile;
    cwist_sstring *b = cwist_sstring_create();
    append_mail_css(b);
    cwist_sstring_append(b, "<div class='hero'><h1>Mail</h1></div>");

    /* Folder tabs with unread badge on INBOX. */
    cwist_sstring_append(b, "<div class='mail-tabs'>");
    const char *folders[] = {MAIL_FOLDER_INBOX, MAIL_FOLDER_SENT, MAIL_FOLDER_TRASH};
    for (size_t i = 0; i < sizeof(folders) / sizeof(folders[0]); i++) {
        cwist_sstring_append(b, "<a href='/mail?folder=");
        cwist_sstring_append(b, folders[i]);
        cwist_sstring_append(b, !strcmp(folder, folders[i]) ? "' class='active'>" : "'>");
        cwist_sstring_append(b, folders[i]);
        if (!strcmp(folders[i], MAIL_FOLDER_INBOX) && unread > 0) {
            char badge[64];
            snprintf(badge, sizeof(badge), "<span class='mail-badge'>%d</span>", unread);
            cwist_sstring_append(b, badge);
        }
        cwist_sstring_append(b, "</a>");
    }
    cwist_sstring_append(b, "</div>");

    append_msg_banner(b, msg);

    /* Compose form (not on Trash). */
    if (strcmp(folder, MAIL_FOLDER_TRASH)) {
        cwist_sstring_append(b,
            "<details class='card' style='margin:12px 0'><summary style='cursor:pointer;font-weight:600'>Compose</summary>"
            "<form class='mail-form' action='/mail/send' method='post'>"
            "<label>To (comma-separated local usernames or full addresses)</label>"
            "<input type='text' name='to' id='mail-to' autocomplete='off' required>"
            "<label>Subject</label>"
            "<input type='text' name='subject' required>"
            "<label>Body</label>"
            "<textarea name='body' rows='8' required></textarea>"
            "<button type='submit' class='btn' style='margin-top:10px'>Send</button>"
            "</form></details>"
            "<script>"
            "(function(){"
            "var i=document.getElementById('mail-to');if(!i)return;"
            "var ul=document.createElement('ul');"
            "ul.style.cssText='position:absolute;z-index:50;margin:2px 0 0;padding:4px;list-style:none;"
            "background:var(--card-bg,#fff);border:1px solid #ccc;border-radius:4px;"
            "max-height:220px;overflow-y:auto;min-width:180px;display:none;';"
            "i.parentNode.style.position='relative';"
            "i.parentNode.appendChild(ul);"
            "var t=null,seq=0,items=[],act=-1;"
            "function token(){var v=i.value;var idx=v.lastIndexOf(',');return (idx<0?v:v.slice(idx+1)).trim();}"
            "function close(){ul.style.display='none';ul.innerHTML='';items=[];act=-1;}"
            "function render(us){"
            "ul.innerHTML='';items=[];act=-1;"
            "us.forEach(function(u){"
            "var li=document.createElement('li');"
            "li.textContent=u.username;"
            "li.style.cssText='padding:4px 8px;cursor:pointer;';"
            "li.addEventListener('mousedown',function(e){e.preventDefault();pick(u.username);});"
            "ul.appendChild(li);items.push(li);});"
            "ul.style.display='block';}"
            "function pick(name){"
            "var v=i.value;var idx=v.lastIndexOf(',');"
            "i.value=(idx<0?'':v.slice(0,idx+1))+name+', ';"
            "close();i.focus();}"
            "i.addEventListener('input',function(){"
            "clearTimeout(t);"
            "var v=token();"
            "if(v.length<1||v.indexOf('@')>=0){close();return;}"
            "t=setTimeout(function(){"
            "var my=++seq;"
            "fetch('/mail/users?q='+encodeURIComponent(v))"
            ".then(function(r){return r.json();})"
            ".then(function(us){"
            "if(my!==seq)return;"
            "if(!Array.isArray(us)||!us.length){close();return;}"
            "render(us);})"
            ".catch(function(){});"
            "},200);});"
            "i.addEventListener('keydown',function(e){"
            "if(ul.style.display!=='block')return;"
            "if(e.key==='ArrowDown'||e.key==='ArrowUp'){"
            "e.preventDefault();"
            "if(!items.length)return;"
            "act=(act+(e.key==='ArrowDown'?1:-1)+items.length)%items.length;"
            "items.forEach(function(li,k){"
            "li.style.background=k===act?'#789':'transparent';"
            "li.style.color=k===act?'#fff':'';});"
            "}else if(e.key==='Enter'){"
            "if(act>=0&&items[act]){e.preventDefault();pick(items[act].textContent);}"
            "}else if(e.key==='Escape'){close();}"
            "});"
            "i.addEventListener('blur',function(){setTimeout(close,150);});"
            "})();"
            "</script>");
    }

    /* Message table. */
    cwist_sstring_append(b, "<table class='mail-table'><thead><tr><th>");
    cwist_sstring_append(b, !strcmp(folder, MAIL_FOLDER_SENT) ? "To" : "From");
    cwist_sstring_append(b, "</th><th>Subject</th><th>Date</th><th></th></tr></thead><tbody>");
    cJSON *e = NULL;
    cJSON_ArrayForEach(e, emails) {
        int id = json_int(e, "id", 0);
        int is_read = json_int(e, "is_read", 1);
        cJSON *from = cJSON_GetObjectItem(e, !strcmp(folder, MAIL_FOLDER_SENT) ? "to_addrs" : "from_addr");
        cJSON *subj = cJSON_GetObjectItem(e, "subject");
        cJSON *created = cJSON_GetObjectItem(e, "created_at");
        cwist_sstring_append(b, "<tr class='");
        cwist_sstring_append(b, is_read ? "" : "mail-unread");
        cwist_sstring_append(b, "'><td>");
        cwist_sstring_append_escaped(b, cJSON_IsString(from) ? from->valuestring : "");
        cwist_sstring_append(b, "</td><td><a href='/mail/view?id=");
        char num[16];
        snprintf(num, sizeof(num), "%d", id);
        cwist_sstring_append(b, num);
        cwist_sstring_append(b, "'>");
        cwist_sstring_append_escaped(b, cJSON_IsString(subj) ? subj->valuestring : "(no subject)");
        cwist_sstring_append(b, "</a></td><td>");
        cwist_sstring_append_escaped(b, cJSON_IsString(created) ? created->valuestring : "");
        cwist_sstring_append(b, "</td><td style='white-space:nowrap'>");
        /* Read/unread toggle is meaningless for messages the viewer sent;
         * Sent rows - and the viewer's own sent copies sitting in Trash -
         * get Delete only. */
        bool hide_toggle = !strcmp(folder, MAIL_FOLDER_SENT);
        if (!hide_toggle && !strcmp(folder, MAIL_FOLDER_TRASH) && own_addr && own_addr[0]) {
            cJSON *fa = cJSON_GetObjectItem(e, "from_addr");
            if (cJSON_IsString(fa) && strcasecmp(fa->valuestring, own_addr) == 0) hide_toggle = true;
        }
        if (!hide_toggle) {
            cwist_sstring_append(b, "<form action='/mail/read' method='post' style='display:inline'>"
                                    "<input type='hidden' name='id' value='");
            cwist_sstring_append(b, num);
            cwist_sstring_append(b, "'><input type='hidden' name='folder' value='");
            cwist_sstring_append(b, folder);
            cwist_sstring_append(b, "'><input type='hidden' name='value' value='");
            cwist_sstring_append(b, is_read ? "0'>" : "1'>");
            cwist_sstring_append(b, "<button type='submit' class='btn btn-outline' style='padding:2px 8px;font-size:12px'>");
            cwist_sstring_append(b, is_read ? "Mark unread" : "Mark read");
            cwist_sstring_append(b, "</button></form> ");
        }
        cwist_sstring_append(b, "<form action='/mail/delete' method='post' style='display:inline'>"
                                "<input type='hidden' name='id' value='");
        cwist_sstring_append(b, num);
        cwist_sstring_append(b, "'><input type='hidden' name='folder' value='");
        cwist_sstring_append(b, folder);
        cwist_sstring_append(b, "'><button type='submit' class='btn btn-outline' style='padding:2px 8px;font-size:12px'>"
                                "Delete</button></form></td></tr>");
    }
    if (!cJSON_GetArraySize(emails))
        cwist_sstring_append(b, "<tr><td colspan='4' style='text-align:center;color:var(--muted)'>No messages.</td></tr>");
    cwist_sstring_append(b, "</tbody></table>");

    /* Empty-trash button + pagination. */
    if (!strcmp(folder, MAIL_FOLDER_TRASH)) {
        cwist_sstring_append(b, "<form action='/mail/empty-trash' method='post' data-confirm='Permanently delete everything in Trash?'>"
                                "<button type='submit' class='btn btn-outline' style='color:#c00;border-color:#c00'>Empty Trash</button></form>");
    }
    if (total_pages > 1) {
        cwist_sstring_append(b, "<div style='margin:16px 0'>");
        for (int p = 1; p <= total_pages; p++) {
            char link[128];
            if (p == page)
                snprintf(link, sizeof(link), " <b>%d</b> ", p);
            else
                snprintf(link, sizeof(link), " <a href='/mail?folder=%s&page=%d'>%d</a> ", folder, p, p);
            cwist_sstring_append(b, link);
        }
        cwist_sstring_append(b, "</div>");
    }

    cwist_sstring *page_html = render_page("Mail", b->data, dark, user_role, profile_pic, is_mobile);
    cwist_sstring_destroy(b);
    return page_html;
}

cwist_sstring *render_mail_view(cJSON *email, bool dark, const char *user_role,
                                const char *profile_pic, bool is_mobile) {
    cwist_sstring *b = cwist_sstring_create();
    append_mail_css(b);
    int id = json_int(email, "id", 0);
    const char *folder = "INBOX";
    cJSON *folder_obj = cJSON_GetObjectItem(email, "folder");
    if (cJSON_IsString(folder_obj) && folder_obj->valuestring[0]) folder = folder_obj->valuestring;

    cwist_sstring_append(b, "<div class='hero'><h1>");
    cJSON *subj = cJSON_GetObjectItem(email, "subject");
    cwist_sstring_append_escaped(b, cJSON_IsString(subj) ? subj->valuestring : "(no subject)");
    cwist_sstring_append(b, "</h1></div>");
    cwist_sstring_append(b, "<p><a href='/mail?folder=");
    cwist_sstring_append(b, folder);
    cwist_sstring_append(b, "'>&larr; Back to ");
    cwist_sstring_append(b, folder);
    cwist_sstring_append(b, "</a></p>");

    cwist_sstring_append(b, "<div class='card'><table class='mail-table'>");
    const char *rows[][2] = {
        {"From", "from_addr"}, {"To", "to_addrs"}, {"Date", "created_at"},
    };
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        cwist_sstring_append(b, "<tr><th style='width:80px'>");
        cwist_sstring_append(b, rows[i][0]);
        cwist_sstring_append(b, "</th><td>");
        cJSON *v = cJSON_GetObjectItem(email, rows[i][1]);
        cwist_sstring_append_escaped(b, cJSON_IsString(v) ? v->valuestring : "");
        cwist_sstring_append(b, "</td></tr>");
    }
    cwist_sstring_append(b, "</table>");

    /* Body: escaped, newlines become <br>. */
    cwist_sstring_append(b, "<div class='mail-body'>");
    cJSON *body = cJSON_GetObjectItem(email, "body_text");
    if (cJSON_IsString(body)) {
        const char *p = body->valuestring;
        while (*p) {
            const char *nl = strchr(p, '\n');
            if (nl) {
                cwist_sstring_append_len(b, p, (size_t)(nl - p));
                cwist_sstring_append(b, "<br>");
                p = nl + 1;
            } else {
                cwist_sstring_append_escaped(b, p);
                break;
            }
        }
    }
    cwist_sstring_append(b, "</div>");

    /* Actions: reply + delete. */
    cJSON *from = cJSON_GetObjectItem(email, "from_addr");
    const char *from_addr = cJSON_IsString(from) ? from->valuestring : "";
    cwist_sstring_append(b, "<div style='margin-top:16px;display:flex;gap:8px;flex-wrap:wrap'>");
    cwist_sstring_append(b, "<details class='card' style='flex:1;min-width:260px'><summary style='cursor:pointer;font-weight:600'>Reply</summary>"
                            "<form class='mail-form' action='/mail/send' method='post'>"
                            "<input type='hidden' name='to' value='");
    cwist_sstring_append_escaped(b, from_addr);
    cJSON *mid = cJSON_GetObjectItem(email, "message_id");
    cwist_sstring_append(b, "'><input type='hidden' name='in_reply_to' value='");
    cwist_sstring_append_escaped(b, cJSON_IsString(mid) ? mid->valuestring : "");
    cwist_sstring_append(b, "'><label>Subject</label><input type='text' name='subject' value='Re: ");
    cwist_sstring_append_escaped(b, cJSON_IsString(subj) ? subj->valuestring : "");
    cwist_sstring_append(b, "'><label>Body</label><textarea name='body' rows='8'></textarea>"
                            "<button type='submit' class='btn' style='margin-top:10px'>Send</button></form></details>");
    char num[16];
    snprintf(num, sizeof(num), "%d", id);
    cwist_sstring_append(b, "<form action='/mail/delete' method='post'>"
                            "<input type='hidden' name='id' value='");
    cwist_sstring_append(b, num);
    cwist_sstring_append(b, "'><input type='hidden' name='folder' value='");
    cwist_sstring_append(b, folder);
    cwist_sstring_append(b, "'><button type='submit' class='btn btn-outline' style='color:#c00;border-color:#c00'>Delete</button></form>");
    cwist_sstring_append(b, "</div></div>");

    cwist_sstring *page_html = render_page("Mail", b->data, dark, user_role, profile_pic, is_mobile);
    cwist_sstring_destroy(b);
    return page_html;
}

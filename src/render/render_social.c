/* Pages for password recovery and personal blocks. */
#include "render.h"
#include "render_internal.h"
#include "utils/spam_guard.h"
#include <cwist/core/sstring/sstring.h>
#include <stdio.h>
#include <string.h>

static void append_alert(cwist_sstring *b, const char *message, bool ok) {
    if (!message || !message[0]) return;
    cwist_sstring_append(b, ok ? "<div class='alert' style='border-color:var(--accent)'>" : "<div class='alert'>");
    cwist_sstring_append_escaped(b, message);
    cwist_sstring_append(b, "</div>");
}

static cwist_sstring *finish(const char *title, cwist_sstring *b, bool dark, const char *role,
                             const char *profile_pic, bool is_mobile) {
    render_page_meta meta = { .noindex = true };
    render_set_page_meta(&meta);
    cwist_sstring *page = render_page(title, b->data, dark, role, profile_pic, is_mobile);
    cwist_sstring_destroy(b);
    return page;
}

cwist_sstring *render_forgot_password(bool dark, const char *message, bool ok, bool is_mobile) {
    cwist_sstring *b = cwist_sstring_create();
    cwist_sstring_append(b, "<div class='card' style='max-width:420px;margin:40px auto;'><h2 style='margin-top:0'>Reset password</h2>");
    append_alert(b, message, ok);
    cwist_sstring_append(b,
        "<p style='color:var(--muted);font-size:14px'>Enter your username or the email address on your account. "
        "If it matches an account with an email address, a reset link valid for one hour is sent there.</p>"
        "<form action='/forgot-password' method='post'>"
        "<label>Username or email</label><input name='login' autocomplete='username' required maxlength='254'>");
    spam_guard_append_fields(b);
    cwist_sstring_append(b,
        "<button type='submit' class='btn' style='margin-top:8px;width:100%'>Send reset link</button></form>"
        "<p style='color:var(--muted);font-size:13px;margin-bottom:0'>No email on your account? Ask the site administrator for a reset link.</p>"
        "</div><p style='text-align:center'><a href='/login'>Back to login</a></p>");
    return finish("Reset password", b, dark, NULL, NULL, is_mobile);
}

cwist_sstring *render_reset_password(bool dark, const char *token, const char *error, bool is_mobile) {
    cwist_sstring *b = cwist_sstring_create();
    cwist_sstring_append(b, "<div class='card' style='max-width:420px;margin:40px auto;'><h2 style='margin-top:0'>Choose a new password</h2>");
    append_alert(b, error, false);
    cwist_sstring_append(b, "<form action='/reset-password' method='post'><input type='hidden' name='token' value='");
    cwist_sstring_append_escaped(b, token ? token : "");
    cwist_sstring_append(b,
        "'><label>New password</label><input name='new_password' type='password' autocomplete='new-password' minlength='6' required>"
        "<label>Confirm new password</label><input name='confirm_password' type='password' autocomplete='new-password' minlength='6' required>"
        "<button type='submit' class='btn' style='margin-top:8px;width:100%'>Set new password</button></form>"
        "<p style='color:var(--muted);font-size:13px;margin-bottom:0'>Every device signed in to this account is signed out.</p>"
        "</div>");
    return finish("Choose a new password", b, dark, NULL, NULL, is_mobile);
}

cwist_sstring *render_admin_reset_link(bool dark, const char *role, const char *profile_pic, const char *username,
                                       const char *link, bool mailed, bool is_mobile) {
    cwist_sstring *b = cwist_sstring_create();
    cwist_sstring_append(b, "<div class='card' style='max-width:640px;margin:40px auto;'><h2 style='margin-top:0'>Password reset link</h2><p>Reset link for <strong>");
    cwist_sstring_append_escaped(b, username ? username : "");
    cwist_sstring_append(b, "</strong>. It works once and expires in 24 hours; a newer link replaces it.</p>");
    if (mailed) cwist_sstring_append(b, "<p>It was also emailed to the address on the account.</p>");
    cwist_sstring_append(b, "<input readonly style='width:100%;font-family:var(--font-mono,monospace)' onfocus='this.select()' value='");
    cwist_sstring_append_escaped(b, link ? link : "");
    cwist_sstring_append(b, "'><p style='color:var(--muted);font-size:13px'>Pass it to the user over a channel you trust. "
                            "Anyone holding it can take over the account until it is used.</p>"
                            "<p><a href='/admin/users'>Back to users</a></p></div>");
    return finish("Password reset link", b, dark, role, profile_pic, is_mobile);
}

cwist_sstring *render_blocked_users(cJSON *blocks, bool dark, const char *role, const char *profile_pic,
                                    const char *message, bool is_mobile) {
    cwist_sstring *b = cwist_sstring_create();
    cwist_sstring_append(b, "<div class='card' style='max-width:640px;margin:40px auto;'><h2 style='margin-top:0'>Blocked users</h2>");
    append_alert(b, message, true);
    cwist_sstring_append(b,
        "<p style='color:var(--muted);font-size:14px'>You do not see their posts in lists, their comments and guestbook "
        "entries are collapsed, and they cannot mail you, write in your guestbook or trigger your notifications. "
        "They are not told about the block.</p>");
    int n = cJSON_IsArray(blocks) ? cJSON_GetArraySize(blocks) : 0;
    if (n == 0) {
        cwist_sstring_append(b, "<p>You have not blocked anyone.</p>");
    } else {
        cwist_sstring_append(b, "<ul style='list-style:none;padding:0;margin:0'>");
        cJSON *row = NULL;
        cJSON_ArrayForEach(row, blocks) {
            int id = json_int(row, "blocked_id", 0);
            const char *name = json_string_or_empty(row, "nickname");
            if (!name[0]) name = json_string_or_empty(row, "username");
            if (!name[0]) name = "Deleted user";
            char buf[400];
            cwist_sstring_append(b, "<li style='display:flex;gap:12px;align-items:center;justify-content:space-between;padding:8px 0;border-bottom:1px solid var(--border)'>");
            snprintf(buf, sizeof(buf), "<a href='/user/%d'>", id);
            cwist_sstring_append(b, buf);
            cwist_sstring_append_escaped(b, name);
            cwist_sstring_append(b, "</a><span style='color:var(--muted);font-size:13px'>");
            cwist_sstring_append_escaped(b, json_string_or_empty(row, "created_at"));
            snprintf(buf, sizeof(buf),
                     "</span><form class='block-form' action='/user/%d/block' method='post' style='margin:0'><input type='hidden' name='action' value='unblock'>"
                     "<input type='hidden' name='return' value='/account/blocks'>", id);
            cwist_sstring_append(b, buf);
            cwist_sstring_append(b, "<button type='submit' class='btn btn-outline' style='font-size:12px;padding:4px 10px'>Unblock</button></form></li>");
        }
        cwist_sstring_append(b, "</ul>");
        cwist_sstring_append(b, "<script>(function(){if(!window.fetch)return;document.querySelectorAll('form.block-form').forEach(function(f){f.addEventListener('submit',function(e){e.preventDefault();var btn=f.querySelector('button'),a=f.elements.action;if(btn.dataset.confirm&&a.value==='block'&&!window.confirm(btn.dataset.confirm))return;btn.disabled=true;fetch(f.getAttribute(\'action\'),{method:'POST',credentials:'same-origin',headers:{'Content-Type':'application/x-www-form-urlencoded','X-Requested-With':'fetch'},body:'action='+a.value}).then(function(r){return r.json();}).then(function(d){if(!d.ok)return;var li=f.closest('li');if(li&&!d.blocked){li.parentNode.removeChild(li);return;}a.value=d.blocked?'unblock':'block';btn.textContent=d.blocked?'Unblock':'Block user';var note=f.querySelector('.block-note');if(note)note.style.display=d.blocked?'':'none';}).catch(function(){}).then(function(){btn.disabled=false;});},true);});})();</script>");
    }
    cwist_sstring_append(b, "<p><a href='/account/settings'>Back to settings</a></p></div>");
    return finish("Blocked users", b, dark, role, profile_pic, is_mobile);
}

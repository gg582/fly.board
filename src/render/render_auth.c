#define _POSIX_C_SOURCE 200809L
#include "render.h"
#include "render_internal.h"
#include <cwist/core/sstring/sstring.h>
#include <stdio.h>

cwist_sstring *render_login(bool dark, const char *error, bool is_mobile) {
    const char *fields =
        "<label>Username</label><input name='username' placeholder='username' required>"
        "<label>Password</label><input name='password' type='password' placeholder='password' required>";
    cwist_sstring *body = build_form("Login", "/login", "post", fields, "Login", error, dark);
    cwist_sstring_append(body, "<p style='text-align:center'><a href='/register'>Create account</a></p>");
    cwist_sstring_append(body, login_register_script);
    cwist_sstring *page = render_page("Login", body->data, dark, NULL, NULL, is_mobile);
    cwist_sstring_destroy(body);
    return page;
}

cwist_sstring *render_register(bool dark, const char *error, bool is_mobile, cJSON *legal_docs) {
    cwist_sstring *fields = cwist_sstring_create();
    cwist_sstring_append(fields,
        "<label>Username</label><input name='username' placeholder='username' required>"
        "<label>Email</label><input name='email' type='email' placeholder='email' required>"
        "<label>Password</label><input name='password' type='password' placeholder='password' required>");

    if (legal_docs && cJSON_GetArraySize(legal_docs) > 0) {
        cwist_sstring_append(fields, "<hr style='border:0;border-top:1px solid var(--border);margin:16px 0'>");
        cwist_sstring_append(fields, "<h3 style='margin:0 0 8px;font-size:16px'>Terms &amp; Conditions</h3>");
        cJSON *doc;
        cJSON_ArrayForEach(doc, legal_docs) {
            cJSON *name = cJSON_GetObjectItem(doc, "name");
            cJSON *title = cJSON_GetObjectItem(doc, "title");
            cJSON *html = cJSON_GetObjectItem(doc, "html");
            cJSON *req = cJSON_GetObjectItem(doc, "required");
            if (!name || !title || !html) continue;

            cwist_sstring_append(fields, "<div style='margin-bottom:12px'>");
            cwist_sstring_append(fields, "<div style='font-weight:600;font-size:14px;margin-bottom:4px'>");
            cwist_sstring_append_escaped(fields, title->valuestring);
            if (req && req->valueint) {
                cwist_sstring_append(fields, " <span style='color:var(--accent)'>*</span>");
            }
            cwist_sstring_append(fields, "</div>");

            cwist_sstring_append(fields,
                "<div style='max-height:160px;overflow-y:auto;border:1px solid var(--border);"
                "padding:8px;border-radius:4px;background:var(--bg)'>");
            cwist_sstring_append(fields, "<div class='markdown-body' style='font-size:13px'>");
            cwist_sstring_append(fields, html->valuestring);
            cwist_sstring_append(fields, "</div></div>");

            char checkbox[512];
            snprintf(checkbox, sizeof(checkbox),
                "<label style='display:flex;align-items:center;gap:6px;margin-top:6px;font-size:13px;cursor:pointer'>"
                "<input type='checkbox' name='legal_%s' %s>"
                "I agree"
                "</label>",
                name->valuestring,
                (req && req->valueint) ? "required" : "");
            cwist_sstring_append(fields, checkbox);
            cwist_sstring_append(fields, "</div>");
        }
    }

    cwist_sstring *body = build_form("Register", "/register", "post", fields->data, "Register", error, dark);
    cwist_sstring_append(body, "<p style='text-align:center'><a href='/login'>Already have an account?</a></p>");
    cwist_sstring_append(body, login_register_script);
    cwist_sstring *page = render_page("Register", body->data, dark, NULL, NULL, is_mobile);
    cwist_sstring_destroy(body);
    cwist_sstring_destroy(fields);
    return page;
}

cwist_sstring *render_password_change(bool dark, const char *user_role, const char *profile_pic, const char *error, bool is_mobile) {
    const char *fields =
        "<label>Current Password</label><input name='current_password' type='password' placeholder='Current password' required>"
        "<label>New Password</label><input name='new_password' type='password' placeholder='min 6 chars' required>"
        "<label>Confirm New Password</label><input name='confirm_password' type='password' placeholder='Confirm new password' required>";
    cwist_sstring *body = build_form("Change Password", "/account/password", "post", fields, "Update Password", error, dark);
    cwist_sstring_append(body, "<p style='text-align:center'><a href='/account/settings'>Back to settings</a></p>");
    cwist_sstring_append(body, login_register_script);
    cwist_sstring *page = render_page("Change Password", body->data, dark, user_role, profile_pic, is_mobile);
    cwist_sstring_destroy(body);
    return page;
}

#define _POSIX_C_SOURCE 200809L
#include "render.h"
#include "render_internal.h"
#include <cwist/core/sstring/sstring.h>

cwist_sstring *render_login(bool dark, const char *error) {
    const char *fields =
        "<label>Username</label><input name='username' placeholder='username' required>"
        "<label>Password</label><input name='password' type='password' placeholder='password' required>";
    cwist_sstring *body = build_form("Login", "/login", "post", fields, "Login", error, dark);
    cwist_sstring_append(body, "<p style='text-align:center'><a href='/register'>Create account</a></p>");
    cwist_sstring_append(body, login_register_script);
    cwist_sstring *page = render_page("Login", body->data, dark, NULL, NULL);
    cwist_sstring_destroy(body);
    return page;
}

cwist_sstring *render_register(bool dark, const char *error) {
    const char *fields =
        "<label>Username</label><input name='username' placeholder='username' required>"
        "<label>Email</label><input name='email' type='email' placeholder='email' required>"
        "<label>Password</label><input name='password' type='password' placeholder='password' required>";
    cwist_sstring *body = build_form("Register", "/register", "post", fields, "Register", error, dark);
    cwist_sstring_append(body, "<p style='text-align:center'><a href='/login'>Already have an account?</a></p>");
    cwist_sstring_append(body, login_register_script);
    cwist_sstring *page = render_page("Register", body->data, dark, NULL, NULL);
    cwist_sstring_destroy(body);
    return page;
}

cwist_sstring *render_password_change(bool dark, const char *user_role, const char *profile_pic, const char *error) {
    const char *fields =
        "<label>Current Password</label><input name='current_password' type='password' placeholder='Current password' required>"
        "<label>New Password</label><input name='new_password' type='password' placeholder='min 6 chars' required>"
        "<label>Confirm New Password</label><input name='confirm_password' type='password' placeholder='Confirm new password' required>";
    cwist_sstring *body = build_form("Change Password", "/account/password", "post", fields, "Update Password", error, dark);
    cwist_sstring_append(body, "<p style='text-align:center'><a href='/account/settings'>Back to settings</a></p>");
    cwist_sstring_append(body, login_register_script);
    cwist_sstring *page = render_page("Change Password", body->data, dark, user_role, profile_pic);
    cwist_sstring_destroy(body);
    return page;
}

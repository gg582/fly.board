#define _POSIX_C_SOURCE 200809L
#include "render.h"
#include "theme.h"
#include "config/config.h"
#include <cwist/core/html/builder.h>
#include <cwist/core/mem/alloc.h>
#include <md4c-html.h>
#include <md4c.h>
#include <string.h>
#include <stdio.h>

static void md_output_cb(const MD_CHAR *data, MD_SIZE size, void *userdata) {
    cwist_sstring_append_len((cwist_sstring *)userdata, data, size);
}

cwist_sstring *render_markdown_to_html(const char *md) {
    cwist_sstring *html = cwist_sstring_create();
    if (!html) return NULL;
    cwist_sstring_assign(html, "");
    unsigned flags = MD_DIALECT_GITHUB | MD_FLAG_TABLES | MD_FLAG_PERMISSIVEAUTOLINKS | MD_FLAG_STRIKETHROUGH | MD_FLAG_TASKLISTS;
    int rc = md_html(md, (MD_SIZE)strlen(md), md_output_cb, html, flags, 0);
    if (rc != 0) {
        cwist_sstring_destroy(html);
        return NULL;
    }
    return html;
}

static cwist_html_element_t *nav_link(const char *href, const char *label) {
    cwist_html_element_t *a = cwist_html_element_create("a");
    cwist_html_element_add_attr(a, "href", href);
    cwist_html_element_add_attr(a, "class", "nav-item");
    cwist_html_element_set_text(a, label);
    return a;
}

cwist_sstring *render_page(const char *title, const char *body_html, bool dark, const char *user_role, const char *profile_pic) {
    cwist_html_element_t *html = cwist_html_element_create("html");
    cwist_html_element_add_attr(html, "lang", "ko");

    cwist_html_element_t *head = cwist_html_element_create("head");
    cwist_html_element_t *meta = cwist_html_element_create("meta");
    cwist_html_element_add_attr(meta, "charset", "utf-8");
    cwist_html_element_t *vp = cwist_html_element_create("meta");
    cwist_html_element_add_attr(vp, "name", "viewport");
    cwist_html_element_add_attr(vp, "content", "width=device-width, initial-scale=1");
    cwist_html_element_t *title_el = cwist_html_element_create("title");
    cwist_html_element_set_text(title_el, title);
    cwist_html_element_t *style = cwist_html_element_create("link");
    cwist_html_element_add_attr(style, "rel", "stylesheet");
    char css_url[64];
    snprintf(css_url, sizeof(css_url), "/theme.css?mode=%s", dark ? "dark" : "light");
    cwist_html_element_add_attr(style, "href", css_url);

    cwist_html_element_add_child(head, meta);
    cwist_html_element_add_child(head, vp);
    cwist_html_element_add_child(head, title_el);
    cwist_html_element_add_child(head, style);

    /* Inline theme toggle script */
    cwist_html_element_t *script = cwist_html_element_create("script");
    cwist_html_element_set_text(script,
        "(function(){"
        "var d=document.documentElement,c=document.cookie.match(/theme=dark/);"
        "if(c)d.classList.add('dark');"
        "window.toggleTheme=function(){"
        "var nd=!d.classList.contains('dark');"
        "document.cookie='theme='+(nd?'dark':'light')+';path=/;max-age=31536000';"
        "location.reload();};"
        "})();");
    cwist_html_element_add_child(head, script);

    cwist_html_element_t *body = cwist_html_element_create("body");
    if (dark) cwist_html_element_add_class(body, "dark");

    /* Nav */
    cwist_html_element_t *nav = cwist_html_element_create("nav");
    cwist_html_element_add_class(nav, "topbar");
    cwist_html_element_t *brand = cwist_html_element_create("a");
    cwist_html_element_add_attr(brand, "href", "/");
    cwist_html_element_add_class(brand, "topbar-brand");
    cwist_html_element_t *brand_logo = cwist_html_element_create("img");
    cwist_html_element_add_attr(brand_logo, "src", "/img/logo.png");
    cwist_html_element_add_attr(brand_logo, "alt", "Logo");
    cwist_html_element_add_class(brand_logo, "topbar-logo");
    cwist_html_element_t *brand_title = cwist_html_element_create("span");
    cwist_html_element_add_class(brand_title, "topbar-title");
    cwist_html_element_set_text(brand_title, g_config.title);
    cwist_html_element_add_child(brand, brand_logo);
    cwist_html_element_add_child(brand, brand_title);
    cwist_html_element_add_child(nav, brand);

    cwist_html_element_t *navlinks = cwist_html_element_create("div");
    cwist_html_element_add_class(navlinks, "nav-links");
    cwist_html_element_add_child(navlinks, nav_link("/boards", "Boards"));
    if (user_role && strcmp(user_role, "admin") == 0) {
        cwist_html_element_add_child(navlinks, nav_link("/admin/users", "Admin"));
    }
    if (user_role && user_role[0]) {
        const char *display_pp = profile_pic;
        if ((!display_pp || !display_pp[0]) && strcmp(user_role, "admin") == 0) {
            display_pp = "/img/logo.png";
        }

        if (display_pp && display_pp[0]) {
            cwist_html_element_t *p_link = cwist_html_element_create("a");
            cwist_html_element_add_attr(p_link, "href", "/profile");
            cwist_html_element_t *img = cwist_html_element_create("img");
            cwist_html_element_add_attr(img, "src", display_pp);
            cwist_html_element_add_class(img, "profile-pic-small");
            cwist_html_element_add_child(p_link, img);
            cwist_html_element_add_child(navlinks, p_link);
        } else {
            cwist_html_element_add_child(navlinks, nav_link("/profile", "Profile"));
        }
        cwist_html_element_add_child(navlinks, nav_link("/logout", "Logout"));
    } else {
        cwist_html_element_add_child(navlinks, nav_link("/login", "Login"));
        cwist_html_element_add_child(navlinks, nav_link("/register", "Register"));
    }
    cwist_html_element_t *theme_btn = cwist_html_element_create("button");
    cwist_html_element_add_attr(theme_btn, "onclick", "toggleTheme()");
    cwist_html_element_add_attr(theme_btn, "class", "btn btn-outline");
    cwist_html_element_add_attr(theme_btn, "style", "padding:6px 12px;font-size:13px;");
    cwist_html_element_set_text(theme_btn, dark ? "Light" : "Dark");
    cwist_html_element_add_child(navlinks, theme_btn);
    cwist_html_element_add_child(nav, navlinks);

    cwist_html_element_t *shell = cwist_html_element_create("div");
    cwist_html_element_add_class(shell, "shell fade-in");

    cwist_html_element_t *main_el = cwist_html_element_create("main");
    cwist_html_element_add_class(main_el, "content");
    cwist_html_element_set_text(main_el, body_html);
    cwist_html_element_add_child(shell, main_el);

    cwist_html_element_t *footer = cwist_html_element_create("footer");
    cwist_html_element_add_class(footer, "site-footer");

    cwist_html_element_t *footer_content = cwist_html_element_create("div");
    cwist_html_element_add_class(footer_content, "footer-content");

    cwist_html_element_t *footer_text = cwist_html_element_create("span");
    cwist_html_element_set_text(footer_text, g_config.brand_footer);
    cwist_html_element_add_child(footer_content, footer_text);

    cwist_html_element_t *footer_logo = cwist_html_element_create("img");
    cwist_html_element_add_attr(footer_logo, "src", "/img/logo.png");
    cwist_html_element_add_attr(footer_logo, "alt", "Logo");
    cwist_html_element_add_class(footer_logo, "footer-logo");
    cwist_html_element_add_child(footer_content, footer_logo);

    cwist_html_element_add_child(footer, footer_content);

    cwist_html_element_add_child(body, nav);
    cwist_html_element_add_child(body, shell);
    cwist_html_element_add_child(body, footer);
    cwist_html_element_add_child(html, head);
    cwist_html_element_add_child(html, body);

    cwist_sstring *out = cwist_html_render(html);
    cwist_html_element_destroy(html);
    if (out) {
        cwist_sstring *doc = cwist_sstring_create();
        cwist_sstring_assign(doc, "<!doctype html>");
        cwist_sstring_append_sstring(doc, out);
        cwist_sstring_destroy(out);
        return doc;
    }
    return NULL;
}

static cwist_sstring *build_form(const char *title, const char *action, const char *method,
                                 const char *fields_html, const char *btn_text, const char *error, bool dark) {
    (void)dark;
    cwist_sstring *s = cwist_sstring_create();
    cwist_sstring_assign(s, "<div class='card' style='max-width:420px;margin:40px auto;'>");
    cwist_sstring_append(s, "<h2 style='margin-top:0'>");
    cwist_sstring_append(s, title);
    cwist_sstring_append(s, "</h2>");
    if (error && error[0]) {
        cwist_sstring_append(s, "<div class='alert'>");
        cwist_sstring_append_escaped(s, error);
        cwist_sstring_append(s, "</div>");
    }
    cwist_sstring_append(s, "<form action='");
    cwist_sstring_append(s, action);
    cwist_sstring_append(s, "' method='");
    cwist_sstring_append(s, method);
    cwist_sstring_append(s, "'>");
    cwist_sstring_append(s, fields_html);
    cwist_sstring_append(s, "<button type='submit' class='btn' style='margin-top:8px;width:100%'>");
    cwist_sstring_append(s, btn_text);
    cwist_sstring_append(s, "</button></form></div>");
    return s;
}

static const char *login_register_script =
    "<script>"
    "(function(){"
    "async function sha512(str){"
    "const buf=await crypto.subtle.digest('SHA-512',new TextEncoder().encode(str));"
    "return Array.from(new Uint8Array(buf)).map(b=>b.toString(16).padStart(2,'0')).join('');"
    "}"
    "document.querySelectorAll('form[action=\"/login\"],form[action=\"/register\"],form[action=\"/account/password\"]').forEach(function(form){"
    "form.addEventListener('submit',async function(e){"
    "var pw=form.querySelector('input[type=\"password\"]');"
    "if(pw&&!pw.dataset.hashed){"
    "e.preventDefault();"
    "pw.value=await sha512('fly.board'+pw.value);"
    "pw.dataset.hashed='1';"
    "form.submit();"
    "}"
    "});"
    "});"
    "})();"
    "</script>";

static char *format_join_date(const char *iso_date) {
    static char buf[128];
    if (!iso_date || !iso_date[0]) {
        snprintf(buf, sizeof(buf), "Joined recently");
        return buf;
    }
    int year, mon, day;
    if (sscanf(iso_date, "%d-%d-%d", &year, &mon, &day) == 3) {
        const char *months[] = {"January","February","March","April","May","June",
                                "July","August","September","October","November","December"};
        if (mon >= 1 && mon <= 12) {
            snprintf(buf, sizeof(buf), "Signed in %s %d, %d", months[mon - 1], day, year);
            return buf;
        }
    }
    snprintf(buf, sizeof(buf), "Joined %s", iso_date);
    return buf;
}

cwist_sstring *render_profile(cJSON *user, bool dark, const char *user_role, const char *profile_pic, bool is_own_profile) {
    cwist_sstring *b = cwist_sstring_create();
    const char *username = cJSON_GetObjectItem(user, "username")->valuestring;
    const char *email = cJSON_GetObjectItem(user, "email")->valuestring;
    cJSON *pp = cJSON_GetObjectItem(user, "profile_pic");
    const char *user_profile_pic = (pp && pp->type == cJSON_String) ? pp->valuestring : "";
    cJSON *nickname_obj = cJSON_GetObjectItem(user, "nickname");
    const char *nickname = (nickname_obj && nickname_obj->type == cJSON_String) ? nickname_obj->valuestring : "";
    cJSON *bio_obj = cJSON_GetObjectItem(user, "bio");
    const char *bio = (bio_obj && bio_obj->type == cJSON_String) ? bio_obj->valuestring : "";
    cJSON *created_obj = cJSON_GetObjectItem(user, "created_at");
    const char *created_at = (created_obj && created_obj->type == cJSON_String) ? created_obj->valuestring : "";
    cJSON *uid_obj = cJSON_GetObjectItem(user, "id");
    int user_id = uid_obj ? uid_obj->valueint : 0;

    const char *display_pic = user_profile_pic;
    if ((!display_pic || !display_pic[0]) && user_role && strcmp(user_role, "admin") == 0) {
        display_pic = "/img/logo.png";
    }

    cwist_sstring_append(b, "<div class='card' style='max-width:600px;margin:20px auto;text-align:center'>");
    cwist_sstring_append(b, "<h2>User Profile</h2>");

    if (display_pic && display_pic[0]) {
        cwist_sstring_append(b, "<img src='");
        cwist_sstring_append(b, display_pic);
        cwist_sstring_append(b, "' class='profile-pic' style='margin:20px 0'><br>");
    } else {
        cwist_sstring_append(b, "<div style='width:100px;height:100px;background:#eee;border-radius:50%;margin:20px auto;display:flex;align-items:center;justify-content:center;color:#999'>No Pic</div>");
    }

    if (nickname && nickname[0]) {
        cwist_sstring_append(b, "<h3 style='margin-bottom:4px'>");
        cwist_sstring_append_escaped(b, nickname);
        cwist_sstring_append(b, "</h3>");
        cwist_sstring_append(b, "<p style='color:var(--muted);margin-top:0'>@");
        cwist_sstring_append_escaped(b, username);
        cwist_sstring_append(b, "</p>");
    } else {
        cwist_sstring_append(b, "<h3>");
        cwist_sstring_append_escaped(b, username);
        cwist_sstring_append(b, "</h3>");
    }

    cwist_sstring_append(b, "<p style='color:var(--muted)'>");
    cwist_sstring_append_escaped(b, email);
    cwist_sstring_append(b, "</p>");

    cwist_sstring_append(b, "<p style='color:var(--muted);font-size:13px'>");
    cwist_sstring_append(b, format_join_date(created_at));
    cwist_sstring_append(b, "</p>");

    if (bio && bio[0]) {
        cwist_sstring_append(b, "<div style='margin:16px 0;padding:12px;background:var(--bg-alt);border-radius:8px;text-align:left'>");
        cwist_sstring_append(b, "<p style='margin:0;color:var(--fg)'>");
        cwist_sstring_append_escaped(b, bio);
        cwist_sstring_append(b, "</p>");
        cwist_sstring_append(b, "</div>");
    }

    if (is_own_profile) {
        cwist_sstring_append(b, "<hr style='margin:20px 0;border:0;border-top:1px solid var(--border)'>");
        cwist_sstring_append(b, "<a href='/account/settings' class='btn btn-outline'>Account Settings</a>");
    } else if (user_id > 0) {
        cwist_sstring_append(b, "<hr style='margin:20px 0;border:0;border-top:1px solid var(--border)'>");
        char uid_buf[32];
        snprintf(uid_buf, sizeof(uid_buf), "%d", user_id);
        cwist_sstring_append(b, "<a href='/user/");
        cwist_sstring_append(b, uid_buf);
        cwist_sstring_append(b, "' class='btn btn-outline'>View Public Profile</a>");
    }

    cwist_sstring_append(b, "</div>");

    cwist_sstring *res = render_page("Profile", b->data, dark, user_role, profile_pic);
    cwist_sstring_destroy(b);
    return res;
}

cwist_sstring *render_account_settings(cJSON *user, bool dark, const char *profile_pic, const char *error) {
    cwist_sstring *b = cwist_sstring_create();
    const char *username = cJSON_GetObjectItem(user, "username")->valuestring;
    const char *email = cJSON_GetObjectItem(user, "email")->valuestring;
    cJSON *pp = cJSON_GetObjectItem(user, "profile_pic");
    const char *current_pic = (pp && pp->type == cJSON_String) ? pp->valuestring : "";
    cJSON *nickname_obj = cJSON_GetObjectItem(user, "nickname");
    const char *nickname = (nickname_obj && nickname_obj->type == cJSON_String) ? nickname_obj->valuestring : "";
    cJSON *bio_obj = cJSON_GetObjectItem(user, "bio");
    const char *bio = (bio_obj && bio_obj->type == cJSON_String) ? bio_obj->valuestring : "";

    cwist_sstring_append(b, "<div class='card' style='max-width:600px;margin:20px auto'>");
    cwist_sstring_append(b, "<h2>Account Settings</h2>");

    if (current_pic && current_pic[0]) {
        cwist_sstring_append(b, "<div style='text-align:center;margin-bottom:16px'><img src='");
        cwist_sstring_append(b, current_pic);
        cwist_sstring_append(b, "' class='profile-pic' style='width:80px;height:80px'></div>");
    }

    if (error && error[0]) {
        cwist_sstring_append(b, "<div class='alert'>");
        cwist_sstring_append_escaped(b, error);
        cwist_sstring_append(b, "</div>");
    }

    cwist_sstring_append(b, "<form action='/account/settings' method='POST' enctype='multipart/form-data'>");

    cwist_sstring_append(b, "<label>Nickname</label><input name='nickname' value='");
    cwist_sstring_append_escaped(b, nickname);
    cwist_sstring_append(b, "' placeholder='Your display name'>");

    cwist_sstring_append(b, "<label>Bio</label><textarea name='bio' rows='4' placeholder='Tell us about yourself'>");
    cwist_sstring_append_escaped(b, bio);
    cwist_sstring_append(b, "</textarea>");

    cwist_sstring_append(b, "<label>Profile Picture</label><input type='file' name='profile_pic' accept='image/*'>");
    cwist_sstring_append(b, "<small style='color:var(--muted)'>Leave empty to keep current picture.</small>");

    cwist_sstring_append(b, "<div style='margin-top:16px;display:flex;gap:10px'>");
    cwist_sstring_append(b, "<button type='submit' class='btn'>Save Changes</button>");
    cwist_sstring_append(b, "<a href='/profile' class='btn btn-outline'>Cancel</a>");
    cwist_sstring_append(b, "</div>");

    cwist_sstring_append(b, "</form>");

    cwist_sstring_append(b, "<hr style='margin:24px 0;border:0;border-top:1px solid var(--border)'>");
    cwist_sstring_append(b, "<a href='/account/password' class='btn btn-outline'>Change Password</a>");

    cwist_sstring_append(b, "<hr style='margin:24px 0;border:0;border-top:1px solid var(--border)'>");
    cwist_sstring_append(b, "<p style='color:var(--muted);font-size:13px'>Username: ");
    cwist_sstring_append_escaped(b, username);
    cwist_sstring_append(b, "<br>Email: ");
    cwist_sstring_append_escaped(b, email);
    cwist_sstring_append(b, "</p>");

    cwist_sstring_append(b, "</div>");

    cwist_sstring *res = render_page("Account Settings", b->data, dark, "user", profile_pic);
    cwist_sstring_destroy(b);
    return res;
}

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

cwist_sstring *render_password_change(bool dark, const char *error) {
    const char *fields =
        "<label>Current Password</label><input name='current_password' type='password' placeholder='Current password' required>"
        "<label>New Password</label><input name='new_password' type='password' placeholder='min 6 chars' required>"
        "<label>Confirm New Password</label><input name='confirm_password' type='password' placeholder='Confirm new password' required>";
    cwist_sstring *body = build_form("Change Password", "/account/password", "post", fields, "Update Password", error, dark);
    cwist_sstring_append(body, "<p style='text-align:center'><a href='/account/settings'>Back to settings</a></p>");
    cwist_sstring_append(body, login_register_script);
    cwist_sstring *page = render_page("Change Password", body->data, dark, "user", NULL);
    cwist_sstring_destroy(body);
    return page;
}

cwist_sstring *render_post_list(cJSON *posts, cJSON *boards, bool dark, const char *user_role, int page, int total_pages, const char *board_slug, const char *profile_pic) {
    cwist_sstring *b = cwist_sstring_create();
    cwist_sstring_assign(b, "<div class='hero'><h1>");
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
    cwist_sstring_append(b, "<textarea id='md-editor' name='content' rows='18' style='width:100%;font-family:monospace;font-size:14px;' required>");
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

cwist_sstring *render_board_list(cJSON *boards, bool dark, const char *user_role, const char *profile_pic) {
    cwist_sstring *b = cwist_sstring_create();
    cwist_sstring_assign(b, "<div class='hero'><h1>");
    cwist_sstring_append_escaped(b, g_config.title);
    cwist_sstring_append(b, "</h1><p>");
    cwist_sstring_append_escaped(b, g_config.subtitle);
    cwist_sstring_append(b, "</p></div>");
    if (user_role && strcmp(user_role, "admin") == 0) {
        cwist_sstring_append(b, "<a href='/board/new' class='btn' style='margin-bottom:18px'>New Board</a>");
    }
    cwist_sstring_append(b, "<div class='post-grid'>");
    if (boards) {
        int n = cJSON_GetArraySize(boards);
        for (int i = 0; i < n; i++) {
            cJSON *bo = cJSON_GetArrayItem(boards, i);
            cJSON *slug = cJSON_GetObjectItem(bo, "slug");
            cJSON *name = cJSON_GetObjectItem(bo, "name");
            cJSON *desc = cJSON_GetObjectItem(bo, "description");
            cwist_sstring_append(b, "<article class='card'>");
            cwist_sstring_append(b, "<a href='/board/");
            cwist_sstring_append(b, slug->valuestring);
            cwist_sstring_append(b, "'><h3 style='margin-top:0'>");
            cwist_sstring_append_escaped(b, name->valuestring);
            cwist_sstring_append(b, "</h3></a>");
            cwist_sstring_append(b, "<p style='color:var(--muted);font-size:14px'>");
            cwist_sstring_append_escaped(b, desc && desc->valuestring[0] ? desc->valuestring : "");
            cwist_sstring_append(b, "</p>");
            if (user_role && strcmp(user_role, "admin") == 0) {
                cJSON *bid = cJSON_GetObjectItem(bo, "id");
                char bid_buf[32];
                snprintf(bid_buf, sizeof(bid_buf), "%d", bid->valueint);
                cwist_sstring_append(b, "<div style='margin-top:10px;display:flex;gap:8px'>");
                cwist_sstring_append(b, "<a href='/board/");
                cwist_sstring_append(b, bid_buf);
                cwist_sstring_append(b, "/delete' class='btn btn-outline' style='font-size:12px;padding:4px 10px' onclick='return confirm(\"Delete this board? This action cannot be undone.\")'>Delete</a>");
                cwist_sstring_append(b, "</div>");
            }
            cwist_sstring_append(b, "</article>");
        }
    }
    cwist_sstring_append(b, "</div>");
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

cwist_sstring *render_user_admin(cJSON *users, bool dark, const char *profile_pic) {
    cwist_sstring *b = cwist_sstring_create();
    cwist_sstring_assign(b, "<div class='hero'><h1>User Admin</h1></div>");
    cwist_sstring_append(b, "<div class='card'><table style='width:100%;border-collapse:collapse'>");
    cwist_sstring_append(b, "<thead><tr><th style='text-align:left;padding:8px'>User</th><th style='text-align:left;padding:8px'>Email</th><th style='text-align:left;padding:8px'>Role</th><th style='text-align:left;padding:8px'>Action</th></tr></thead><tbody>");
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
            cwist_sstring_append(b, "</td><td style='padding:8px'>");
            cwist_sstring_append_escaped(b, role->valuestring);
            cwist_sstring_append(b, "</td><td style='padding:8px'>");
            cwist_sstring_append(b, "<form style='display:inline' action='/admin/user/role' method='post'>");
            cwist_sstring_append(b, "<input type='hidden' name='id' value='");
            char uid_buf[32];
            snprintf(uid_buf, sizeof(uid_buf), "%d", uid->valueint);
            cwist_sstring_append(b, uid_buf);
            cwist_sstring_append(b, "'>");
            cwist_sstring_append(b, "<select name='role'><option value='user'>user</option><option value='admin'>admin</option></select>");
            cwist_sstring_append(b, "<button type='submit' class='btn' style='font-size:12px;padding:4px 10px;margin-left:6px'>Set</button></form>");
            cwist_sstring_append(b, " <form style='display:inline' action='/unregister' method='post'>");
            cwist_sstring_append(b, "<input type='hidden' name='id' value='");
            cwist_sstring_append(b, uid_buf);
            cwist_sstring_append(b, "'>");
            cwist_sstring_append(b, "<button type='submit' class='btn btn-outline' style='font-size:12px;padding:4px 10px' onclick='return confirm(\"Delete?\")'>Delete</button></form>");
            cwist_sstring_append(b, "</td></tr>");
        }
    }
    cwist_sstring_append(b, "</tbody></table></div>");
    cwist_sstring *page = render_page("User Admin", b->data, dark, "admin", profile_pic);
    cwist_sstring_destroy(b);
    return page;
}

cwist_sstring *render_file_detail(cJSON *file, cJSON *comments, bool dark, const char *user_role, const char *profile_pic) {
    (void)file; (void)comments;
    return render_page("File Detail", "<p>File detail page</p>", dark, user_role, profile_pic);
}

cwist_sstring *render_file_repo(cJSON *files, bool dark, const char *profile_pic) {
    cwist_sstring *b = cwist_sstring_create();
    cwist_sstring_assign(b, "<div class='hero'><h1>File Repository</h1><p>Shared files and attachments.</p></div>");
    cwist_sstring_append(b, "<div class='card' style='max-width:560px;margin:0 auto'><form action='/file/upload' method='post' enctype='multipart/form-data'>");
    cwist_sstring_append(b, "<label>Upload file</label><input type='file' name='file' required>");
    cwist_sstring_append(b, "<button type='submit' class='btn' style='margin-top:8px'>Upload</button>");
    cwist_sstring_append(b, "<small style='color:var(--muted);display:block;margin-top:6px'>Large files (&gt;1MB) are stored on volume; small files in DB.</small>");
    cwist_sstring_append(b, "</form></div>");

    cwist_sstring_append(b, "<h3 style='margin-top:28px'>Files</h3><div class='post-grid'>");
    if (files) {
        int n = cJSON_GetArraySize(files);
        for (int i = 0; i < n; i++) {
            cJSON *f = cJSON_GetArrayItem(files, i);
            cJSON *fid = cJSON_GetObjectItem(f, "id");
            cJSON *fname = cJSON_GetObjectItem(f, "filename");
            cJSON *stype = cJSON_GetObjectItem(f, "storage_type");
            cJSON *sz = cJSON_GetObjectItem(f, "size");
            char fid_buf[32];
            snprintf(fid_buf, sizeof(fid_buf), "%d", fid->valueint);
            char sz_buf[32];
            snprintf(sz_buf, sizeof(sz_buf), "%lld", (long long)sz->valueint);
            cwist_sstring_append(b, "<article class='card'>");
            cwist_sstring_append(b, "<h4 style='margin-top:0'>");
            cwist_sstring_append_escaped(b, fname->valuestring);
            cwist_sstring_append(b, "</h4>");
            cwist_sstring_append(b, "<p style='color:var(--muted);font-size:13px'>");
            cwist_sstring_append(b, stype->valuestring);
            cwist_sstring_append(b, " &middot; ");
            cwist_sstring_append(b, sz_buf);
            cwist_sstring_append(b, " bytes</p>");
            cwist_sstring_append(b, "<div style='display:flex;gap:8px'><a href='/file/");
            cwist_sstring_append(b, fid_buf);
            cwist_sstring_append(b, "' class='btn' style='font-size:12px;padding:4px 10px'>Download</a>");
            cwist_sstring_append(b, "<form style='display:inline' action='/file/delete' method='post'>");
            cwist_sstring_append(b, "<input type='hidden' name='id' value='");
            cwist_sstring_append(b, fid_buf);
            cwist_sstring_append(b, "'>");
            cwist_sstring_append(b, "<button type='submit' class='btn btn-outline' style='font-size:12px;padding:4px 10px'>Delete</button></form></div>");
            cwist_sstring_append(b, "</article>");
        }
    }
    cwist_sstring_append(b, "</div>");
    cwist_sstring *page = render_page("Files", b->data, dark, "", profile_pic);
    cwist_sstring_destroy(b);
    return page;
}

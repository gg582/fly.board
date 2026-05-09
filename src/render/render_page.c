#define _POSIX_C_SOURCE 200809L
#include "render.h"
#include "render_internal.h"
#include "theme.h"
#include "config/config.h"
#include <cwist/core/html/builder.h>
#include <cwist/core/mem/alloc.h>
#include <cwist/core/sstring/sstring.h>
#include <string.h>
#include <stdio.h>

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

    /* Highlight.js syntax highlighting */
    cwist_html_element_t *hl_css = cwist_html_element_create("link");
    cwist_html_element_add_attr(hl_css, "rel", "stylesheet");
    cwist_html_element_add_attr(hl_css, "href",
        dark
        ? "https://cdnjs.cloudflare.com/ajax/libs/highlight.js/11.9.0/styles/github-dark.min.css"
        : "https://cdnjs.cloudflare.com/ajax/libs/highlight.js/11.9.0/styles/github.min.css");
    cwist_html_element_add_child(head, hl_css);

    cwist_html_element_t *hl_js = cwist_html_element_create("script");
    cwist_html_element_add_attr(hl_js, "src",
        "https://cdnjs.cloudflare.com/ajax/libs/highlight.js/11.9.0/highlight.min.js");
    cwist_html_element_add_child(head, hl_js);

    cwist_html_element_t *hl_init = cwist_html_element_create("script");
    cwist_html_element_set_text(hl_init,
        "document.addEventListener('DOMContentLoaded',function(){hljs.highlightAll();});");
    cwist_html_element_add_child(head, hl_init);

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
    cwist_html_element_t *brand_title = cwist_html_element_create("span");
    cwist_html_element_add_class(brand_title, "topbar-title");
    cwist_html_element_set_text(brand_title, g_config.title);
    cwist_html_element_add_child(brand, brand_title);
    cwist_html_element_add_child(nav, brand);

    cwist_html_element_t *navlinks = cwist_html_element_create("div");
    cwist_html_element_add_class(navlinks, "nav-links");
    cwist_html_element_add_child(navlinks, nav_link("/", "Home"));
    cwist_html_element_add_child(navlinks, nav_link("/boards", "Boards"));
    cwist_html_element_add_child(navlinks, nav_link("/files", "Files"));
    if (user_role && strcmp(user_role, "admin") == 0) {
        cwist_html_element_add_child(navlinks, nav_link("/admin/users", "Admin"));
    }
    if (user_role && user_role[0]) {
        const char *display_pp = profile_pic;

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

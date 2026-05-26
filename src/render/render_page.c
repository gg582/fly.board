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

static __thread char g_nav_profile_name[128];
static __thread char g_nav_profile_account[128];

void render_set_nav_profile(const char *display_name, const char *account_name) {
    if (display_name && display_name[0]) {
        snprintf(g_nav_profile_name, sizeof(g_nav_profile_name), "%s", display_name);
    } else {
        g_nav_profile_name[0] = '\0';
    }

    if (account_name && account_name[0]) {
        snprintf(g_nav_profile_account, sizeof(g_nav_profile_account), "%s", account_name);
    } else {
        g_nav_profile_account[0] = '\0';
    }
}

static void render_clear_nav_profile(void) {
    g_nav_profile_name[0] = '\0';
    g_nav_profile_account[0] = '\0';
}

cwist_sstring *render_page(const char *title, const char *body_html, bool dark, const char *user_role, const char *profile_pic, bool is_mobile) {

    cwist_html_element_t *html = cwist_html_element_create("html");
    cwist_html_element_add_attr(html, "lang", "ko");
    if (is_mobile) cwist_html_element_add_class(html, "mobile");

    cwist_html_element_t *head = cwist_html_element_create("head");
    cwist_html_element_t *meta = cwist_html_element_create("meta");
    cwist_html_element_add_attr(meta, "charset", "utf-8");
    cwist_html_element_t *vp = cwist_html_element_create("meta");
    cwist_html_element_add_attr(vp, "name", "viewport");
    cwist_html_element_add_attr(vp, "content", "width=device-width, initial-scale=1");
    cwist_html_element_t *title_el = cwist_html_element_create("title");
    cwist_html_element_set_text(title_el, title);
    cwist_html_element_add_child(head, meta);
    cwist_html_element_add_child(head, vp);
    cwist_html_element_add_child(head, title_el);

    if (g_config.favicon[0]) {
        cwist_html_element_t *favicon_el = cwist_html_element_create("link");
        cwist_html_element_add_attr(favicon_el, "rel", "icon");
        char favicon_url[512];
        snprintf(favicon_url, sizeof(favicon_url), "/assets/img/%s", g_config.favicon);
        cwist_html_element_add_attr(favicon_el, "href", favicon_url);
        cwist_html_element_add_child(head, favicon_el);
    }

    /* Preconnect + dns-prefetch to critical origins */
    cwist_html_element_t *preconnect_jsdelivr = cwist_html_element_create("link");
    cwist_html_element_add_attr(preconnect_jsdelivr, "rel", "preconnect");
    cwist_html_element_add_attr(preconnect_jsdelivr, "href", "https://cdn.jsdelivr.net");
    cwist_html_element_add_attr(preconnect_jsdelivr, "crossorigin", "");
    cwist_html_element_add_child(head, preconnect_jsdelivr);

    cwist_html_element_t *dns_jsdelivr = cwist_html_element_create("link");
    cwist_html_element_add_attr(dns_jsdelivr, "rel", "dns-prefetch");
    cwist_html_element_add_attr(dns_jsdelivr, "href", "https://cdn.jsdelivr.net");
    cwist_html_element_add_child(head, dns_jsdelivr);

    cwist_html_element_t *preconnect_cdnjs = cwist_html_element_create("link");
    cwist_html_element_add_attr(preconnect_cdnjs, "rel", "preconnect");
    cwist_html_element_add_attr(preconnect_cdnjs, "href", "https://cdnjs.cloudflare.com");
    cwist_html_element_add_attr(preconnect_cdnjs, "crossorigin", "");
    cwist_html_element_add_child(head, preconnect_cdnjs);

    cwist_html_element_t *dns_cdnjs = cwist_html_element_create("link");
    cwist_html_element_add_attr(dns_cdnjs, "rel", "dns-prefetch");
    cwist_html_element_add_attr(dns_cdnjs, "href", "https://cdnjs.cloudflare.com");
    cwist_html_element_add_child(head, dns_cdnjs);


    /* Web Fonts */
    cwist_html_element_t *font_preconnect_google = cwist_html_element_create("link");
    cwist_html_element_add_attr(font_preconnect_google, "rel", "preconnect");
    cwist_html_element_add_attr(font_preconnect_google, "href", "https://fonts.googleapis.com");
    cwist_html_element_add_child(head, font_preconnect_google);

    cwist_html_element_t *font_preconnect_gstatic = cwist_html_element_create("link");
    cwist_html_element_add_attr(font_preconnect_gstatic, "rel", "preconnect");
    cwist_html_element_add_attr(font_preconnect_gstatic, "href", "https://fonts.gstatic.com");
    cwist_html_element_add_attr(font_preconnect_gstatic, "crossorigin", "");
    cwist_html_element_add_child(head, font_preconnect_gstatic);

    cwist_html_element_t *font_space = cwist_html_element_create("link");
    cwist_html_element_add_attr(font_space, "rel", "stylesheet");
    cwist_html_element_add_attr(font_space, "href", "https://fonts.googleapis.com/css2?family=Space+Grotesk:wght@400;500;700&family=IBM+Plex+Sans+KR:wght@400;500;700&family=Inter:wght@400;500;600;700&family=Source+Serif+4:ital,wght@0,400;0,600;1,400&display=swap");
    cwist_html_element_add_child(head, font_space);

    cwist_html_element_t *font_pretendard = cwist_html_element_create("link");
    cwist_html_element_add_attr(font_pretendard, "rel", "stylesheet");
    cwist_html_element_add_attr(font_pretendard, "href", "https://cdn.jsdelivr.net/gh/orioncactus/pretendard@v1.3.9/dist/web/variable/pretendardvariable-dynamic-subset.css");
    cwist_html_element_add_child(head, font_pretendard);

    cwist_html_element_t *font_d2coding = cwist_html_element_create("link");
    cwist_html_element_add_attr(font_d2coding, "rel", "stylesheet");
    cwist_html_element_add_attr(font_d2coding, "href", "https://cdn.jsdelivr.net/gh/joungkyun/font-d2coding@master/d2coding.css");
    cwist_html_element_add_child(head, font_d2coding);

    /* Highlight.js syntax highlighting */
    cwist_html_element_t *hl_css = cwist_html_element_create("link");
    cwist_html_element_add_attr(hl_css, "rel", "stylesheet");
    cwist_html_element_add_attr(hl_css, "id", "hl-theme");
    cwist_html_element_add_attr(hl_css, "href",
        dark
        ? "https://cdnjs.cloudflare.com/ajax/libs/highlight.js/11.9.0/styles/github-dark.min.css"
        : "https://cdnjs.cloudflare.com/ajax/libs/highlight.js/11.9.0/styles/github.min.css");
    cwist_html_element_add_child(head, hl_css);

    cwist_html_element_t *hl_js = cwist_html_element_create("script");
    cwist_html_element_add_attr(hl_js, "src",
        "https://cdnjs.cloudflare.com/ajax/libs/highlight.js/11.9.0/highlight.min.js");
    cwist_html_element_add_child(head, hl_js);

    cwist_html_element_t *hl_fortran = cwist_html_element_create("script");
    cwist_html_element_add_attr(hl_fortran, "src",
        "https://cdnjs.cloudflare.com/ajax/libs/highlight.js/11.9.0/languages/fortran.min.js");
    cwist_html_element_add_child(head, hl_fortran);

    /* Progressive multi-theme loader: inline critical CSS to prevent FOUC */
    char *critical_css = theme_build_css(dark);
    cwist_html_element_t *dyn_style = cwist_html_element_create("style");
    cwist_html_element_add_attr(dyn_style, "id", "dyn-theme");
    cwist_html_element_set_text(dyn_style, critical_css ? critical_css : "");
    cwist_html_element_add_child(head, dyn_style);
    free(critical_css);

    cwist_html_element_t *layout_js = cwist_html_element_create("script");
    cwist_html_element_add_attr(layout_js, "src", "/assets/js/layout.js");
    cwist_html_element_add_child(head, layout_js);

    cwist_html_element_t *body = cwist_html_element_create("body");
    if (dark) cwist_html_element_add_class(body, "dark");
    if (is_mobile) cwist_html_element_add_class(body, "mobile");


    /* Nav */
    cwist_html_element_t *nav = cwist_html_element_create("nav");
    cwist_html_element_add_class(nav, "topbar");

    cwist_html_element_t *burger_btn = cwist_html_element_create("button");
    cwist_html_element_add_attr(burger_btn, "type", "button");
    cwist_html_element_add_attr(burger_btn, "class", "burger-btn");
    cwist_html_element_add_attr(burger_btn, "aria-label", "Menu");
    cwist_html_element_add_attr(burger_btn, "aria-expanded", "false");
    cwist_html_element_t *burger_icon = cwist_html_element_create("span");
    cwist_html_element_add_class(burger_icon, "burger-icon");
    cwist_html_element_set_text(burger_icon, "\u2630");
    cwist_html_element_add_child(burger_btn, burger_icon);
    cwist_html_element_add_child(nav, burger_btn);

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
    cwist_html_element_t *boards_wrap = cwist_html_element_create("div");
    cwist_html_element_add_class(boards_wrap, "nav-board-dropdown");
    cwist_html_element_t *boards_link = cwist_html_element_create("a");
    cwist_html_element_add_attr(boards_link, "href", "/boards");
    cwist_html_element_add_attr(boards_link, "class", "nav-item nav-board-trigger");
    cwist_html_element_set_text(boards_link, "Boards");
    cwist_html_element_add_child(boards_wrap, boards_link);
    cwist_html_element_t *boards_menu = cwist_html_element_create("div");
    cwist_html_element_add_attr(boards_menu, "id", "boards-dropdown");
    cwist_html_element_add_class(boards_menu, "nav-board-menu");
    cwist_html_element_t *boards_all = cwist_html_element_create("a");
    cwist_html_element_add_attr(boards_all, "href", "/boards");
    cwist_html_element_add_attr(boards_all, "class", "nav-board-subitem nav-board-subitem-all");
    cwist_html_element_set_text(boards_all, "All Boards");
    cwist_html_element_add_child(boards_menu, boards_all);
    cwist_html_element_t *boards_list = cwist_html_element_create("div");
    cwist_html_element_add_attr(boards_list, "id", "boards-dropdown-list");
    cwist_html_element_add_class(boards_list, "nav-board-menu-list");
    cwist_html_element_add_child(boards_menu, boards_list);
    cwist_html_element_add_child(boards_wrap, boards_menu);
    cwist_html_element_add_child(navlinks, boards_wrap);
    cwist_html_element_add_child(navlinks, nav_link("/files", "Files"));
    if (user_role && strcmp(user_role, "admin") == 0) {
        cwist_html_element_add_child(navlinks, nav_link("/admin/users", "Admin"));
    }
    /* Mobile account header (first in navlinks) */
    cwist_html_element_t *mobile_acct = cwist_html_element_create("div");
    cwist_html_element_add_class(mobile_acct, "mobile-account-header");

    cwist_html_element_t *mobile_pic_link = cwist_html_element_create("a");
    cwist_html_element_add_attr(mobile_pic_link, "href", (user_role && user_role[0]) ? "/profile" : "/login");
    cwist_html_element_add_class(mobile_pic_link, "mobile-profile-link");

    const char *display_pp = profile_pic;
    if (display_pp && display_pp[0]) {
        cwist_html_element_t *img = cwist_html_element_create("img");
        cwist_html_element_add_attr(img, "src", display_pp);
        cwist_html_element_add_attr(img, "alt", "Profile");
        cwist_html_element_add_class(img, "mobile-profile-pic");
        cwist_html_element_add_child(mobile_pic_link, img);
    } else {
        cwist_html_element_t *default_icon = cwist_html_element_create("span");
        cwist_html_element_add_class(default_icon, "mobile-profile-pic mobile-profile-default");
        cwist_html_element_set_text(default_icon, "\u263A");
        cwist_html_element_add_child(mobile_pic_link, default_icon);
    }
    cwist_html_element_add_child(mobile_acct, mobile_pic_link);

    cwist_html_element_t *mobile_auth_wrap = cwist_html_element_create("div");
    cwist_html_element_add_class(mobile_auth_wrap, "mobile-auth-btns");
    if (user_role && user_role[0]) {
        cwist_html_element_t *logout_btn = nav_link("/logout", "Logout");
        cwist_html_element_add_class(logout_btn, "btn btn-outline");
        cwist_html_element_add_child(mobile_auth_wrap, logout_btn);
    } else {
        cwist_html_element_t *login_btn = nav_link("/login", "Login");
        cwist_html_element_add_class(login_btn, "btn btn-outline");
        cwist_html_element_add_child(mobile_auth_wrap, login_btn);
        cwist_html_element_t *reg_btn = nav_link("/register", "Register");
        cwist_html_element_add_class(reg_btn, "btn btn-outline");
        cwist_html_element_add_child(mobile_auth_wrap, reg_btn);
    }
    cwist_html_element_add_child(mobile_acct, mobile_auth_wrap);
    cwist_html_element_add_child(navlinks, mobile_acct);

    if (user_role && user_role[0]) {
        const char *display_pp2 = profile_pic;

        if (display_pp2 && display_pp2[0]) {
            cwist_html_element_t *p_link = cwist_html_element_create("a");
            cwist_html_element_add_attr(p_link, "href", "/profile");
            cwist_html_element_t *img = cwist_html_element_create("img");
            cwist_html_element_add_attr(img, "src", display_pp2);
            cwist_html_element_add_attr(img, "width", "24");
            cwist_html_element_add_attr(img, "height", "24");
            cwist_html_element_add_class(img, "profile-pic-small");
            cwist_html_element_add_child(p_link, img);
            cwist_html_element_add_class(p_link, "nav-item desktop-only");
            cwist_html_element_add_child(navlinks, p_link);
        } else {
            cwist_html_element_t *p_link = nav_link("/profile", "Profile");
            cwist_html_element_add_class(p_link, "desktop-only");
            cwist_html_element_add_child(navlinks, p_link);
        }
        cwist_html_element_t *logout_link = nav_link("/logout", "Logout");
        cwist_html_element_add_class(logout_link, "desktop-only");
        cwist_html_element_add_child(navlinks, logout_link);
    } else {
        cwist_html_element_t *login_link = nav_link("/login", "Login");
        cwist_html_element_add_class(login_link, "desktop-only");
        cwist_html_element_add_child(navlinks, login_link);
        cwist_html_element_t *reg_link = nav_link("/register", "Register");
        cwist_html_element_add_class(reg_link, "desktop-only");
        cwist_html_element_add_child(navlinks, reg_link);
    }

    cwist_html_element_add_child(nav, navlinks);

    /* Theme toggle — simple click-to-toggle button */
    cwist_html_element_t *theme_wrapper = cwist_html_element_create("div");
    cwist_html_element_add_class(theme_wrapper, "theme-switch");

    cwist_html_element_t *theme_btn = cwist_html_element_create("button");
    cwist_html_element_add_attr(theme_btn, "type", "button");
    cwist_html_element_add_attr(theme_btn, "id", "theme-toggle-btn");
    cwist_html_element_add_attr(theme_btn, "class", "btn btn-outline theme-toggle-btn");
    cwist_html_element_set_text(theme_btn, dark ? "\u25CF" : "\u25CB");
    cwist_html_element_add_child(theme_wrapper, theme_btn);

    cwist_html_element_add_child(nav, theme_wrapper);

    cwist_html_element_t *shell = cwist_html_element_create("div");
    if (shell) cwist_html_element_add_class(shell, "shell fade-in");

    cwist_html_element_t *main_el = cwist_html_element_create("main");
    if (main_el) {
        cwist_html_element_add_class(main_el, "content");
        /* Use a placeholder to avoid potential memory/escaping issues in the builder for large body_html */
        cwist_html_element_set_text(main_el, "<!--CWIST_BODY_PLACEHOLDER-->");
        if (shell) cwist_html_element_add_child(shell, main_el);
    }

    cwist_html_element_t *footer = cwist_html_element_create("footer");
    if (footer) cwist_html_element_add_class(footer, "site-footer");

    cwist_html_element_t *footer_content = cwist_html_element_create("div");
    if (footer_content) cwist_html_element_add_class(footer_content, "footer-content");

    cwist_html_element_t *footer_text = cwist_html_element_create("span");
    if (footer_text) {
        cwist_html_element_set_text(footer_text, g_config.brand_footer);
        if (footer_content) cwist_html_element_add_child(footer_content, footer_text);
    }

    cwist_html_element_t *footer_logo = cwist_html_element_create("img");
    if (footer_logo) {
        char footer_logo_path[512];
        if (g_config.blog_logo[0]) {
            snprintf(footer_logo_path, sizeof(footer_logo_path), "/assets/img/%s", g_config.blog_logo);
        } else {
            strcpy(footer_logo_path, "/assets/img/logo.png");
        }
        cwist_html_element_add_attr(footer_logo, "src", footer_logo_path);
        cwist_html_element_add_attr(footer_logo, "alt", "Logo");
        cwist_html_element_add_attr(footer_logo, "width", "24");
        cwist_html_element_add_attr(footer_logo, "height", "16");
        cwist_html_element_add_attr(footer_logo, "data-tasfa-skip", "1");
        cwist_html_element_add_attr(footer_logo, "fetchpriority", "high");
        cwist_html_element_add_class(footer_logo, "footer-logo");
        if (footer_content) cwist_html_element_add_child(footer_content, footer_logo);
    }

    if (footer && footer_content) cwist_html_element_add_child(footer, footer_content);

    if (body) {
        if (nav) cwist_html_element_add_child(body, nav);

        cwist_html_element_t *overlay = cwist_html_element_create("div");
        if (overlay) {
            cwist_html_element_add_class(overlay, "mobile-overlay");
            cwist_html_element_add_child(body, overlay);
        }

        if (shell) cwist_html_element_add_child(body, shell);
        if (footer) cwist_html_element_add_child(body, footer);

        cwist_html_element_t *tasfa_script = cwist_html_element_create("script");
        if (tasfa_script) {
            cwist_html_element_add_attr(tasfa_script, "src", "/assets/js/tasfa-download.js?v=5");
            cwist_html_element_add_child(body, tasfa_script);
        }
    }

    if (head) cwist_html_element_add_child(html, head);
    if (body) cwist_html_element_add_child(html, body);

    cwist_sstring *out = cwist_html_render(html);
    cwist_html_element_destroy(html);

    if (out) {
        cwist_sstring *doc = cwist_sstring_create();
        cwist_sstring_assign(doc, "<!doctype html>");
        
        /* Inject the actual body_html by replacing the placeholder */
        const char *placeholder = "<!--CWIST_BODY_PLACEHOLDER-->";
        char *pos = strstr(out->data, placeholder);
        if (pos) {
            size_t head_len = (size_t)(pos - out->data);
            cwist_sstring_append_len(doc, out->data, head_len);
            cwist_sstring_append(doc, body_html ? body_html : "");
            cwist_sstring_append(doc, pos + strlen(placeholder));
        } else {
            cwist_sstring_append_sstring(doc, out);
        }
        
        cwist_sstring_destroy(out);
        return doc;
    }
    return NULL;
}

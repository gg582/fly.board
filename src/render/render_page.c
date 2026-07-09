#define _POSIX_C_SOURCE 200809L
#include "render.h"
#include "render_internal.h"
#include "theme.h"
#include "config/config.h"
#include "utils/image_inline.h"
#include <cwist/core/html/builder.h>
#include <cwist/core/mem/alloc.h>
#include <cwist/core/sstring/sstring.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <sys/stat.h>
#include <pthread.h>

/* Inlined static asset cache. Page-critical scripts, the current highlight
 * theme, and the font-face stylesheets are embedded directly so the first
 * render does not block on extra high-RTT round trips. Larger libraries
 * (highlight.js, KaTeX, TASFA) and the actual font files are served as
 * separate cached files so the browser can reuse them across navigations. */
typedef struct {
    char *jwt_js;
    char *layout_js;
    char *font_css;          /* google-fonts.css (large; kept external by default) */
    char *font_css_small;    /* pretendard.css + d2coding.css (small; inlined) */
    /* Optional CDN libraries that can be inlined when FLYBOARD_INLINE_CDN is set */
    char *highlight_light_css;
    char *highlight_dark_css;
    char *highlight_js;
    char *highlight_fortran_js;
    char *katex_js;
    char *katex_css;
} inline_assets_t;

static inline_assets_t g_inline_assets;
static pthread_once_t g_inline_assets_once = PTHREAD_ONCE_INIT;

static char *read_file_to_string(const char *path);

static char *concat_two_files(const char *a, const char *b);

static void load_inline_assets(void) {
    g_inline_assets.jwt_js            = read_file_to_string("public/js/jwt.js");
    g_inline_assets.layout_js         = read_file_to_string("public/js/layout.js");
    /* Split font CSS: the Google Fonts file is large and font-heavy, so keep it
     * as a separate cached request.  Only the small local fallback font CSS is
     * inlined into the first payload. */
    g_inline_assets.font_css          = read_file_to_string("public/css/google-fonts.css");
    g_inline_assets.font_css_small    = concat_two_files("public/css/pretendard.css",
                                                           "public/css/d2coding.css");
    g_inline_assets.highlight_light_css = read_file_to_string("public/inline_assets/highlight-light.css");
    g_inline_assets.highlight_dark_css  = read_file_to_string("public/inline_assets/highlight-dark.css");
    g_inline_assets.highlight_js        = read_file_to_string("public/inline_assets/highlight.js");
    g_inline_assets.highlight_fortran_js = read_file_to_string("public/inline_assets/highlight-fortran.js");
    g_inline_assets.katex_js            = read_file_to_string("public/inline_assets/katex.js");
    g_inline_assets.katex_css           = read_file_to_string("public/inline_assets/katex.css");
}

static char *concat_two_files(const char *a, const char *b) {
    char *fa = read_file_to_string(a);
    char *fb = read_file_to_string(b);
    size_t la = fa ? strlen(fa) : 0;
    size_t lb = fb ? strlen(fb) : 0;
    if (la + lb == 0) {
        free(fa); free(fb);
        return NULL;
    }
    char *out = (char *)malloc(la + lb + 1);
    if (!out) { free(fa); free(fb); return NULL; }
    size_t pos = 0;
    if (fa) { memcpy(out + pos, fa, la); pos += la; free(fa); }
    if (fb) { memcpy(out + pos, fb, lb); pos += lb; free(fb); }
    out[pos] = '\0';
    return out;
}

static inline_assets_t *get_inline_assets(void) {
    pthread_once(&g_inline_assets_once, load_inline_assets);
    return &g_inline_assets;
}

static bool env_flag_on(const char *name) {
    const char *env = getenv(name);
    return env && (strcmp(env, "1") == 0 || strcasecmp(env, "true") == 0 || strcasecmp(env, "on") == 0);
}

static bool inline_all_enabled(void)     { return env_flag_on("FLYBOARD_INLINE_ALL_ASSETS"); }
static bool inline_shell_enabled(void)   { return inline_all_enabled() || env_flag_on("FLYBOARD_INLINE_SHELL"); }
static bool inline_cdn_enabled(void)     { return inline_all_enabled() || env_flag_on("FLYBOARD_INLINE_CDN"); }

static size_t inline_max_asset_size(void) {
    const char *env = getenv("FLYBOARD_INLINE_MAX_ASSET_SIZE");
    if (env) {
        long val = strtol(env, NULL, 10);
        if (val > 0) return (size_t)val;
    }
    return 64 * 1024; /* keep each inlined script/style chunk under ~64 KiB */
}

static bool inline_asset_fits(const char *asset) {
    if (!asset || !asset[0]) return false;
    return strlen(asset) <= inline_max_asset_size();
}

static __thread char g_nav_profile_name[128];
static __thread char g_nav_profile_account[128];

static char *read_file_to_string(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    struct stat st;
    if (fstat(fileno(f), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
        fclose(f);
        return NULL;
    }
    char *buf = (char *)malloc((size_t)st.st_size + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, (size_t)st.st_size, f);
    fclose(f);
    if (n != (size_t)st.st_size) { free(buf); return NULL; }
    buf[n] = '\0';
    return buf;
}

static bool body_needs_highlight(const char *html) {
    return html && strstr(html, "<code class=\"language-") != NULL;
}

static bool body_needs_katex(const char *html) {
    return html && (strstr(html, "class=\"math-block\"") != NULL ||
                    strstr(html, "class=\"math-inline\"") != NULL);
}

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

    const char *favicon_url = image_inline_favicon();
    if (favicon_url) {
        cwist_html_element_t *favicon_el = cwist_html_element_create("link");
        cwist_html_element_add_attr(favicon_el, "rel", "icon");
        cwist_html_element_add_attr(favicon_el, "href", favicon_url);
        cwist_html_element_add_child(head, favicon_el);
    }

    bool needs_hl = body_needs_highlight(body_html);
    bool needs_katex = body_needs_katex(body_html);

    /* Progressive multi-theme loader: inline critical CSS to prevent FOUC */
    char *critical_css = theme_build_css(dark);
    cwist_html_element_t *dyn_style = cwist_html_element_create("style");
    cwist_html_element_add_attr(dyn_style, "id", "dyn-theme");
    cwist_html_element_set_text(dyn_style, critical_css ? critical_css : "");
    cwist_html_element_add_child(head, dyn_style);
    free(critical_css);

    /* Placeholder for critical shell assets: font CSS and jwt.js + layout.js.
       These are required for the page chrome and theme to render correctly. */
    cwist_html_element_t *inline_head_shell = cwist_html_element_create("meta");
    cwist_html_element_add_attr(inline_head_shell, "data-inline-head-shell", "1");
    cwist_html_element_add_child(head, inline_head_shell);

    /* Placeholder for optional external CDN assets: highlight theme CSS and
       KaTeX CSS. These are loaded from third-party CDNs by default. */
    cwist_html_element_t *inline_head_cdn = cwist_html_element_create("meta");
    cwist_html_element_add_attr(inline_head_cdn, "data-inline-head-cdn", "1");
    cwist_html_element_add_child(head, inline_head_cdn);

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
        cwist_html_element_t *admin_wrap = cwist_html_element_create("div");
        cwist_html_element_add_class(admin_wrap, "nav-admin-dropdown");
        cwist_html_element_t *admin_trigger = cwist_html_element_create("a");
        cwist_html_element_add_attr(admin_trigger, "href", "/admin");
        cwist_html_element_add_attr(admin_trigger, "class", "nav-item nav-admin-trigger");
        cwist_html_element_set_text(admin_trigger, "Dashboard");
        cwist_html_element_add_child(admin_wrap, admin_trigger);
        cwist_html_element_t *admin_menu = cwist_html_element_create("div");
        cwist_html_element_add_attr(admin_menu, "class", "nav-admin-menu");
        cwist_html_element_t *admin_users = cwist_html_element_create("a");
        cwist_html_element_add_attr(admin_users, "href", "/admin/users");
        cwist_html_element_add_attr(admin_users, "class", "nav-admin-subitem");
        cwist_html_element_set_text(admin_users, "Users");
        cwist_html_element_add_child(admin_menu, admin_users);
        cwist_html_element_t *admin_boards = cwist_html_element_create("a");
        cwist_html_element_add_attr(admin_boards, "href", "/admin/boards");
        cwist_html_element_add_attr(admin_boards, "class", "nav-admin-subitem");
        cwist_html_element_set_text(admin_boards, "Manage Boards");
        cwist_html_element_add_child(admin_menu, admin_boards);
        cwist_html_element_add_child(admin_wrap, admin_menu);
        cwist_html_element_add_child(navlinks, admin_wrap);
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
        const char *footer_logo_url = image_inline_logo();
        if (!footer_logo_url) footer_logo_url = "/assets/img/logo.png";
        cwist_html_element_add_attr(footer_logo, "src", footer_logo_url);
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

        /* Placeholder for optional CDN scripts (highlight.js, KaTeX). */
        cwist_html_element_t *inline_body_cdn = cwist_html_element_create("meta");
        if (inline_body_cdn) {
            cwist_html_element_add_attr(inline_body_cdn, "data-inline-body-cdn", "1");
            cwist_html_element_add_child(body, inline_body_cdn);
        }

        /* Placeholder for application scripts (TASFA downloader). */
        cwist_html_element_t *inline_body_app = cwist_html_element_create("meta");
        if (inline_body_app) {
            cwist_html_element_add_attr(inline_body_app, "data-inline-body-app", "1");
            cwist_html_element_add_child(body, inline_body_app);
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

        /* Replace the critical-shell head marker with font CSS and the
         * jwt.js + layout.js bundle. These are required for the page chrome. */
        const char *head_shell_marker = "<meta data-inline-head-shell=\"1\">";
        char *pos_head_shell = strstr(doc->data, head_shell_marker);
        if (pos_head_shell) {
            inline_assets_t *a = get_inline_assets();
            cwist_sstring *head_shell = cwist_sstring_create();
            if (head_shell) {
                /* Fonts are split: the large Google Fonts sheet is always
                 * loaded separately so it does not bloat the first payload.
                 * The small local fallbacks are inlined when shell inlining is
                 * enabled and they fit under the per-asset budget. */
                cwist_sstring_append(head_shell, "<link rel=\"stylesheet\" href=\"/assets/css/google-fonts.css\">");
                if (inline_shell_enabled() && inline_asset_fits(a->font_css_small)) {
                    cwist_sstring_append(head_shell, "<style>");
                    cwist_sstring_append(head_shell, a->font_css_small);
                    cwist_sstring_append(head_shell, "</style>");
                } else {
                    cwist_sstring_append(head_shell, "<link rel=\"stylesheet\" href=\"/assets/css/pretendard.css\">");
                    cwist_sstring_append(head_shell, "<link rel=\"stylesheet\" href=\"/assets/css/d2coding.css\">");
                }

                if (inline_shell_enabled()) {
                    cwist_sstring_append(head_shell, "<script>");
                    if (a->jwt_js) cwist_sstring_append(head_shell, a->jwt_js);
                    if (a->jwt_js && a->layout_js) cwist_sstring_append(head_shell, "\n");
                    if (a->layout_js) cwist_sstring_append(head_shell, a->layout_js);
                    cwist_sstring_append(head_shell, "</script>");

                    if (!a->jwt_js) {
                        cwist_sstring_append(head_shell, "<script src=\"/assets/js/jwt.js?v=1\" defer></script>");
                    }
                    if (!a->layout_js) {
                        cwist_sstring_append(head_shell, "<script src=\"/assets/js/layout.js\" defer></script>");
                    }
                } else {
                    cwist_sstring_append(head_shell, "<script src=\"/assets/js/jwt.js?v=1\" defer></script>");
                    cwist_sstring_append(head_shell, "<script src=\"/assets/js/layout.js\" defer></script>");
                }

                cwist_sstring *doc2 = cwist_sstring_create();
                if (doc2) {
                    size_t before = (size_t)(pos_head_shell - doc->data);
                    cwist_sstring_append_len(doc2, doc->data, before);
                    cwist_sstring_append(doc2, head_shell->data);
                    cwist_sstring_append(doc2, pos_head_shell + strlen(head_shell_marker));
                    cwist_sstring_assign(doc, doc2->data);
                    cwist_sstring_destroy(doc2);
                }
                cwist_sstring_destroy(head_shell);
            }
        }

        /* Replace the CDN head marker with highlight theme CSS and KaTeX CSS.
         * By default these come from external CDNs; FLYBOARD_INLINE_CDN swaps
         * them for locally cached inlined copies. */
        const char *head_cdn_marker = "<meta data-inline-head-cdn=\"1\">";
        char *pos_head_cdn = strstr(doc->data, head_cdn_marker);
        if (pos_head_cdn) {
            inline_assets_t *a = get_inline_assets();
            cwist_sstring *head_cdn = cwist_sstring_create();
            if (head_cdn) {
                /* Load the current highlight theme. The client-side toggle
                 * switches the link href when the user changes theme.
                 * Even with inlining enabled, oversized CSS is kept external. */
                const char *hl_css = dark ? a->highlight_dark_css : a->highlight_light_css;
                if (inline_cdn_enabled() && inline_asset_fits(hl_css)) {
                    cwist_sstring_append(head_cdn, "<style id=\"hl-theme\" data-active=\"");
                    cwist_sstring_append(head_cdn, dark ? "dark" : "light");
                    cwist_sstring_append(head_cdn, "\">");
                    cwist_sstring_append(head_cdn, hl_css);
                    cwist_sstring_append(head_cdn, "</style>");
                } else {
                    cwist_sstring_append(head_cdn, "<link rel=\"stylesheet\" href=\"https://cdnjs.cloudflare.com/ajax/libs/highlight.js/11.9.0/styles/github");
                    cwist_sstring_append(head_cdn, dark ? "-dark" : "");
                    cwist_sstring_append(head_cdn, ".min.css\" id=\"hl-theme\" data-active=\"");
                    cwist_sstring_append(head_cdn, dark ? "dark" : "light");
                    cwist_sstring_append(head_cdn, "\">");
                }

                if (needs_katex) {
                    if (inline_cdn_enabled() && inline_asset_fits(a->katex_css)) {
                        cwist_sstring_append(head_cdn, "<style>");
                        cwist_sstring_append(head_cdn, a->katex_css);
                        cwist_sstring_append(head_cdn, "</style>");
                    } else {
                        cwist_sstring_append(head_cdn, "<link rel=\"stylesheet\" href=\"https://cdn.jsdelivr.net/npm/katex@0.16.9/dist/katex.min.css\">");
                    }
                }

                cwist_sstring *doc2 = cwist_sstring_create();
                if (doc2) {
                    size_t before = (size_t)(pos_head_cdn - doc->data);
                    cwist_sstring_append_len(doc2, doc->data, before);
                    cwist_sstring_append(doc2, head_cdn->data);
                    cwist_sstring_append(doc2, pos_head_cdn + strlen(head_cdn_marker));
                    cwist_sstring_assign(doc, doc2->data);
                    cwist_sstring_destroy(doc2);
                }
                cwist_sstring_destroy(head_cdn);
            }
        }

        /* Replace the CDN body marker with highlight.js and KaTeX scripts.
         * These are optional page-content libraries loaded from CDN unless
         * FLYBOARD_INLINE_CDN is enabled. */
        const char *body_cdn_marker = "<meta data-inline-body-cdn=\"1\">";
        char *pos_body_cdn = strstr(doc->data, body_cdn_marker);
        if (pos_body_cdn) {
            inline_assets_t *a = get_inline_assets();
            cwist_sstring *body_cdn = cwist_sstring_create();
            if (body_cdn) {
                if (needs_hl) {
                    if (inline_cdn_enabled() &&
                        inline_asset_fits(a->highlight_js) &&
                        inline_asset_fits(a->highlight_fortran_js)) {
                        cwist_sstring_append(body_cdn, "<script>");
                        cwist_sstring_append(body_cdn, a->highlight_js);
                        cwist_sstring_append(body_cdn, "\n");
                        cwist_sstring_append(body_cdn, a->highlight_fortran_js);
                        cwist_sstring_append(body_cdn, "\nhljs.highlightAll();</script>");
                    } else {
                        cwist_sstring_append(body_cdn,
                            "<script src=\"https://cdnjs.cloudflare.com/ajax/libs/highlight.js/11.9.0/highlight.min.js\"></script>"
                            "<script src=\"https://cdnjs.cloudflare.com/ajax/libs/highlight.js/11.9.0/languages/fortran.min.js\"></script>"
                            "<script>hljs.highlightAll();</script>");
                    }
                }
                if (needs_katex) {
                    if (inline_cdn_enabled() && inline_asset_fits(a->katex_js)) {
                        cwist_sstring_append(body_cdn, "<script>");
                        cwist_sstring_append(body_cdn, a->katex_js);
                        cwist_sstring_append(body_cdn, "</script>");
                    } else {
                        cwist_sstring_append(body_cdn,
                            "<script src=\"https://cdn.jsdelivr.net/npm/katex@0.16.9/dist/katex.min.js\"></script>");
                    }
                    cwist_sstring_append(body_cdn, "<script src=\"/assets/js/katex-render.js\"></script>");
                }

                cwist_sstring *doc2 = cwist_sstring_create();
                if (doc2) {
                    size_t before = (size_t)(pos_body_cdn - doc->data);
                    cwist_sstring_append_len(doc2, doc->data, before);
                    cwist_sstring_append(doc2, body_cdn->data);
                    cwist_sstring_append(doc2, pos_body_cdn + strlen(body_cdn_marker));
                    cwist_sstring_assign(doc, doc2->data);
                    cwist_sstring_destroy(doc2);
                }
                cwist_sstring_destroy(body_cdn);
            }
        }

        /* Replace the application-script body marker with deferred app scripts
         * such as the TASFA downloader. These are part of the page shell but are
         * not required for first paint. */
        const char *body_app_marker = "<meta data-inline-body-app=\"1\">";
        char *pos_body_app = strstr(doc->data, body_app_marker);
        if (pos_body_app) {
            cwist_sstring *body_app = cwist_sstring_create();
            if (body_app) {
                if (g_config.use_tasfa) {
                    cwist_sstring_append(body_app,
                        "<script src=\"/assets/js/tasfa-download.js\" defer></script>");
                }

                cwist_sstring *doc2 = cwist_sstring_create();
                if (doc2) {
                    size_t before = (size_t)(pos_body_app - doc->data);
                    cwist_sstring_append_len(doc2, doc->data, before);
                    cwist_sstring_append(doc2, body_app->data);
                    cwist_sstring_append(doc2, pos_body_app + strlen(body_app_marker));
                    cwist_sstring_assign(doc, doc2->data);
                    cwist_sstring_destroy(doc2);
                }
                cwist_sstring_destroy(body_app);
            }
        }

        cwist_sstring_destroy(out);
        return doc;
    }
    return NULL;
}

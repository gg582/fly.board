#include "../theme.h"
#include <cjson/cJSON.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static cJSON *create_rule(const char *sel) {
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "sel", sel);
    cJSON_AddItemToObject(r, "decls", cJSON_CreateObject());
    return r;
}

static void add_decl(cJSON *r, const char *p, const char *v) {
    cJSON *decls = cJSON_GetObjectItem(r, "decls");
    if (decls) cJSON_AddStringToObject(decls, p, v);
}

void rule_root(cJSON *vars, theme_color_t *t) {
    cJSON_AddStringToObject(vars, "--bg", t->bg);
    cJSON_AddStringToObject(vars, "--fg", t->fg);
    cJSON_AddStringToObject(vars, "--muted", t->muted);
    cJSON_AddStringToObject(vars, "--panel", t->panel);
    cJSON_AddStringToObject(vars, "--accent", t->accent);
    cJSON_AddStringToObject(vars, "--accent2", t->accent2);
    cJSON_AddStringToObject(vars, "--border", t->border);
    cJSON_AddStringToObject(vars, "--shadow", t->shadow);
    cJSON_AddStringToObject(vars, "--hover", t->hover);
    cJSON_AddStringToObject(vars, "--code-bg", t->code_bg);
    cJSON_AddStringToObject(vars, "--glass-bg", t->glass_bg);
    cJSON_AddStringToObject(vars, "--glass-border", t->glass_border);
    cJSON_AddStringToObject(vars, "--font-body", "'Manrope', 'IBM Plex Sans KR', 'Pretendard Variable', 'Pretendard', -apple-system, BlinkMacSystemFont, system-ui, 'Segoe UI', sans-serif");
    cJSON_AddStringToObject(vars, "--font-display", "'Sora', 'IBM Plex Sans KR', 'Pretendard Variable', sans-serif");
    cJSON_AddStringToObject(vars, "--font-ui", "'IBM Plex Sans KR', 'Manrope', 'Pretendard Variable', sans-serif");
    cJSON_AddStringToObject(vars, "--font-mono", "'JetBrains Mono', 'Fira Code', 'D2Coding', Consolas, Monaco, 'Courier New', monospace");
}

void rule_base(cJSON *rules) {
    cJSON *a = create_rule("*");
    add_decl(a, "box-sizing", "border-box");
    cJSON_AddItemToArray(rules, a);

    cJSON *b = create_rule("html, body");
    add_decl(b, "margin", "0");
    add_decl(b, "padding", "0");
    add_decl(b, "max-width", "100%");
    add_decl(b, "overflow-x", "clip");
    cJSON_AddItemToArray(rules, b);

    cJSON *nav_open = create_rule("html.nav-open, body.nav-open");
    add_decl(nav_open, "overflow", "hidden");
    add_decl(nav_open, "touch-action", "none");
    add_decl(nav_open, "overscroll-behavior", "none");
    cJSON_AddItemToArray(rules, nav_open);

    /* JetBrains Mono @font-face for code */
    cJSON *ff = create_rule("@font-face");
    add_decl(ff, "font-family", "'JetBrains Mono'");
    add_decl(ff, "src", "url('https://cdn.jsdelivr.net/gh/JetBrains/JetBrainsMono@master/web/websites/JetBrainsMono-Regular.woff2') format('woff2')");
    add_decl(ff, "font-weight", "400");
    add_decl(ff, "font-style", "normal");
    add_decl(ff, "font-display", "swap");
    cJSON_AddItemToArray(rules, ff);

    cJSON *body = create_rule("body");
    add_decl(body, "background", "var(--bg)");
    add_decl(body, "color", "var(--fg)");
    add_decl(body, "font", "17px/1.85 var(--font-body)");
    add_decl(body, "font-weight", "400");
    add_decl(body, "letter-spacing", "-0.015em");
    add_decl(body, "font-feature-settings", "'kern' 1, 'liga' 1, 'calt' 1");
    add_decl(body, "transition", "background 0.5s ease, color 0.5s ease");
    cJSON_AddItemToArray(rules, body);

    cJSON *headings = create_rule("h1, h2, h3, h4");
    add_decl(headings, "font-family", "var(--font-display)");
    add_decl(headings, "letter-spacing", "-0.04em");
    add_decl(headings, "line-height", "1.06");
    add_decl(headings, "font-weight", "800");
    cJSON_AddItemToArray(rules, headings);

    cJSON *copy = create_rule("p, li, input, textarea, select, .markdown-body");
    add_decl(copy, "font-size", "1rem");
    cJSON_AddItemToArray(rules, copy);

    cJSON *ui_copy = create_rule("button, label, .btn, .nav-item, .post-badge, .comment-meta, .board-post-meta, .post-row-meta");
    add_decl(ui_copy, "font-family", "var(--font-ui)");
    add_decl(ui_copy, "letter-spacing", "-0.015em");
    cJSON_AddItemToArray(rules, ui_copy);

    cJSON *link = create_rule("a");
    add_decl(link, "color", "var(--accent)");
    add_decl(link, "text-decoration", "none");
    add_decl(link, "transition", "color 0.2s ease");
    cJSON_AddItemToArray(rules, link);

    cJSON *linkh = create_rule("a:hover");
    add_decl(linkh, "color", "var(--accent2)");
    cJSON_AddItemToArray(rules, linkh);

    cJSON *sel = create_rule("::selection");
    add_decl(sel, "background", "var(--fg)");
    add_decl(sel, "color", "var(--bg)");
    cJSON_AddItemToArray(rules, sel);
}

void rule_layout(cJSON *rules) {
    cJSON *shell = create_rule(".shell");
    add_decl(shell, "max-width", "1400px");
    add_decl(shell, "margin", "0 auto");
    add_decl(shell, "padding", "16px");
    cJSON_AddItemToArray(rules, shell);

    cJSON *content = create_rule(".content");
    add_decl(content, "width", "100%");
    add_decl(content, "display", "grid");
    add_decl(content, "grid-template-columns", "minmax(0, 1fr) minmax(0, 880px) minmax(0, 1fr)");
    add_decl(content, "margin", "0 auto");
    add_decl(content, "padding", "0 20px 40px");
    add_decl(content, "min-width", "0");
    add_decl(content, "overflow-x", "clip");
    cJSON_AddItemToArray(rules, content);

    cJSON *content_children = create_rule(".content > *");
    add_decl(content_children, "grid-column", "2");
    add_decl(content_children, "width", "100%");
    add_decl(content_children, "max-width", "100%");
    add_decl(content_children, "min-width", "0");
    cJSON_AddItemToArray(rules, content_children);

    cJSON *nav = create_rule(".topbar");
    add_decl(nav, "display", "flex");
    add_decl(nav, "align-items", "center");
    add_decl(nav, "justify-content", "space-between");
    add_decl(nav, "gap", "16px");
    add_decl(nav, "width", "100%");
    add_decl(nav, "margin", "0");
    add_decl(nav, "padding", "0 16px");
    add_decl(nav, "background", "color-mix(in srgb, var(--glass-bg) 92%, transparent)");
    add_decl(nav, "border-bottom", "1px solid var(--glass-border)");
    add_decl(nav, "position", "sticky");
    add_decl(nav, "top", "0");
    add_decl(nav, "z-index", "100");
    add_decl(nav, "backdrop-filter", "blur(24px) saturate(180%)");
    add_decl(nav, "-webkit-backdrop-filter", "blur(24px) saturate(180%)");
    add_decl(nav, "opacity", "0.96");
    add_decl(nav, "box-shadow", "0 10px 40px color-mix(in srgb, var(--shadow) 45%, transparent)");
    add_decl(nav, "transition", "background 0.25s ease, border-color 0.25s ease, box-shadow 0.25s ease, opacity 0.25s ease");
    cJSON_AddItemToArray(rules, nav);

    cJSON *navlinks = create_rule(".nav-links");
    add_decl(navlinks, "display", "flex");
    add_decl(navlinks, "gap", "12px");
    add_decl(navlinks, "align-items", "center");
    add_decl(navlinks, "flex-wrap", "wrap");
    add_decl(navlinks, "min-width", "0");
    cJSON_AddItemToArray(rules, navlinks);

    cJSON *topbar_search = create_rule(".topbar-search");
    add_decl(topbar_search, "display", "inline-flex");
    add_decl(topbar_search, "align-items", "center");
    add_decl(topbar_search, "gap", "6px");
    cJSON_AddItemToArray(rules, topbar_search);

    cJSON *topbar_search_input = create_rule(".topbar-search input");
    add_decl(topbar_search_input, "width", "160px");
    add_decl(topbar_search_input, "padding", "6px 10px");
    add_decl(topbar_search_input, "font-size", "14px");
    cJSON_AddItemToArray(rules, topbar_search_input);

    cJSON *navitem = create_rule(".nav-item");
    add_decl(navitem, "padding", "9px 12px");
    add_decl(navitem, "border-radius", "0");
    add_decl(navitem, "border-bottom", "1px solid transparent");
    add_decl(navitem, "font-weight", "600");
    add_decl(navitem, "font-size", "15px");
    add_decl(navitem, "letter-spacing", "-0.02em");
    add_decl(navitem, "color", "var(--fg)");
    add_decl(navitem, "opacity", "0.86");
    add_decl(navitem, "transition", "background 0.18s ease, color 0.18s ease, opacity 0.18s ease, border-color 0.18s ease");
    cJSON_AddItemToArray(rules, navitem);

    cJSON *navitemh = create_rule(".nav-item:hover");
    add_decl(navitemh, "background", "color-mix(in srgb, var(--glass-bg) 36%, transparent)");
    add_decl(navitemh, "color", "var(--accent)");
    add_decl(navitemh, "opacity", "1");
    add_decl(navitemh, "border-bottom-color", "var(--accent)");
    cJSON_AddItemToArray(rules, navitemh);

    cJSON *brand = create_rule(".topbar-brand");
    add_decl(brand, "display", "flex");
    add_decl(brand, "flex-direction", "column");
    add_decl(brand, "align-items", "flex-start");
    add_decl(brand, "gap", "4px");
    add_decl(brand, "line-height", "1.2");
    cJSON_AddItemToArray(rules, brand);

    cJSON *brand_title = create_rule(".topbar-title");
    add_decl(brand_title, "font-weight", "800");
    add_decl(brand_title, "font-size", "22px");
    add_decl(brand_title, "font-family", "var(--font-display)");
    add_decl(brand_title, "letter-spacing", "-0.045em");
    cJSON_AddItemToArray(rules, brand_title);

    cJSON *desktop_only = create_rule(".desktop-only");
    add_decl(desktop_only, "display", "inline-flex");
    cJSON_AddItemToArray(rules, desktop_only);

    cJSON *mobile_only = create_rule(".mobile-only");
    add_decl(mobile_only, "display", "none");
    cJSON_AddItemToArray(rules, mobile_only);

    cJSON *board_dd = create_rule(".nav-board-dropdown");
    add_decl(board_dd, "position", "relative");
    add_decl(board_dd, "display", "inline-flex");
    add_decl(board_dd, "align-items", "center");
    cJSON_AddItemToArray(rules, board_dd);

    cJSON *board_menu = create_rule(".nav-board-menu");
    add_decl(board_menu, "position", "absolute");
    add_decl(board_menu, "top", "calc(100% + 10px)");
    add_decl(board_menu, "left", "0");
    add_decl(board_menu, "min-width", "240px");
    add_decl(board_menu, "max-height", "360px");
    add_decl(board_menu, "overflow-y", "auto");
    add_decl(board_menu, "background", "color-mix(in srgb, var(--glass-bg) 96%, transparent)");
    add_decl(board_menu, "backdrop-filter", "blur(22px) saturate(180%)");
    add_decl(board_menu, "-webkit-backdrop-filter", "blur(22px) saturate(180%)");
    add_decl(board_menu, "border", "1px solid var(--glass-border)");
    add_decl(board_menu, "box-shadow", "0 18px 48px color-mix(in srgb, var(--shadow) 60%, transparent)");
    add_decl(board_menu, "padding", "8px");
    add_decl(board_menu, "display", "none");
    add_decl(board_menu, "opacity", "0");
    add_decl(board_menu, "pointer-events", "none");
    add_decl(board_menu, "transform", "translateY(-8px)");
    add_decl(board_menu, "transition", "opacity 0.18s ease, transform 0.18s ease, border-color 0.18s ease");
    cJSON_AddItemToArray(rules, board_menu);

    cJSON *board_menu_open = create_rule(".nav-board-dropdown:hover .nav-board-menu");
    add_decl(board_menu_open, "display", "block");
    add_decl(board_menu_open, "opacity", "1");
    add_decl(board_menu_open, "pointer-events", "auto");
    add_decl(board_menu_open, "transform", "translateY(0)");
    cJSON_AddItemToArray(rules, board_menu_open);

    cJSON *board_menu_list = create_rule(".nav-board-menu-list");
    add_decl(board_menu_list, "display", "flex");
    add_decl(board_menu_list, "flex-direction", "column");
    cJSON_AddItemToArray(rules, board_menu_list);

    cJSON *board_sub = create_rule(".nav-board-subitem");
    add_decl(board_sub, "display", "block");
    add_decl(board_sub, "padding", "10px 12px");
    add_decl(board_sub, "color", "var(--fg)");
    add_decl(board_sub, "font-size", "14px");
    add_decl(board_sub, "font-weight", "600");
    add_decl(board_sub, "letter-spacing", "-0.02em");
    add_decl(board_sub, "border-radius", "0");
    add_decl(board_sub, "transition", "background 0.18s ease, color 0.18s ease, opacity 0.18s ease");
    add_decl(board_sub, "opacity", "0.9");
    cJSON_AddItemToArray(rules, board_sub);

    cJSON *board_sub_h = create_rule(".nav-board-subitem:hover");
    add_decl(board_sub_h, "background", "color-mix(in srgb, var(--glass-bg) 42%, transparent)");
    add_decl(board_sub_h, "color", "var(--accent)");
    add_decl(board_sub_h, "opacity", "1");
    cJSON_AddItemToArray(rules, board_sub_h);

    cJSON *board_sub_all = create_rule(".nav-board-subitem-all");
    add_decl(board_sub_all, "font-weight", "700");
    add_decl(board_sub_all, "border-bottom", "1px solid var(--glass-border)");
    add_decl(board_sub_all, "margin-bottom", "6px");
    add_decl(board_sub_all, "border-radius", "0");
    cJSON_AddItemToArray(rules, board_sub_all);

    cJSON *board_empty = create_rule(".nav-board-empty");
    add_decl(board_empty, "display", "block");
    add_decl(board_empty, "padding", "10px 12px");
    add_decl(board_empty, "font-size", "13px");
    add_decl(board_empty, "color", "var(--muted)");
    cJSON_AddItemToArray(rules, board_empty);

    cJSON *footer = create_rule(".site-footer");
    add_decl(footer, "text-align", "center");
    add_decl(footer, "padding", "40px 24px");
    add_decl(footer, "color", "var(--muted)");
    add_decl(footer, "font-size", "13px");
    add_decl(footer, "border-top", "1px solid var(--border)");
    add_decl(footer, "margin-top", "40px");
    add_decl(footer, "transition", "border-color 0.5s ease, color 0.5s ease");
    cJSON_AddItemToArray(rules, footer);

    cJSON *fc = create_rule(".footer-content");
    add_decl(fc, "display", "flex");
    add_decl(fc, "align-items", "center");
    add_decl(fc, "justify-content", "center");
    add_decl(fc, "gap", "8px");
    cJSON_AddItemToArray(rules, fc);

    cJSON *fl = create_rule(".footer-logo");
    add_decl(fl, "height", "16px");
    add_decl(fl, "width", "auto");
    add_decl(fl, "opacity", "0.6");
    cJSON_AddItemToArray(rules, fl);

    cJSON *pp = create_rule(".profile-pic");
    add_decl(pp, "width", "40px");
    add_decl(pp, "height", "40px");
    add_decl(pp, "border-radius", "50%");
    add_decl(pp, "object-fit", "cover");
    add_decl(pp, "border", "1px solid var(--border)");
    cJSON_AddItemToArray(rules, pp);

    cJSON *pp_s = create_rule(".profile-pic-small");
    add_decl(pp_s, "width", "24px");
    add_decl(pp_s, "height", "24px");
    add_decl(pp_s, "border-radius", "50%");
    add_decl(pp_s, "object-fit", "cover");
    cJSON_AddItemToArray(rules, pp_s);

    cJSON *burger = create_rule(".burger-btn");
    add_decl(burger, "display", "none");
    add_decl(burger, "align-items", "center");
    add_decl(burger, "justify-content", "center");
    add_decl(burger, "background", "none");
    add_decl(burger, "border", "none");
    add_decl(burger, "color", "var(--fg)");
    add_decl(burger, "font-size", "24px");
    add_decl(burger, "line-height", "1");
    add_decl(burger, "cursor", "pointer");
    add_decl(burger, "padding", "0");
    add_decl(burger, "width", "40px");
    add_decl(burger, "height", "40px");
    add_decl(burger, "border-radius", "0");
    add_decl(burger, "position", "relative");
    add_decl(burger, "z-index", "102");
    add_decl(burger, "transition", "color 0.2s ease, background 0.2s ease");
    cJSON_AddItemToArray(rules, burger);

    cJSON *burger_h = create_rule(".burger-btn:hover");
    add_decl(burger_h, "color", "var(--accent)");
    add_decl(burger_h, "background", "var(--hover)");
    cJSON_AddItemToArray(rules, burger_h);

    cJSON *burger_icon = create_rule(".burger-icon");
    add_decl(burger_icon, "display", "inline-block");
    add_decl(burger_icon, "transform-origin", "50% 50%");
    add_decl(burger_icon, "transition", "transform 0.4s cubic-bezier(0.2, 0.7, 0, 1)");
    cJSON_AddItemToArray(rules, burger_icon);

    cJSON *burger_open = create_rule(".burger-btn.open .burger-icon");
    add_decl(burger_open, "transform", "rotate(180deg)");
    cJSON_AddItemToArray(rules, burger_open);

    cJSON *overlay = create_rule(".mobile-overlay");
    add_decl(overlay, "display", "none");
    add_decl(overlay, "position", "fixed");
    add_decl(overlay, "top", "0");
    add_decl(overlay, "left", "0");
    add_decl(overlay, "width", "100vw");
    add_decl(overlay, "height", "100vh");
    add_decl(overlay, "background", "rgba(8,10,18,0.24)");
    add_decl(overlay, "backdrop-filter", "blur(6px)");
    add_decl(overlay, "-webkit-backdrop-filter", "blur(6px)");
    add_decl(overlay, "opacity", "0");
    add_decl(overlay, "pointer-events", "none");
    add_decl(overlay, "transition", "opacity 0.22s ease");
    add_decl(overlay, "z-index", "98");
    cJSON_AddItemToArray(rules, overlay);

    cJSON *overlay_open = create_rule(".mobile-overlay.open");
    add_decl(overlay_open, "opacity", "1");
    add_decl(overlay_open, "pointer-events", "auto");
    cJSON_AddItemToArray(rules, overlay_open);

    cJSON *ua_desktop_only = create_rule("html.mobile-device .desktop-only");
    add_decl(ua_desktop_only, "display", "none");
    cJSON_AddItemToArray(rules, ua_desktop_only);

    cJSON *ua_mobile_only = create_rule("html.mobile-device .mobile-only");
    add_decl(ua_mobile_only, "display", "block");
    cJSON_AddItemToArray(rules, ua_mobile_only);

    cJSON *ua_burger = create_rule("html.mobile-device .burger-btn");
    add_decl(ua_burger, "display", "inline-flex");
    cJSON_AddItemToArray(rules, ua_burger);

    cJSON *ua_navlinks = create_rule("html.mobile-device .nav-links");
    add_decl(ua_navlinks, "position", "fixed");
    add_decl(ua_navlinks, "top", "0");
    add_decl(ua_navlinks, "left", "0");
    add_decl(ua_navlinks, "width", "260px");
    add_decl(ua_navlinks, "height", "100dvh");
    add_decl(ua_navlinks, "max-height", "100dvh");
    add_decl(ua_navlinks, "background", "color-mix(in srgb, var(--glass-bg) 96%, transparent)");
    add_decl(ua_navlinks, "backdrop-filter", "blur(24px) saturate(180%)");
    add_decl(ua_navlinks, "-webkit-backdrop-filter", "blur(24px) saturate(180%)");
    add_decl(ua_navlinks, "flex-direction", "column");
    add_decl(ua_navlinks, "align-items", "stretch");
    add_decl(ua_navlinks, "gap", "0");
    add_decl(ua_navlinks, "padding", "56px 0 24px");
    add_decl(ua_navlinks, "overflow-y", "auto");
    add_decl(ua_navlinks, "overscroll-behavior", "contain");
    add_decl(ua_navlinks, "-webkit-overflow-scrolling", "touch");
    add_decl(ua_navlinks, "transform", "translateX(-100%)");
    add_decl(ua_navlinks, "transition", "transform 0.3s ease");
    add_decl(ua_navlinks, "z-index", "101");
    add_decl(ua_navlinks, "border-right", "1px solid var(--border)");
    add_decl(ua_navlinks, "border-top", "1px solid var(--glass-border)");
    add_decl(ua_navlinks, "display", "none");
    cJSON_AddItemToArray(rules, ua_navlinks);

    cJSON *ua_navlinks_open = create_rule("html.mobile-device .nav-links.open");
    add_decl(ua_navlinks_open, "display", "flex");
    add_decl(ua_navlinks_open, "transform", "translateX(0)");
    cJSON_AddItemToArray(rules, ua_navlinks_open);

    cJSON *ua_navitem = create_rule("html.mobile-device .nav-item");
    add_decl(ua_navitem, "padding", "14px 24px");
    add_decl(ua_navitem, "border-bottom", "1px solid var(--border)");
    cJSON_AddItemToArray(rules, ua_navitem);

    cJSON *ua_theme_switch = create_rule("html.mobile-device .theme-switch");
    add_decl(ua_theme_switch, "padding", "14px 24px");
    cJSON_AddItemToArray(rules, ua_theme_switch);

    cJSON *ua_overlay = create_rule("html.mobile-device .mobile-overlay");
    add_decl(ua_overlay, "display", "block");
    cJSON_AddItemToArray(rules, ua_overlay);
}

void rule_components(cJSON *rules) {
    cJSON *card = create_rule(".card");
    add_decl(card, "background", "color-mix(in srgb, var(--glass-bg) 92%, transparent)");
    add_decl(card, "backdrop-filter", "blur(22px) saturate(170%)");
    add_decl(card, "-webkit-backdrop-filter", "blur(22px) saturate(170%)");
    add_decl(card, "border", "1px solid var(--glass-border)");
    add_decl(card, "border-radius", "0");
    add_decl(card, "padding", "26px");
    add_decl(card, "box-shadow", "0 8px 30px color-mix(in srgb, var(--shadow) 52%, transparent)");
    add_decl(card, "transition", "box-shadow 0.2s ease, background 0.2s ease, border-color 0.2s ease, opacity 0.2s ease");
    add_decl(card, "opacity", "0.98");
    cJSON_AddItemToArray(rules, card);

    cJSON *cardh = create_rule(".card:hover");
    add_decl(cardh, "box-shadow", "0 10px 26px color-mix(in srgb, var(--shadow) 56%, transparent)");
    add_decl(cardh, "opacity", "1");
    cJSON_AddItemToArray(rules, cardh);

    cJSON *btn = create_rule(".btn");
    add_decl(btn, "display", "inline-flex");
    add_decl(btn, "align-items", "center");
    add_decl(btn, "gap", "6px");
    add_decl(btn, "padding", "8px 16px");
    add_decl(btn, "border", "none");
    add_decl(btn, "border-radius", "0");
    add_decl(btn, "background", "var(--accent)");
    add_decl(btn, "color", "#fff");
    add_decl(btn, "font-weight", "600");
    add_decl(btn, "letter-spacing", "0.02em");
    add_decl(btn, "cursor", "pointer");
    add_decl(btn, "transition", "filter 0.2s ease, transform 0.15s ease, background 0.5s ease, box-shadow 0.2s ease");
    cJSON_AddItemToArray(rules, btn);

    cJSON *btnh = create_rule(".btn:hover");
    add_decl(btnh, "filter", "brightness(1.1)");
    add_decl(btnh, "transform", "scale(1.02)");
    cJSON_AddItemToArray(rules, btnh);

    cJSON *btna = create_rule(".btn:active");
    add_decl(btna, "transform", "scale(0.98)");
    add_decl(btna, "box-shadow", "inset 0 2px 4px rgba(0,0,0,0.1)");
    cJSON_AddItemToArray(rules, btna);

    cJSON *btnf = create_rule(".btn:focus-visible");
    add_decl(btnf, "outline", "none");
    add_decl(btnf, "box-shadow", "0 0 0 3px rgba(79,70,229,0.25)");
    cJSON_AddItemToArray(rules, btnf);

    cJSON *btn2 = create_rule(".btn-outline");
    add_decl(btn2, "background", "transparent");
    add_decl(btn2, "color", "var(--accent)");
    add_decl(btn2, "border", "1px solid var(--accent)");
    cJSON_AddItemToArray(rules, btn2);

    cJSON *input = create_rule("input, textarea, select");
    add_decl(input, "width", "100%");
    add_decl(input, "padding", "12px 14px");
    add_decl(input, "border", "1px solid var(--glass-border)");
    add_decl(input, "border-radius", "0");
    add_decl(input, "background", "color-mix(in srgb, var(--glass-bg) 90%, transparent)");
    add_decl(input, "backdrop-filter", "blur(14px)");
    add_decl(input, "-webkit-backdrop-filter", "blur(14px)");
    add_decl(input, "color", "var(--fg)");
    add_decl(input, "font", "inherit");
    add_decl(input, "outline", "none");
    add_decl(input, "transition", "border-color 0.2s ease, box-shadow 0.2s ease, background 0.5s ease, color 0.5s ease");
    cJSON_AddItemToArray(rules, input);

    cJSON *inputf = create_rule("input:focus, textarea:focus, select:focus");
    add_decl(inputf, "border-color", "var(--accent)");
    add_decl(inputf, "box-shadow", "0 0 0 3px rgba(79,70,229,0.15)");
    cJSON_AddItemToArray(rules, inputf);

    cJSON *placeholder = create_rule("::placeholder");
    add_decl(placeholder, "color", "var(--muted)");
    add_decl(placeholder, "opacity", "0.7");
    cJSON_AddItemToArray(rules, placeholder);

    cJSON *label = create_rule("label");
    add_decl(label, "display", "block");
    add_decl(label, "margin-bottom", "6px");
    add_decl(label, "font-weight", "600");
    add_decl(label, "font-size", "14px");
    cJSON_AddItemToArray(rules, label);

    cJSON *alert = create_rule(".alert");
    add_decl(alert, "padding", "12px 14px");
    add_decl(alert, "border-radius", "0");
    add_decl(alert, "background", "rgba(239,68,68,0.08)");
    add_decl(alert, "color", "#ef4444");
    add_decl(alert, "border", "1px solid rgba(239,68,68,0.25)");
    add_decl(alert, "margin-bottom", "14px");
    cJSON_AddItemToArray(rules, alert);

    cJSON *tbl_scroll = create_rule(".table-scroll");
    add_decl(tbl_scroll, "width", "100%");
    add_decl(tbl_scroll, "overflow-x", "auto");
    cJSON_AddItemToArray(rules, tbl_scroll);

    cJSON *admin_tbl = create_rule(".admin-user-table");
    add_decl(admin_tbl, "min-width", "680px");
    cJSON_AddItemToArray(rules, admin_tbl);

    cJSON *admin_role_col = create_rule(".admin-user-table .admin-role-col");
    add_decl(admin_role_col, "width", "120px");
    cJSON_AddItemToArray(rules, admin_role_col);

    cJSON *admin_action_col = create_rule(".admin-user-table .admin-action-col");
    add_decl(admin_action_col, "width", "270px");
    cJSON_AddItemToArray(rules, admin_action_col);

    cJSON *admin_action_cell = create_rule(".admin-user-table .admin-action-cell");
    add_decl(admin_action_cell, "white-space", "nowrap");
    cJSON_AddItemToArray(rules, admin_action_cell);

    cJSON *admin_form = create_rule(".admin-role-form, .admin-delete-form");
    add_decl(admin_form, "display", "inline-flex");
    add_decl(admin_form, "align-items", "center");
    cJSON_AddItemToArray(rules, admin_form);

    cJSON *admin_select = create_rule(".admin-role-select");
    add_decl(admin_select, "width", "auto");
    add_decl(admin_select, "min-width", "96px");
    cJSON_AddItemToArray(rules, admin_select);

    /* Code copy button */
    cJSON *pre_wrap = create_rule(".markdown-body pre");
    add_decl(pre_wrap, "position", "relative");
    cJSON_AddItemToArray(rules, pre_wrap);

    cJSON *code_copy = create_rule(".code-copy");
    add_decl(code_copy, "position", "absolute");
    add_decl(code_copy, "top", "8px");
    add_decl(code_copy, "right", "8px");
    add_decl(code_copy, "padding", "4px 10px");
    add_decl(code_copy, "font-size", "12px");
    add_decl(code_copy, "font-weight", "600");
    add_decl(code_copy, "border-radius", "6px");
    add_decl(code_copy, "border", "1px solid var(--border)");
    add_decl(code_copy, "background", "var(--panel)");
    add_decl(code_copy, "color", "var(--muted)");
    add_decl(code_copy, "cursor", "pointer");
    add_decl(code_copy, "opacity", "0");
    add_decl(code_copy, "transition", "opacity 0.2s ease, color 0.2s ease, background 0.2s ease");
    cJSON_AddItemToArray(rules, code_copy);

    cJSON *pre_hover = create_rule(".markdown-body pre:hover .code-copy");
    add_decl(pre_hover, "opacity", "1");
    cJSON_AddItemToArray(rules, pre_hover);

    cJSON *code_copy_h = create_rule(".code-copy:hover");
    add_decl(code_copy_h, "background", "var(--accent)");
    add_decl(code_copy_h, "color", "#fff");
    add_decl(code_copy_h, "border-color", "var(--accent)");
    cJSON_AddItemToArray(rules, code_copy_h);

    /* Comment thread redesign */
    cJSON *cnode = create_rule(".comment-node");
    add_decl(cnode, "border-left", "2px solid var(--border)");
    add_decl(cnode, "padding-left", "12px");
    add_decl(cnode, "margin-top", "12px");
    add_decl(cnode, "position", "relative");
    cJSON_AddItemToArray(rules, cnode);

    cJSON *cheader = create_rule(".comment-header");
    add_decl(cheader, "display", "flex");
    add_decl(cheader, "align-items", "center");
    add_decl(cheader, "gap", "8px");
    add_decl(cheader, "margin-bottom", "6px");
    cJSON_AddItemToArray(rules, cheader);

    cJSON *cavatar = create_rule(".comment-avatar");
    add_decl(cavatar, "width", "28px");
    add_decl(cavatar, "height", "28px");
    add_decl(cavatar, "border-radius", "50%");
    add_decl(cavatar, "background", "var(--accent)");
    add_decl(cavatar, "color", "#fff");
    add_decl(cavatar, "display", "flex");
    add_decl(cavatar, "align-items", "center");
    add_decl(cavatar, "justify-content", "center");
    add_decl(cavatar, "font-size", "12px");
    add_decl(cavatar, "font-weight", "700");
    add_decl(cavatar, "flex-shrink", "0");
    cJSON_AddItemToArray(rules, cavatar);

    cJSON *cmeta = create_rule(".comment-meta");
    add_decl(cmeta, "font-size", "13px");
    add_decl(cmeta, "color", "var(--muted)");
    add_decl(cmeta, "font-weight", "500");
    cJSON_AddItemToArray(rules, cmeta);

    cJSON *cdate = create_rule(".comment-date");
    add_decl(cdate, "font-weight", "400");
    add_decl(cdate, "opacity", "0.8");
    cJSON_AddItemToArray(rules, cdate);

    cJSON *cbody = create_rule(".comment-body");
    add_decl(cbody, "font-size", "15px");
    add_decl(cbody, "line-height", "1.6");
    cJSON_AddItemToArray(rules, cbody);

    cJSON *cbody_p = create_rule(".comment-body p");
    add_decl(cbody_p, "margin", "0 0 8px");
    cJSON_AddItemToArray(rules, cbody_p);
}

void rule_home(cJSON *rules) {
    cJSON *hero = create_rule(".hero");
    add_decl(hero, "padding", "48px 0 36px");
    add_decl(hero, "text-align", "center");
    cJSON_AddItemToArray(rules, hero);

    cJSON *hero_h1 = create_rule(".hero h1");
    add_decl(hero_h1, "font-size", "clamp(3.1rem, 6vw, 5.6rem)");
    add_decl(hero_h1, "margin", "0 0 10px");
    add_decl(hero_h1, "font-family", "var(--font-display)");
    add_decl(hero_h1, "letter-spacing", "-0.06em");
    add_decl(hero_h1, "line-height", "1.1");
    add_decl(hero_h1, "font-weight", "900");
    add_decl(hero_h1, "text-shadow", "0 2px 16px var(--shadow)");
    cJSON_AddItemToArray(rules, hero_h1);

    cJSON *hero_logo = create_rule(".hero-logo");
    add_decl(hero_logo, "height", "120px");
    add_decl(hero_logo, "width", "auto");
    add_decl(hero_logo, "margin-bottom", "12px");
    cJSON_AddItemToArray(rules, hero_logo);

    cJSON *hero_p = create_rule(".hero p");
    add_decl(hero_p, "color", "var(--muted)");
    add_decl(hero_p, "font-size", "20px");
    add_decl(hero_p, "max-width", "640px");
    add_decl(hero_p, "margin", "0 auto");
    add_decl(hero_p, "line-height", "1.6");
    add_decl(hero_p, "letter-spacing", "-0.01em");
    cJSON_AddItemToArray(rules, hero_p);

    cJSON *grid = create_rule(".post-grid");
    add_decl(grid, "display", "grid");
    add_decl(grid, "grid-template-columns", "repeat(auto-fill, minmax(300px, 1fr))");
    add_decl(grid, "gap", "18px");
    add_decl(grid, "margin-top", "24px");
    cJSON_AddItemToArray(rules, grid);

    cJSON *tag = create_rule(".tag");
    add_decl(tag, "display", "inline-block");
    add_decl(tag, "padding", "6px 14px");
    add_decl(tag, "border-radius", "999px");
    add_decl(tag, "background", "var(--hover)");
    add_decl(tag, "border", "1px solid var(--border)");
    add_decl(tag, "font-size", "12px");
    add_decl(tag, "font-weight", "600");
    add_decl(tag, "color", "var(--accent)");
    add_decl(tag, "margin", "4px 6px");
    add_decl(tag, "transition", "background 0.5s ease, color 0.5s ease, border-color 0.5s ease, transform 0.2s ease");
    cJSON_AddItemToArray(rules, tag);

    cJSON *tagh = create_rule(".tag:hover");
    add_decl(tagh, "transform", "translateY(-1px)");
    add_decl(tagh, "border-color", "var(--accent)");
    cJSON_AddItemToArray(rules, tagh);

    cJSON *board_sec = create_rule(".board-section");
    add_decl(board_sec, "background", "color-mix(in srgb, var(--glass-bg) 90%, transparent)");
    add_decl(board_sec, "border", "1px solid var(--glass-border)");
    add_decl(board_sec, "border-radius", "0");
    add_decl(board_sec, "padding", "22px");
    add_decl(board_sec, "backdrop-filter", "blur(20px) saturate(160%)");
    add_decl(board_sec, "-webkit-backdrop-filter", "blur(20px) saturate(160%)");
    add_decl(board_sec, "box-shadow", "0 10px 30px color-mix(in srgb, var(--shadow) 45%, transparent)");
    add_decl(board_sec, "transition", "background 0.5s ease, border-color 0.5s ease");
    cJSON_AddItemToArray(rules, board_sec);
}

void rule_boards(cJSON *rules) {
    cJSON *grid = create_rule(".board-grid");
    add_decl(grid, "display", "grid");
    add_decl(grid, "grid-template-columns", "repeat(auto-fill, minmax(360px, 1fr))");
    add_decl(grid, "gap", "24px");
    add_decl(grid, "max-width", "1200px");
    add_decl(grid, "margin", "0 auto");
    add_decl(grid, "padding", "0 0 40px");
    cJSON_AddItemToArray(rules, grid);

    cJSON *blist = create_rule(".board-list");
    add_decl(blist, "max-width", "1100px");
    add_decl(blist, "margin", "0 auto");
    add_decl(blist, "padding", "20px 0 40px");
    add_decl(blist, "display", "flex");
    add_decl(blist, "flex-direction", "column");
    add_decl(blist, "gap", "20px");
    cJSON_AddItemToArray(rules, blist);

    cJSON *bline = create_rule(".board-line");
    add_decl(bline, "display", "grid");
    add_decl(bline, "gap", "10px");
    add_decl(bline, "padding", "24px");
    add_decl(bline, "background", "color-mix(in srgb, var(--glass-bg) 88%, transparent)");
    add_decl(bline, "border", "1px solid var(--glass-border)");
    add_decl(bline, "border-radius", "0");
    add_decl(bline, "backdrop-filter", "blur(18px) saturate(155%)");
    add_decl(bline, "-webkit-backdrop-filter", "blur(18px) saturate(155%)");
    add_decl(bline, "box-shadow", "0 4px 18px color-mix(in srgb, var(--shadow) 28%, transparent)");
    add_decl(bline, "opacity", "0.98");
    add_decl(bline, "transition", "box-shadow 0.18s ease, background 0.18s ease, border-color 0.18s ease, opacity 0.18s ease");
    cJSON_AddItemToArray(rules, bline);

    cJSON *blineh = create_rule(".board-line:hover");
    add_decl(blineh, "transform", "none");
    add_decl(blineh, "box-shadow", "0 4px 18px color-mix(in srgb, var(--shadow) 32%, transparent)");
    add_decl(blineh, "border-color", "color-mix(in srgb, var(--glass-border) 60%, var(--accent) 40%)");
    add_decl(blineh, "opacity", "1");
    cJSON_AddItemToArray(rules, blineh);

    cJSON *bline_head = create_rule(".board-line-head");
    add_decl(bline_head, "display", "flex");
    add_decl(bline_head, "justify-content", "space-between");
    add_decl(bline_head, "align-items", "baseline");
    add_decl(bline_head, "gap", "10px");
    cJSON_AddItemToArray(rules, bline_head);

    cJSON *bline_title = create_rule(".board-line-title");
    add_decl(bline_title, "margin", "0");
    add_decl(bline_title, "font-size", "clamp(1.7rem, 2.8vw, 2.35rem)");
    add_decl(bline_title, "font-family", "var(--font-display)");
    add_decl(bline_title, "font-weight", "800");
    add_decl(bline_title, "letter-spacing", "-0.045em");
    add_decl(bline_title, "color", "var(--fg)");
    cJSON_AddItemToArray(rules, bline_title);

    cJSON *card = create_rule(".board-card");
    add_decl(card, "background", "var(--glass-bg)");
    add_decl(card, "backdrop-filter", "blur(20px) saturate(180%)");
    add_decl(card, "-webkit-backdrop-filter", "blur(20px) saturate(180%)");
    add_decl(card, "border", "1px solid var(--glass-border)");
    add_decl(card, "border-radius", "0");
    add_decl(card, "padding", "28px");
    add_decl(card, "box-shadow", "0 4px 18px color-mix(in srgb, var(--shadow) 28%, transparent)");
    add_decl(card, "transition", "transform 0.35s ease, box-shadow 0.35s ease, background 0.5s ease, border-color 0.5s ease");
    add_decl(card, "display", "flex");
    add_decl(card, "flex-direction", "column");
    add_decl(card, "gap", "16px");
    cJSON_AddItemToArray(rules, card);

    cJSON *cardh = create_rule(".board-card:hover");
    add_decl(cardh, "transform", "none");
    add_decl(cardh, "box-shadow", "0 4px 18px color-mix(in srgb, var(--shadow) 32%, transparent)");
    cJSON_AddItemToArray(rules, cardh);

    cJSON *ch = create_rule(".board-card-header");
    add_decl(ch, "display", "flex");
    add_decl(ch, "justify-content", "space-between");
    add_decl(ch, "align-items", "flex-start");
    add_decl(ch, "gap", "12px");
    cJSON_AddItemToArray(rules, ch);

    cJSON *ch2 = create_rule(".board-card h2");
    add_decl(ch2, "margin", "0");
    add_decl(ch2, "font-size", "clamp(1.45rem, 2vw, 1.9rem)");
    add_decl(ch2, "font-family", "var(--font-display)");
    add_decl(ch2, "font-weight", "800");
    add_decl(ch2, "color", "var(--fg)");
    add_decl(ch2, "letter-spacing", "-0.3px");
    cJSON_AddItemToArray(rules, ch2);

    cJSON *ch2a = create_rule(".board-card h2 a");
    add_decl(ch2a, "color", "inherit");
    add_decl(ch2a, "text-decoration", "none");
    add_decl(ch2a, "transition", "color 0.2s ease");
    cJSON_AddItemToArray(rules, ch2a);

    cJSON *ch2ah = create_rule(".board-card h2 a:hover");
    add_decl(ch2ah, "color", "var(--accent)");
    cJSON_AddItemToArray(rules, ch2ah);

    cJSON *desc = create_rule(".board-card-desc");
    add_decl(desc, "color", "var(--muted)");
    add_decl(desc, "font-size", "15px");
    add_decl(desc, "line-height", "1.75");
    add_decl(desc, "margin", "0");
    add_decl(desc, "min-height", "22px");
    cJSON_AddItemToArray(rules, desc);

    cJSON *plist = create_rule(".post-list");
    add_decl(plist, "display", "flex");
    add_decl(plist, "flex-direction", "column");
    add_decl(plist, "gap", "12px");
    add_decl(plist, "max-width", "920px");
    add_decl(plist, "margin", "0 auto");
    cJSON_AddItemToArray(rules, plist);

    cJSON *prow = create_rule(".post-row");
    add_decl(prow, "background", "var(--panel)");
    add_decl(prow, "border", "1px solid var(--border)");
    add_decl(prow, "border-radius", "0");
    add_decl(prow, "text-align", "left");
    add_decl(prow, "overflow", "hidden");
    add_decl(prow, "transition", "background 0.2s ease, border-color 0.2s ease, transform 0.2s ease");
    cJSON_AddItemToArray(rules, prow);

    cJSON *prow_h = create_rule(".post-row:hover");
    add_decl(prow_h, "background", "var(--hover)");
    add_decl(prow_h, "border-color", "var(--accent)");
    cJSON_AddItemToArray(rules, prow_h);

    cJSON *prow_head = create_rule(".post-row-head");
    add_decl(prow_head, "display", "flex");
    add_decl(prow_head, "justify-content", "flex-start");
    add_decl(prow_head, "gap", "8px");
    add_decl(prow_head, "padding", "10px 16px");
    add_decl(prow_head, "border-bottom", "1px solid var(--border)");
    cJSON_AddItemToArray(rules, prow_head);

    cJSON *prow_title = create_rule(".post-row-title");
    add_decl(prow_title, "font-size", "18px");
    add_decl(prow_title, "font-family", "var(--font-display)");
    add_decl(prow_title, "font-weight", "800");
    add_decl(prow_title, "color", "var(--fg)");
    add_decl(prow_title, "text-decoration", "none");
    add_decl(prow_title, "display", "block");
    add_decl(prow_title, "padding", "12px 16px");
    add_decl(prow_title, "border-bottom", "1px solid var(--border)");
    add_decl(prow_title, "transition", "color 0.2s ease");
    cJSON_AddItemToArray(rules, prow_title);

    cJSON *prow_title_h = create_rule(".post-row-title:hover");
    add_decl(prow_title_h, "color", "var(--accent)");
    cJSON_AddItemToArray(rules, prow_title_h);

    cJSON *prow_sum = create_rule(".post-row-summary");
    add_decl(prow_sum, "font-size", "14px");
    add_decl(prow_sum, "color", "var(--muted)");
    add_decl(prow_sum, "line-height", "1.5");
    add_decl(prow_sum, "margin", "0");
    add_decl(prow_sum, "padding", "10px 16px");
    add_decl(prow_sum, "border-bottom", "1px solid var(--border)");
    cJSON_AddItemToArray(rules, prow_sum);

    cJSON *prow_meta = create_rule(".post-row-meta");
    add_decl(prow_meta, "display", "flex");
    add_decl(prow_meta, "flex-wrap", "wrap");
    add_decl(prow_meta, "gap", "10px");
    add_decl(prow_meta, "align-items", "center");
    add_decl(prow_meta, "justify-content", "flex-start");
    add_decl(prow_meta, "padding", "10px 16px");
    add_decl(prow_meta, "color", "var(--muted)");
    add_decl(prow_meta, "font-size", "13px");
    cJSON_AddItemToArray(rules, prow_meta);

    cJSON *dot = create_rule(".dot");
    add_decl(dot, "width", "4px");
    add_decl(dot, "height", "4px");
    add_decl(dot, "background", "var(--muted)");
    add_decl(dot, "border-radius", "50%");
    add_decl(dot, "display", "inline-block");
    cJSON_AddItemToArray(rules, dot);

    cJSON *list = create_rule(".board-post-list");
    add_decl(list, "list-style", "none");
    add_decl(list, "padding", "0");
    add_decl(list, "margin", "0");
    add_decl(list, "display", "flex");
    add_decl(list, "flex-direction", "column");
    add_decl(list, "gap", "0");
    add_decl(list, "border-top", "1px solid var(--border)");
    cJSON_AddItemToArray(rules, list);

    cJSON *item = create_rule(".board-post-item");
    add_decl(item, "display", "flex");
    add_decl(item, "flex-direction", "column");
    add_decl(item, "padding", "14px 12px");
    add_decl(item, "border-bottom", "1px solid var(--border)");
    add_decl(item, "border-left", "3px solid transparent");
    add_decl(item, "transition", "background 0.2s ease, border-color 0.2s ease");
    add_decl(item, "text-align", "left");
    cJSON_AddItemToArray(rules, item);

    cJSON *itemh = create_rule(".board-post-item:hover");
    add_decl(itemh, "background", "var(--hover)");
    add_decl(itemh, "border-left-color", "var(--accent)");
    cJSON_AddItemToArray(rules, itemh);

    cJSON *ptitle = create_rule(".board-post-title");
    add_decl(ptitle, "font-size", "1.18rem");
    add_decl(ptitle, "font-family", "var(--font-display)");
    add_decl(ptitle, "font-weight", "800");
    add_decl(ptitle, "color", "var(--fg)");
    add_decl(ptitle, "text-decoration", "none");
    add_decl(ptitle, "display", "inline");
    add_decl(ptitle, "padding", "0");
    add_decl(ptitle, "border-bottom", "none");
    add_decl(ptitle, "transition", "color 0.2s ease");
    cJSON_AddItemToArray(rules, ptitle);

    cJSON *ptitleh = create_rule(".board-post-title:hover");
    add_decl(ptitleh, "color", "var(--accent)");
    cJSON_AddItemToArray(rules, ptitleh);

    cJSON *psum = create_rule(".board-post-summary");
    add_decl(psum, "font-size", "0.98rem");
    add_decl(psum, "color", "var(--muted)");
    add_decl(psum, "line-height", "1.5");
    add_decl(psum, "margin", "0");
    add_decl(psum, "padding", "0");
    add_decl(psum, "border-bottom", "none");
    add_decl(psum, "margin-top", "6px");
    add_decl(psum, "display", "-webkit-box");
    add_decl(psum, "-webkit-line-clamp", "2");
    add_decl(psum, "-webkit-box-orient", "vertical");
    add_decl(psum, "overflow", "hidden");
    cJSON_AddItemToArray(rules, psum);

    cJSON *meta = create_rule(".board-post-meta");
    add_decl(meta, "display", "flex");
    add_decl(meta, "flex-wrap", "wrap");
    add_decl(meta, "gap", "8px");
    add_decl(meta, "align-items", "center");
    add_decl(meta, "justify-content", "flex-start");
    add_decl(meta, "padding", "6px 0 0");
    cJSON_AddItemToArray(rules, meta);

    cJSON *badge = create_rule(".post-badge");
    add_decl(badge, "display", "inline-flex");
    add_decl(badge, "align-items", "center");
    add_decl(badge, "gap", "4px");
    add_decl(badge, "padding", "3px 10px");
    add_decl(badge, "border-radius", "2px");
    add_decl(badge, "background", "var(--hover)");
    add_decl(badge, "border", "1px solid var(--border)");
    add_decl(badge, "font-size", "12px");
    add_decl(badge, "color", "var(--muted)");
    add_decl(badge, "font-weight", "500");
    add_decl(badge, "transition", "background 0.2s ease, border-color 0.2s ease");
    cJSON_AddItemToArray(rules, badge);

    cJSON *pdate = create_rule(".board-post-date");
    add_decl(pdate, "font-size", "12px");
    add_decl(pdate, "color", "var(--muted)");
    add_decl(pdate, "white-space", "nowrap");
    add_decl(pdate, "font-variant-numeric", "tabular-nums");
    cJSON_AddItemToArray(rules, pdate);

    cJSON *empty = create_rule(".board-card-empty");
    add_decl(empty, "color", "var(--muted)");
    add_decl(empty, "font-size", "13px");
    add_decl(empty, "text-align", "left");
    add_decl(empty, "padding", "10px 0");
    add_decl(empty, "background", "var(--panel)");
    add_decl(empty, "border-radius", "0");
    add_decl(empty, "border", "1px dashed var(--border)");
    add_decl(empty, "margin", "0");
    cJSON_AddItemToArray(rules, empty);

    cJSON *prow_notice = create_rule(".post-row-notice");
    add_decl(prow_notice, "border-left", "3px solid var(--accent)");
    add_decl(prow_notice, "background", "var(--hover)");
    cJSON_AddItemToArray(rules, prow_notice);

    cJSON *typo_list = create_rule(".board-typography-list");
    add_decl(typo_list, "border-top", "2px solid var(--border)");
    cJSON_AddItemToArray(rules, typo_list);

    cJSON *typo_row = create_rule(".board-typography-list .post-row, .post-row-typography");
    add_decl(typo_row, "background", "transparent");
    add_decl(typo_row, "border", "none");
    add_decl(typo_row, "border-bottom", "1px solid var(--border)");
    add_decl(typo_row, "padding", "18px 0");
    cJSON_AddItemToArray(rules, typo_row);

    cJSON *typo_row_h = create_rule(".board-typography-list .post-row:hover, .post-row-typography:hover");
    add_decl(typo_row_h, "background", "transparent");
    add_decl(typo_row_h, "border-color", "var(--border)");
    cJSON_AddItemToArray(rules, typo_row_h);

    cJSON *typo_title = create_rule(".board-typography-list .post-row-title");
    add_decl(typo_title, "font-size", "clamp(1.15rem, 1.7vw, 1.35rem)");
    add_decl(typo_title, "padding", "0");
    add_decl(typo_title, "border", "none");
    add_decl(typo_title, "line-height", "1.4");
    cJSON_AddItemToArray(rules, typo_title);

    cJSON *typo_head = create_rule(".board-typography-list .post-row-head");
    add_decl(typo_head, "padding", "0 0 8px");
    add_decl(typo_head, "border", "none");
    cJSON_AddItemToArray(rules, typo_head);

    cJSON *typo_sum = create_rule(".board-typography-list .post-row-summary");
    add_decl(typo_sum, "padding", "6px 0 0");
    add_decl(typo_sum, "border", "none");
    add_decl(typo_sum, "font-size", "0.95rem");
    cJSON_AddItemToArray(rules, typo_sum);

    cJSON *typo_meta = create_rule(".board-typography-list .post-row-meta");
    add_decl(typo_meta, "padding", "8px 0 0");
    cJSON_AddItemToArray(rules, typo_meta);
}

void rule_markdown(cJSON *rules) {
    cJSON *md = create_rule(".markdown-body");
    add_decl(md, "width", "min(100%, 760px)");
    add_decl(md, "max-width", "100%");
    add_decl(md, "margin", "0 auto");
    add_decl(md, "line-height", "1.8");
    add_decl(md, "min-width", "0");
    add_decl(md, "overflow-wrap", "anywhere");
    add_decl(md, "word-break", "break-word");
    cJSON_AddItemToArray(rules, md);

    cJSON *article = create_rule("article");
    add_decl(article, "width", "100%");
    add_decl(article, "max-width", "100%");
    add_decl(article, "margin", "0 auto");
    add_decl(article, "padding", "0 0 6px");
    add_decl(article, "min-width", "0");
    cJSON_AddItemToArray(rules, article);

    cJSON *article_children = create_rule("article > *");
    add_decl(article_children, "max-width", "100%");
    add_decl(article_children, "min-width", "0");
    cJSON_AddItemToArray(rules, article_children);

    cJSON *md_blocks = create_rule(".markdown-body p, .markdown-body li, .markdown-body blockquote, .markdown-body figcaption");
    add_decl(md_blocks, "overflow-wrap", "anywhere");
    add_decl(md_blocks, "word-break", "break-word");
    cJSON_AddItemToArray(rules, md_blocks);

    cJSON *md_links = create_rule(".markdown-body a");
    add_decl(md_links, "overflow-wrap", "anywhere");
    add_decl(md_links, "word-break", "break-word");
    cJSON_AddItemToArray(rules, md_links);

    cJSON *md_h1 = create_rule(".markdown-body h1");
    add_decl(md_h1, "font-size", "2.25rem");
    add_decl(md_h1, "font-family", "var(--font-display)");
    add_decl(md_h1, "font-weight", "800");
    add_decl(md_h1, "letter-spacing", "-0.03em");
    add_decl(md_h1, "line-height", "1.2");
    add_decl(md_h1, "margin-top", "48px");
    add_decl(md_h1, "margin-bottom", "20px");
    cJSON_AddItemToArray(rules, md_h1);

    cJSON *md_h2 = create_rule(".markdown-body h2");
    add_decl(md_h2, "font-size", "1.75rem");
    add_decl(md_h2, "font-family", "var(--font-display)");
    add_decl(md_h2, "font-weight", "700");
    add_decl(md_h2, "letter-spacing", "-0.02em");
    add_decl(md_h2, "line-height", "1.25");
    add_decl(md_h2, "margin-top", "40px");
    add_decl(md_h2, "margin-bottom", "16px");
    cJSON_AddItemToArray(rules, md_h2);

    cJSON *md_h3 = create_rule(".markdown-body h3");
    add_decl(md_h3, "font-size", "1.4rem");
    add_decl(md_h3, "font-family", "var(--font-display)");
    add_decl(md_h3, "font-weight", "700");
    add_decl(md_h3, "letter-spacing", "-0.015em");
    add_decl(md_h3, "line-height", "1.3");
    add_decl(md_h3, "margin-top", "32px");
    add_decl(md_h3, "margin-bottom", "14px");
    cJSON_AddItemToArray(rules, md_h3);

    cJSON *md_img = create_rule(".markdown-body img, .markdown-body video, .markdown-body audio");
    add_decl(md_img, "max-width", "100%");
    add_decl(md_img, "width", "auto");
    add_decl(md_img, "height", "auto");
    add_decl(md_img, "border-radius", "12px");
    add_decl(md_img, "box-shadow", "0 2px 12px var(--shadow)");
    add_decl(md_img, "display", "block");
    add_decl(md_img, "margin", "24px auto");
    add_decl(md_img, "transition", "transform 0.3s ease, box-shadow 0.3s ease");
    cJSON_AddItemToArray(rules, md_img);

    cJSON *md_imgh = create_rule(".markdown-body img:hover");
    add_decl(md_imgh, "transform", "scale(1.01)");
    add_decl(md_imgh, "box-shadow", "0 8px 24px var(--shadow)");
    cJSON_AddItemToArray(rules, md_imgh);

    cJSON *md_fig = create_rule(".markdown-body figure");
    add_decl(md_fig, "margin", "24px 0");
    add_decl(md_fig, "border-radius", "12px");
    add_decl(md_fig, "overflow", "hidden");
    cJSON_AddItemToArray(rules, md_fig);

    cJSON *md_pre = create_rule(".markdown-body pre");
    add_decl(md_pre, "background", "var(--code-bg)");
    add_decl(md_pre, "padding", "16px");
    add_decl(md_pre, "border-radius", "10px");
    add_decl(md_pre, "max-width", "100%");
    add_decl(md_pre, "overflow-x", "auto");
    add_decl(md_pre, "overflow-y", "hidden");
    add_decl(md_pre, "white-space", "pre");
    add_decl(md_pre, "border", "1px solid var(--border)");
    add_decl(md_pre, "font-family", "var(--font-mono)");
    add_decl(md_pre, "font-size", "14px");
    add_decl(md_pre, "line-height", "1.6");
    add_decl(md_pre, "transition", "background 0.5s ease, border-color 0.5s ease, opacity 0.2s ease");
    add_decl(md_pre, "font-feature-settings", "\"liga\" 1, \"calt\" 1");
    cJSON_AddItemToArray(rules, md_pre);

    cJSON *md_pre_code = create_rule(".markdown-body pre code");
    add_decl(md_pre_code, "font-family", "inherit");
    add_decl(md_pre_code, "font-size", "inherit");
    add_decl(md_pre_code, "white-space", "inherit");
    add_decl(md_pre_code, "word-break", "normal");
    add_decl(md_pre_code, "overflow-wrap", "normal");
    cJSON_AddItemToArray(rules, md_pre_code);

    cJSON *md_pre_span = create_rule(".markdown-body pre code span");
    add_decl(md_pre_span, "transition", "color 0.3s ease, background-color 0.3s ease");
    cJSON_AddItemToArray(rules, md_pre_span);

    cJSON *md_code = create_rule(".markdown-body code:not(pre code)");
    add_decl(md_code, "background", "var(--code-bg)");
    add_decl(md_code, "padding", "2px 6px");
    add_decl(md_code, "border-radius", "4px");
    add_decl(md_code, "font-size", "0.92em");
    add_decl(md_code, "overflow-wrap", "anywhere");
    add_decl(md_code, "word-break", "break-word");
    add_decl(md_code, "transition", "background 0.5s ease");
    add_decl(md_code, "font-family", "var(--font-mono)");
    add_decl(md_code, "font-feature-settings", "\"liga\" 1, \"calt\" 1");
    cJSON_AddItemToArray(rules, md_code);

    cJSON *md_blockquote = create_rule(".markdown-body blockquote");
    add_decl(md_blockquote, "border-left", "4px solid var(--accent)");
    add_decl(md_blockquote, "background", "var(--hover)");
    add_decl(md_blockquote, "padding", "12px 16px");
    add_decl(md_blockquote, "margin", "18px 0");
    add_decl(md_blockquote, "border-radius", "0 8px 8px 0");
    add_decl(md_blockquote, "font-style", "italic");
    add_decl(md_blockquote, "transition", "background 0.5s ease, border-color 0.5s ease");
    cJSON_AddItemToArray(rules, md_blockquote);

    cJSON *md_tbl = create_rule(".markdown-body table");
    add_decl(md_tbl, "display", "block");
    add_decl(md_tbl, "border-collapse", "collapse");
    add_decl(md_tbl, "width", "100%");
    add_decl(md_tbl, "max-width", "100%");
    add_decl(md_tbl, "margin", "18px 0");
    add_decl(md_tbl, "overflow-x", "auto");
    cJSON_AddItemToArray(rules, md_tbl);

    cJSON *md_thtd = create_rule(".markdown-body th, .markdown-body td");
    add_decl(md_thtd, "border", "1px solid var(--border)");
    add_decl(md_thtd, "padding", "8px 10px");
    add_decl(md_thtd, "overflow-wrap", "anywhere");
    add_decl(md_thtd, "word-break", "break-word");
    add_decl(md_thtd, "transition", "border-color 0.5s ease");
    cJSON_AddItemToArray(rules, md_thtd);

    cJSON *md_th = create_rule(".markdown-body th");
    add_decl(md_th, "background", "var(--hover)");
    add_decl(md_th, "font-weight", "600");
    add_decl(md_th, "transition", "background 0.5s ease");
    cJSON_AddItemToArray(rules, md_th);

    cJSON *md_zebra = create_rule(".markdown-body tbody tr:nth-child(even)");
    add_decl(md_zebra, "background", "var(--hover)");
    add_decl(md_zebra, "transition", "background 0.5s ease");
    cJSON_AddItemToArray(rules, md_zebra);

    cJSON *slider = create_rule("#theme-slider");
    add_decl(slider, "-webkit-appearance", "none");
    add_decl(slider, "appearance", "none");
    add_decl(slider, "height", "4px");
    add_decl(slider, "background", "var(--border)");
    add_decl(slider, "border-radius", "2px");
    add_decl(slider, "outline", "none");
    add_decl(slider, "margin", "0");
    cJSON_AddItemToArray(rules, slider);

    cJSON *slider_thumb = create_rule("#theme-slider::-webkit-slider-thumb");
    add_decl(slider_thumb, "-webkit-appearance", "none");
    add_decl(slider_thumb, "appearance", "none");
    add_decl(slider_thumb, "width", "12px");
    add_decl(slider_thumb, "height", "12px");
    add_decl(slider_thumb, "border-radius", "50%");
    add_decl(slider_thumb, "background", "var(--accent)");
    add_decl(slider_thumb, "cursor", "pointer");
    cJSON_AddItemToArray(rules, slider_thumb);

    cJSON *slider_thumb_moz = create_rule("#theme-slider::-moz-range-thumb");
    add_decl(slider_thumb_moz, "width", "12px");
    add_decl(slider_thumb_moz, "height", "12px");
    add_decl(slider_thumb_moz, "border-radius", "50%");
    add_decl(slider_thumb_moz, "background", "var(--accent)");
    add_decl(slider_thumb_moz, "cursor", "pointer");
    add_decl(slider_thumb_moz, "border", "none");
    cJSON_AddItemToArray(rules, slider_thumb_moz);

    cJSON *theme_switch = create_rule(".theme-switch");
    add_decl(theme_switch, "display", "inline-flex");
    add_decl(theme_switch, "align-items", "center");
    cJSON_AddItemToArray(rules, theme_switch);

    cJSON *theme_btn = create_rule(".theme-toggle-btn");
    add_decl(theme_btn, "min-width", "96px");
    add_decl(theme_btn, "display", "inline-flex");
    add_decl(theme_btn, "align-items", "center");
    add_decl(theme_btn, "justify-content", "center");
    add_decl(theme_btn, "gap", "10px");
    add_decl(theme_btn, "padding", "10px 14px");
    add_decl(theme_btn, "background", "color-mix(in srgb, var(--glass-bg) 54%, transparent)");
    add_decl(theme_btn, "border", "1px solid color-mix(in srgb, var(--glass-border) 88%, transparent)");
    add_decl(theme_btn, "color", "var(--fg)");
    add_decl(theme_btn, "box-shadow", "0 8px 22px color-mix(in srgb, var(--shadow) 28%, transparent)");
    add_decl(theme_btn, "backdrop-filter", "blur(14px) saturate(150%)");
    add_decl(theme_btn, "-webkit-backdrop-filter", "blur(14px) saturate(150%)");
    add_decl(theme_btn, "transition", "background 0.18s ease, border-color 0.18s ease, color 0.18s ease, box-shadow 0.18s ease, transform 0.18s ease");
    cJSON_AddItemToArray(rules, theme_btn);

    cJSON *theme_btn_h = create_rule(".theme-toggle-btn:hover");
    add_decl(theme_btn_h, "background", "color-mix(in srgb, var(--glass-bg) 74%, transparent)");
    add_decl(theme_btn_h, "border-color", "color-mix(in srgb, var(--accent) 36%, var(--glass-border))");
    add_decl(theme_btn_h, "color", "var(--accent)");
    add_decl(theme_btn_h, "box-shadow", "0 12px 28px color-mix(in srgb, var(--shadow) 34%, transparent)");
    add_decl(theme_btn_h, "transform", "translateY(-1px)");
    cJSON_AddItemToArray(rules, theme_btn_h);

    cJSON *theme_icon = create_rule(".theme-spin-icon");
    add_decl(theme_icon, "display", "inline-block");
    add_decl(theme_icon, "transform-origin", "50% 50%");
    add_decl(theme_icon, "transition", "transform 0.4s cubic-bezier(0.2, 0.7, 0, 1)");
    cJSON_AddItemToArray(rules, theme_icon);

    cJSON *theme_icon_spin = create_rule(".theme-spin-icon.spin");
    add_decl(theme_icon_spin, "transform", "rotate(180deg)");
    cJSON_AddItemToArray(rules, theme_icon_spin);

    cJSON *adv_btn_after = create_rule(".adv-toggle-btn::after");
    add_decl(adv_btn_after, "content", "'▾'");
    add_decl(adv_btn_after, "display", "inline-block");
    add_decl(adv_btn_after, "margin-left", "4px");
    add_decl(adv_btn_after, "transition", "transform 0.2s ease");
    cJSON_AddItemToArray(rules, adv_btn_after);

    cJSON *adv_btn_open = create_rule(".adv-toggle-btn.open::after");
    add_decl(adv_btn_open, "transform", "rotate(180deg)");
    cJSON_AddItemToArray(rules, adv_btn_open);

    cJSON *dropdown_panel = create_rule(".dropdown-panel");
    add_decl(dropdown_panel, "padding", "10px 0 0");
    cJSON_AddItemToArray(rules, dropdown_panel);

    cJSON *file_repo_actions = create_rule(".file-repo-upload-actions, .file-card-actions");
    add_decl(file_repo_actions, "align-items", "center");
    add_decl(file_repo_actions, "row-gap", "10px");
    cJSON_AddItemToArray(rules, file_repo_actions);

    cJSON *file_card_actions_btn = create_rule(".file-card-actions .btn");
    add_decl(file_card_actions_btn, "min-height", "30px");
    add_decl(file_card_actions_btn, "line-height", "1.2");
    add_decl(file_card_actions_btn, "justify-content", "center");
    cJSON_AddItemToArray(rules, file_card_actions_btn);

    cJSON *media_card = create_rule(".media-card");
    add_decl(media_card, "display", "grid");
    add_decl(media_card, "grid-template-columns", "96px minmax(0, 1fr)");
    add_decl(media_card, "grid-template-areas", "'thumb info' 'thumb actions'");
    add_decl(media_card, "gap", "12px");
    add_decl(media_card, "align-items", "center");
    cJSON_AddItemToArray(rules, media_card);

    cJSON *media_thumb = create_rule(".media-thumb");
    add_decl(media_thumb, "grid-area", "thumb");
    add_decl(media_thumb, "width", "96px");
    add_decl(media_thumb, "height", "96px");
    add_decl(media_thumb, "min-width", "96px");
    add_decl(media_thumb, "border-radius", "0");
    add_decl(media_thumb, "overflow", "hidden");
    add_decl(media_thumb, "background", "var(--hover)");
    add_decl(media_thumb, "display", "flex");
    add_decl(media_thumb, "align-items", "center");
    add_decl(media_thumb, "justify-content", "center");
    cJSON_AddItemToArray(rules, media_thumb);

    cJSON *media_info = create_rule(".media-info");
    add_decl(media_info, "grid-area", "info");
    add_decl(media_info, "min-width", "0");
    cJSON_AddItemToArray(rules, media_info);

    cJSON *media_name = create_rule(".media-name");
    add_decl(media_name, "font-weight", "700");
    add_decl(media_name, "line-height", "1.4");
    add_decl(media_name, "overflow-wrap", "break-word");
    add_decl(media_name, "word-break", "normal");
    add_decl(media_name, "hyphens", "auto");
    cJSON_AddItemToArray(rules, media_name);

    cJSON *media_status = create_rule(".media-status");
    add_decl(media_status, "margin-top", "6px");
    add_decl(media_status, "font-size", "13px");
    add_decl(media_status, "color", "var(--muted)");
    add_decl(media_status, "overflow-wrap", "break-word");
    cJSON_AddItemToArray(rules, media_status);

    cJSON *media_actions = create_rule(".media-actions");
    add_decl(media_actions, "grid-area", "actions");
    add_decl(media_actions, "display", "flex");
    add_decl(media_actions, "flex-wrap", "wrap");
    add_decl(media_actions, "align-items", "center");
    add_decl(media_actions, "gap", "8px");
    cJSON_AddItemToArray(rules, media_actions);

    cJSON *media_actions_btn = create_rule(".media-actions .btn");
    add_decl(media_actions_btn, "justify-content", "center");
    cJSON_AddItemToArray(rules, media_actions_btn);

    cJSON *media_cancel = create_rule(".media-cancel-btn, .media-remove-btn");
    add_decl(media_cancel, "background", "transparent");
    add_decl(media_cancel, "color", "#ef4444");
    add_decl(media_cancel, "border", "1px solid #ef4444");
    cJSON_AddItemToArray(rules, media_cancel);

    cJSON *media_cancel_h = create_rule(".media-cancel-btn:hover, .media-remove-btn:hover");
    add_decl(media_cancel_h, "filter", "brightness(1.1)");
    add_decl(media_cancel_h, "background", "rgba(239,68,68,0.08)");
    cJSON_AddItemToArray(rules, media_cancel_h);

    cJSON *media_retry = create_rule(".media-retry-btn");
    add_decl(media_retry, "background", "var(--accent)");
    add_decl(media_retry, "color", "#fff");
    add_decl(media_retry, "border", "none");
    cJSON_AddItemToArray(rules, media_retry);

    /* File repository list */
    cJSON *fr_list = create_rule(".file-repo-list");
    add_decl(fr_list, "display", "grid");
    add_decl(fr_list, "grid-template-columns", "minmax(0, 1fr)");
    add_decl(fr_list, "gap", "12px");
    add_decl(fr_list, "margin-top", "18px");
    cJSON_AddItemToArray(rules, fr_list);

    cJSON *fr_card = create_rule(".file-repo-card");
    add_decl(fr_card, "padding", "14px");
    cJSON_AddItemToArray(rules, fr_card);

    cJSON *fr_inner = create_rule(".file-repo-card-inner");
    add_decl(fr_inner, "display", "grid");
    add_decl(fr_inner, "grid-template-columns", "64px minmax(0, 1fr)");
    add_decl(fr_inner, "gap", "14px");
    add_decl(fr_inner, "align-items", "center");
    cJSON_AddItemToArray(rules, fr_inner);

    cJSON *fr_thumb = create_rule(".file-repo-thumb");
    add_decl(fr_thumb, "width", "64px");
    add_decl(fr_thumb, "height", "64px");
    add_decl(fr_thumb, "background", "var(--hover)");
    add_decl(fr_thumb, "display", "flex");
    add_decl(fr_thumb, "align-items", "center");
    add_decl(fr_thumb, "justify-content", "center");
    add_decl(fr_thumb, "overflow", "hidden");
    cJSON_AddItemToArray(rules, fr_thumb);

    cJSON *fr_thumb_media = create_rule(".file-repo-thumb img.file-thumb-media, .file-repo-thumb video.file-thumb-media");
    add_decl(fr_thumb_media, "width", "100%");
    add_decl(fr_thumb_media, "height", "100%");
    add_decl(fr_thumb_media, "object-fit", "cover");
    add_decl(fr_thumb_media, "display", "block");
    cJSON_AddItemToArray(rules, fr_thumb_media);

    cJSON *fr_thumb_icon = create_rule(".file-thumb-icon");
    add_decl(fr_thumb_icon, "font-size", "11px");
    add_decl(fr_thumb_icon, "font-weight", "700");
    add_decl(fr_thumb_icon, "color", "var(--muted)");
    add_decl(fr_thumb_icon, "letter-spacing", "0.05em");
    cJSON_AddItemToArray(rules, fr_thumb_icon);

    cJSON *fr_info = create_rule(".file-repo-card-info");
    add_decl(fr_info, "min-width", "0");
    cJSON_AddItemToArray(rules, fr_info);

    cJSON *math_block = create_rule(".math-block");
    add_decl(math_block, "display", "block");
    add_decl(math_block, "overflow-x", "auto");
    add_decl(math_block, "margin", "16px 0");
    cJSON_AddItemToArray(rules, math_block);

    cJSON *math_inline = create_rule(".math-inline");
    add_decl(math_inline, "display", "inline-block");
    cJSON_AddItemToArray(rules, math_inline);
}

void rule_animations(cJSON *rules) {
    cJSON *kf = create_rule("@keyframes fadeIn");
    add_decl(kf, "from", "opacity:0; transform: translateY(12px) scale(0.98)");
    add_decl(kf, "to", "opacity:1; transform: translateY(0) scale(1)");
    cJSON_AddItemToArray(rules, kf);

    cJSON *anim = create_rule(".fade-in");
    add_decl(anim, "animation", "fadeIn 0.5s ease both");
    cJSON_AddItemToArray(rules, anim);

    cJSON *shimmer_kf = create_rule("@keyframes shimmer");
    add_decl(shimmer_kf, "0%", "background-position: -200% 0");
    add_decl(shimmer_kf, "100%", "background-position: 200% 0");
    cJSON_AddItemToArray(rules, shimmer_kf);

    cJSON *shimmer = create_rule(".img-skeleton");
    add_decl(shimmer, "background", "linear-gradient(90deg, var(--hover) 25%, var(--panel) 50%, var(--hover) 75%)");
    add_decl(shimmer, "background-size", "200% 100%");
    add_decl(shimmer, "animation", "shimmer 1.5s infinite");
    cJSON_AddItemToArray(rules, shimmer);

    /* Stagger children */
    cJSON *stagger = create_rule(".stagger > *");
    add_decl(stagger, "animation", "fadeIn 0.5s ease both");
    cJSON_AddItemToArray(rules, stagger);

    for (int i = 1; i <= 12; i++) {
        char sel[32];
        snprintf(sel, sizeof(sel), ".stagger > *:nth-child(%d)", i);
        cJSON *st = create_rule(sel);
        char delay[16];
        snprintf(delay, sizeof(delay), "%.2fs", i * 0.05);
        add_decl(st, "animation-delay", delay);
        cJSON_AddItemToArray(rules, st);
    }
}

void rule_media(cJSON *rules) {
    cJSON *mq = create_rule("@media (max-width: 768px)");
    add_decl(mq, ".shell", "padding: 16px");
    add_decl(mq, ".topbar", "align-items: center");
    add_decl(mq, ".topbar", "padding: 0 12px");
    add_decl(mq, ".desktop-only", "display: none");
    add_decl(mq, ".mobile-only", "display: block");
    add_decl(mq, ".burger-btn", "display: inline-flex");
    add_decl(mq, ".nav-links", "position: fixed");
    add_decl(mq, ".nav-links", "top: 0");
    add_decl(mq, ".nav-links", "left: 0");
    add_decl(mq, ".nav-links", "width: 260px");
    add_decl(mq, ".nav-links", "height: 100dvh");
    add_decl(mq, ".nav-links", "max-height: 100dvh");
    add_decl(mq, ".nav-links", "background: color-mix(in srgb, var(--glass-bg) 96%, transparent)");
    add_decl(mq, ".nav-links", "backdrop-filter: blur(24px) saturate(180%)");
    add_decl(mq, ".nav-links", "-webkit-backdrop-filter: blur(24px) saturate(180%)");
    add_decl(mq, ".nav-links", "flex-direction: column");
    add_decl(mq, ".nav-links", "align-items: stretch");
    add_decl(mq, ".nav-links", "gap: 0");
    add_decl(mq, ".nav-links", "padding: 56px 0 24px");
    add_decl(mq, ".nav-links", "overflow-y: auto");
    add_decl(mq, ".nav-links", "overscroll-behavior: contain");
    add_decl(mq, ".nav-links", "-webkit-overflow-scrolling: touch");
    add_decl(mq, ".nav-links", "transform: translateX(-100%)");
    add_decl(mq, ".nav-links", "transition: transform 0.3s ease");
    add_decl(mq, ".nav-links", "z-index: 101");
    add_decl(mq, ".nav-links", "border-right: 1px solid var(--border)");
    add_decl(mq, ".nav-links", "border-top: 1px solid var(--glass-border)");
    add_decl(mq, ".nav-links", "display: none");
    add_decl(mq, ".nav-links.open", "display: flex");
    add_decl(mq, ".nav-links.open", "transform: translateX(0)");
    add_decl(mq, ".topbar-search", "display: flex");
    add_decl(mq, ".topbar-search", "width: 100%");
    add_decl(mq, ".topbar-search", "padding: 14px 24px");
    add_decl(mq, ".topbar-search", "border-bottom: 1px solid var(--border)");
    add_decl(mq, ".topbar-search input", "flex: 1");
    add_decl(mq, ".topbar-search input", "width: auto");
    add_decl(mq, ".nav-item", "padding: 14px 24px");
    add_decl(mq, ".nav-item", "border-bottom: 1px solid var(--border)");
    add_decl(mq, ".nav-item:hover", "border-bottom-color: var(--border)");
    add_decl(mq, ".theme-switch", "padding: 14px 24px");
    add_decl(mq, ".theme-toggle-btn", "width: 100%");
    add_decl(mq, ".theme-toggle-btn", "justify-content: space-between");
    add_decl(mq, ".file-repo-upload-actions", "margin-top: 18px");
    add_decl(mq, ".file-repo-upload-actions", "row-gap: 12px");
    add_decl(mq, ".file-card-actions", "margin-top: 14px");
    add_decl(mq, ".file-card-actions", "gap: 12px");
    add_decl(mq, ".file-card-actions .btn", "margin-right: 2px");
    add_decl(mq, ".media-card", "grid-template-columns: 1fr");
    add_decl(mq, ".media-card", "grid-template-areas: 'thumb' 'info' 'actions'");
    add_decl(mq, ".media-card", "gap: 14px");
    add_decl(mq, ".media-thumb", "width: 100%");
    add_decl(mq, ".media-thumb", "height: 160px");
    add_decl(mq, ".media-thumb", "min-width: 0");
    add_decl(mq, ".media-actions", "display: grid");
    add_decl(mq, ".media-actions", "grid-template-columns: repeat(2, minmax(0, 1fr))");
    add_decl(mq, ".media-actions", "gap: 10px");
    add_decl(mq, ".media-actions .btn", "width: 100%");
    add_decl(mq, ".media-actions .btn:nth-last-child(1):nth-child(odd)", "grid-column: 1 / -1");
    add_decl(mq, ".media-status", "margin-top: 8px");
    add_decl(mq, ".mobile-overlay", "display: block");
    add_decl(mq, ".hero-logo", "height: 100px");
    add_decl(mq, ".hero h1", "font-size: clamp(2rem, 8vw, 3rem)");
    add_decl(mq, ".hero p", "font-size: 17px");
    add_decl(mq, ".post-grid", "grid-template-columns: 1fr");
    add_decl(mq, ".board-grid", "grid-template-columns: 1fr");
    add_decl(mq, ".board-list", "border-top: 1px solid var(--border)");
    add_decl(mq, ".board-line", "padding: 16px");
    add_decl(mq, ".board-line-title", "font-size: 1.45rem");
    cJSON_AddItemToArray(rules, mq);
}

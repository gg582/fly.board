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

static cJSON *create_mobile_rule(const char *sel) {
    char buf[512];
    snprintf(buf, sizeof(buf), "html.mobile %s, body.mobile %s", sel, sel);
    return create_rule(buf);
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
}

void rule_base(cJSON *rules) {
    cJSON *a = create_rule("*");
    add_decl(a, "box-sizing", "border-box");
    cJSON_AddItemToArray(rules, a);

    cJSON *b = create_rule("html, body");
    add_decl(b, "margin", "0");
    add_decl(b, "padding", "0");
    cJSON_AddItemToArray(rules, b);

    /* Font imports */
    cJSON *ff_outfit = create_rule("@import");
    add_decl(ff_outfit, "url", "'https://fonts.googleapis.com/css2?family=Outfit:wght@100..900&family=Space+Grotesk:wght@300..700&family=IBM+Plex+Sans+KR:wght@300..700&family=Inter:wght@400..700&family=Source+Serif+4:ital,wght@0,400;0,600;1,400&display=swap'");
    cJSON_AddItemToArray(rules, ff_outfit);

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
    add_decl(body, "font-family", "'Space Grotesk', 'IBM Plex Sans KR', 'Pretendard Variable', 'Pretendard', sans-serif");
    add_decl(body, "font-size", "16px");
    add_decl(body, "line-height", "1.7");
    add_decl(body, "font-weight", "450");
    add_decl(body, "letter-spacing", "-0.01em");
    add_decl(body, "transition", "background 0.5s ease, color 0.5s ease");
    add_decl(body, "-webkit-font-smoothing", "antialiased");
    cJSON_AddItemToArray(rules, body);

    cJSON *h1 = create_rule("h1, .hero h1");
    add_decl(h1, "font-family", "'Outfit', sans-serif");
    add_decl(h1, "font-weight", "800");
    add_decl(h1, "letter-spacing", "-0.05em");
    add_decl(h1, "line-height", "1.1");
    add_decl(h1, "color", "var(--fg)");
    cJSON_AddItemToArray(rules, h1);

    cJSON *h2 = create_rule("h2, .board-line-title, .board-card h2");
    add_decl(h2, "font-family", "'Outfit', sans-serif");
    add_decl(h2, "font-weight", "750");
    add_decl(h2, "letter-spacing", "-0.04em");
    add_decl(h2, "line-height", "1.15");
    add_decl(h2, "color", "var(--fg)");
    cJSON_AddItemToArray(rules, h2);

    cJSON *h3 = create_rule("h3");
    add_decl(h3, "font-family", "'Outfit', sans-serif");
    add_decl(h3, "font-weight", "700");
    add_decl(h3, "letter-spacing", "-0.03em");
    add_decl(h3, "line-height", "1.2");
    add_decl(h3, "color", "var(--fg)");
    cJSON_AddItemToArray(rules, h3);

    cJSON *h4 = create_rule("h4");
    add_decl(h4, "font-family", "'Outfit', sans-serif");
    add_decl(h4, "font-weight", "600");
    add_decl(h4, "letter-spacing", "-0.02em");
    add_decl(h4, "line-height", "1.25");
    add_decl(h4, "color", "var(--muted)");
    cJSON_AddItemToArray(rules, h4);

    cJSON *h5h6 = create_rule("h5, h6");
    add_decl(h5h6, "font-family", "'Outfit', sans-serif");
    add_decl(h5h6, "font-weight", "500");
    add_decl(h5h6, "letter-spacing", "-0.01em");
    add_decl(h5h6, "line-height", "1.3");
    add_decl(h5h6, "color", "var(--muted)");
    cJSON_AddItemToArray(rules, h5h6);

    cJSON *topbar_title = create_rule(".topbar-title");
    add_decl(topbar_title, "font-family", "'Outfit', sans-serif");
    add_decl(topbar_title, "font-weight", "800");
    add_decl(topbar_title, "letter-spacing", "-0.04em");
    add_decl(topbar_title, "line-height", "1.2");
    cJSON_AddItemToArray(rules, topbar_title);

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
    add_decl(shell, "padding", "24px");
    cJSON_AddItemToArray(rules, shell);

    cJSON *nav = create_rule(".topbar");
    add_decl(nav, "display", "flex");
    add_decl(nav, "align-items", "center");
    add_decl(nav, "justify-content", "space-between");
    add_decl(nav, "gap", "16px");
    add_decl(nav, "width", "100%");
    add_decl(nav, "margin", "0");
    add_decl(nav, "padding", "0 16px");
    add_decl(nav, "background", "var(--panel)");
    add_decl(nav, "border-bottom", "1px solid var(--border)");
    add_decl(nav, "position", "sticky");
    add_decl(nav, "top", "0");
    add_decl(nav, "z-index", "100");
    add_decl(nav, "backdrop-filter", "blur(10px)");
    add_decl(nav, "transition", "background 0.5s ease, border-color 0.5s ease");
    cJSON_AddItemToArray(rules, nav);

    cJSON *navlinks = create_rule(".nav-links");
    add_decl(navlinks, "display", "flex");
    add_decl(navlinks, "gap", "12px");
    add_decl(navlinks, "align-items", "center");
    add_decl(navlinks, "flex-wrap", "wrap");
    add_decl(navlinks, "min-width", "0");
    cJSON_AddItemToArray(rules, navlinks);

    cJSON *navitem = create_rule(".nav-item");
    add_decl(navitem, "padding", "8px 10px");
    add_decl(navitem, "border-radius", "0");
    add_decl(navitem, "border-bottom", "1px solid transparent");
    add_decl(navitem, "font-family", "'Inter', 'IBM Plex Sans KR', 'Pretendard Variable', sans-serif");
    add_decl(navitem, "font-weight", "500");
    add_decl(navitem, "font-size", "14px");
    add_decl(navitem, "color", "var(--fg)");
    add_decl(navitem, "transition", "background 0.2s ease, color 0.2s ease");
    cJSON_AddItemToArray(rules, navitem);

    cJSON *navitemh = create_rule(".nav-item:hover");
    add_decl(navitemh, "background", "transparent");
    add_decl(navitemh, "color", "var(--accent)");
    add_decl(navitemh, "border-bottom-color", "var(--accent)");
    cJSON_AddItemToArray(rules, navitemh);

    cJSON *board_dd = create_rule(".nav-board-dropdown");
    add_decl(board_dd, "position", "relative");
    add_decl(board_dd, "display", "inline-flex");
    add_decl(board_dd, "align-items", "center");
    add_decl(board_dd, "padding-bottom", "6px");
    add_decl(board_dd, "margin-bottom", "-6px");
    cJSON_AddItemToArray(rules, board_dd);

    cJSON *board_menu = create_rule(".nav-board-menu");
    add_decl(board_menu, "position", "absolute");
    add_decl(board_menu, "top", "100%");
    add_decl(board_menu, "left", "0");
    add_decl(board_menu, "min-width", "220px");
    add_decl(board_menu, "max-height", "320px");
    add_decl(board_menu, "overflow-y", "auto");
    add_decl(board_menu, "background", "var(--panel)");
    add_decl(board_menu, "border", "1px solid var(--border)");
    add_decl(board_menu, "box-shadow", "0 10px 26px var(--shadow)");
    add_decl(board_menu, "padding", "6px");
    add_decl(board_menu, "display", "block");
    add_decl(board_menu, "visibility", "hidden");
    add_decl(board_menu, "opacity", "0");
    add_decl(board_menu, "pointer-events", "none");
    add_decl(board_menu, "transform", "translateY(-16px)");
    add_decl(board_menu, "transition", "opacity 0.4s cubic-bezier(0.165, 0.84, 0.44, 1), transform 0.4s cubic-bezier(0.165, 0.84, 0.44, 1), visibility 0.4s");
    cJSON_AddItemToArray(rules, board_menu);

    cJSON *board_menu_open = create_rule(".nav-board-dropdown:hover .nav-board-menu");
    add_decl(board_menu_open, "visibility", "visible");
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
    add_decl(board_sub, "padding", "8px 10px");
    add_decl(board_sub, "color", "var(--fg)");
    add_decl(board_sub, "font-size", "13px");
    add_decl(board_sub, "font-weight", "500");
    cJSON_AddItemToArray(rules, board_sub);

    cJSON *board_sub_h = create_rule(".nav-board-subitem:hover");
    add_decl(board_sub_h, "background", "var(--hover)");
    add_decl(board_sub_h, "color", "var(--accent)");
    cJSON_AddItemToArray(rules, board_sub_h);

    cJSON *board_sub_all = create_rule(".nav-board-subitem-all");
    add_decl(board_sub_all, "font-weight", "700");
    add_decl(board_sub_all, "border-bottom", "1px solid var(--border)");
    add_decl(board_sub_all, "margin-bottom", "4px");
    cJSON_AddItemToArray(rules, board_sub_all);

    cJSON *board_empty = create_rule(".nav-board-empty");
    add_decl(board_empty, "display", "block");
    add_decl(board_empty, "padding", "8px 10px");
    add_decl(board_empty, "font-size", "12px");
    add_decl(board_empty, "color", "var(--muted)");
    cJSON_AddItemToArray(rules, board_empty);

    cJSON *brand = create_rule(".topbar-brand");
    add_decl(brand, "display", "flex");
    add_decl(brand, "flex-direction", "column");
    add_decl(brand, "align-items", "flex-start");
    add_decl(brand, "gap", "4px");
    add_decl(brand, "line-height", "1.2");
    cJSON_AddItemToArray(rules, brand);

    cJSON *brand_title = create_rule(".topbar-title");
    add_decl(brand_title, "font-weight", "800");
    add_decl(brand_title, "font-size", "20px");
    add_decl(brand_title, "letter-spacing", "-0.04em");
    cJSON_AddItemToArray(rules, brand_title);

    cJSON *footer = create_rule(".site-footer");
    add_decl(footer, "text-align", "center");
    add_decl(footer, "padding", "80px 24px");
    add_decl(footer, "color", "var(--muted)");
    add_decl(footer, "font-size", "13px");
    add_decl(footer, "border-top", "1px solid var(--border)");
    add_decl(footer, "margin-top", "80px");
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
    add_decl(pp, "border-radius", "0");
    add_decl(pp, "object-fit", "cover");
    add_decl(pp, "border", "1px solid var(--border)");
    cJSON_AddItemToArray(rules, pp);

    cJSON *pp_s = create_rule(".profile-pic-small");
    add_decl(pp_s, "width", "24px");
    add_decl(pp_s, "height", "24px");
    add_decl(pp_s, "border-radius", "0");
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
    add_decl(overlay, "background", "rgba(0,0,0,0.45)");
    add_decl(overlay, "opacity", "0");
    add_decl(overlay, "pointer-events", "none");
    add_decl(overlay, "transition", "opacity 0.3s ease");
    add_decl(overlay, "z-index", "98");
    cJSON_AddItemToArray(rules, overlay);

    cJSON *overlay_open = create_rule(".mobile-overlay.open");
    add_decl(overlay_open, "opacity", "1");
    add_decl(overlay_open, "pointer-events", "auto");
    cJSON_AddItemToArray(rules, overlay_open);
}

void rule_components(cJSON *rules) {
    cJSON *card = create_rule(".card");
    add_decl(card, "background", "var(--panel)");
    add_decl(card, "border", "1px solid var(--border)");
    add_decl(card, "border-radius", "0");
    add_decl(card, "padding", "24px");
    add_decl(card, "box-shadow", "0 4px 16px var(--shadow)");
    add_decl(card, "transition", "transform 0.25s ease, box-shadow 0.25s ease, background 0.5s ease, border-color 0.5s ease");
    cJSON_AddItemToArray(rules, card);

    cJSON *cardh = create_rule(".card:hover");
    add_decl(cardh, "transform", "translateY(-3px)");
    add_decl(cardh, "box-shadow", "0 16px 40px var(--shadow)");
    cJSON_AddItemToArray(rules, cardh);

    cJSON *btn = create_rule(".btn");
    add_decl(btn, "display", "inline-flex");
    add_decl(btn, "align-items", "center");
    add_decl(btn, "gap", "8px");
    add_decl(btn, "padding", "10px 20px");
    add_decl(btn, "border", "none");
    add_decl(btn, "border-radius", "0");
    add_decl(btn, "background", "var(--accent)");
    add_decl(btn, "color", "#fff");
    add_decl(btn, "font-family", "'Inter', 'IBM Plex Sans KR', 'Pretendard Variable', sans-serif");
    add_decl(btn, "font-weight", "600");
    add_decl(btn, "letter-spacing", "0.02em");
    add_decl(btn, "cursor", "pointer");
    add_decl(btn, "transition", "filter 0.2s ease, transform 0.15s ease, background 0.5s ease, box-shadow 0.2s ease");
    cJSON_AddItemToArray(rules, btn);

    cJSON *btnh = create_rule(".btn:hover");
    add_decl(btnh, "filter", "brightness(1.1)");
    add_decl(btnh, "transform", "translateY(-1px)");
    add_decl(btnh, "box-shadow", "0 4px 12px var(--shadow)");
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
    add_decl(input, "border", "1px solid var(--border)");
    add_decl(input, "border-radius", "0");
    add_decl(input, "background", "var(--panel)");
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

    cJSON *media_card = create_rule(".media-card");
    add_decl(media_card, "display", "grid");
    add_decl(media_card, "grid-template-columns", "96px minmax(0,1fr) auto");
    add_decl(media_card, "gap", "12px");
    add_decl(media_card, "align-items", "center");
    add_decl(media_card, "padding", "10px");
    add_decl(media_card, "border", "1px solid var(--border)");
    add_decl(media_card, "background", "var(--panel)");
    cJSON_AddItemToArray(rules, media_card);

    cJSON *media_thumb = create_rule(".media-thumb");
    add_decl(media_thumb, "width", "96px");
    add_decl(media_thumb, "height", "72px");
    add_decl(media_thumb, "display", "flex");
    add_decl(media_thumb, "align-items", "center");
    add_decl(media_thumb, "justify-content", "center");
    add_decl(media_thumb, "overflow", "hidden");
    add_decl(media_thumb, "border", "1px solid var(--border)");
    add_decl(media_thumb, "background", "var(--bg)");
    add_decl(media_thumb, "color", "var(--muted)");
    add_decl(media_thumb, "font-size", "12px");
    add_decl(media_thumb, "font-weight", "700");
    cJSON_AddItemToArray(rules, media_thumb);

    cJSON *media_thumb_asset = create_rule(".media-thumb img, .media-thumb video");
    add_decl(media_thumb_asset, "width", "100%");
    add_decl(media_thumb_asset, "height", "100%");
    add_decl(media_thumb_asset, "object-fit", "cover");
    add_decl(media_thumb_asset, "display", "block");
    cJSON_AddItemToArray(rules, media_thumb_asset);

    cJSON *media_info = create_rule(".media-info");
    add_decl(media_info, "min-width", "0");
    cJSON_AddItemToArray(rules, media_info);

    cJSON *media_name = create_rule(".media-name");
    add_decl(media_name, "font-size", "14px");
    add_decl(media_name, "font-weight", "600");
    add_decl(media_name, "line-height", "1.35");
    add_decl(media_name, "overflow", "hidden");
    add_decl(media_name, "text-overflow", "ellipsis");
    add_decl(media_name, "white-space", "nowrap");
    cJSON_AddItemToArray(rules, media_name);

    cJSON *media_status = create_rule(".media-status");
    add_decl(media_status, "color", "var(--muted)");
    add_decl(media_status, "font-size", "12px");
    add_decl(media_status, "line-height", "1.4");
    add_decl(media_status, "margin-top", "4px");
    cJSON_AddItemToArray(rules, media_status);

    cJSON *media_actions = create_rule(".media-actions");
    add_decl(media_actions, "display", "flex");
    add_decl(media_actions, "gap", "8px");
    add_decl(media_actions, "flex-wrap", "wrap");
    add_decl(media_actions, "justify-content", "flex-end");
    cJSON_AddItemToArray(rules, media_actions);

    cJSON *media_actions_btn = create_rule(".media-actions .btn");
    add_decl(media_actions_btn, "font-size", "12px");
    add_decl(media_actions_btn, "padding", "4px 10px");
    cJSON_AddItemToArray(rules, media_actions_btn);

    cJSON *media_progress = create_rule(".media-progress-bar");
    add_decl(media_progress, "height", "4px");
    add_decl(media_progress, "background", "var(--border)");
    add_decl(media_progress, "overflow", "hidden");
    add_decl(media_progress, "margin-top", "8px");
    cJSON_AddItemToArray(rules, media_progress);

    cJSON *media_progress_inner = create_rule(".media-progress-inner");
    add_decl(media_progress_inner, "height", "100%");
    add_decl(media_progress_inner, "width", "0%");
    add_decl(media_progress_inner, "background", "var(--accent)");
    add_decl(media_progress_inner, "transition", "width 0.2s ease");
    cJSON_AddItemToArray(rules, media_progress_inner);

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
    add_decl(code_copy, "border-radius", "0");
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
    add_decl(cnode, "border-left", "3px solid var(--border)");
    add_decl(cnode, "padding-left", "14px");
    add_decl(cnode, "margin-top", "16px");
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
    add_decl(cavatar, "border-radius", "0");
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
    add_decl(cmeta, "font-family", "'Inter', 'IBM Plex Sans KR', 'Pretendard Variable', sans-serif");
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
    add_decl(hero, "padding", "80px 0 64px");
    add_decl(hero, "text-align", "center");
    cJSON_AddItemToArray(rules, hero);

    cJSON *hero_h1 = create_rule(".hero h1");
    add_decl(hero_h1, "font-size", "clamp(2.5rem, 6vw, 4.5rem)");
    add_decl(hero_h1, "margin", "0 0 16px");
    add_decl(hero_h1, "letter-spacing", "-0.05em");
    add_decl(hero_h1, "line-height", "1.05");
    add_decl(hero_h1, "font-weight", "850");
    add_decl(hero_h1, "text-shadow", "0 2px 16px var(--shadow)");
    cJSON_AddItemToArray(rules, hero_h1);

    cJSON *hero_logo = create_rule(".hero-logo");
    add_decl(hero_logo, "height", "120px");
    add_decl(hero_logo, "width", "auto");
    add_decl(hero_logo, "margin-bottom", "12px");
    cJSON_AddItemToArray(rules, hero_logo);

    cJSON *hero_p = create_rule(".hero p");
    add_decl(hero_p, "color", "var(--muted)");
    add_decl(hero_p, "font-size", "18px");
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
    add_decl(tag, "border-radius", "0");
    add_decl(tag, "background", "var(--hover)");
    add_decl(tag, "border", "1px solid var(--border)");
    add_decl(tag, "font-family", "'Inter', 'IBM Plex Sans KR', 'Pretendard Variable', sans-serif");
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
    add_decl(board_sec, "background", "var(--panel)");
    add_decl(board_sec, "border", "1px solid var(--border)");
    add_decl(board_sec, "border-radius", "0");
    add_decl(board_sec, "padding", "20px");
    add_decl(board_sec, "box-shadow", "0 2px 4px rgba(0,0,0,0.04), 0 8px 20px var(--shadow)");
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
    add_decl(blist, "padding", "28px 0 64px");
    add_decl(blist, "display", "flex");
    add_decl(blist, "flex-direction", "column");
    add_decl(blist, "gap", "28px");
    cJSON_AddItemToArray(rules, blist);

    cJSON *bline = create_rule(".board-line");
    add_decl(bline, "display", "grid");
    add_decl(bline, "gap", "16px");
    add_decl(bline, "padding", "36px");
    add_decl(bline, "background", "color-mix(in srgb, var(--glass-bg) 88%, transparent)");
    add_decl(bline, "border", "1px solid var(--glass-border)");
    add_decl(bline, "border-radius", "0");
    add_decl(bline, "backdrop-filter", "blur(18px) saturate(155%)");
    add_decl(bline, "-webkit-backdrop-filter", "blur(18px) saturate(155%)");
    add_decl(bline, "box-shadow", "0 4px 18px color-mix(in srgb, var(--shadow) 28%, transparent)");
    add_decl(bline, "opacity", "0.98");
    add_decl(bline, "position", "relative");
    add_decl(bline, "overflow", "hidden");
    add_decl(bline, "transition", "box-shadow 0.18s ease, background 0.18s ease, border-color 0.18s ease, opacity 0.18s ease");
    cJSON_AddItemToArray(rules, bline);

    cJSON *bline_before = create_rule(".board-line::before");
    add_decl(bline_before, "content", "''");
    add_decl(bline_before, "position", "absolute");
    add_decl(bline_before, "left", "0");
    add_decl(bline_before, "top", "0");
    add_decl(bline_before, "bottom", "0");
    add_decl(bline_before, "width", "4px");
    add_decl(bline_before, "background", "linear-gradient(180deg, var(--accent), transparent)");
    add_decl(bline_before, "opacity", "0.78");
    cJSON_AddItemToArray(rules, bline_before);

    cJSON *blineh = create_rule(".board-line:hover");
    add_decl(blineh, "transform", "none");
    add_decl(blineh, "box-shadow", "0 4px 18px color-mix(in srgb, var(--shadow) 32%, transparent)");
    add_decl(blineh, "border-color", "color-mix(in srgb, var(--glass-border) 60%, var(--accent) 40%)");
    add_decl(blineh, "opacity", "1");
    cJSON_AddItemToArray(rules, blineh);

    cJSON *bline_hot = create_rule(".board-line-hot");
    add_decl(bline_hot, "padding", "42px");
    add_decl(bline_hot, "border-left", "4px solid var(--accent)");
    add_decl(bline_hot, "box-shadow", "0 8px 30px color-mix(in srgb, var(--shadow) 40%, transparent)");
    cJSON_AddItemToArray(rules, bline_hot);

    cJSON *bline_hot_title = create_rule(".board-line-hot .board-line-title");
    add_decl(bline_hot_title, "font-size", "clamp(2rem, 3.2vw, 2.8rem)");
    cJSON_AddItemToArray(rules, bline_hot_title);

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
    add_decl(card, "box-shadow", "0 8px 32px var(--shadow)");
    add_decl(card, "transition", "transform 0.35s ease, box-shadow 0.35s ease, background 0.5s ease, border-color 0.5s ease");
    add_decl(card, "display", "flex");
    add_decl(card, "flex-direction", "column");
    add_decl(card, "gap", "16px");
    cJSON_AddItemToArray(rules, card);

    cJSON *cardh = create_rule(".board-card:hover");
    add_decl(cardh, "transform", "translateY(-6px) scale(1.01)");
    add_decl(cardh, "box-shadow", "0 16px 48px var(--shadow)");
    cJSON_AddItemToArray(rules, cardh);

    cJSON *ch = create_rule(".board-card-header");
    add_decl(ch, "display", "flex");
    add_decl(ch, "justify-content", "space-between");
    add_decl(ch, "align-items", "flex-start");
    add_decl(ch, "gap", "12px");
    cJSON_AddItemToArray(rules, ch);

    cJSON *ch2 = create_rule(".board-card h2");
    add_decl(ch2, "margin", "0");
    add_decl(ch2, "font-size", "24px");
    add_decl(ch2, "font-weight", "800");
    add_decl(ch2, "color", "var(--fg)");
    add_decl(ch2, "letter-spacing", "-0.03em");
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
    add_decl(desc, "font-size", "14px");
    add_decl(desc, "line-height", "1.6");
    add_decl(desc, "margin", "0");
    add_decl(desc, "min-height", "22px");
    cJSON_AddItemToArray(rules, desc);

    cJSON *point_row = create_rule(".board-point-row");
    add_decl(point_row, "display", "flex");
    add_decl(point_row, "flex-wrap", "wrap");
    add_decl(point_row, "gap", "10px");
    add_decl(point_row, "align-items", "center");
    add_decl(point_row, "margin", "2px 0 4px");
    cJSON_AddItemToArray(rules, point_row);

    cJSON *point = create_rule(".board-point");
    add_decl(point, "display", "inline-flex");
    add_decl(point, "align-items", "center");
    add_decl(point, "gap", "8px");
    add_decl(point, "padding", "7px 12px");
    add_decl(point, "border", "1px solid var(--border)");
    add_decl(point, "background", "color-mix(in srgb, var(--panel) 82%, transparent)");
    add_decl(point, "color", "var(--fg)");
    add_decl(point, "font-size", "12px");
    add_decl(point, "font-weight", "800");
    add_decl(point, "line-height", "1");
    add_decl(point, "letter-spacing", "0");
    cJSON_AddItemToArray(rules, point);

    cJSON *point_label = create_rule(".board-point span");
    add_decl(point_label, "color", "var(--muted)");
    add_decl(point_label, "font-size", "10px");
    add_decl(point_label, "font-weight", "700");
    add_decl(point_label, "letter-spacing", "0");
    cJSON_AddItemToArray(rules, point_label);

    cJSON *point_primary = create_rule(".board-point-primary");
    add_decl(point_primary, "border-color", "color-mix(in srgb, var(--accent) 48%, var(--border))");
    add_decl(point_primary, "background", "color-mix(in srgb, var(--accent) 12%, var(--panel))");
    add_decl(point_primary, "color", "var(--accent)");
    cJSON_AddItemToArray(rules, point_primary);

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
    add_decl(prow, "box-shadow", "0 2px 8px var(--shadow)");
    add_decl(prow, "transition", "background 0.2s ease, border-color 0.2s ease, transform 0.2s ease, box-shadow 0.2s ease");
    cJSON_AddItemToArray(rules, prow);

    cJSON *prow_h = create_rule(".post-row:hover");
    add_decl(prow_h, "background", "var(--hover)");
    add_decl(prow_h, "border-color", "var(--accent)");
    add_decl(prow_h, "transform", "translateY(-2px)");
    add_decl(prow_h, "box-shadow", "0 4px 12px rgba(0,0,0,0.06), 0 2px 6px rgba(0,0,0,0.04)");
    cJSON_AddItemToArray(rules, prow_h);

    cJSON *prow_feat = create_rule(".post-row.featured");
    add_decl(prow_feat, "border-left-width", "4px");
    add_decl(prow_feat, "border-left-color", "var(--accent)");
    add_decl(prow_feat, "padding", "16px 0");
    add_decl(prow_feat, "box-shadow", "0 6px 20px color-mix(in srgb, var(--shadow) 35%, transparent)");
    cJSON_AddItemToArray(rules, prow_feat);

    cJSON *prow_feat_title = create_rule(".post-row.featured .post-row-title");
    add_decl(prow_feat_title, "font-size", "1.25em");
    add_decl(prow_feat_title, "font-weight", "900");
    cJSON_AddItemToArray(rules, prow_feat_title);

    cJSON *prow_head = create_rule(".post-row-head");
    add_decl(prow_head, "display", "flex");
    add_decl(prow_head, "justify-content", "flex-start");
    add_decl(prow_head, "gap", "8px");
    add_decl(prow_head, "padding", "10px 16px 10px 28px");
    add_decl(prow_head, "border-bottom", "1px solid var(--border)");
    cJSON_AddItemToArray(rules, prow_head);

    cJSON *prow_title = create_rule(".post-row-title");
    add_decl(prow_title, "font-size", "18px");
    add_decl(prow_title, "font-family", "var(--font-display)");
    add_decl(prow_title, "font-weight", "800");
    add_decl(prow_title, "color", "var(--fg)");
    add_decl(prow_title, "text-decoration", "none");
    add_decl(prow_title, "display", "block");
    add_decl(prow_title, "padding", "12px 16px 12px 28px");
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
    add_decl(prow_sum, "padding", "10px 16px 10px 28px");
    add_decl(prow_sum, "border-bottom", "1px solid var(--border)");
    cJSON_AddItemToArray(rules, prow_sum);

    cJSON *prow_meta = create_rule(".post-row-meta");
    add_decl(prow_meta, "display", "flex");
    add_decl(prow_meta, "flex-wrap", "wrap");
    add_decl(prow_meta, "gap", "10px");
    add_decl(prow_meta, "align-items", "center");
    add_decl(prow_meta, "justify-content", "flex-start");
    add_decl(prow_meta, "padding", "10px 16px 10px 28px");
    add_decl(prow_meta, "color", "var(--muted)");
    add_decl(prow_meta, "font-family", "'Inter', 'IBM Plex Sans KR', 'Pretendard Variable', sans-serif");
    add_decl(prow_meta, "font-size", "13px");
    cJSON_AddItemToArray(rules, prow_meta);

    cJSON *dot = create_rule(".dot");
    add_decl(dot, "width", "4px");
    add_decl(dot, "height", "4px");
    add_decl(dot, "background", "var(--muted)");
    add_decl(dot, "border-radius", "0");
    add_decl(dot, "display", "inline-block");
    cJSON_AddItemToArray(rules, dot);

    cJSON *list = create_rule(".board-post-list");
    add_decl(list, "list-style", "none");
    add_decl(list, "padding", "0");
    add_decl(list, "margin", "4px 0 0");
    add_decl(list, "display", "flex");
    add_decl(list, "flex-direction", "column");
    add_decl(list, "gap", "0");
    add_decl(list, "border-top", "1px solid var(--border)");
    cJSON_AddItemToArray(rules, list);

    cJSON *item = create_rule(".board-post-item");
    add_decl(item, "display", "flex");
    add_decl(item, "flex-direction", "column");
    add_decl(item, "padding", "20px 18px");
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
    add_decl(ptitle, "font-size", "1.05rem");
    add_decl(ptitle, "font-weight", "700");
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
    add_decl(psum, "font-size", "0.9rem");
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
    add_decl(meta, "gap", "10px");
    add_decl(meta, "align-items", "center");
    add_decl(meta, "justify-content", "flex-start");
    add_decl(meta, "padding", "8px 0 0");
    add_decl(meta, "font-family", "'Inter', 'IBM Plex Sans KR', 'Pretendard Variable', sans-serif");
    add_decl(meta, "font-size", "12px");
    cJSON_AddItemToArray(rules, meta);

    cJSON *badge = create_rule(".post-badge");
    add_decl(badge, "display", "inline-flex");
    add_decl(badge, "align-items", "center");
    add_decl(badge, "gap", "4px");
    add_decl(badge, "padding", "3px 10px");
    add_decl(badge, "border-radius", "0");
    add_decl(badge, "background", "var(--panel)");
    add_decl(badge, "border", "1px solid var(--border)");
    add_decl(badge, "font-family", "'Inter', 'IBM Plex Sans KR', 'Pretendard Variable', sans-serif");
    add_decl(badge, "font-size", "12px");
    add_decl(badge, "color", "var(--muted)");
    add_decl(badge, "font-weight", "600");
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
    add_decl(empty, "padding", "16px 18px");
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
    add_decl(typo_row, "padding", "18px 20px");
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
    add_decl(md, "max-width", "760px");
    add_decl(md, "margin", "0 auto");
    add_decl(md, "line-height", "1.8");
    add_decl(md, "overflow-wrap", "break-word");
    add_decl(md, "word-break", "break-word");
    add_decl(md, "min-width", "0");
    cJSON_AddItemToArray(rules, md);

    cJSON *article = create_rule("article");
    add_decl(article, "max-width", "920px");
    add_decl(article, "margin", "0 auto");
    add_decl(article, "padding", "0 0 6px");
    add_decl(article, "overflow-wrap", "break-word");
    add_decl(article, "word-break", "break-word");
    add_decl(article, "min-width", "0");
    cJSON_AddItemToArray(rules, article);

    cJSON *md_h1 = create_rule(".markdown-body h1");
    add_decl(md_h1, "font-size", "2.25rem");
    add_decl(md_h1, "font-weight", "800");
    add_decl(md_h1, "letter-spacing", "-0.03em");
    add_decl(md_h1, "line-height", "1.2");
    add_decl(md_h1, "margin-top", "48px");
    add_decl(md_h1, "margin-bottom", "20px");
    cJSON_AddItemToArray(rules, md_h1);

    cJSON *md_h2 = create_rule(".markdown-body h2");
    add_decl(md_h2, "font-size", "1.75rem");
    add_decl(md_h2, "font-weight", "700");
    add_decl(md_h2, "letter-spacing", "-0.02em");
    add_decl(md_h2, "line-height", "1.25");
    add_decl(md_h2, "margin-top", "40px");
    add_decl(md_h2, "margin-bottom", "16px");
    cJSON_AddItemToArray(rules, md_h2);

    cJSON *md_h3 = create_rule(".markdown-body h3");
    add_decl(md_h3, "font-size", "1.4rem");
    add_decl(md_h3, "font-weight", "700");
    add_decl(md_h3, "letter-spacing", "-0.015em");
    add_decl(md_h3, "line-height", "1.3");
    add_decl(md_h3, "margin-top", "32px");
    add_decl(md_h3, "margin-bottom", "14px");
    cJSON_AddItemToArray(rules, md_h3);

    cJSON *md_img = create_rule(".markdown-body img, .markdown-body video, .markdown-body audio");
    add_decl(md_img, "max-width", "100%");
    add_decl(md_img, "height", "auto");
    add_decl(md_img, "border-radius", "0");
    add_decl(md_img, "box-shadow", "0 2px 12px var(--shadow)");
    add_decl(md_img, "display", "block");
    add_decl(md_img, "margin", "24px auto");
    add_decl(md_img, "transition", "transform 0.3s ease, box-shadow 0.3s ease");
    cJSON_AddItemToArray(rules, md_img);

    cJSON *editor_preview_media = create_rule("#md-preview .markdown-body img, #md-preview .markdown-body video");
    add_decl(editor_preview_media, "max-width", "240px");
    add_decl(editor_preview_media, "max-height", "180px");
    add_decl(editor_preview_media, "width", "auto");
    add_decl(editor_preview_media, "height", "auto");
    add_decl(editor_preview_media, "object-fit", "contain");
    add_decl(editor_preview_media, "margin", "12px 0");
    cJSON_AddItemToArray(rules, editor_preview_media);

    cJSON *editor_preview_audio = create_rule("#md-preview .markdown-body audio");
    add_decl(editor_preview_audio, "width", "240px");
    add_decl(editor_preview_audio, "max-width", "100%");
    add_decl(editor_preview_audio, "margin", "12px 0");
    cJSON_AddItemToArray(rules, editor_preview_audio);

    cJSON *md_imgh = create_rule(".markdown-body img:hover");
    add_decl(md_imgh, "transform", "scale(1.01)");
    add_decl(md_imgh, "box-shadow", "0 8px 24px var(--shadow)");
    cJSON_AddItemToArray(rules, md_imgh);

    cJSON *md_fig = create_rule(".markdown-body figure");
    add_decl(md_fig, "margin", "24px 0");
    add_decl(md_fig, "border-radius", "0");
    add_decl(md_fig, "overflow", "hidden");
    cJSON_AddItemToArray(rules, md_fig);

    cJSON *md_pre = create_rule(".markdown-body pre");
    add_decl(md_pre, "background", "var(--code-bg)");
    add_decl(md_pre, "padding", "16px");
    add_decl(md_pre, "border-radius", "0");
    add_decl(md_pre, "overflow", "auto");
    add_decl(md_pre, "border", "1px solid var(--border)");
    add_decl(md_pre, "font-family", "'JetBrains Mono', 'Fira Code', 'D2Coding', Consolas, Monaco, 'Courier New', monospace");
    add_decl(md_pre, "font-size", "14px");
    add_decl(md_pre, "line-height", "1.6");
    add_decl(md_pre, "transition", "background 0.5s ease, border-color 0.5s ease, opacity 0.2s ease");
    add_decl(md_pre, "font-feature-settings", "\"liga\" 1, \"calt\" 1");
    cJSON_AddItemToArray(rules, md_pre);

    cJSON *md_pre_code = create_rule(".markdown-body pre code");
    add_decl(md_pre_code, "font-family", "inherit");
    add_decl(md_pre_code, "font-size", "inherit");
    cJSON_AddItemToArray(rules, md_pre_code);

    cJSON *md_pre_span = create_rule(".markdown-body pre code span");
    add_decl(md_pre_span, "transition", "color 0.3s ease, background-color 0.3s ease");
    cJSON_AddItemToArray(rules, md_pre_span);

    cJSON *md_code = create_rule(".markdown-body code:not(pre code)");
    add_decl(md_code, "background", "var(--code-bg)");
    add_decl(md_code, "padding", "2px 6px");
    add_decl(md_code, "border-radius", "0");
    add_decl(md_code, "font-size", "0.92em");
    add_decl(md_code, "transition", "background 0.5s ease");
    add_decl(md_code, "font-family", "'JetBrains Mono', 'Fira Code', 'D2Coding', monospace");
    add_decl(md_code, "font-feature-settings", "\"liga\" 1, \"calt\" 1");
    add_decl(md_code, "word-break", "break-word");
    add_decl(md_code, "overflow-wrap", "break-word");
    cJSON_AddItemToArray(rules, md_code);

    cJSON *md_blockquote = create_rule(".markdown-body blockquote");
    add_decl(md_blockquote, "border-left", "4px solid var(--accent)");
    add_decl(md_blockquote, "background", "var(--hover)");
    add_decl(md_blockquote, "padding", "16px 20px");
    add_decl(md_blockquote, "margin", "18px 0");
    add_decl(md_blockquote, "border-radius", "0");
    add_decl(md_blockquote, "font-family", "'Source Serif 4', 'IBM Plex Sans KR', serif");
    add_decl(md_blockquote, "font-style", "italic");
    add_decl(md_blockquote, "font-size", "1.05rem");
    add_decl(md_blockquote, "line-height", "1.7");
    add_decl(md_blockquote, "transition", "background 0.5s ease, border-color 0.5s ease");
    cJSON_AddItemToArray(rules, md_blockquote);

    cJSON *md_tbl = create_rule(".markdown-body table");
    add_decl(md_tbl, "border-collapse", "collapse");
    add_decl(md_tbl, "width", "100%");
    add_decl(md_tbl, "margin", "18px 0");
    add_decl(md_tbl, "table-layout", "fixed");
    add_decl(md_tbl, "display", "block");
    add_decl(md_tbl, "overflow-x", "auto");
    cJSON_AddItemToArray(rules, md_tbl);

    cJSON *md_thtd = create_rule(".markdown-body th, .markdown-body td");
    add_decl(md_thtd, "border", "1px solid var(--border)");
    add_decl(md_thtd, "padding", "8px 10px");
    add_decl(md_thtd, "transition", "border-color 0.5s ease");
    add_decl(md_thtd, "word-break", "break-word");
    add_decl(md_thtd, "overflow-wrap", "break-word");
    cJSON_AddItemToArray(rules, md_thtd);

    cJSON *md_th = create_rule(".markdown-body th");
    add_decl(md_th, "background", "var(--hover)");
    add_decl(md_th, "font-weight", "600");
    add_decl(md_th, "transition", "background 0.5s ease");
    cJSON_AddItemToArray(rules, md_th);

    cJSON *md_a = create_rule(".markdown-body a");
    add_decl(md_a, "word-break", "break-word");
    add_decl(md_a, "overflow-wrap", "break-word");
    cJSON_AddItemToArray(rules, md_a);

    cJSON *md_zebra = create_rule(".markdown-body tbody tr:nth-child(even)");
    add_decl(md_zebra, "background", "var(--hover)");
    add_decl(md_zebra, "transition", "background 0.5s ease");
    cJSON_AddItemToArray(rules, md_zebra);

    cJSON *slider = create_rule("#theme-slider");
    add_decl(slider, "-webkit-appearance", "none");
    add_decl(slider, "appearance", "none");
    add_decl(slider, "height", "4px");
    add_decl(slider, "background", "var(--border)");
    add_decl(slider, "border-radius", "0");
    add_decl(slider, "outline", "none");
    add_decl(slider, "margin", "0");
    cJSON_AddItemToArray(rules, slider);

    cJSON *slider_thumb = create_rule("#theme-slider::-webkit-slider-thumb");
    add_decl(slider_thumb, "-webkit-appearance", "none");
    add_decl(slider_thumb, "appearance", "none");
    add_decl(slider_thumb, "width", "12px");
    add_decl(slider_thumb, "height", "12px");
    add_decl(slider_thumb, "border-radius", "0");
    add_decl(slider_thumb, "background", "var(--accent)");
    add_decl(slider_thumb, "cursor", "pointer");
    cJSON_AddItemToArray(rules, slider_thumb);

    cJSON *slider_thumb_moz = create_rule("#theme-slider::-moz-range-thumb");
    add_decl(slider_thumb_moz, "width", "12px");
    add_decl(slider_thumb_moz, "height", "12px");
    add_decl(slider_thumb_moz, "border-radius", "0");
    add_decl(slider_thumb_moz, "background", "var(--accent)");
    add_decl(slider_thumb_moz, "cursor", "pointer");
    add_decl(slider_thumb_moz, "border", "none");
    cJSON_AddItemToArray(rules, slider_thumb_moz);

    cJSON *theme_switch = create_rule(".theme-switch");
    add_decl(theme_switch, "position", "relative");
    add_decl(theme_switch, "display", "inline-flex");
    add_decl(theme_switch, "flex-direction", "column");
    add_decl(theme_switch, "align-items", "center");
    cJSON_AddItemToArray(rules, theme_switch);

    cJSON *theme_btn = create_rule(".theme-toggle-btn");
    add_decl(theme_btn, "min-width", "36px");
    add_decl(theme_btn, "height", "36px");
    add_decl(theme_btn, "padding", "0");
    add_decl(theme_btn, "justify-content", "center");
    add_decl(theme_btn, "font-size", "18px");
    add_decl(theme_btn, "line-height", "1");
    cJSON_AddItemToArray(rules, theme_btn);

    cJSON *theme_dd = create_rule(".theme-dropdown");
    add_decl(theme_dd, "position", "absolute");
    add_decl(theme_dd, "top", "calc(100% + 6px)");
    add_decl(theme_dd, "right", "0");
    add_decl(theme_dd, "min-width", "120px");
    add_decl(theme_dd, "background", "var(--panel)");
    add_decl(theme_dd, "border", "1px solid var(--border)");
    add_decl(theme_dd, "box-shadow", "0 10px 26px var(--shadow)");
    add_decl(theme_dd, "display", "flex");
    add_decl(theme_dd, "flex-direction", "column");
    add_decl(theme_dd, "padding", "6px");
    add_decl(theme_dd, "visibility", "hidden");
    add_decl(theme_dd, "opacity", "0");
    add_decl(theme_dd, "pointer-events", "none");
    add_decl(theme_dd, "transform", "translateY(-16px)");
    add_decl(theme_dd, "transition", "opacity 0.4s cubic-bezier(0.165, 0.84, 0.44, 1), transform 0.4s cubic-bezier(0.165, 0.84, 0.44, 1), visibility 0.4s");
    cJSON_AddItemToArray(rules, theme_dd);

    cJSON *theme_dd_open = create_rule(".theme-dropdown.open");
    add_decl(theme_dd_open, "visibility", "visible");
    add_decl(theme_dd_open, "opacity", "1");
    add_decl(theme_dd_open, "pointer-events", "auto");
    add_decl(theme_dd_open, "transform", "translateY(0)");
    cJSON_AddItemToArray(rules, theme_dd_open);

    cJSON *theme_opt = create_rule(".theme-option");
    add_decl(theme_opt, "text-align", "left");
    add_decl(theme_opt, "padding", "8px 10px");
    add_decl(theme_opt, "background", "transparent");
    add_decl(theme_opt, "border", "none");
    add_decl(theme_opt, "color", "var(--fg)");
    add_decl(theme_opt, "font", "inherit");
    add_decl(theme_opt, "cursor", "pointer");
    cJSON_AddItemToArray(rules, theme_opt);

    cJSON *theme_opt_h = create_rule(".theme-option:hover");
    add_decl(theme_opt_h, "background", "var(--hover)");
    cJSON_AddItemToArray(rules, theme_opt_h);

    cJSON *theme_opt_active = create_rule(".theme-option.active");
    add_decl(theme_opt_active, "color", "var(--accent)");
    add_decl(theme_opt_active, "font-weight", "700");
    cJSON_AddItemToArray(rules, theme_opt_active);

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

    cJSON *mobile_acct_hdr = create_rule(".mobile-account-header");
    add_decl(mobile_acct_hdr, "display", "none");
    cJSON_AddItemToArray(rules, mobile_acct_hdr);

    cJSON *mobile_profile_link = create_rule(".mobile-profile-link");
    add_decl(mobile_profile_link, "display", "block");
    add_decl(mobile_profile_link, "flex-shrink", "0");
    cJSON_AddItemToArray(rules, mobile_profile_link);

    cJSON *mobile_profile_pic = create_rule(".mobile-profile-pic");
    add_decl(mobile_profile_pic, "width", "48px");
    add_decl(mobile_profile_pic, "height", "48px");
    add_decl(mobile_profile_pic, "border-radius", "0");
    add_decl(mobile_profile_pic, "object-fit", "cover");
    add_decl(mobile_profile_pic, "display", "block");
    cJSON_AddItemToArray(rules, mobile_profile_pic);

    cJSON *mobile_profile_default = create_rule(".mobile-profile-default");
    add_decl(mobile_profile_default, "background", "var(--accent)");
    add_decl(mobile_profile_default, "color", "var(--panel)");
    add_decl(mobile_profile_default, "display", "flex");
    add_decl(mobile_profile_default, "align-items", "center");
    add_decl(mobile_profile_default, "justify-content", "center");
    add_decl(mobile_profile_default, "font-size", "24px");
    cJSON_AddItemToArray(rules, mobile_profile_default);

    cJSON *mobile_auth_btns = create_rule(".mobile-auth-btns");
    add_decl(mobile_auth_btns, "display", "flex");
    add_decl(mobile_auth_btns, "gap", "8px");
    add_decl(mobile_auth_btns, "align-items", "center");
    cJSON_AddItemToArray(rules, mobile_auth_btns);
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
    cJSON *mq_acct = create_mobile_rule(".mobile-account-header");
    add_decl(mq_acct, "display", "flex");
    add_decl(mq_acct, "align-items", "center");
    add_decl(mq_acct, "justify-content", "space-between");
    add_decl(mq_acct, "padding", "16px 24px");
    add_decl(mq_acct, "border-bottom", "1px solid var(--border)");
    add_decl(mq_acct, "gap", "12px");
    cJSON_AddItemToArray(rules, mq_acct);

    cJSON *mq_desktop_hide = create_mobile_rule(".desktop-only");
    add_decl(mq_desktop_hide, "display", "none");
    cJSON_AddItemToArray(rules, mq_desktop_hide);

    cJSON *mq = create_mobile_rule(".shell");
    add_decl(mq, "padding", "16px");
    cJSON_AddItemToArray(rules, mq);

    cJSON *mq2 = create_mobile_rule(".topbar");
    add_decl(mq2, "align-items", "center");
    add_decl(mq2, "padding", "0 12px");
    cJSON_AddItemToArray(rules, mq2);

    cJSON *mq3 = create_mobile_rule(".burger-btn");
    add_decl(mq3, "display", "inline-flex");
    cJSON_AddItemToArray(rules, mq3);

    cJSON *mq4 = create_mobile_rule(".nav-links");
    add_decl(mq4, "display", "none");
    add_decl(mq4, "position", "fixed");
    add_decl(mq4, "top", "60px");
    add_decl(mq4, "left", "0");
    add_decl(mq4, "width", "100%");
    add_decl(mq4, "flex-direction", "column");
    add_decl(mq4, "background", "var(--panel)");
    add_decl(mq4, "align-items", "stretch");
    add_decl(mq4, "gap", "0");
    add_decl(mq4, "padding", "16px 0");
    add_decl(mq4, "z-index", "101");
    add_decl(mq4, "border-bottom", "1px solid var(--border)");
    cJSON_AddItemToArray(rules, mq4);

    cJSON *mq5 = create_mobile_rule(".nav-links.open");
    add_decl(mq5, "display", "flex");
    cJSON_AddItemToArray(rules, mq5);

    cJSON *mq6 = create_mobile_rule(".nav-item");
    add_decl(mq6, "padding", "14px 24px");
    add_decl(mq6, "border-bottom", "1px solid var(--border)");
    cJSON_AddItemToArray(rules, mq6);

    cJSON *mq7 = create_mobile_rule(".nav-item:hover");
    add_decl(mq7, "border-bottom-color", "var(--border)");
    cJSON_AddItemToArray(rules, mq7);

    cJSON *mq8 = create_mobile_rule(".nav-board-dropdown");
    add_decl(mq8, "width", "100%");
    cJSON_AddItemToArray(rules, mq8);

    cJSON *mq_media_card = create_mobile_rule(".media-card");
    add_decl(mq_media_card, "grid-template-columns", "72px minmax(0,1fr)");
    cJSON_AddItemToArray(rules, mq_media_card);

    cJSON *mq_media_thumb = create_mobile_rule(".media-thumb");
    add_decl(mq_media_thumb, "width", "72px");
    add_decl(mq_media_thumb, "height", "54px");
    cJSON_AddItemToArray(rules, mq_media_thumb);

    cJSON *mq_media_actions = create_mobile_rule(".media-actions");
    add_decl(mq_media_actions, "grid-column", "1 / -1");
    add_decl(mq_media_actions, "justify-content", "flex-start");
    cJSON_AddItemToArray(rules, mq_media_actions);

    cJSON *mq9 = create_mobile_rule(".nav-board-menu");
    add_decl(mq9, "position", "static");
    add_decl(mq9, "display", "none");
    add_decl(mq9, "opacity", "0");
    add_decl(mq9, "pointer-events", "none");
    add_decl(mq9, "transform", "translateY(-8px)");
    add_decl(mq9, "box-shadow", "none");
    add_decl(mq9, "border", "none");
    add_decl(mq9, "padding", "0");
    add_decl(mq9, "width", "100%");
    add_decl(mq9, "overflow", "hidden");
    add_decl(mq9, "max-height", "0");
    add_decl(mq9, "transition", "max-height 0.3s ease, opacity 0.2s ease, transform 0.2s ease");
    cJSON_AddItemToArray(rules, mq9);

    cJSON *mq10 = create_mobile_rule(".nav-board-menu.open");
    add_decl(mq10, "display", "block");
    add_decl(mq10, "opacity", "1");
    add_decl(mq10, "pointer-events", "auto");
    add_decl(mq10, "transform", "translateY(0)");
    add_decl(mq10, "max-height", "300px");
    add_decl(mq10, "padding", "4px 0");
    add_decl(mq10, "background", "var(--hover)");
    add_decl(mq10, "border-left", "3px solid var(--accent)");
    cJSON_AddItemToArray(rules, mq10);

    cJSON *mq11 = create_mobile_rule(".nav-board-menu-list");
    add_decl(mq11, "display", "none");
    cJSON_AddItemToArray(rules, mq11);

    cJSON *mq12 = create_mobile_rule(".nav-board-dropdown:hover .nav-board-menu");
    add_decl(mq12, "display", "none !important");
    add_decl(mq12, "opacity", "0");
    add_decl(mq12, "pointer-events", "none");
    cJSON_AddItemToArray(rules, mq12);

    cJSON *mq13 = create_mobile_rule(".nav-board-trigger");
    add_decl(mq13, "cursor", "pointer");
    cJSON_AddItemToArray(rules, mq13);

    cJSON *mq14 = create_mobile_rule(".nav-board-subitem");
    add_decl(mq14, "padding", "10px 24px");
    add_decl(mq14, "border-bottom", "1px solid var(--border)");
    add_decl(mq14, "color", "var(--muted)");
    add_decl(mq14, "font-size", "14px");
    cJSON_AddItemToArray(rules, mq14);

    cJSON *mq15 = create_mobile_rule(".nav-board-subitem-all");
    add_decl(mq15, "color", "var(--fg)");
    add_decl(mq15, "font-weight", "700");
    add_decl(mq15, "border-bottom", "1px solid var(--border)");
    add_decl(mq15, "margin-bottom", "0");
    cJSON_AddItemToArray(rules, mq15);

    cJSON *mq16 = create_rule("html.mobile .dropdown, body.mobile .dropdown, html.mobile .dropdown-content, body.mobile .dropdown-content");
    add_decl(mq16, "position", "static");
    add_decl(mq16, "display", "block");
    cJSON_AddItemToArray(rules, mq16);

    cJSON *mq17 = create_mobile_rule(".dropdown-content a");
    add_decl(mq17, "display", "list-item");
    add_decl(mq17, "list-style-type", "disc");
    add_decl(mq17, "margin-left", "20px");
    cJSON_AddItemToArray(rules, mq17);

    cJSON *mq18 = create_mobile_rule(".theme-switch");
    add_decl(mq18, "padding", "0");
    cJSON_AddItemToArray(rules, mq18);

    cJSON *mq20 = create_mobile_rule(".mobile-overlay");
    add_decl(mq20, "display", "block");
    cJSON_AddItemToArray(rules, mq20);

    cJSON *mq21 = create_mobile_rule(".hero-logo");
    add_decl(mq21, "height", "100px");
    cJSON_AddItemToArray(rules, mq21);

    cJSON *mq22 = create_mobile_rule(".hero h1");
    add_decl(mq22, "font-size", "clamp(2rem, 8vw, 3rem)");
    cJSON_AddItemToArray(rules, mq22);

    cJSON *mq23 = create_mobile_rule(".post-grid");
    add_decl(mq23, "grid-template-columns", "1fr");
    cJSON_AddItemToArray(rules, mq23);

    cJSON *mq24 = create_mobile_rule(".board-grid");
    add_decl(mq24, "grid-template-columns", "1fr");
    cJSON_AddItemToArray(rules, mq24);

    cJSON *mq25 = create_mobile_rule(".board-list");
    add_decl(mq25, "border-top", "1px solid var(--border)");
    cJSON_AddItemToArray(rules, mq25);

    cJSON *mq26 = create_mobile_rule(".board-line");
    add_decl(mq26, "padding", "16px");
    cJSON_AddItemToArray(rules, mq26);

    cJSON *mq27 = create_mobile_rule(".board-line-title");
    add_decl(mq27, "font-size", "1.2rem");
    cJSON_AddItemToArray(rules, mq27);
}

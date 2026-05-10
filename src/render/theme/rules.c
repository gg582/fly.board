#include "../theme.h"
#include <cjson/cJSON.h>
#include <stdlib.h>
#include <string.h>

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
}

void rule_base(cJSON *rules) {
    cJSON *a = create_rule("*");
    add_decl(a, "box-sizing", "border-box");
    cJSON_AddItemToArray(rules, a);

    cJSON *b = create_rule("html, body");
    add_decl(b, "margin", "0");
    add_decl(b, "padding", "0");
    cJSON_AddItemToArray(rules, b);

    cJSON *body = create_rule("body");
    add_decl(body, "background", "var(--bg)");
    add_decl(body, "color", "var(--fg)");
    add_decl(body, "font", "16px/1.65 'Pretendard GOV', 'Pretendard', system-ui, -apple-system, Arial, sans-serif");
    add_decl(body, "transition", "background 0.5s ease, color 0.5s ease");
    cJSON_AddItemToArray(rules, body);

    cJSON *link = create_rule("a");
    add_decl(link, "color", "var(--accent)");
    add_decl(link, "text-decoration", "none");
    add_decl(link, "transition", "color 0.2s ease");
    cJSON_AddItemToArray(rules, link);

    cJSON *linkh = create_rule("a:hover");
    add_decl(linkh, "color", "var(--accent2)");
    cJSON_AddItemToArray(rules, linkh);
}

void rule_layout(cJSON *rules) {
    cJSON *shell = create_rule(".shell");
    add_decl(shell, "max-width", "1400px");
    add_decl(shell, "margin", "0 auto");
    add_decl(shell, "padding", "16px");
    cJSON_AddItemToArray(rules, shell);

    cJSON *nav = create_rule(".topbar");
    add_decl(nav, "display", "flex");
    add_decl(nav, "align-items", "center");
    add_decl(nav, "justify-content", "space-between");
    add_decl(nav, "gap", "16px");
    add_decl(nav, "width", "100%");
    add_decl(nav, "margin", "0");
    add_decl(nav, "padding", "0");
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

    cJSON *brand = create_rule(".topbar-brand");
    add_decl(brand, "display", "flex");
    add_decl(brand, "flex-direction", "column");
    add_decl(brand, "align-items", "flex-start");
    add_decl(brand, "gap", "4px");
    add_decl(brand, "line-height", "1.2");
    cJSON_AddItemToArray(rules, brand);

    cJSON *brand_title = create_rule(".topbar-title");
    add_decl(brand_title, "font-weight", "800");
    add_decl(brand_title, "font-size", "18px");
    cJSON_AddItemToArray(rules, brand_title);

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
}

void rule_components(cJSON *rules) {
    cJSON *card = create_rule(".card");
    add_decl(card, "background", "var(--panel)");
    add_decl(card, "border", "1px solid var(--border)");
    add_decl(card, "border-radius", "12px");
    add_decl(card, "padding", "22px");
    add_decl(card, "box-shadow", "0 2px 8px var(--shadow)");
    add_decl(card, "transition", "transform 0.25s ease, box-shadow 0.25s ease, background 0.5s ease, border-color 0.5s ease");
    cJSON_AddItemToArray(rules, card);

    cJSON *cardh = create_rule(".card:hover");
    add_decl(cardh, "transform", "translateY(-3px)");
    add_decl(cardh, "box-shadow", "0 8px 24px var(--shadow)");
    cJSON_AddItemToArray(rules, cardh);

    cJSON *btn = create_rule(".btn");
    add_decl(btn, "display", "inline-flex");
    add_decl(btn, "align-items", "center");
    add_decl(btn, "gap", "6px");
    add_decl(btn, "padding", "8px 16px");
    add_decl(btn, "border", "none");
    add_decl(btn, "border-radius", "8px");
    add_decl(btn, "background", "var(--accent)");
    add_decl(btn, "color", "#fff");
    add_decl(btn, "font-weight", "600");
    add_decl(btn, "cursor", "pointer");
    add_decl(btn, "transition", "filter 0.2s ease, transform 0.15s ease, background 0.5s ease");
    cJSON_AddItemToArray(rules, btn);

    cJSON *btnh = create_rule(".btn:hover");
    add_decl(btnh, "filter", "brightness(1.1)");
    add_decl(btnh, "transform", "scale(1.02)");
    cJSON_AddItemToArray(rules, btnh);

    cJSON *btna = create_rule(".btn:active");
    add_decl(btna, "transform", "scale(0.98)");
    cJSON_AddItemToArray(rules, btna);

    cJSON *btn2 = create_rule(".btn-outline");
    add_decl(btn2, "background", "transparent");
    add_decl(btn2, "color", "var(--accent)");
    add_decl(btn2, "border", "1px solid var(--accent)");
    cJSON_AddItemToArray(rules, btn2);

    cJSON *input = create_rule("input, textarea, select");
    add_decl(input, "width", "100%");
    add_decl(input, "padding", "10px 12px");
    add_decl(input, "border", "1px solid var(--border)");
    add_decl(input, "border-radius", "8px");
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
    add_decl(alert, "border-radius", "8px");
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
}

void rule_home(cJSON *rules) {
    cJSON *hero = create_rule(".hero");
    add_decl(hero, "padding", "48px 0 36px");
    add_decl(hero, "text-align", "center");
    cJSON_AddItemToArray(rules, hero);

    cJSON *hero_h1 = create_rule(".hero h1");
    add_decl(hero_h1, "font-size", "44px");
    add_decl(hero_h1, "margin", "0 0 10px");
    add_decl(hero_h1, "letter-spacing", "-0.5px");
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
    cJSON_AddItemToArray(rules, hero_p);

    cJSON *grid = create_rule(".post-grid");
    add_decl(grid, "display", "grid");
    add_decl(grid, "grid-template-columns", "repeat(auto-fill, minmax(300px, 1fr))");
    add_decl(grid, "gap", "18px");
    add_decl(grid, "margin-top", "24px");
    cJSON_AddItemToArray(rules, grid);

    cJSON *tag = create_rule(".tag");
    add_decl(tag, "display", "inline-block");
    add_decl(tag, "padding", "4px 10px");
    add_decl(tag, "border-radius", "999px");
    add_decl(tag, "background", "var(--hover)");
    add_decl(tag, "font-size", "12px");
    add_decl(tag, "font-weight", "600");
    add_decl(tag, "color", "var(--accent)");
    add_decl(tag, "margin-right", "6px");
    add_decl(tag, "transition", "background 0.5s ease, color 0.5s ease");
    cJSON_AddItemToArray(rules, tag);

    cJSON *board_sec = create_rule(".board-section");
    add_decl(board_sec, "background", "var(--panel)");
    add_decl(board_sec, "border", "1px solid var(--border)");
    add_decl(board_sec, "border-radius", "12px");
    add_decl(board_sec, "padding", "20px");
    add_decl(board_sec, "box-shadow", "0 2px 8px var(--shadow)");
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

    cJSON *card = create_rule(".board-card");
    add_decl(card, "background", "var(--glass-bg)");
    add_decl(card, "backdrop-filter", "blur(20px) saturate(180%)");
    add_decl(card, "-webkit-backdrop-filter", "blur(20px) saturate(180%)");
    add_decl(card, "border", "1px solid var(--glass-border)");
    add_decl(card, "border-radius", "20px");
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
    add_decl(ch2, "font-size", "22px");
    add_decl(ch2, "font-weight", "700");
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
    add_decl(desc, "font-size", "14px");
    add_decl(desc, "line-height", "1.6");
    add_decl(desc, "margin", "0");
    add_decl(desc, "min-height", "22px");
    cJSON_AddItemToArray(rules, desc);

    cJSON *plist = create_rule(".post-list");
    add_decl(plist, "display", "flex");
    add_decl(plist, "flex-direction", "column");
    add_decl(plist, "gap", "12px");
    add_decl(plist, "max-width", "800px");
    add_decl(plist, "margin", "0 auto");
    cJSON_AddItemToArray(rules, plist);

    cJSON *prow = create_rule(".post-row");
    add_decl(prow, "background", "var(--panel)");
    add_decl(prow, "border", "1px solid var(--border)");
    add_decl(prow, "border-radius", "12px");
    add_decl(prow, "text-align", "center");
    add_decl(prow, "overflow", "hidden");
    add_decl(prow, "transition", "background 0.2s ease, border-color 0.2s ease, transform 0.2s ease");
    cJSON_AddItemToArray(rules, prow);

    cJSON *prow_h = create_rule(".post-row:hover");
    add_decl(prow_h, "background", "var(--hover)");
    add_decl(prow_h, "border-color", "var(--accent)");
    cJSON_AddItemToArray(rules, prow_h);

    cJSON *prow_head = create_rule(".post-row-head");
    add_decl(prow_head, "display", "flex");
    add_decl(prow_head, "justify-content", "center");
    add_decl(prow_head, "gap", "8px");
    add_decl(prow_head, "padding", "10px 16px");
    add_decl(prow_head, "border-bottom", "1px solid var(--border)");
    cJSON_AddItemToArray(rules, prow_head);

    cJSON *prow_title = create_rule(".post-row-title");
    add_decl(prow_title, "font-size", "16px");
    add_decl(prow_title, "font-weight", "700");
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
    add_decl(prow_sum, "font-size", "13px");
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
    add_decl(prow_meta, "justify-content", "center");
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
    add_decl(list, "gap", "10px");
    cJSON_AddItemToArray(rules, list);

    cJSON *item = create_rule(".board-post-item");
    add_decl(item, "display", "flex");
    add_decl(item, "flex-direction", "column");
    add_decl(item, "background", "var(--panel)");
    add_decl(item, "border-radius", "14px");
    add_decl(item, "border", "1px solid var(--border)");
    add_decl(item, "transition", "background 0.2s ease, border-color 0.2s ease, transform 0.2s ease");
    add_decl(item, "text-align", "center");
    add_decl(item, "overflow", "hidden");
    cJSON_AddItemToArray(rules, item);

    cJSON *itemh = create_rule(".board-post-item:hover");
    add_decl(itemh, "background", "var(--hover)");
    add_decl(itemh, "border-color", "var(--accent)");
    cJSON_AddItemToArray(rules, itemh);

    cJSON *ptitle = create_rule(".board-post-title");
    add_decl(ptitle, "font-size", "15px");
    add_decl(ptitle, "font-weight", "700");
    add_decl(ptitle, "color", "var(--fg)");
    add_decl(ptitle, "text-decoration", "none");
    add_decl(ptitle, "display", "block");
    add_decl(ptitle, "padding", "12px 16px");
    add_decl(ptitle, "border-bottom", "1px solid var(--border)");
    add_decl(ptitle, "transition", "color 0.2s ease");
    cJSON_AddItemToArray(rules, ptitle);

    cJSON *ptitleh = create_rule(".board-post-title:hover");
    add_decl(ptitleh, "color", "var(--accent)");
    cJSON_AddItemToArray(rules, ptitleh);

    cJSON *psum = create_rule(".board-post-summary");
    add_decl(psum, "font-size", "13px");
    add_decl(psum, "color", "var(--muted)");
    add_decl(psum, "line-height", "1.5");
    add_decl(psum, "margin", "0");
    add_decl(psum, "padding", "10px 16px");
    add_decl(psum, "border-bottom", "1px solid var(--border)");
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
    add_decl(meta, "justify-content", "center");
    add_decl(meta, "padding", "10px 16px");
    cJSON_AddItemToArray(rules, meta);

    cJSON *badge = create_rule(".post-badge");
    add_decl(badge, "display", "inline-flex");
    add_decl(badge, "align-items", "center");
    add_decl(badge, "gap", "4px");
    add_decl(badge, "padding", "3px 10px");
    add_decl(badge, "border-radius", "999px");
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
    add_decl(empty, "text-align", "center");
    add_decl(empty, "padding", "16px");
    add_decl(empty, "background", "var(--panel)");
    add_decl(empty, "border-radius", "12px");
    add_decl(empty, "border", "1px dashed var(--border)");
    add_decl(empty, "margin", "0");
    cJSON_AddItemToArray(rules, empty);
}

void rule_markdown(cJSON *rules) {
    cJSON *md = create_rule(".markdown-body");
    add_decl(md, "max-width", "720px");
    add_decl(md, "margin", "0 auto");
    add_decl(md, "line-height", "1.8");
    cJSON_AddItemToArray(rules, md);

    cJSON *md_img = create_rule(".markdown-body img, .markdown-body video, .markdown-body audio");
    add_decl(md_img, "max-width", "100%");
    add_decl(md_img, "border-radius", "10px");
    add_decl(md_img, "box-shadow", "0 2px 10px var(--shadow)");
    add_decl(md_img, "transition", "transform 0.3s ease");
    cJSON_AddItemToArray(rules, md_img);

    cJSON *md_imgh = create_rule(".markdown-body img:hover");
    add_decl(md_imgh, "transform", "scale(1.01)");
    cJSON_AddItemToArray(rules, md_imgh);

    cJSON *md_pre = create_rule(".markdown-body pre");
    add_decl(md_pre, "background", "var(--code-bg)");
    add_decl(md_pre, "padding", "16px");
    add_decl(md_pre, "border-radius", "10px");
    add_decl(md_pre, "overflow", "auto");
    add_decl(md_pre, "border", "1px solid var(--border)");
    add_decl(md_pre, "font-family", "'Fira Code', 'JetBrains Mono', Consolas, Monaco, 'Courier New', monospace");
    add_decl(md_pre, "font-size", "14px");
    add_decl(md_pre, "line-height", "1.6");
    add_decl(md_pre, "transition", "background 0.5s ease, border-color 0.5s ease, opacity 0.2s ease");
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
    add_decl(md_code, "border-radius", "4px");
    add_decl(md_code, "font-size", "0.92em");
    add_decl(md_code, "transition", "background 0.5s ease");
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
    add_decl(md_tbl, "border-collapse", "collapse");
    add_decl(md_tbl, "width", "100%");
    add_decl(md_tbl, "margin", "18px 0");
    cJSON_AddItemToArray(rules, md_tbl);

    cJSON *md_thtd = create_rule(".markdown-body th, .markdown-body td");
    add_decl(md_thtd, "border", "1px solid var(--border)");
    add_decl(md_thtd, "padding", "8px 10px");
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

    cJSON *md_h = create_rule(".markdown-body h1, .markdown-body h2, .markdown-body h3");
    add_decl(md_h, "margin-top", "36px");
    add_decl(md_h, "margin-bottom", "16px");
    add_decl(md_h, "letter-spacing", "-0.3px");
    cJSON_AddItemToArray(rules, md_h);

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
}

void rule_animations(cJSON *rules) {
    cJSON *kf = create_rule("@keyframes fadeIn");
    add_decl(kf, "from", "opacity:0; transform: translateY(8px)");
    add_decl(kf, "to", "opacity:1; transform: translateY(0)");
    cJSON_AddItemToArray(rules, kf);

    cJSON *anim = create_rule(".fade-in");
    add_decl(anim, "animation", "fadeIn 0.5s ease both");
    cJSON_AddItemToArray(rules, anim);
}

void rule_media(cJSON *rules) {
    cJSON *mq = create_rule("@media (max-width: 768px)");
    add_decl(mq, ".shell", "padding: 16px");
    add_decl(mq, ".topbar", "flex-wrap: wrap");
    add_decl(mq, ".topbar", "align-items: flex-start");
    add_decl(mq, ".topbar", "width: 100%");
    add_decl(mq, ".topbar", "margin: 0");
    add_decl(mq, ".topbar", "padding: 0");
    add_decl(mq, ".nav-links", "width: 100%");
    add_decl(mq, ".nav-links", "justify-content: flex-start");
    add_decl(mq, ".hero-logo", "height: 100px");
    add_decl(mq, ".hero h1", "font-size: 30px");
    add_decl(mq, ".post-grid", "grid-template-columns: 1fr");
    add_decl(mq, ".board-grid", "grid-template-columns: 1fr");
    add_decl(mq, ".board-card", "padding: 20px");
    add_decl(mq, ".board-card h2", "font-size: 20px");
    cJSON_AddItemToArray(rules, mq);
}


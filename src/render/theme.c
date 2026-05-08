#include "theme.h"
#include <cwist/core/mem/alloc.h>
#include <string.h>

typedef struct theme_color {
    const char *bg;
    const char *fg;
    const char *muted;
    const char *panel;
    const char *accent;
    const char *accent2;
    const char *border;
    const char *shadow;
    const char *hover;
    const char *code_bg;
} theme_color_t;

static theme_color_t light = {
    .bg = "#f8fafc",
    .fg = "#0f172a",
    .muted = "#64748b",
    .panel = "#ffffff",
    .accent = "#0f766e",
    .accent2 = "#14b8a6",
    .border = "#e2e8f0",
    .shadow = "rgba(0,0,0,0.06)",
    .hover = "#f1f5f9",
    .code_bg = "#f1f5f9"
};

static theme_color_t dark = {
    .bg = "#0b0f19",
    .fg = "#e2e8f0",
    .muted = "#94a3b8",
    .panel = "#111827",
    .accent = "#2dd4bf",
    .accent2 = "#5eead4",
    .border = "#1f2937",
    .shadow = "rgba(0,0,0,0.35)",
    .hover = "#1e293b",
    .code_bg = "#1e293b"
};

static void add_decl(cwist_css_rule_t *r, const char *p, const char *v) { cwist_css_rule_add_decl(r, p, v); }

static void rule_root(cwist_css_builder_t *css, theme_color_t *t) {
    cwist_css_rule_t *r = cwist_css_rule_create(":root");
    add_decl(r, "--bg", t->bg);
    add_decl(r, "--fg", t->fg);
    add_decl(r, "--muted", t->muted);
    add_decl(r, "--panel", t->panel);
    add_decl(r, "--accent", t->accent);
    add_decl(r, "--accent2", t->accent2);
    add_decl(r, "--border", t->border);
    add_decl(r, "--shadow", t->shadow);
    add_decl(r, "--hover", t->hover);
    add_decl(r, "--code-bg", t->code_bg);
    cwist_css_builder_add_rule(css, r);
}

static void rule_base(cwist_css_builder_t *css) {
    cwist_css_rule_t *a = cwist_css_rule_create("*");
    add_decl(a, "box-sizing", "border-box");
    cwist_css_builder_add_rule(css, a);

    cwist_css_rule_t *b = cwist_css_rule_create("html, body");
    add_decl(b, "margin", "0");
    add_decl(b, "padding", "0");
    cwist_css_builder_add_rule(css, b);

    cwist_css_rule_t *body = cwist_css_rule_create("body");
    add_decl(body, "background", "var(--bg)");
    add_decl(body, "color", "var(--fg)");
    add_decl(body, "font", "16px/1.65 'Inter', system-ui, -apple-system, Arial, sans-serif");
    add_decl(body, "transition", "background 0.4s ease, color 0.4s ease");
    cwist_css_builder_add_rule(css, body);

    cwist_css_rule_t *link = cwist_css_rule_create("a");
    add_decl(link, "color", "var(--accent)");
    add_decl(link, "text-decoration", "none");
    add_decl(link, "transition", "color 0.2s ease");
    cwist_css_builder_add_rule(css, link);

    cwist_css_rule_t *linkh = cwist_css_rule_create("a:hover");
    add_decl(linkh, "color", "var(--accent2)");
    cwist_css_builder_add_rule(css, linkh);
}

static void rule_layout(cwist_css_builder_t *css) {
    cwist_css_rule_t *shell = cwist_css_rule_create(".shell");
    add_decl(shell, "max-width", "1100px");
    add_decl(shell, "margin", "0 auto");
    add_decl(shell, "padding", "24px");
    cwist_css_builder_add_rule(css, shell);

    cwist_css_rule_t *nav = cwist_css_rule_create(".topbar");
    add_decl(nav, "display", "flex");
    add_decl(nav, "align-items", "center");
    add_decl(nav, "justify-content", "space-between");
    add_decl(nav, "gap", "16px");
    add_decl(nav, "padding", "14px 24px");
    add_decl(nav, "background", "var(--panel)");
    add_decl(nav, "border-bottom", "1px solid var(--border)");
    add_decl(nav, "position", "sticky");
    add_decl(nav, "top", "0");
    add_decl(nav, "z-index", "100");
    add_decl(nav, "backdrop-filter", "blur(10px)");
    add_decl(nav, "transition", "background 0.4s ease, border-color 0.4s ease");
    cwist_css_builder_add_rule(css, nav);

    cwist_css_rule_t *navlinks = cwist_css_rule_create(".nav-links");
    add_decl(navlinks, "display", "flex");
    add_decl(navlinks, "gap", "18px");
    add_decl(navlinks, "align-items", "center");
    cwist_css_builder_add_rule(css, navlinks);

    cwist_css_rule_t *footer = cwist_css_rule_create(".site-footer");
    add_decl(footer, "text-align", "center");
    add_decl(footer, "padding", "40px 24px");
    add_decl(footer, "color", "var(--muted)");
    add_decl(footer, "font-size", "13px");
    add_decl(footer, "border-top", "1px solid var(--border)");
    add_decl(footer, "margin-top", "40px");
    cwist_css_builder_add_rule(css, footer);

    cwist_css_rule_t *fc = cwist_css_rule_create(".footer-content");
    add_decl(fc, "display", "flex");
    add_decl(fc, "align-items", "center");
    add_decl(fc, "justify-content", "center");
    add_decl(fc, "gap", "8px");
    cwist_css_builder_add_rule(css, fc);

    cwist_css_rule_t *fl = cwist_css_rule_create(".footer-logo");
    add_decl(fl, "height", "16px");
    add_decl(fl, "width", "auto");
    add_decl(fl, "opacity", "0.6");
    cwist_css_builder_add_rule(css, fl);

    cwist_css_rule_t *pp = cwist_css_rule_create(".profile-pic");
    add_decl(pp, "width", "40px");
    add_decl(pp, "height", "40px");
    add_decl(pp, "border-radius", "50%");
    add_decl(pp, "object-fit", "cover");
    add_decl(pp, "border", "1px solid var(--border)");
    cwist_css_builder_add_rule(css, pp);

    cwist_css_rule_t *pp_s = cwist_css_rule_create(".profile-pic-small");
    add_decl(pp_s, "width", "24px");
    add_decl(pp_s, "height", "24px");
    add_decl(pp_s, "border-radius", "50%");
    add_decl(pp_s, "object-fit", "cover");
    cwist_css_builder_add_rule(css, pp_s);
}

static void rule_components(cwist_css_builder_t *css) {
    cwist_css_rule_t *card = cwist_css_rule_create(".card");
    add_decl(card, "background", "var(--panel)");
    add_decl(card, "border", "1px solid var(--border)");
    add_decl(card, "border-radius", "12px");
    add_decl(card, "padding", "22px");
    add_decl(card, "box-shadow", "0 2px 8px var(--shadow)");
    add_decl(card, "transition", "transform 0.25s ease, box-shadow 0.25s ease, background 0.4s ease");
    cwist_css_builder_add_rule(css, card);

    cwist_css_rule_t *cardh = cwist_css_rule_create(".card:hover");
    add_decl(cardh, "transform", "translateY(-3px)");
    add_decl(cardh, "box-shadow", "0 8px 24px var(--shadow)");
    cwist_css_builder_add_rule(css, cardh);

    cwist_css_rule_t *btn = cwist_css_rule_create(".btn");
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
    add_decl(btn, "transition", "filter 0.2s ease, transform 0.15s ease");
    cwist_css_builder_add_rule(css, btn);

    cwist_css_rule_t *btnh = cwist_css_rule_create(".btn:hover");
    add_decl(btnh, "filter", "brightness(1.1)");
    add_decl(btnh, "transform", "scale(1.02)");
    cwist_css_builder_add_rule(css, btnh);

    cwist_css_rule_t *btn2 = cwist_css_rule_create(".btn-outline");
    add_decl(btn2, "background", "transparent");
    add_decl(btn2, "color", "var(--accent)");
    add_decl(btn2, "border", "1px solid var(--accent)");
    cwist_css_builder_add_rule(css, btn2);

    cwist_css_rule_t *input = cwist_css_rule_create("input, textarea, select");
    add_decl(input, "width", "100%");
    add_decl(input, "padding", "10px 12px");
    add_decl(input, "border", "1px solid var(--border)");
    add_decl(input, "border-radius", "8px");
    add_decl(input, "background", "var(--panel)");
    add_decl(input, "color", "var(--fg)");
    add_decl(input, "font", "inherit");
    add_decl(input, "outline", "none");
    add_decl(input, "transition", "border-color 0.2s ease, box-shadow 0.2s ease");
    cwist_css_builder_add_rule(css, input);

    cwist_css_rule_t *inputf = cwist_css_rule_create("input:focus, textarea:focus, select:focus");
    add_decl(inputf, "border-color", "var(--accent)");
    add_decl(inputf, "box-shadow", "0 0 0 3px rgba(20,184,166,0.15)");
    cwist_css_builder_add_rule(css, inputf);

    cwist_css_rule_t *label = cwist_css_rule_create("label");
    add_decl(label, "display", "block");
    add_decl(label, "margin-bottom", "6px");
    add_decl(label, "font-weight", "600");
    add_decl(label, "font-size", "14px");
    cwist_css_builder_add_rule(css, label);

    cwist_css_rule_t *alert = cwist_css_rule_create(".alert");
    add_decl(alert, "padding", "12px 14px");
    add_decl(alert, "border-radius", "8px");
    add_decl(alert, "background", "rgba(239,68,68,0.08)");
    add_decl(alert, "color", "#ef4444");
    add_decl(alert, "border", "1px solid rgba(239,68,68,0.25)");
    add_decl(alert, "margin-bottom", "14px");
    cwist_css_builder_add_rule(css, alert);
}

static void rule_home(cwist_css_builder_t *css) {
    cwist_css_rule_t *hero = cwist_css_rule_create(".hero");
    add_decl(hero, "padding", "48px 0 36px");
    add_decl(hero, "text-align", "center");
    cwist_css_builder_add_rule(css, hero);

    cwist_css_rule_t *hero_h1 = cwist_css_rule_create(".hero h1");
    add_decl(hero_h1, "font-size", "44px");
    add_decl(hero_h1, "margin", "0 0 10px");
    add_decl(hero_h1, "letter-spacing", "-0.5px");
    cwist_css_builder_add_rule(css, hero_h1);

    cwist_css_rule_t *hero_p = cwist_css_rule_create(".hero p");
    add_decl(hero_p, "color", "var(--muted)");
    add_decl(hero_p, "font-size", "18px");
    add_decl(hero_p, "max-width", "640px");
    add_decl(hero_p, "margin", "0 auto");
    cwist_css_builder_add_rule(css, hero_p);

    cwist_css_rule_t *grid = cwist_css_rule_create(".post-grid");
    add_decl(grid, "display", "grid");
    add_decl(grid, "grid-template-columns", "repeat(auto-fill, minmax(300px, 1fr))");
    add_decl(grid, "gap", "18px");
    add_decl(grid, "margin-top", "24px");
    cwist_css_builder_add_rule(css, grid);

    cwist_css_rule_t *tag = cwist_css_rule_create(".tag");
    add_decl(tag, "display", "inline-block");
    add_decl(tag, "padding", "4px 10px");
    add_decl(tag, "border-radius", "999px");
    add_decl(tag, "background", "var(--hover)");
    add_decl(tag, "font-size", "12px");
    add_decl(tag, "font-weight", "600");
    add_decl(tag, "color", "var(--accent)");
    add_decl(tag, "margin-right", "6px");
    cwist_css_builder_add_rule(css, tag);
}

static void rule_markdown(cwist_css_builder_t *css) {
    cwist_css_rule_t *md = cwist_css_rule_create(".markdown-body");
    add_decl(md, "line-height", "1.75");
    cwist_css_builder_add_rule(css, md);

    cwist_css_rule_t *md_img = cwist_css_rule_create(".markdown-body img, .markdown-body video, .markdown-body audio");
    add_decl(md_img, "max-width", "100%");
    add_decl(md_img, "border-radius", "10px");
    add_decl(md_img, "box-shadow", "0 2px 10px var(--shadow)");
    add_decl(md_img, "transition", "transform 0.3s ease");
    cwist_css_builder_add_rule(css, md_img);

    cwist_css_rule_t *md_imgh = cwist_css_rule_create(".markdown-body img:hover");
    add_decl(md_imgh, "transform", "scale(1.01)");
    cwist_css_builder_add_rule(css, md_imgh);

    cwist_css_rule_t *md_pre = cwist_css_rule_create(".markdown-body pre");
    add_decl(md_pre, "background", "var(--code-bg)");
    add_decl(md_pre, "padding", "16px");
    add_decl(md_pre, "border-radius", "10px");
    add_decl(md_pre, "overflow", "auto");
    add_decl(md_pre, "border", "1px solid var(--border)");
    cwist_css_builder_add_rule(css, md_pre);

    cwist_css_rule_t *md_tbl = cwist_css_rule_create(".markdown-body table");
    add_decl(md_tbl, "border-collapse", "collapse");
    add_decl(md_tbl, "width", "100%");
    add_decl(md_tbl, "margin", "18px 0");
    cwist_css_builder_add_rule(css, md_tbl);

    cwist_css_rule_t *md_thtd = cwist_css_rule_create(".markdown-body th, .markdown-body td");
    add_decl(md_thtd, "border", "1px solid var(--border)");
    add_decl(md_thtd, "padding", "8px 10px");
    cwist_css_builder_add_rule(css, md_thtd);

    cwist_css_rule_t *md_h = cwist_css_rule_create(".markdown-body h1, .markdown-body h2, .markdown-body h3");
    add_decl(md_h, "margin-top", "28px");
    add_decl(md_h, "margin-bottom", "12px");
    add_decl(md_h, "letter-spacing", "-0.3px");
    cwist_css_builder_add_rule(css, md_h);
}

static void rule_animations(cwist_css_builder_t *css) {
    cwist_css_rule_t *kf = cwist_css_rule_create("@keyframes fadeIn");
    add_decl(kf, "from", "opacity:0; transform: translateY(8px)");
    add_decl(kf, "to", "opacity:1; transform: translateY(0)");
    cwist_css_builder_add_rule(css, kf);

    cwist_css_rule_t *anim = cwist_css_rule_create(".fade-in");
    add_decl(anim, "animation", "fadeIn 0.5s ease both");
    cwist_css_builder_add_rule(css, anim);
}

static void rule_media(cwist_css_builder_t *css) {
    cwist_css_rule_t *mq = cwist_css_rule_create("@media (max-width: 768px)");
    add_decl(mq, ".shell", "padding: 16px");
    add_decl(mq, ".topbar", "flex-wrap: wrap");
    add_decl(mq, ".hero h1", "font-size: 30px");
    add_decl(mq, ".post-grid", "grid-template-columns: 1fr");
    cwist_css_builder_add_rule(css, mq);
}

cwist_css_builder_t *theme_build(bool dark_mode) {
    theme_color_t *t = dark_mode ? &dark : &light;
    cwist_css_builder_t *css = cwist_css_builder_create();
    rule_root(css, t);
    rule_base(css);
    rule_layout(css);
    rule_components(css);
    rule_home(css);
    rule_markdown(css);
    rule_animations(css);
    rule_media(css);
    return css;
}

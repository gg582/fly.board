#include "../theme.h"
#include "../../config/config.h"
#include <cjson/cJSON.h>
#include <stdlib.h>
#include <string.h>


theme_color_t light = {
    .bg = "#f6f7f9", .fg = "#1a1a2e", .muted = "#6b7280",
    .panel = "#ffffff", .accent = "#4f46e5", .accent2 = "#6366f1",
    .border = "#e5e7eb", .shadow = "rgba(0,0,0,0.08)",
    .hover = "#f3f4f6", .code_bg = "#f4f4f5",
    .glass_bg = "rgba(255,255,255,0.54)", .glass_border = "rgba(255,255,255,0.7)"
};

theme_color_t dark = {
    .bg = "#0f0f13", .fg = "#e4e4e7", .muted = "#a1a1aa",
    .panel = "#1a1a1e", .accent = "#818cf8", .accent2 = "#a5b4fc",
    .border = "#3f3f46", .shadow = "rgba(0,0,0,0.4)",
    .hover = "#27272a", .code_bg = "#27272a",
    .glass_bg = "rgba(24,24,27,0.48)", .glass_border = "rgba(255,255,255,0.12)"
};

theme_color_t ocean = {
    .bg = "#0b1d2e", .fg = "#c8e1f4", .muted = "#7a9ab8",
    .panel = "#11283d", .accent = "#38bdf8", .accent2 = "#7dd3fc",
    .border = "#1e3a5f", .shadow = "rgba(0,0,0,0.4)",
    .hover = "#163450", .code_bg = "#0f172a",
    .glass_bg = "rgba(17,40,61,0.46)", .glass_border = "rgba(125,211,252,0.2)"
};

theme_color_t forest = {
    .bg = "#0f1f17", .fg = "#d1e7dd", .muted = "#8fb39a",
    .panel = "#162b20", .accent = "#34d399", .accent2 = "#6ee7b7",
    .border = "#22543d", .shadow = "rgba(0,0,0,0.4)",
    .hover = "#1c3a2a", .code_bg = "#14281e",
    .glass_bg = "rgba(22,43,32,0.46)", .glass_border = "rgba(110,231,183,0.18)"
};

theme_color_t sepia = {
    .bg = "#f4ecd8", .fg = "#433422", .muted = "#8c7b66",
    .panel = "#efe6d0", .accent = "#b45309", .accent2 = "#d97706",
    .border = "#d6c6a8", .shadow = "rgba(67,52,34,0.08)",
    .hover = "#eaddc5", .code_bg = "#e8dec3",
    .glass_bg = "rgba(239,230,208,0.56)", .glass_border = "rgba(255,255,255,0.62)"
};

theme_color_t *theme_by_name(const char *name) {
    if (!name) return &light;
    if (strcmp(name, "dark") == 0) return &dark;
    if (strcmp(name, "ocean") == 0) return &ocean;
    if (strcmp(name, "forest") == 0) return &forest;
    if (strcmp(name, "sepia") == 0) return &sepia;
    return &light;
}


/* Forward declarations from rules.c */
void rule_root(cJSON *vars, theme_color_t *t);
void rule_base(cJSON *rules);
void rule_layout(cJSON *rules);
void rule_components(cJSON *rules);
void rule_home(cJSON *rules);
void rule_boards(cJSON *rules);
void rule_markdown(cJSON *rules);
void rule_animations(cJSON *rules);
void rule_media(cJSON *rules);

void apply_config_accents() {
    if (g_config.accent[0] != '\0') {
        light.accent = g_config.accent;
        dark.accent = g_config.accent;
        ocean.accent = g_config.accent;
        forest.accent = g_config.accent;
        sepia.accent = g_config.accent;

        /* For simplicity, we just reuse the accent color for accent2 as well if not doing complex logic */
        light.accent2 = g_config.accent;
        dark.accent2 = g_config.accent;
        ocean.accent2 = g_config.accent;
        forest.accent2 = g_config.accent;
        sepia.accent2 = g_config.accent;
    }
}

cJSON *build_theme_object(const char *name, theme_color_t *t) {
    apply_config_accents();
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", name);

    cJSON *vars = cJSON_CreateObject();
    rule_root(vars, t);
    cJSON_AddItemToObject(root, "vars", vars);

    cJSON *rules = cJSON_CreateArray();
    rule_base(rules);
    rule_layout(rules);
    rule_components(rules);
    rule_home(rules);
    rule_boards(rules);
    rule_markdown(rules);
    rule_animations(rules);
    rule_media(rules);
    cJSON_AddItemToObject(root, "rules", rules);
    return root;
}

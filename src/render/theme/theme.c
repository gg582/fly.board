#include "../theme.h"
#include "../../config/config.h"
#include <cjson/cJSON.h>
#include <stdlib.h>
#include <string.h>


theme_color_t light = {
    .bg = "#f6f7f9", .fg = "#111827", .muted = "#4b5563",
    .panel = "#ffffff", .accent = "#4f46e5", .accent2 = "#4338ca",
    .border = "#d1d5db", .shadow = "rgba(0,0,0,0.08)",
    .hover = "#e5e7eb", .code_bg = "#e4e4e7",
    .glass_bg = "rgba(255,255,255,0.78)", .glass_border = "rgba(209,213,219,0.42)"
};

theme_color_t dark = {
    .bg = "#0f0f13", .fg = "#f4f4f5", .muted = "#71717a",
    .panel = "#1a1a1e", .accent = "#818cf8", .accent2 = "#6366f1",
    .border = "#52525b", .shadow = "rgba(0,0,0,0.5)",
    .hover = "#3f3f46", .code_bg = "#3f3f46",
    .glass_bg = "rgba(24,24,27,0.72)", .glass_border = "rgba(82,82,91,0.35)"
};

theme_color_t ocean = {
    .bg = "#0b1d2e", .fg = "#e0f2fe", .muted = "#5a8ab8",
    .panel = "#11283d", .accent = "#38bdf8", .accent2 = "#0284c7",
    .border = "#2a4d70", .shadow = "rgba(0,0,0,0.5)",
    .hover = "#1e4060", .code_bg = "#0f172a",
    .glass_bg = "rgba(17,40,61,0.72)", .glass_border = "rgba(42,77,112,0.35)"
};

theme_color_t forest = {
    .bg = "#0f1f17", .fg = "#d1e7dd", .muted = "#6b9a7a",
    .panel = "#162b20", .accent = "#34d399", .accent2 = "#059669",
    .border = "#2a5a42", .shadow = "rgba(0,0,0,0.5)",
    .hover = "#1c4a35", .code_bg = "#14281e",
    .glass_bg = "rgba(22,43,32,0.72)", .glass_border = "rgba(42,90,66,0.35)"
};

theme_color_t sepia = {
    .bg = "#f4ecd8", .fg = "#2d1f0e", .muted = "#6b5b45",
    .panel = "#efe6d0", .accent = "#b45309", .accent2 = "#78350f",
    .border = "#c7b596", .shadow = "rgba(67,52,34,0.10)",
    .hover = "#dfd3b8", .code_bg = "#ddd0b0",
    .glass_bg = "rgba(239,230,208,0.82)", .glass_border = "rgba(199,181,150,0.42)"
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


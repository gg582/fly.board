#include "../theme.h"
#include "../../config/config.h"
#include <cjson/cJSON.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>

theme_color_t light = {
    .dark_family = false,
    .bg = "#f6f7f9", .fg = "#111827", .muted = "#4b5563",
    .panel = "#ffffff", .accent = "#4f46e5", .accent2 = "#4338ca",
    .border = "#d1d5db", .shadow = "color-mix(in srgb, var(--shadow-base) 8%, transparent)",
    .hover = "#e5e7eb", .code_bg = "#e4e4e7",
    .glass_bg = "rgba(255,255,255,0.78)", .glass_border = "rgba(209,213,219,0.42)",
    .shadow_base = "#111827"
};

theme_color_t dark = {
    .dark_family = true,
    .bg = "#0f0f13", .fg = "#f4f4f5", .muted = "#a1a1aa",
    .panel = "#1a1a1e", .accent = "#818cf8", .accent2 = "#6366f1",
    .border = "#52525b", .shadow = "color-mix(in srgb, var(--shadow-base) 50%, transparent)",
    .hover = "#3f3f46", .code_bg = "#3f3f46",
    .glass_bg = "rgba(24,24,27,0.72)", .glass_border = "rgba(82,82,91,0.35)",
    .shadow_base = "#050505"
};

theme_color_t ocean = {
    .dark_family = true,
    .bg = "#0b1d2e", .fg = "#e0f2fe", .muted = "#91b8d4",
    .panel = "#11283d", .accent = "#38bdf8", .accent2 = "#0284c7",
    .border = "#2a4d70", .shadow = "color-mix(in srgb, var(--shadow-base) 50%, transparent)",
    .hover = "#1e4060", .code_bg = "#0f172a",
    .glass_bg = "rgba(17,40,61,0.72)", .glass_border = "rgba(42,77,112,0.35)",
    .shadow_base = "#02060a"
};

theme_color_t forest = {
    .dark_family = true,
    .bg = "#0f1f17", .fg = "#d1e7dd", .muted = "#9dc3aa",
    .panel = "#162b20", .accent = "#34d399", .accent2 = "#059669",
    .border = "#2a5a42", .shadow = "color-mix(in srgb, var(--shadow-base) 50%, transparent)",
    .hover = "#1c4a35", .code_bg = "#14281e",
    .glass_bg = "rgba(22,43,32,0.72)", .glass_border = "rgba(42,90,66,0.35)",
    .shadow_base = "#030805"
};

theme_color_t sepia = {
    .dark_family = false,
    .bg = "#f4ecd8", .fg = "#2d1f0e", .muted = "#6b5b45",
    .panel = "#efe6d0", .accent = "#b45309", .accent2 = "#78350f",
    .border = "#c7b596", .shadow = "color-mix(in srgb, var(--shadow-base) 10%, transparent)",
    .hover = "#dfd3b8", .code_bg = "#ddd0b0",
    .glass_bg = "rgba(239,230,208,0.82)", .glass_border = "rgba(199,181,150,0.42)",
    .shadow_base = "#1a1208"
};

theme_color_t paper = {
    .dark_family = false,
    .bg = "#f7f5f2", .fg = "#292524", .muted = "#6b6661",
    .panel = "#fffdfb", .accent = "#2563eb", .accent2 = "#1d4ed8",
    .border = "#d8d2cb", .shadow = "color-mix(in srgb, var(--shadow-base) 9%, transparent)",
    .hover = "#e9e4de", .code_bg = "#eae6e1",
    .glass_bg = "rgba(255,253,251,0.80)", .glass_border = "rgba(120,113,108,0.24)",
    .shadow_base = "#292524"
};

theme_color_t glacier = {
    .dark_family = false,
    .bg = "#f1f6f8", .fg = "#17242b", .muted = "#526872",
    .panel = "#fbfdfe", .accent = "#0f766e", .accent2 = "#115e59",
    .border = "#c5d5da", .shadow = "color-mix(in srgb, var(--shadow-base) 10%, transparent)",
    .hover = "#dfeaed", .code_bg = "#e1ebee",
    .glass_bg = "rgba(251,253,254,0.80)", .glass_border = "rgba(82,104,114,0.24)",
    .shadow_base = "#17242b"
};

theme_color_t rose = {
    .dark_family = false,
    .bg = "#fbf5f6", .fg = "#302328", .muted = "#76636a",
    .panel = "#fffafb", .accent = "#b83a5b", .accent2 = "#992f4c",
    .border = "#e2cdd3", .shadow = "color-mix(in srgb, var(--shadow-base) 9%, transparent)",
    .hover = "#f0dfe4", .code_bg = "#f0e3e7",
    .glass_bg = "rgba(255,250,251,0.80)", .glass_border = "rgba(184,58,91,0.20)",
    .shadow_base = "#302328"
};

theme_color_t midnight = {
    .dark_family = true,
    .bg = "#111820", .fg = "#e8eef4", .muted = "#9aaaba",
    .panel = "#19232e", .accent = "#7dd3fc", .accent2 = "#38bdf8",
    .border = "#354657", .shadow = "color-mix(in srgb, var(--shadow-base) 48%, transparent)",
    .hover = "#293947", .code_bg = "#202d39",
    .glass_bg = "rgba(25,35,46,0.76)", .glass_border = "rgba(125,211,252,0.18)",
    .shadow_base = "#05090d"
};

theme_color_t plum = {
    .dark_family = true,
    .bg = "#1b151f", .fg = "#f0e8f3", .muted = "#b1a2b8",
    .panel = "#27202d", .accent = "#d8b4fe", .accent2 = "#c084fc",
    .border = "#493c51", .shadow = "color-mix(in srgb, var(--shadow-base) 48%, transparent)",
    .hover = "#3b2f43", .code_bg = "#302737",
    .glass_bg = "rgba(39,32,45,0.76)", .glass_border = "rgba(216,180,254,0.17)",
    .shadow_base = "#09060b"
};

theme_color_t ember = {
    .dark_family = true,
    .bg = "#1d1815", .fg = "#f4ede7", .muted = "#b7a99e",
    .panel = "#2a231e", .accent = "#fb923c", .accent2 = "#f97316",
    .border = "#4d4036", .shadow = "color-mix(in srgb, var(--shadow-base) 48%, transparent)",
    .hover = "#40332a", .code_bg = "#342b24",
    .glass_bg = "rgba(42,35,30,0.76)", .glass_border = "rgba(251,146,60,0.17)",
    .shadow_base = "#0b0806"
};

static pthread_rwlock_t g_modes_lock = PTHREAD_RWLOCK_INITIALIZER;

void get_special_themes(char *out_light, char *out_dark) {
    pthread_rwlock_rdlock(&g_modes_lock);
    strcpy(out_light, "light");
    strcpy(out_dark, "dark");
    if (g_config.use_special_modes[0]) {
        char temp[64];
        snprintf(temp, sizeof(temp), "%.*s", (int)sizeof(temp) - 1, g_config.use_special_modes);
        char *comma = strchr(temp, ',');
        char *p1 = temp;
        char *p2 = comma ? comma + 1 : NULL;
        if (comma) *comma = '\0';
        while (*p1 == ' ') p1++;
        char *e1 = p1 + strlen(p1);
        while (e1 > p1 && e1[-1] == ' ') *--e1 = '\0';
        if (p1[0]) strcpy(out_light, p1);
        if (p2) {
            while (*p2 == ' ') p2++;
            char *e2 = p2 + strlen(p2);
            while (e2 > p2 && e2[-1] == ' ') *--e2 = '\0';
            if (p2[0]) strcpy(out_dark, p2);
        }
    }
    pthread_rwlock_unlock(&g_modes_lock);
}

bool theme_modes_load(const char *path) {
    FILE *file = fopen(path, "r");
    if (!file) return false;
    char line[512];
    char modes[64] = "";
    bool found = false;
    while (fgets(line, sizeof(line), file)) {
        if (strncmp(line, "use_special_modes=", 18) != 0) continue;
        snprintf(modes, sizeof(modes), "%s", line + 18);
        modes[strcspn(modes, "\r\n")] = '\0';
        found = true;
        break;
    }
    fclose(file);
    if (!found) return false;
    pthread_rwlock_wrlock(&g_modes_lock);
    snprintf(g_config.use_special_modes, sizeof(g_config.use_special_modes), "%s", modes);
    pthread_rwlock_unlock(&g_modes_lock);
    return true;
}

theme_color_t *theme_builtin_by_name(const char *name) {
    if (!name) return &light;
    if (strcmp(name, "dark") == 0) return &dark;
    if (strcmp(name, "ocean") == 0) return &ocean;
    if (strcmp(name, "forest") == 0) return &forest;
    if (strcmp(name, "sepia") == 0) return &sepia;
    if (strcmp(name, "paper") == 0) return &paper;
    if (strcmp(name, "glacier") == 0) return &glacier;
    if (strcmp(name, "rose") == 0) return &rose;
    if (strcmp(name, "midnight") == 0) return &midnight;
    if (strcmp(name, "plum") == 0) return &plum;
    if (strcmp(name, "ember") == 0) return &ember;
    return &light;
}

theme_color_t *theme_by_name(const char *name) {
    char l_theme[64], d_theme[64];
    get_special_themes(l_theme, d_theme);
    const char *resolved = name;
    if (!resolved) resolved = l_theme;
    else if (strcmp(resolved, "light") == 0) resolved = l_theme;
    else if (strcmp(resolved, "dark") == 0) resolved = d_theme;
    if (strcmp(resolved, "custom") == 0 && theme_custom_available())
        return theme_custom_is_dark() ? &dark : &light;
    return theme_builtin_by_name(resolved);
}

void rule_root(cJSON *vars, theme_color_t *t);
void rule_base(cJSON *rules);
void rule_layout(cJSON *rules);
void rule_components(cJSON *rules);
void rule_home(cJSON *rules);
void rule_boards(cJSON *rules);
void rule_markdown(cJSON *rules);
void rule_animations(cJSON *rules);
void rule_media(cJSON *rules);

cJSON *build_theme_object(const char *name, theme_color_t *t) {
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

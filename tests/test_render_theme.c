/* Unit test: src/render/theme theme_build_json output shape.
 *
 * Covers: theme JSON parses as JSON, carries the expected keys (colors,
 * fonts, background targets), reflects dark mode, honors the accent config
 * override, theme_build_css emits a CSS string containing a variable
 * definition, and each theme gets the wallpaper of its light/dark family.
 *
 * Build:
 *   gcc -O1 -Iinclude -Isrc -I/home/yjlee/cwist/include -I/home/yjlee/cwist/lib \
 *       -I/home/yjlee/cwist/lib/cjson \
 *       tests/test_render_theme.c tests/stubs/render_test_stubs.c \
 *       src/render/theme/theme.o src/render/theme/json.o \
 *       src/render/theme/css.o src/render/theme/rules.o \
 *       /home/yjlee/cwist/libcwist.a /home/yjlee/cwist/lib/libttak/lib/libttak.a \
 *       /home/yjlee/cwist/lib/cjson/libcjson.a \
 *       -lpthread -lm -ldl -o /tmp/test_render_theme
 * Run: /tmp/test_render_theme
 */
#include "config/config.h"
#include "render/theme.h"
#include <cjson/cJSON.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int failures = 0;
#define CHECK(cond, name) do { \
    if (cond) printf("PASS: %s\n", name); \
    else { printf("FAIL: %s (line %d)\n", name, __LINE__); failures++; } \
} while (0)

int main(void) {
    memset(&g_config, 0, sizeof(g_config));
    g_config.accent[0] = '\0'; /* no override: built-in accents */

    /* ---- dark mode ---- */
    char *json = theme_build_json(true);
    CHECK(json && json[0], "theme_build_json(dark) produces output");
    cJSON *root = json ? cJSON_Parse(json) : NULL;
    CHECK(root != NULL, "dark theme JSON parses");
    if (root) {
        cJSON *name = cJSON_GetObjectItem(root, "name");
        CHECK(cJSON_IsString(name), "has name string");
        CHECK(name && strcmp(name->valuestring, "dark") == 0, "name is 'dark'");
        cJSON *vars = cJSON_GetObjectItem(root, "vars");
        CHECK(cJSON_IsObject(vars), "has vars object");
        if (vars) {
            const char *keys[] = {"--bg", "--fg", "--muted", "--panel", "--accent", "--border", NULL};
            for (int i = 0; keys[i]; i++) {
                cJSON *v = cJSON_GetObjectItem(vars, keys[i]);
                char label[64];
                snprintf(label, sizeof(label), "dark var '%s' present", keys[i]);
                CHECK(cJSON_IsString(v) && v->valuestring[0] != '\0', label);
            }
        }
        free(json);
        cJSON_Delete(root);
    }

    /* ---- light mode ---- */
    json = theme_build_json(false);
    root = json ? cJSON_Parse(json) : NULL;
    CHECK(root != NULL, "light theme JSON parses");
    if (root) {
        cJSON *name = cJSON_GetObjectItem(root, "name");
        CHECK(name && strcmp(name->valuestring, "light") == 0, "name is 'light'");
        free(json);
        cJSON_Delete(root);
    }

    /* ---- accent override from config ---- */
    snprintf(g_config.accent, sizeof(g_config.accent), "#ff0088");
    json = theme_build_json(false);
    root = json ? cJSON_Parse(json) : NULL;
    CHECK(root != NULL, "accent-override JSON parses");
    if (root) {
        cJSON *accent = cJSON_GetObjectItem(cJSON_GetObjectItem(root, "vars"), "--accent");
        CHECK(cJSON_IsString(accent) && strcmp(accent->valuestring, "#ff0088") == 0,
              "config accent override applied");
        free(json);
        cJSON_Delete(root);
    }

    /* ---- css output ---- */
    g_config.accent[0] = '\0';
    char *css = theme_build_css(false);
    CHECK(css && strstr(css, "--"), "theme_build_css emits CSS variables");
    CHECK(css && strstr(css, "#"), "theme_build_css emits color values");
    free(css);

    /* ---- full-page wallpaper follows the palette's light/dark family ----
     * Regression: building the vars from a copy of the shared palette broke
     * the pointer-identity check and gave every theme the dark wallpaper
     * (dark text on a black page after toggling to light). */
    snprintf(g_config.bg_full_light, sizeof(g_config.bg_full_light), "bg-light.svg");
    snprintf(g_config.bg_full_dark, sizeof(g_config.bg_full_dark), "bg-dark.svg");
    snprintf(g_config.accent, sizeof(g_config.accent), "#ff0088");
    char *all = theme_build_all_json();
    cJSON *arr = all ? cJSON_Parse(all) : NULL;
    CHECK(cJSON_IsArray(arr), "theme_build_all_json parses as an array");
    const struct { const char *name; const char *wallpaper; } want[] = {
        {"light", "bg-light.svg"}, {"sepia", "bg-light.svg"},
        {"dark", "bg-dark.svg"}, {"ocean", "bg-dark.svg"}, {"forest", "bg-dark.svg"},
    };
    for (size_t i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
        cJSON *t = NULL, *it = NULL;
        cJSON_ArrayForEach(it, arr) {
            cJSON *n = cJSON_GetObjectItem(it, "name");
            if (cJSON_IsString(n) && strcmp(n->valuestring, want[i].name) == 0) t = it;
        }
        cJSON *vars = t ? cJSON_GetObjectItem(t, "vars") : NULL;
        cJSON *bg = vars ? cJSON_GetObjectItem(vars, "--full-page-background") : NULL;
        cJSON *acc = vars ? cJSON_GetObjectItem(vars, "--accent") : NULL;
        char label[96];
        snprintf(label, sizeof(label), "%s theme uses %s", want[i].name, want[i].wallpaper);
        CHECK(cJSON_IsString(bg) && strstr(bg->valuestring, want[i].wallpaper), label);
        snprintf(label, sizeof(label), "%s theme keeps the accent override", want[i].name);
        CHECK(cJSON_IsString(acc) && strcmp(acc->valuestring, "#ff0088") == 0, label);
    }
    cJSON_Delete(arr);
    free(all);
    g_config.accent[0] = '\0';

    /* ---- theme lookup ---- */
    CHECK(theme_by_name("ocean") == &ocean, "theme_by_name finds ocean");
    CHECK(theme_by_name("nonexistent-theme") == &light, "theme_by_name falls back to light");

    printf("%s\n", failures == 0 ? "ALL PASS" : "SOME FAILED");
    return failures == 0 ? 0 : 1;
}

/* Unit test: src/render/theme theme_build_json output shape.
 *
 * Covers: theme JSON parses as JSON, carries the expected keys (colors,
 * fonts, background targets), reflects dark mode, honors the accent config
 * override, and theme_build_css emits a CSS string containing a variable
 * definition.
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

    /* ---- theme lookup ---- */
    CHECK(theme_by_name("ocean") == &ocean, "theme_by_name finds ocean");
    CHECK(theme_by_name("nonexistent-theme") == &light, "theme_by_name falls back to light");

    printf("%s\n", failures == 0 ? "ALL PASS" : "SOME FAILED");
    return failures == 0 ? 0 : 1;
}

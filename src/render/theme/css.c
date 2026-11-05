/* css.c - CSS string builder */
#include "../theme.h"
#include <cjson/cJSON.h>
#include <cwist/core/sstring/sstring.h>
#include <stdlib.h>
#include <string.h>

char *theme_build_css(bool dark_mode) {
    char l_theme[64], d_theme[64];
    get_special_themes(l_theme, d_theme);
    const char *resolved_name = dark_mode ? d_theme : l_theme;
    theme_color_t *t = theme_by_name(resolved_name);

    cJSON *theme = strcmp(resolved_name, "custom") == 0 ? theme_custom_object() : NULL;
    if (!theme) theme = build_theme_object(dark_mode ? "dark" : "light", t);
    cJSON *vars = cJSON_GetObjectItem(theme, "vars");
    cJSON *rules = cJSON_GetObjectItem(theme, "rules");

    cwist_sstring *css = cwist_sstring_create();
    cwist_sstring_append(css, ":root{");
    if (vars) {
        cJSON *v = vars->child;
        while (v) {
            if (cJSON_IsString(v) && v->string) {
                cwist_sstring_append(css, v->string);
                cwist_sstring_append(css, ":");
                cwist_sstring_append(css, v->valuestring);
                cwist_sstring_append(css, ";");
            }
            v = v->next;
        }
    }
    cwist_sstring_append(css, "}");

    if (rules) {
        int n = cJSON_GetArraySize(rules);
        for (int i = 0; i < n; i++) {
            cJSON *r = cJSON_GetArrayItem(rules, i);
            cJSON *sel = cJSON_GetObjectItem(r, "sel");
            cJSON *decls = cJSON_GetObjectItem(r, "decls");
            if (!sel || !sel->valuestring || !decls) continue;
            cwist_sstring_append(css, sel->valuestring);
            cwist_sstring_append(css, "{");
            cJSON *d = decls->child;
            while (d) {
                if (cJSON_IsString(d) && d->string) {
                    cwist_sstring_append(css, d->string);
                    cwist_sstring_append(css, ":");
                    cwist_sstring_append(css, d->valuestring);
                    cwist_sstring_append(css, ";");
                }
                d = d->next;
            }
            cwist_sstring_append(css, "}");
        }
    }

    cJSON_Delete(theme);
    char *out = strdup(css->data);
    cwist_sstring_destroy(css);
    return out;
}

char *theme_build_font_css(void) {
    cJSON *theme = build_theme_object("light", &light);
    cJSON *rules = cJSON_GetObjectItemCaseSensitive(theme, "rules");
    cwist_sstring *css = cwist_sstring_create();
    cJSON *rule = NULL;
    cJSON_ArrayForEach(rule, rules) {
        cJSON *selector = cJSON_GetObjectItemCaseSensitive(rule, "sel");
        cJSON *decls = cJSON_GetObjectItemCaseSensitive(rule, "decls");
        cwist_sstring *selected = cwist_sstring_create();
        for (cJSON *decl = decls ? decls->child : NULL; decl; decl = decl->next) {
            if (!cJSON_IsString(decl) || !decl->string) continue;
            if (strcmp(decl->string, "font-family") != 0 && strcmp(decl->string, "font-weight") != 0 &&
                strcmp(decl->string, "letter-spacing") != 0) continue;
            cwist_sstring_append(selected, decl->string);
            cwist_sstring_append(selected, ":");
            cwist_sstring_append(selected, decl->valuestring);
            cwist_sstring_append(selected, ";");
        }
        if (selected->size && cJSON_IsString(selector)) {
            cwist_sstring_append(css, selector->valuestring);
            cwist_sstring_append(css, "{");
            cwist_sstring_append(css, selected->data);
            cwist_sstring_append(css, "}");
        }
        cwist_sstring_destroy(selected);
    }
    cJSON_Delete(theme);
    char *out = strdup(css->data ? css->data : "");
    cwist_sstring_destroy(css);
    return out;
}

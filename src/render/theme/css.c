/* css.c - CSS string builder */
#include "../theme.h"
#include <cjson/cJSON.h>
#include <cwist/core/sstring/sstring.h>
#include <stdlib.h>
#include <string.h>

char *theme_build_css(bool dark_mode) {
    cJSON *theme = build_theme_object(dark_mode ? "dark" : "light", dark_mode ? &dark : &light);
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

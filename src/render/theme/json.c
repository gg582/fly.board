#include "../theme.h"
#include <cjson/cJSON.h>
#include <stdlib.h>
#include <string.h>

char *theme_build_json(bool dark_mode) {
    char l_theme[64], d_theme[64];
    get_special_themes(l_theme, d_theme);
    const char *resolved_name = dark_mode ? d_theme : l_theme;
    theme_color_t *t = theme_by_name(resolved_name);

    cJSON *theme = build_theme_object(dark_mode ? "dark" : "light", t);
    char *out = cJSON_PrintUnformatted(theme);
    cJSON_Delete(theme);
    return out;
}

char *theme_build_all_json(void) {
    cJSON *arr = cJSON_CreateArray();
    cJSON_AddItemToArray(arr, build_theme_object("light", theme_by_name("light")));
    cJSON_AddItemToArray(arr, build_theme_object("dark", theme_by_name("dark")));
    cJSON_AddItemToArray(arr, build_theme_object("ocean", &ocean));
    cJSON_AddItemToArray(arr, build_theme_object("forest", &forest));
    cJSON_AddItemToArray(arr, build_theme_object("sepia", &sepia));
    char *out = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return out;
}

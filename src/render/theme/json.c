#include "../theme.h"
#include <cjson/cJSON.h>
#include <stdlib.h>
#include <string.h>

char *theme_build_json(bool dark_mode) {
    cJSON *theme = build_theme_object(dark_mode ? "dark" : "light", dark_mode ? &dark : &light);
    char *out = cJSON_PrintUnformatted(theme);
    cJSON_Delete(theme);
    return out;
}

char *theme_build_all_json(void) {
    cJSON *arr = cJSON_CreateArray();
    cJSON_AddItemToArray(arr, build_theme_object("light", &light));
    cJSON_AddItemToArray(arr, build_theme_object("dark", &dark));
    cJSON_AddItemToArray(arr, build_theme_object("ocean", &ocean));
    cJSON_AddItemToArray(arr, build_theme_object("forest", &forest));
    cJSON_AddItemToArray(arr, build_theme_object("sepia", &sepia));
    char *out = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return out;
}

#ifndef FLYBOARD_THEME_H
#define FLYBOARD_THEME_H

#include <stdbool.h>
#include <cjson/cJSON.h>

typedef struct theme_color {
    bool dark_family;
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
    const char *glass_bg;
    const char *glass_border;
    const char *shadow_base;
} theme_color_t;

extern theme_color_t light;
extern theme_color_t dark;
extern theme_color_t ocean;
extern theme_color_t forest;
extern theme_color_t sepia;
extern theme_color_t paper;
extern theme_color_t glacier;
extern theme_color_t rose;
extern theme_color_t midnight;
extern theme_color_t plum;
extern theme_color_t ember;

theme_color_t *theme_by_name(const char *name);
theme_color_t *theme_builtin_by_name(const char *name);
void get_special_themes(char *out_light, char *out_dark);
bool theme_modes_load(const char *path);

cJSON *build_theme_object(const char *name, theme_color_t *t);
char *theme_build_json(bool dark_mode);
char *theme_build_all_json(void);
char *theme_build_css(bool dark_mode);
char *theme_build_font_css(void);

bool theme_custom_load(const char *path);
bool theme_custom_validate(const char *text, size_t length, char *error, size_t error_size);
bool theme_custom_save(const char *path, const char *text, size_t length, char *error, size_t error_size);
cJSON *theme_custom_object(void);
bool theme_custom_available(void);
bool theme_custom_is_dark(void);

#endif

#ifndef FLYBOARD_THEME_H
#define FLYBOARD_THEME_H

#include <stdbool.h>
#include <cjson/cJSON.h>

typedef struct theme_color {
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
} theme_color_t;

extern theme_color_t light;
extern theme_color_t dark;
extern theme_color_t ocean;
extern theme_color_t forest;
extern theme_color_t sepia;

cJSON *build_theme_object(const char *name, theme_color_t *t);
char *theme_build_json(bool dark_mode);
char *theme_build_all_json(void);
char *theme_build_css(bool dark_mode);

#endif

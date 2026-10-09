/* Test-local stand-ins for symbols render objects reference but that belong
 * to subsystems outside the scope of these unit tests.
 *
 * Used by: test_render_md.c, test_render_theme.c
 */
#include "config/config.h"
#include <stdio.h>
#include <stdbool.h>

blog_config_t g_config;
font_settings_t g_font_settings;

/* No real image probing in unit tests: report "unknown dimensions". */
bool get_image_dimensions(const char *path, int *w, int *h) {
    (void)path; (void)w; (void)h;
    return false;
}

/* Background-inversion helpers normally in src/config/config.c + image
 * pipeline; neutral answers keep theme rule generation deterministic. */
bool config_bg_invert_enabled(const char *target) { (void)target; return false; }
void config_resolve_bg(const char *light_img, const char *dark_img, const char *target,
                       bool dark_mode, const char **out_img, bool *out_invert) {
    (void)target;
    const char *img = dark_mode && dark_img && dark_img[0] ? dark_img : light_img;
    if (!img) img = "";
    if (out_img) *out_img = img;
    if (out_invert) *out_invert = false;
}
bool image_invert_variant(const char *src, const char *algo, char *out, size_t out_len) {
    (void)algo;
    if (!out || !out_len) return false;
    snprintf(out, out_len, "%s", src ? src : "");
    return false;
}

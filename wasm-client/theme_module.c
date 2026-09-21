/**
 * @file theme_module.c
 * @brief Emscripten entry for client-side theme CSS/JSON generation.
 *
 * Compiles the production theme pipeline (theme.c + rules.c + css.c +
 * json.c) into a browser WASM module against CWIST's libcwist_wasm.a.
 * The admin panel can preview theme changes live without round-tripping
 * the server, and untrusted theme JSON is parsed off the main process.
 *
 * Server-only inputs are stubbed: the module has no filesystem, so
 * image_invert_variant() returns NULL (no inverted background variants)
 * and config_resolve_bg() never sets the invert flag (equivalent to
 * production when bg_invert_color is empty).
 *
 * Input JSON may override: accent, roundness, bg_full_light, bg_full_dark,
 * use_special_modes.  Protocol for the native reference driver:
 *   stdin:  u8 mode (0 = css light, 1 = css dark, 2 = all-json), optional
 *           override JSON
 *   stdout: generated text
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cjson/cJSON.h>

#include "../src/config/config.h"
#include "../src/render/theme.h"

/* Server-only globals, stubbed for the sandbox. */
font_settings_t g_font_settings = {0};

blog_config_t g_config = {
    .accent = "#4f46e5",
    .roundness = 1.0f,
    .use_special_modes = "",
    .bg_full_light = "",
    .bg_full_dark = "",
    .bg_invert_algo = "luminance",
    .bg_invert_color = "",
};

void config_resolve_bg(const char *light_img, const char *dark_img, const char *target,
                       bool dark_mode, const char **out_img, bool *out_invert) {
    (void)target;
    bool has_light = light_img && light_img[0];
    bool has_dark = dark_img && dark_img[0];
    const char *img = dark_mode ? (has_dark ? dark_img : light_img)
                                : (has_light ? light_img : dark_img);
    if (!img || !img[0])
        img = NULL;
    /* Inverted variants are a server-side build artifact; the module has no
     * filesystem, so the invert flag stays false (production behaves the
     * same when bg_invert_color is empty). */
    if (out_img)
        *out_img = img;
    if (out_invert)
        *out_invert = false;
}

const char *image_invert_variant(const char *filename) {
    (void)filename;
    return NULL;
}

bool config_bg_invert_enabled(const char *target) {
    (void)target;
    /* g_config.bg_invert_color is empty, so production returns false too. */
    return false;
}

static void apply_overrides(const char *json) {
    if (!json || !json[0])
        return;
    cJSON *o = cJSON_Parse(json);
    if (!o)
        return;
    cJSON *v;
    if ((v = cJSON_GetObjectItem(o, "accent")) && cJSON_IsString(v))
        snprintf(g_config.accent, sizeof(g_config.accent), "%s", v->valuestring);
    if ((v = cJSON_GetObjectItem(o, "roundness")) && cJSON_IsNumber(v))
        g_config.roundness = (float)v->valuedouble;
    if ((v = cJSON_GetObjectItem(o, "bg_full_light")) && cJSON_IsString(v))
        snprintf(g_config.bg_full_light, sizeof(g_config.bg_full_light), "%s", v->valuestring);
    if ((v = cJSON_GetObjectItem(o, "bg_full_dark")) && cJSON_IsString(v))
        snprintf(g_config.bg_full_dark, sizeof(g_config.bg_full_dark), "%s", v->valuestring);
    if ((v = cJSON_GetObjectItem(o, "use_special_modes")) && cJSON_IsString(v))
        snprintf(g_config.use_special_modes, sizeof(g_config.use_special_modes), "%s",
                 v->valuestring);
    cJSON_Delete(o);
}

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
#define FB_KEEPALIVE EMSCRIPTEN_KEEPALIVE
#else
#define FB_KEEPALIVE
#endif

FB_KEEPALIVE
char *fb_theme_css(int dark_mode, const char *overrides_json) {
    apply_overrides(overrides_json);
    return theme_build_css(dark_mode != 0);
}

FB_KEEPALIVE
char *fb_theme_json(int dark_mode, const char *overrides_json) {
    apply_overrides(overrides_json);
    return theme_build_json(dark_mode != 0);
}

FB_KEEPALIVE
char *fb_theme_all_json(const char *overrides_json) {
    apply_overrides(overrides_json);
    return theme_build_all_json();
}

FB_KEEPALIVE
void fb_theme_free(void *ptr) {
    free(ptr);
}

#ifndef __EMSCRIPTEN__
int main(void) {
    int mode = fgetc(stdin);
    if (mode < 0 || mode > 2)
        return 2;
    size_t cap = 1 << 12, len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf)
        return 3;
    size_t n;
    while ((n = fread(buf + len, 1, cap - len - 1, stdin)) > 0) {
        len += n;
        if (len == cap - 1) {
            cap *= 2;
            char *nb = (char *)realloc(buf, cap);
            if (!nb) {
                free(buf);
                return 3;
            }
            buf = nb;
        }
    }
    buf[len] = '\0';
    char *out = mode == 2 ? fb_theme_all_json(buf) : fb_theme_css(mode, buf);
    free(buf);
    if (!out)
        return 1;
    fputs(out, stdout);
    free(out);
    return 0;
}
#endif

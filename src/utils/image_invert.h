#ifndef IMAGE_INVERT_H
#define IMAGE_INVERT_H

#include <stdbool.h>

/* Build perceptually inverted (luminance flip, hue preserved) variants of
 * the configured background images whose target is listed in
 * bg_invert_color.  Call once after the config is loaded. */
void image_invert_cache_build(void);

/* Returns the generated variant filename (a plain file under public/img/,
 * safe to serve as /assets/img/<name>) holding the inverted form of
 * filename, or NULL when no variant exists (target not listed, source
 * missing, or encoding unsupported). */
const char *image_invert_variant(const char *filename);

/* One-call resolution of the effective hero background for a mode: applies
 * config_resolve_bg, then swaps in the inverted variant when required.
 * Returns the display URL (or NULL), the filename of the image actually
 * shown (for contrast analysis), and whether the CSS-filter fallback is
 * needed.  Returned pointers stay valid until RESOLVE_RING further calls. */
const char *image_bg_resolve(const char *light_img, const char *dark_img, const char *target,
                             bool dark_mode, const char **out_shown_name, bool *out_css_filter);

/* --- Theme-switchable hero backgrounds ------------------------------------
 * Hero backgrounds are baked into the HTML per mode, but the theme toggle
 * only swaps CSS.  Like the toplevel wallpaper (a CSS variable), the hero
 * must follow the toggle instantly: the renderer therefore emits BOTH modes
 * (image URL, contrast-analysis styles, overlay, logo filter) as data
 * attributes and layout.js swaps them on toggle. */

#include <cwist/core/sstring/sstring.h>

typedef struct {
    const char *url;         /* display URL for this mode (NULL when unset) */
    const char *shown_name;  /* filename actually shown (analysis input) */
    bool css_filter;         /* true when the CSS-filter fallback applies */
    char shell_style[768];
    char text_style[256];
    char logo_filter[128];
    char overlay_style[256];
} hero_bg_mode_t;

/* Resolve and analyze both modes (index 0 = light, 1 = dark). */
void hero_bg_resolve_modes(const char *light_img, const char *dark_img, const char *target,
                           hero_bg_mode_t modes[2]);

/* Emit the hero wrapper div with both modes in data attributes, the hero-bg
 * img for the current mode, and the current mode's overlay.  Returns false
 * (and emits nothing) when the current mode has no background. */
bool hero_bg_append_open(cwist_sstring *b, const hero_bg_mode_t modes[2], bool dark);

#endif

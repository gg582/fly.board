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

#endif

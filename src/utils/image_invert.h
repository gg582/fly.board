#ifndef IMAGE_INVERT_H
#define IMAGE_INVERT_H

/* Build perceptually inverted (OKLCH lightness flip) variants of the
 * configured background images whose target is listed in bg_invert_color.
 * Call once after the config is loaded. */
void image_invert_cache_build(void);

/* Returns the generated variant filename (a plain file under public/img/,
 * safe to serve as /assets/img/<name>) holding the inverted form of
 * filename, or NULL when no variant exists (target not listed, source
 * missing, or encoding unsupported). */
const char *image_invert_variant(const char *filename);

#endif

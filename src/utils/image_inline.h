#ifndef IMAGE_INLINE_H
#define IMAGE_INLINE_H

#include <stdbool.h>
#include <stddef.h>

/* Build an in-memory cache of configured static images encoded as WebP
 * data URLs. Call once after config is loaded. */
void image_inline_cache_build(void);

const char *image_inline_home_bg(void);
const char *image_inline_boards_bg(void);
const char *image_inline_files_bg(void);
const char *image_inline_logo(void);
const char *image_inline_favicon(void);

/* Resolve a configured background image filename (home/boards/files, light
 * or dark variant) to its cached URL. Returns NULL when nothing is cached
 * for that filename. */
const char *image_inline_bg_url(const char *filename);

/* Generate (when missing/stale) a downscaled WebP variant of a public/img
 * basename capped at `width` pixels wide.  No-op when the variant already
 * exists or would not be smaller than the source. */
void image_inline_make_width_variant(const char *basename, int width);

/* Generate the 768w/1280w hero/background variants used by srcset.  Called
 * from image_inline_cache_build. */
void image_inline_build_responsive_bg(void);

/* Compose a srcset value for a hero/background URL from the on-disk width
 * variants.  Returns false when nothing responsive can be offered. */
bool image_inline_srcset(const char *url, char *out, size_t out_sz);

/* Intrinsic pixel dimensions of an /assets/img/ URL. */
bool image_file_dimensions_from_url(const char *url, int *out_w, int *out_h);

/* Compose a srcset for the hero logo from the asset handler's on-demand
 * ?w=&h= thumbs (128/256/512).  iw/ih are the intrinsic dimensions (0 when
 * unknown); the descriptor width accounts for the thumb's preserved aspect.
 * Returns false when nothing responsive can be offered. */
bool image_logo_srcset(const char *base_url, int iw, int ih, char *out, size_t out_sz);

#endif

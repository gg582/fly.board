#ifndef IMAGE_INLINE_H
#define IMAGE_INLINE_H

/* Build an in-memory cache of configured static images encoded as WebP
 * data URLs. Call once after config is loaded. */
void image_inline_cache_build(void);

const char *image_inline_home_bg(void);
const char *image_inline_boards_bg(void);
const char *image_inline_files_bg(void);
const char *image_inline_logo(void);
const char *image_inline_favicon(void);

#endif

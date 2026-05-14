#ifndef IMAGE_CONTRAST_H
#define IMAGE_CONTRAST_H

#include <stddef.h>

/**
 * Analyze a background image and compute accessible text styles based on
 * LCH perceptual color space contrast across the left, center, and right
 * regions where text is expected to appear.
 *
 * @param image_path      Filesystem path to the image (e.g. "public/img/hero.jpg")
 * @param image_url       Escaped URL for the CSS background-image rule
 * @param bg_style_out    Buffer to receive background CSS
 * @param bg_style_len    Size of bg_style_out
 * @param text_style_out  Buffer to receive color + text-shadow CSS
 * @param text_style_len  Size of text_style_out
 * @param logo_filter_out Buffer to receive filter CSS value for logo image
 * @param logo_filter_len Size of logo_filter_out
 * @return 0 on success, -1 on failure (safe defaults are written to outputs)
 */
int get_image_text_style(const char *image_path, const char *image_url,
                         char *bg_style_out, size_t bg_style_len,
                         char *text_style_out, size_t text_style_len,
                         char *logo_filter_out, size_t logo_filter_len);

#endif

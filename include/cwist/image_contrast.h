#ifndef IMAGE_CONTRAST_H
#define IMAGE_CONTRAST_H

#include <stddef.h>

/**
 * Analyze a background image and compute accessible text styles based on
 * LCH perceptual color space contrast across the left, center, and right
 * regions where text is expected to appear.
 *
 * @param image_path       Filesystem path to the image (e.g. "public/img/hero.jpg")
 * @param image_url        Escaped URL for the background image src
 * @param shell_style_out  Buffer to receive shell container CSS
 * @param shell_style_len  Size of shell_style_out
 * @param text_style_out   Buffer to receive color + text-shadow CSS
 * @param text_style_len   Size of text_style_out
 * @param logo_filter_out  Buffer to receive filter CSS value for logo image
 * @param logo_filter_len  Size of logo_filter_out
 * @param overlay_style_out Buffer to receive overlay CSS (linear-gradient) or empty
 * @param overlay_style_len Size of overlay_style_out
 * @return 0 on success, -1 on failure (safe defaults are written to outputs)
 */
int get_image_text_style(const char *image_path, const char *image_url,
                         char *shell_style_out, size_t shell_style_len,
                         char *text_style_out, size_t text_style_len,
                         char *logo_filter_out, size_t logo_filter_len,
                         char *overlay_style_out, size_t overlay_style_len);

#endif

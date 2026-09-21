/**
 * @file contrast_module.c
 * @brief Emscripten entry for client-side contrast sampling.
 *
 * Exports the production three-band lightness sampler from
 * image_contrast_core.h so the browser can run the hero text-contrast
 * heuristic on an image before upload (or for live preview) without a
 * server round-trip.  Parity with the server core is enforced by
 * wasm-client/test_contrast_diff.py (small epsilon for libm differences).
 *
 * Protocol for the native reference driver (stdin -> stdout):
 *   in:  be32 w, be32 h, then w*h*3 raw RGB bytes
 *   out: 3 x float64le L values, or nothing on failure (exit 1)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "image_contrast_core.h"

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
#define FB_KEEPALIVE EMSCRIPTEN_KEEPALIVE
#else
#define FB_KEEPALIVE
#endif

FB_KEEPALIVE
int fb_contrast_sample(const unsigned char *rgb, int w, int h, double L_out[3]) {
    if (!L_out)
        return -1;
    return fb_contrast_sample_rgb(rgb, w, h, L_out) ? 0 : -1;
}

#ifndef __EMSCRIPTEN__
int main(void) {
    unsigned char hdr[8];
    if (fread(hdr, 1, 8, stdin) != 8)
        return 1;
    int w = (int)((unsigned)hdr[0] << 24 | (unsigned)hdr[1] << 16 | (unsigned)hdr[2] << 8 | hdr[3]);
    int h = (int)((unsigned)hdr[4] << 24 | (unsigned)hdr[5] << 16 | (unsigned)hdr[6] << 8 | hdr[7]);
    if (w < 1 || h < 1 || (size_t)w * (size_t)h > (64u << 20))
        return 1;
    size_t need = (size_t)w * (size_t)h * 3;
    unsigned char *rgb = (unsigned char *)malloc(need);
    if (!rgb)
        return 1;
    if (fread(rgb, 1, need, stdin) != need) {
        free(rgb);
        return 1;
    }
    double L[3];
    int rc = fb_contrast_sample(rgb, w, h, L);
    free(rgb);
    if (rc != 0)
        return 1;
    fwrite(L, sizeof(double), 3, stdout);
    return 0;
}
#endif

/**
 * @file invert_module.c
 * @brief Emscripten entry for client-side image inversion.
 *
 * Exposes the production inversion algorithms from image_invert_core.h to
 * the browser so dark-mode toggles can invert images on the fly without the
 * server's pre-baked variants.  Byte-level parity with the server core is
 * enforced by wasm-client/test_invert_diff.py (allowed a 1-LSB tolerance for
 * libm differences between hosts).
 *
 * Protocol (stdin -> stdout, shared with the native reference driver):
 *   in:  u8 mode (0 = luminance, 1 = oklch), then raw RGBA bytes
 *   out: processed RGBA bytes
 */

#include <stdio.h>
#include <stdlib.h>

#include "image_invert_core.h"

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
#define FB_KEEPALIVE EMSCRIPTEN_KEEPALIVE
#else
#define FB_KEEPALIVE
#endif

FB_KEEPALIVE
unsigned char *fb_invert_rgba(const unsigned char *rgba, long pixel_count, int use_oklch) {
    if (!rgba || pixel_count <= 0)
        return NULL;
    unsigned char *out = (unsigned char *)malloc((size_t)pixel_count * 4);
    if (!out)
        return NULL;
    for (long i = 0; i < pixel_count; i++) {
        const unsigned char *p = rgba + i * 4;
        unsigned char *q = out + i * 4;
        float r = p[0] / 255.0f, g = p[1] / 255.0f, b = p[2] / 255.0f;
        if (use_oklch)
            invert_pixel_oklch(&r, &g, &b);
        else
            invert_pixel_luminance(&r, &g, &b);
        q[0] = (unsigned char)(r * 255.0f + 0.5f);
        q[1] = (unsigned char)(g * 255.0f + 0.5f);
        q[2] = (unsigned char)(b * 255.0f + 0.5f);
        q[3] = p[3];
    }
    return out;
}

FB_KEEPALIVE
void fb_invert_free(void *ptr) {
    free(ptr);
}

#ifndef __EMSCRIPTEN__
int main(void) {
    int mode = fgetc(stdin);
    if (mode != 0 && mode != 1)
        return 1;
    size_t cap = 1 << 16, len = 0;
    unsigned char *buf = (unsigned char *)malloc(cap);
    if (!buf)
        return 1;
    size_t n;
    while ((n = fread(buf + len, 1, cap - len, stdin)) > 0) {
        len += n;
        if (len == cap) {
            cap *= 2;
            unsigned char *nb = (unsigned char *)realloc(buf, cap);
            if (!nb) {
                free(buf);
                return 1;
            }
            buf = nb;
        }
    }
    if (len % 4 != 0) {
        free(buf);
        return 1;
    }
    unsigned char *out = fb_invert_rgba(buf, (long)(len / 4), mode);
    free(buf);
    if (!out)
        return 1;
    fwrite(out, 1, len, stdout);
    fb_invert_free(out);
    return 0;
}
#endif

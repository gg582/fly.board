/**
 * @file size_module.c
 * @brief Emscripten entry for in-memory image dimension probing.
 *
 * Wraps stb_image's stbi_info_from_memory so the browser can read the
 * dimensions of a picked file before upload.  The server probes from disk
 * (image_size.c); both share the same stb build, so behavior matches for
 * every format stb recognizes.
 *
 * Protocol for the native reference driver (stdin -> stdout):
 *   in:  image file bytes
 *   out: be32 w, be32 h on success; exit 1 otherwise
 */

#include <stdio.h>
#include <stdlib.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
#define FB_KEEPALIVE EMSCRIPTEN_KEEPALIVE
#else
#define FB_KEEPALIVE
#endif

FB_KEEPALIVE
int fb_image_size(const unsigned char *data, int len, int *w, int *h) {
    if (!data || len <= 0 || !w || !h)
        return -1;
    int comp = 0;
    return stbi_info_from_memory(data, len, w, h, &comp) ? 0 : -1;
}

#ifndef __EMSCRIPTEN__
int main(void) {
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
    int w = 0, h = 0;
    int rc = fb_image_size(buf, (int)len, &w, &h);
    free(buf);
    if (rc != 0)
        return 1;
    unsigned char out[8] = {(unsigned char)(w >> 24), (unsigned char)(w >> 16),
                            (unsigned char)(w >> 8), (unsigned char)w,
                            (unsigned char)(h >> 24), (unsigned char)(h >> 16),
                            (unsigned char)(h >> 8), (unsigned char)h};
    fwrite(out, 1, 8, stdout);
    return 0;
}
#endif

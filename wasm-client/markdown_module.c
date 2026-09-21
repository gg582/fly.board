/**
 * @file markdown_module.c
 * @brief Emscripten entry for the client-side markdown renderer.
 *
 * Compiles fly.board's render_md.c (the full production pipeline: math
 * protection, md4c, restore/rewrite passes) into a browser WASM module by
 * linking CWIST's libcwist_wasm.a for cwist_sstring.  The browser has no
 * filesystem, so get_image_dimensions is stubbed to "unknown" and the
 * width/height attribute injection is skipped; every other pass matches the
 * server byte for byte (enforced by wasm-client/test_md_diff.py).
 *
 * JS usage (see wasm-client/md_render.js):
 *   const render = await createMdRenderer(wasmBinaryOrUrl);
 *   const html = render("# hello");
 */

#include <emscripten.h>
#include <stdlib.h>

#include "render.h"

/**
 * Browser stand-in for the server's disk-backed image probe.  Without
 * filesystem access the dimensions are unknown; inject_img_attrs then only
 * adds loading/decoding attributes, which is the desired progressive
 * enhancement.
 */
bool get_image_dimensions(const char *path, int *w, int *h) {
    (void)path;
    (void)w;
    (void)h;
    return false;
}

EMSCRIPTEN_KEEPALIVE
char *fb_md_render(const char *md) {
    if (!md)
        return NULL;
    cwist_sstring *html = render_markdown_to_html(md);
    if (!html || !html->data) {
        if (html)
            cwist_sstring_destroy(html);
        return NULL;
    }
    char *out = (char *)malloc(strlen(html->data) + 1);
    if (out)
        strcpy(out, html->data);
    cwist_sstring_destroy(html);
    return out;
}

EMSCRIPTEN_KEEPALIVE
void fb_md_free(void *ptr) {
    free(ptr);
}

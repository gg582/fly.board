/**
 * @file md_native_ref.c
 * @brief Native reference driver for the markdown WASM differential test.
 *
 * Reads markdown from stdin, writes render_markdown_to_html output to
 * stdout.  Linked against CWIST's libcwist.a so the pipeline (sstring,
 * md4c) is the same code the server runs.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "render.h"

int main(void) {
    size_t cap = 1 << 16, len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf)
        return 1;
    size_t n;
    while ((n = fread(buf + len, 1, cap - len, stdin)) > 0) {
        len += n;
        if (len == cap) {
            cap *= 2;
            char *nb = (char *)realloc(buf, cap);
            if (!nb) {
                free(buf);
                return 1;
            }
            buf = nb;
        }
    }
    buf[len] = '\0';
    cwist_sstring *html = render_markdown_to_html(buf);
    free(buf);
    if (!html)
        return 2;
    fwrite(html->data, 1, strlen(html->data), stdout);
    cwist_sstring_destroy(html);
    return 0;
}

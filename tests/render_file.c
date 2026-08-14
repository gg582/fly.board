/* Render a markdown file to HTML on stdout, for manual inspection.
 * Usage: render_file <input.md> [output.html] */
#include "../src/render/render.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <input.md> [output.html]\n", argv[0]);
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("fopen"); return 1; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)size + 1);
    if (fread(buf, 1, (size_t)size, f) != (size_t)size) { perror("fread"); return 1; }
    buf[size] = '\0';
    fclose(f);

    cwist_sstring *html = render_markdown_to_html(buf);
    if (!html) { fprintf(stderr, "render failed\n"); return 1; }

    FILE *out = argc > 2 ? fopen(argv[2], "wb") : stdout;
    if (!out) { perror("fopen out"); return 1; }
    fputs(html->data, out);
    if (argc > 2) fclose(out);
    cwist_sstring_destroy(html);
    free(buf);
    return 0;
}

#include "../src/render/render.h"
#include <stdio.h>
#include <string.h>

static int failures = 0;

static void check(const char *name, const char *md, const char *must_contain, const char *must_not_contain) {
    cwist_sstring *html = render_markdown_to_html(md);
    if (!html) {
        printf("[FAIL] %s: render returned NULL\n", name);
        failures++;
        return;
    }
    int ok = 1;
    if (must_contain && !strstr(html->data, must_contain)) ok = 0;
    if (must_not_contain && strstr(html->data, must_not_contain)) ok = 0;
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) {
        printf("  input: %s\n  output: %s\n", md, html->data);
        if (must_contain) printf("  expected to contain: %s\n", must_contain);
        if (must_not_contain) printf("  expected NOT to contain: %s\n", must_not_contain);
        failures++;
    }
    cwist_sstring_destroy(html);
}

int main(void) {
    check("video tag survives",
          "<video src=\"/file/download/42\" poster=\"/file/preview/42\" controls playsinline style=\"max-width:100%;display:block;\"></video>\n",
          "<video data-tasfa-download=\"/file/download/42\"", NULL);

    check("gif-style video with onclick survives",
          "<video src=\"/file/download/43\" poster=\"/file/preview/43?poster=1\" autoplay loop muted playsinline onclick=\"this.paused ? this.play() : this.pause();\" style=\"max-width:100%;display:block;cursor:pointer;\"></video>\n",
          "autoplay", NULL);

    check("audio tag survives",
          "<audio src=\"/file/download/7\" controls style=\"max-width:100%;display:block;\"></audio>\n",
          "<audio src=\"/file/download/7\" controls", NULL);

    check("event handlers stripped",
          "<video src=\"/file/download/1\" onerror=\"alert(1)\"></video>\n",
          "<video", "onerror");

    check("other raw html still blocked",
          "<script>alert(1)</script>\n",
          NULL, "<script>");

    check("video inside code fence stays literal",
          "```\n<video src=\"/file/download/9\"></video>\n```\n",
          NULL, "<video data-tasfa-download");

    check("video inside inline code stays literal",
          "`<video src=\"/file/download/9\"></video>`\n",
          NULL, "<video data-tasfa-download");

    check("plain markdown still works",
          "# Hello\n\nsome *text* here\n",
          "<em>text</em>", NULL);

    check("math still works",
          "\\[x^2\\]\n",
          "math-block", NULL);

    if (failures == 0) printf("ALL TESTS PASSED\n");
    return failures ? 1 : 0;
}

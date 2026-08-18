#include "../src/render/render.h"
#include <stdio.h>
#include <string.h>

static int failures = 0;

static void check(const char *name, const char *md,
                  const char *must_contain, const char *must_not_contain) {
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
        printf("  input:  %s\n  output: %s\n", md, html->data);
        if (must_contain)     printf("  expected to contain:     %s\n", must_contain);
        if (must_not_contain) printf("  expected NOT to contain: %s\n", must_not_contain);
        failures++;
    }
    cwist_sstring_destroy(html);
}

int main(void) {
    /* --- basic acceptance --- */
    check("youtube embed survives",
          "<iframe width=\"560\" height=\"315\""
          " src=\"https://www.youtube.com/embed/dQw4w9WgXcQ\""
          " title=\"Never Gonna Give You Up\""
          " frameborder=\"0\" allowfullscreen></iframe>\n",
          "<iframe src=\"https://www.youtube.com/embed/dQw4w9WgXcQ\"",
          NULL);

    check("sandbox is injected",
          "<iframe src=\"https://www.youtube.com/embed/dQw4w9WgXcQ\"></iframe>\n",
          "sandbox=\"allow-scripts allow-same-origin allow-presentation\"",
          NULL);

    check("http src is upgraded to https",
          "<iframe src=\"http://www.youtube.com/embed/dQw4w9WgXcQ\"></iframe>\n",
          "src=\"https://www.youtube.com/embed/",
          "src=\"http://");

    check("youtube-nocookie domain accepted",
          "<iframe src=\"https://www.youtube-nocookie.com/embed/abc123\"></iframe>\n",
          "src=\"https://www.youtube-nocookie.com/embed/abc123\"",
          NULL);

    check("width and height preserved",
          "<iframe width=\"800\" height=\"450\""
          " src=\"https://www.youtube.com/embed/dQw4w9WgXcQ\"></iframe>\n",
          "width=\"800\"",
          NULL);

    check("title preserved",
          "<iframe title=\"My Video\""
          " src=\"https://www.youtube.com/embed/dQw4w9WgXcQ\"></iframe>\n",
          "title=\"My Video\"",
          NULL);

    /* --- security: rejected sources --- */
    check("non-youtube src is dropped entirely",
          "<iframe src=\"https://evil.example.com/xss\"></iframe>\n",
          NULL, "<iframe");

    check("javascript: src is dropped",
          "<iframe src=\"javascript:alert(1)\"></iframe>\n",
          NULL, "<iframe");

    check("data: src is dropped",
          "<iframe src=\"data:text/html,<script>alert(1)</script>\"></iframe>\n",
          NULL, "<iframe");

    check("missing src is dropped",
          "<iframe width=\"640\" height=\"360\"></iframe>\n",
          NULL, "<iframe");

    check("youtube watch URL (not embed) is dropped",
          "<iframe src=\"https://www.youtube.com/watch?v=dQw4w9WgXcQ\"></iframe>\n",
          NULL, "<iframe");

    /* --- security: attribute stripping --- */
    check("onload handler stripped",
          "<iframe src=\"https://www.youtube.com/embed/abc\""
          " onload=\"alert(1)\"></iframe>\n",
          "<iframe", "onload");

    check("onerror handler stripped",
          "<iframe src=\"https://www.youtube.com/embed/abc\""
          " onerror=\"alert(1)\"></iframe>\n",
          "<iframe", "onerror");

    check("srcdoc stripped",
          "<iframe src=\"https://www.youtube.com/embed/abc\""
          " srcdoc=\"<script>alert(1)</script>\"></iframe>\n",
          "<iframe", "srcdoc");

    check("user-supplied sandbox overridden",
          "<iframe src=\"https://www.youtube.com/embed/abc\""
          " sandbox=\"allow-all\"></iframe>\n",
          "sandbox=\"allow-scripts allow-same-origin allow-presentation\"",
          "allow-all");

    check("unknown attribute stripped",
          "<iframe src=\"https://www.youtube.com/embed/abc\""
          " data-evil=\"x\"></iframe>\n",
          "<iframe", "data-evil");

    /* --- isolation from other features --- */
    check("iframe inside code fence stays literal",
          "```\n<iframe src=\"https://www.youtube.com/embed/abc\"></iframe>\n```\n",
          NULL, "sandbox=");

    check("iframe inside inline code stays literal",
          "`<iframe src=\"https://www.youtube.com/embed/abc\"></iframe>`\n",
          NULL, "sandbox=");

    check("plain markdown unaffected",
          "# Title\n\nsome **bold** text\n",
          "<strong>bold</strong>", NULL);

    check("other raw HTML still blocked",
          "<script>alert(1)</script>\n",
          NULL, "<script>");

    if (failures == 0) printf("ALL TESTS PASSED\n");
    return failures ? 1 : 0;
}

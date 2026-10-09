/* Unit test: src/render/render_md.c markdown rendering smoke tests.
 *
 * Covers: ATX headers, fenced code blocks, inline code, links, emphasis,
 * lists, tables, blockquotes, and XSS-ish raw HTML/script escaping.
 *
 * Build:
 *   gcc -O1 -Iinclude -Isrc -I/home/yjlee/cwist/include -I/home/yjlee/cwist/lib \
 *       -Ithird_party/md4c/src -I/home/yjlee/cwist/lib/cjson \
 *       tests/test_render_md.c tests/stubs/render_test_stubs.c \
 *       src/render/render_md.o third_party/md4c/build/md4c.o \
 *       third_party/md4c/build/md4c-html.o third_party/md4c/build/entity.o \
 *       /home/yjlee/cwist/libcwist.a /home/yjlee/cwist/lib/libttak/lib/libttak.a /home/yjlee/cwist/lib/cjson/libcjson.a \
 *       -lpthread -lm -ldl -o /tmp/test_render_md
 * Run: /tmp/test_render_md
 */
#include "render/render.h"
#include <cwist/core/sstring/sstring.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;
#define CHECK_SUBSTR(hay, needle, name) do { \
    if ((hay) && strstr((hay), (needle))) printf("PASS: %s\n", name); \
    else { printf("FAIL: %s (line %d, missing '%s')\n", name, __LINE__, needle); failures++; } \
} while (0)
#define CHECK_NSUBSTR(hay, needle, name) do { \
    if ((hay) && !strstr((hay), (needle))) printf("PASS: %s\n", name); \
    else { printf("FAIL: %s (line %d, found '%s')\n", name, __LINE__, needle); failures++; } \
} while (0)

static void expect_md(const char *md, const char *needle, const char *name) {
    cwist_sstring *html = render_markdown_to_html(md);
    CHECK_SUBSTR(html ? html->data : NULL, needle, name);
    if (html) cwist_sstring_destroy(html);
}

int main(void) {
    expect_md("# Title\n", "<h1", "ATX h1 emitted");
    expect_md("# Title\n", "Title", "header text present");
    expect_md("## Sub\n", "<h2", "ATX h2 emitted");
    expect_md("### Deep\n", "<h3", "ATX h3 emitted");

    expect_md("```c\nint x = 1;\n```\n", "<code", "fenced code block emitted");
    expect_md("```c\nint x = 1;\n```\n", "int x = 1;", "code content preserved");
    expect_md("some `inline()` code\n", "<code", "inline code emitted");
    expect_md("some `inline()` code\n", "inline()", "inline code content preserved");

    expect_md("[click](https://example.com)\n", "<a href=", "link emitted");
    expect_md("[click](https://example.com)\n", "https://example.com", "link target present");
    expect_md("**bold** and *em*\n", "<strong>", "bold rendered");
    expect_md("**bold** and *em*\n", "<em>", "emphasis rendered");

    expect_md("- one\n- two\n", "<ul>", "unordered list rendered");
    expect_md("- one\n- two\n", "two", "list item text present");
    expect_md("1. first\n2. second\n", "<ol>", "ordered list rendered");

    expect_md("> quoted text\n", "<blockquote>", "blockquote rendered");
    expect_md("> quoted text\n", "quoted text", "blockquote text present");

    {
        cwist_sstring *html = render_markdown_to_html("| a | b |\n|---|---|\n| 1 | 2 |\n");
        CHECK_SUBSTR(html ? html->data : NULL, "<table", "table rendered");
        CHECK_SUBSTR(html ? html->data : NULL, "<td", "table cell rendered");
        if (html) cwist_sstring_destroy(html);
    }

    /* script injection must not pass through as live markup */
    {
        cwist_sstring *html = render_markdown_to_html("<script>alert(1)</script>\n");
        CHECK_NSUBSTR(html ? html->data : NULL, "<script>", "raw script tag escaped");
        if (html) cwist_sstring_destroy(html);
    }

    /* tiny inputs should not crash (empty string crashes in src, see
     * tests/COVERAGE-GAPS.md) */
    {
        cwist_sstring *html = render_markdown_to_html("\n");
        if (!html) { printf("FAIL: whitespace-only markdown handled (line %d)\n", __LINE__); failures++; } else printf("PASS: whitespace-only markdown handled\n");
        if (html) cwist_sstring_destroy(html);
        html = render_markdown_to_html("x");
        CHECK_SUBSTR(html ? html->data : NULL, "x", "single char rendered");
        if (html) cwist_sstring_destroy(html);
    }

    printf("%s\n", failures == 0 ? "ALL PASS" : "SOME FAILED");
    return failures == 0 ? 0 : 1;
}

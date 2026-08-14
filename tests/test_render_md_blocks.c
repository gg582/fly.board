/* Regression tests for block-level math/TikZ handling in render_md.c.
 *
 * The renderer swaps math and TikZ fragments for plain-text placeholders
 * before md4c runs, then restores real elements afterwards. A placeholder
 * that ends up glued to a neighboring text line makes md4c wrap both in one
 * <p>, so the restored <div>/display span lands inside that paragraph and
 * swallows the surrounding text. These tests pin the block/inline split
 * plus math and TikZ inside tables. */
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
    check("tikz fence followed by text does not swallow the text",
          "```tikz\n\\draw (0,0);\n```\nafter text\n",
          "<p>after text</p>", "<p><div");

    check("tikz fence directly after a paragraph breaks out of it",
          "intro text\n```tikz\n\\draw (0,0);\n```\n",
          "<p>intro text</p>\n<div class=\"tikz-block\">", NULL);

    check("TikZ-heavy math post keeps every fenced diagram as a render target",
          "# lattice\n\n$$\\frac{3n^2-n}{2}$$\n\n```tikz\n\\begin{tikzpicture}[scale=.7]\n\\def\\h{0.8660254}\n\\foreach \\j in {0,...,4} { \\foreach \\i in {0,...,4} { \\fill (\\i,\\j) circle (1pt); } }\n\\end{tikzpicture}\n```\n\n$$\\Delta^2P_q(n)=q$$\n\n```tikz\n\\draw[thick] (0,0) -- (4,0) -- (2,3) -- cycle;\n\\node[above] at (2,3) {$n=4$};\n```\n\nend\n",
          "<div class=\"tikz-block\">", "<pre><code class=\"language-tikz\">");

    check("tikz environment glued to a following line stays a block",
          "\\begin{tikzpicture} \\draw (0,0); \\end{tikzpicture}\nglued line\n",
          "</div>\n<p>glued line</p>", NULL);

    check("single-line tikz environment inside a table cell stays inline",
          "| a | b |\n| - | - |\n| \\begin{tikzpicture} \\draw (0,0); \\end{tikzpicture} | cell |\n",
          "<td><div class=\"tikz-block\">", NULL);

    check("display math between text lines gets its own paragraph",
          "before\n$$\nx\n$$\nafter\n",
          "<p>before</p>\n<p><span class=\"math-block\">x</span></p>\n<p>after</p>", NULL);

    check("display math sharing a line with prose stays inline",
          "inline $$y$$ more text\n",
          "<p>inline <span class=\"math-block\">y</span> more text</p>", NULL);

    check("display math with blank lines and markdown-looking lines",
          "$$\nS\n=\n1\n+\n2\n$$\n",
          "<span class=\"math-block\">S\n=\n1\n+\n2</span>", "<h");

    check("inline math containing a pipe works inside a table cell",
          "| a | b |\n| - | - |\n| $|x|$ | 1 |\n",
          "<td><span class=\"math-inline\">|x|</span></td>", NULL);

    check("display math works inside a table cell",
          "| a | b |\n| - | - |\n| $$\\frac{a}{b}$$ | 1 |\n",
          "<td><span class=\"math-block\">\\frac{a}{b}</span></td>", NULL);

    check("double-backtick code span keeps dollars literal",
          "`` `$x$` `` code\n",
          "<code>`$x$`</code>", "math-inline");

    check("escaped dollars stay literal",
          "costs \\$5 and \\$10\n",
          "costs $5 and $10", "math-inline");

    check("\\[...\\] on its own lines becomes a block paragraph",
          "text\n\\[\ny\n\\]\nnext\n",
          "<p><span class=\"math-block\">y</span></p>\n<p>next</p>", NULL);

    if (failures == 0) printf("ALL TESTS PASSED\n");
    return failures ? 1 : 0;
}

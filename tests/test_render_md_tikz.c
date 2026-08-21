/* TikZ diagram regression tests: exponential/log plots, a polygon, and a
 * sphere, exercised in plain flow, inside tables, and wedged between LaTeX
 * math. TikZJax renders diagrams client-side from <div class="tikz-block">,
 * so the contract is: every diagram survives as exactly one such div, its
 * source is HTML-escaped but otherwise intact, and surrounding content is
 * not swallowed. */
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

static void check_count(const char *name, const char *md, const char *needle, int expected) {
    cwist_sstring *html = render_markdown_to_html(md);
    if (!html) {
        printf("[FAIL] %s: render returned NULL\n", name);
        failures++;
        return;
    }
    int n = 0;
    const char *p = html->data;
    size_t nl = strlen(needle);
    while ((p = strstr(p, needle)) != NULL) { n++; p += nl; }
    printf("[%s] %s\n", n == expected ? "PASS" : "FAIL", name);
    if (n != expected) {
        printf("  expected %d occurrences of: %s\n  got %d\n  output: %s\n",
               expected, needle, n, html->data);
        failures++;
    }
    cwist_sstring_destroy(html);
}

static const char *TIKZ_EXP =
    "\\begin{tikzpicture}[scale=1.1]\n"
    "\\draw[->] (-2,0) -- (3,0) node[right] {$x$};\n"
    "\\draw[->] (0,-1) -- (0,4) node[above] {$y$};\n"
    "\\draw[domain=-2:1.8,smooth,thick,blue] plot (\\x,{exp(\\x)});\n"
    "\\node[blue] at (1.5,3.5) {$y=e^x$};\n"
    "\\end{tikzpicture}\n";

static const char *TIKZ_LOG =
    "\\begin{tikzpicture}[scale=1.1]\n"
    "\\draw[->] (-0.5,0) -- (4,0) node[right] {$x$};\n"
    "\\draw[->] (0,-3) -- (0,2) node[above] {$y$};\n"
    "\\draw[domain=0.05:4,smooth,thick,red] plot (\\x,{ln(\\x)});\n"
    "\\node[red] at (3,1.2) {$y=\\ln x$};\n"
    "\\end{tikzpicture}\n";

static const char *TIKZ_POLYGON =
    "\\begin{tikzpicture}\n"
    "\\draw[thick] (0:2) -- (72:2) -- (144:2) -- (216:2) -- (288:2) -- cycle;\n"
    "\\foreach \\a in {0,72,144,216,288} \\fill (\\a:2) circle (2pt);\n"
    "\\end{tikzpicture}\n";

static const char *TIKZ_SPHERE =
    "\\begin{tikzpicture}\n"
    "\\shade[ball color=blue!20] (0,0) circle (2);\n"
    "\\draw (0,0) circle (2);\n"
    "\\draw (-2,0) arc (180:360:2 and 0.6);\n"
    "\\draw[dashed] (2,0) arc (0:180:2 and 0.6);\n"
    "\\fill (0,0) circle (1pt) node[below] {$O$};\n"
    "\\end{tikzpicture}\n";

int main(void) {
    char doc[4096];

    /* 1. Plain flow: each diagram in its own fenced block. */
    snprintf(doc, sizeof(doc), "## Exponential\n\n```tikz\n%s```\n\nThe graph of $y=e^x$.\n", TIKZ_EXP);
    check("fenced exponential plot keeps its source", doc,
          "plot (\\x,{exp(\\x)})", "<pre><code");
    check("fenced exponential plot becomes a tikz block", doc,
          "<div class=\"tikz-block\">", "language-tikz");

    snprintf(doc, sizeof(doc), "## Logarithm\n\n```tikz\n%s```\n\nThe graph of $y=\\ln x$.\n", TIKZ_LOG);
    check("fenced log plot keeps its source", doc,
          "plot (\\x,{ln(\\x)})", "<pre><code");

    snprintf(doc, sizeof(doc), "## Pentagon\n\n```tikz\n%s```\n\nA regular pentagon.\n", TIKZ_POLYGON);
    check("fenced polygon keeps its source", doc,
          "(72:2) -- (144:2)", "<pre><code");

    snprintf(doc, sizeof(doc), "## Sphere\n\n```tikz\n%s```\n\nA sphere with equator.\n", TIKZ_SPHERE);
    check("fenced sphere keeps its source", doc,
          "ball color=blue!20", "<pre><code");

    /* 2. All four diagrams in one post: four render targets, none degraded
     *    to a code block. */
    snprintf(doc, sizeof(doc),
             "# Graphs\n\n```tikz\n%s```\n\n```tikz\n%s```\n\n```tikz\n%s```\n\n```tikz\n%s```\n\ndone\n",
             TIKZ_EXP, TIKZ_LOG, TIKZ_POLYGON, TIKZ_SPHERE);
    check_count("four fenced diagrams in one post all survive", doc,
                "<div class=\"tikz-block\">", 4);
    check("four fenced diagrams leave no tikz code blocks", doc,
          "<p>done</p>", "language-tikz");

    /* 3. Inside a table: single-line environments stay inline in the cell. */
    snprintf(doc, sizeof(doc),
             "| diagram | name |\n"
             "| - | - |\n"
             "| \\begin{tikzpicture} \\draw[thick] (0:2) -- (72:2) -- (144:2) -- (216:2) -- (288:2) -- cycle; \\end{tikzpicture} | pentagon |\n"
             "| \\begin{tikzpicture} \\shade[ball color=blue!20] (0,0) circle (2); \\draw (0,0) circle (2); \\end{tikzpicture} | sphere |\n");
    check_count("two diagrams inside table cells both survive", doc,
                "<div class=\"tikz-block\">", 2);
    check("table diagram stays inside the cell", doc,
          "<td><div class=\"tikz-block\">", NULL);
    check("table sphere keeps its source", doc,
          "ball color=blue!20", NULL);

    /* 4. Between LaTeX display math, with blank lines. */
    snprintf(doc, sizeof(doc),
             "Derivative of the exponential:\n\n$$\\frac{d}{dx}e^x = e^x$$\n\n```tikz\n%s```\n\nIntegral of the logarithm:\n\n$$\\int \\ln x\\,dx = x\\ln x - x + C$$\n",
             TIKZ_EXP);
    check_count("diagram between display math survives", doc,
                "<div class=\"tikz-block\">", 1);
    check("display math above the diagram survives", doc,
          "<span class=\"math-block\">\\frac{d}{dx}e^x = e^x</span>", NULL);
    check("display math below the diagram survives", doc,
          "\\int \\ln x\\,dx = x\\ln x - x + C", NULL);

    /* 5. Between LaTeX display math, tightly packed without blank lines. */
    snprintf(doc, sizeof(doc),
             "$$\ne^x\n$$\n```tikz\n%s```\n$$\n\\ln x\n$$\nend.\n",
             TIKZ_LOG);
    check_count("tightly packed diagram between display math survives", doc,
                "<div class=\"tikz-block\">", 1);
    check_count("tightly packed display math blocks both survive", doc,
                "<span class=\"math-block\">", 2);
    check("text after a tight diagram is not swallowed", doc,
          "<p>end.</p>", NULL);

    /* 6. Delimiter-free environment (no fence) at block position. */
    snprintf(doc, sizeof(doc), "%sfollowing paragraph.\n", TIKZ_POLYGON);
    check("unfenced environment becomes a tikz block", doc,
          "<div class=\"tikz-block\">", NULL);
    check("unfenced environment does not swallow following text", doc,
          "<p>following paragraph.</p>", NULL);

    /* 7. Inline environment wedged between inline math on one line. */
    snprintf(doc, sizeof(doc),
             "compare $e^x$ \\begin{tikzpicture} \\draw (0,0) -- (1,1); \\end{tikzpicture} with $\\ln x$ here\n");
    check("inline diagram between inline math survives", doc,
          "<div class=\"tikz-block\">", NULL);
    check_count("inline math around the diagram survives", doc,
                "<span class=\"math-inline\">", 2);

    if (failures == 0) printf("ALL TESTS PASSED\n");
    return failures ? 1 : 0;
}

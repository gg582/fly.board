/* Deterministic RSS-growth regression test.
 *
 * Calls render_markdown_to_html() (the hot path shared by post/comment/file
 * rendering) in two batches: a short warm-up batch and a long batch two
 * orders of magnitude larger. Both batches cycle through the same fixed set
 * of inputs, so any RSS growth after the allocator has settled (thread
 * arenas, md4c internal buffers, etc. all warmed up in the first batch)
 * comes from memory the render path failed to free, not from legitimate
 * per-call state.
 *
 * getrusage(RU_MAXRSS) is a running peak, so comparing it after the warm-up
 * batch against after the long batch approximates "does RSS keep climbing
 * as more requests are served", which is exactly what a leak looks like in
 * production without needing to run the server for hours.
 *
 * Usage: test_leak_loop [warmup_iters] [main_iters] [max_kb_per_1k_calls]
 * Exit status: 0 if growth-per-call stays under the threshold, 1 otherwise.
 */
#include "../src/render/render.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>

/* A handful of representative markdown inputs, covering the branchy paths
 * (tables, fenced code, math, tikz) so the loop exercises more than one
 * cheap straight-line path. */
static const char *SAMPLES[] = {
    "# Title\n\nSome **bold** text with a [link](https://example.com).\n",
    "| a | b |\n| - | - |\n| 1 | 2 |\n| 3 | 4 |\n",
    "```c\nint main(void) { return 0; }\n```\n",
    "$$\\frac{3n^2-n}{2}$$\n\nInline $x^2$ math too.\n",
    "```tikz\n\\draw (0,0) -- (1,1);\n```\nafter text\n",
    "> a blockquote\n> spanning two lines\n\n- one\n- two\n- three\n",
};
#define N_SAMPLES (sizeof(SAMPLES) / sizeof(SAMPLES[0]))

static void run_batch(long iters) {
    for (long i = 0; i < iters; i++) {
        cwist_sstring *html = render_markdown_to_html(SAMPLES[i % N_SAMPLES]);
        if (html) {
            cwist_sstring_destroy(html);
        }
    }
}

static long peak_rss_kb(void) {
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) != 0) {
        perror("getrusage");
        exit(2);
    }
    return ru.ru_maxrss; /* KB on Linux */
}

int main(int argc, char **argv) {
    long warmup_iters = argc > 1 ? atol(argv[1]) : 2000;
    long main_iters = argc > 2 ? atol(argv[2]) : 200000;
    long max_kb_per_1k = argc > 3 ? atol(argv[3]) : 16; /* tolerance */

    run_batch(warmup_iters);
    long baseline_kb = peak_rss_kb();

    run_batch(main_iters);
    long final_kb = peak_rss_kb();

    long growth_kb = final_kb - baseline_kb;
    double per_1k_calls = (double)growth_kb / ((double)main_iters / 1000.0);

    printf("test_leak_loop: warmup=%ld main=%ld baseline_rss=%ldKB final_rss=%ldKB "
           "growth=%ldKB (%.3f KB / 1k calls, threshold %ld KB / 1k calls)\n",
           warmup_iters, main_iters, baseline_kb, final_kb, growth_kb, per_1k_calls,
           max_kb_per_1k);

    if (growth_kb < 0) {
        /* ru_maxrss never decreases; a negative delta means something is
         * wrong with the measurement itself, not a pass. */
        fprintf(stderr, "test_leak_loop: FAIL - negative growth, measurement is broken\n");
        return 1;
    }

    if (per_1k_calls > (double)max_kb_per_1k) {
        fprintf(stderr,
                "test_leak_loop: FAIL - RSS grew %.3f KB per 1k calls "
                "(threshold %ld KB per 1k calls); render_markdown_to_html "
                "looks like it is leaking\n",
                per_1k_calls, max_kb_per_1k);
        return 1;
    }

    printf("test_leak_loop: PASS\n");
    return 0;
}

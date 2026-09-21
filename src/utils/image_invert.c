#define _POSIX_C_SOURCE 200809L
#include "image_invert.h"
#include "image_invert_core.h"
#include "image_inline.h"
#include "config/config.h"
#include <cwist/core/log.h>
#include "stb_image.h"
#ifdef HAVE_WEBP
#include <webp/encode.h>
#include <webp/decode.h>
#endif

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* Mechanical CSS inversion (invert(1)) flips hue as well as lightness, so
 * photographs and gradients come out with strongly distorted colors.  This
 * module precomputes inverted variants server-side with one of two
 * hue-preserving algorithms, selected by bg_invert_algo:
 *   - "luminv" (default): linear-luminance flip (Y -> 1 - Y) preserving
 *     chromaticity.  Keeps line art readable (a light-gray line on white
 *     becomes a light line on black); out-of-gamut brights desaturate
 *     toward white on demand, so nothing turns neon.
 *   - "oklch": perceptual lightness flip (L -> 1 - L) keeping chroma and
 *     hue, with chroma damping and gamut clamping.  Smoother on photos and
 *     gradients, but crushes contrast on low-contrast line art. */

#define MAX_VARIANTS 10

typedef struct {
    char orig[256];
    char variant[300];
} invert_entry_t;

static invert_entry_t g_variants[MAX_VARIANTS];
static size_t g_variant_count;

static bool invert_use_oklch(void) {
    return strcmp(g_config.bg_invert_algo, "oklch") == 0;
}

#ifdef HAVE_WEBP
static unsigned char *load_rgba(const char *path, int *w, int *h) {
    int channels = 0;
    unsigned char *data = stbi_load(path, w, h, &channels, 4);
    if (data) return data;

    /* stb cannot read webp; decode through libwebp. */
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    struct stat st;
    if (fstat(fileno(f), &st) != 0 || st.st_size <= 0) { fclose(f); return NULL; }
    unsigned char *buf = (unsigned char *)malloc((size_t)st.st_size);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, (size_t)st.st_size, f);
    fclose(f);
    if (n != (size_t)st.st_size) { free(buf); return NULL; }
    unsigned char *rgba = WebPDecodeRGBA(buf, n, w, h);
    free(buf);
    return rgba;
}

static void build_variant(const char *orig_name) {
    if (!orig_name || !orig_name[0] || g_variant_count >= MAX_VARIANTS) return;
    for (size_t i = 0; i < g_variant_count; i++) {
        if (strcmp(g_variants[i].orig, orig_name) == 0) return;
    }

    /* Prefix doubles as an algorithm/encoder version tag: changing the
     * algorithm or the encoder settings changes the filename, so variants
     * built by older code are never picked up by accident. */
    invert_entry_t *entry = &g_variants[g_variant_count];
    int n = snprintf(entry->variant, sizeof(entry->variant), "%s-%s.webp",
                     invert_use_oklch() ? "oklch-inv2" : "luminv4", orig_name);
    if (n < 0 || n >= (int)sizeof(entry->variant)) return;

    char src_path[600], dst_path[600];
    snprintf(src_path, sizeof(src_path), "public/img/%s", orig_name);
    snprintf(dst_path, sizeof(dst_path), "public/img/%s", entry->variant);

    /* Reuse a previously generated variant while the source is unchanged. */
    struct stat sst, dst;
    if (stat(src_path, &sst) != 0) return;
    if (stat(dst_path, &dst) == 0 && dst.st_mtime >= sst.st_mtime) {
        snprintf(entry->orig, sizeof(entry->orig), "%s", orig_name);
        g_variant_count++;
        image_inline_make_width_variant(entry->variant, 768);
        image_inline_make_width_variant(entry->variant, 1280);
        return;
    }

    int w = 0, h = 0;
    unsigned char *pixels = load_rgba(src_path, &w, &h);
    if (!pixels) {
        CWIST_LOG_WARN("bg_invert_color: cannot decode %s, inversion disabled for it", src_path);
        return;
    }

    bool use_oklch = invert_use_oklch();
    for (long i = 0; i < (long)w * h; i++) {
        unsigned char *px = pixels + i * 4;
        float r = px[0] / 255.0f, g = px[1] / 255.0f, b = px[2] / 255.0f;
        if (use_oklch) {
            invert_pixel_oklch(&r, &g, &b);
        } else {
            invert_pixel_luminance(&r, &g, &b);
            /* Black-floor lift: linear-luminance inversion maps near-white
             * paper (e.g. 253/255) to a few percent of linear light, which
             * the sRGB gamma expands into a visible ~10% gray — with every
             * speck of source noise spread across it.  Subtract a small
             * floor (chromaticity preserved) so near-white sources land on
             * true black while darker content keeps its gradient. */
            float Y = 0.2126f * r + 0.7152f * g + 0.0722f * b;
            const float black_floor = 0.16f;
            if (Y > 0.0f && Y < black_floor) {
                float Y2 = (Y - black_floor) / (1.0f - black_floor);
                if (Y2 < 0.0f) Y2 = 0.0f;
                float s = Y2 / Y;
                r *= s; g *= s; b *= s;
            }
        }
        px[0] = (unsigned char)(r * 255.0f + 0.5f);
        px[1] = (unsigned char)(g * 255.0f + 0.5f);
        px[2] = (unsigned char)(b * 255.0f + 0.5f);
        /* alpha (px[3]) is kept as-is */
    }

    /* Inverted images place flat regions against near-black backgrounds,
     * where lossy mosquito noise around lines becomes very visible.  Prefer
     * lossless webp (great for line art); fall back to high-quality lossy
     * when lossless would be too large (photos). */
    size_t webp_size = 0;
    uint8_t *webp = NULL;

    WebPPicture pic;
    WebPConfig cfg;
    WebPMemoryWriter wrt;
    WebPPictureInit(&pic);
    WebPConfigInit(&cfg);
    WebPMemoryWriterInit(&wrt);
    pic.width = w;
    pic.height = h;
    pic.use_argb = 1;
    if (WebPPictureImportRGBA(&pic, pixels, w * 4)) {
        cfg.method = 6;
        cfg.autofilter = 1;
        cfg.lossless = 1;
        cfg.quality = 92.0f;
        pic.writer = WebPMemoryWrite;
        pic.custom_ptr = &wrt;
        if (WebPEncode(&cfg, &pic)) {
            webp = wrt.mem;
            webp_size = wrt.size;
        }
        /* Photos make lossless explode; cap it and redo as lossy. */
        if (webp && webp_size > 1536 * 1024) {
            WebPMemoryWriterClear(&wrt);
            WebPMemoryWriterInit(&wrt);
            cfg.lossless = 0;
            if (WebPEncode(&cfg, &pic)) {
                webp = wrt.mem;
                webp_size = wrt.size;
            } else {
                webp = NULL;
                webp_size = 0;
            }
        }
    }
    WebPPictureFree(&pic);
    stbi_image_free(pixels);
    if (!webp) {
        WebPMemoryWriterClear(&wrt);
        CWIST_LOG_WARN("bg_invert_color: webp encode failed for %s", src_path);
        return;
    }
    FILE *out = fopen(dst_path, "wb");
    if (out) {
        fwrite(webp, 1, webp_size, out);
        fclose(out);
        snprintf(entry->orig, sizeof(entry->orig), "%s", orig_name);
        g_variant_count++;
        CWIST_LOG_INFO("bg_invert_color: generated %s (%s, %zu KB)", dst_path,
                       cfg.lossless ? "lossless" : "lossy", webp_size / 1024);
        /* Smaller widths for responsive srcset on the dark hero. */
        image_inline_make_width_variant(entry->variant, 768);
        image_inline_make_width_variant(entry->variant, 1280);
    }
    WebPMemoryWriterClear(&wrt);
}
#else
static void build_variant(const char *orig_name) {
    (void)orig_name;
    CWIST_LOG_WARN("bg_invert_color: built without libwebp; server-side inversion unavailable, CSS filter fallback applies");
}
#endif

void image_invert_cache_build(void) {
    if (!g_config.bg_invert_color[0] && !g_config.invert_logo) return;
    if (config_bg_invert_enabled("home")) {
        build_variant(g_config.home_img);
        build_variant(g_config.home_img_dark);
    }
    if (config_bg_invert_enabled("boards")) {
        build_variant(g_config.boards_img);
        build_variant(g_config.boards_img_dark);
    }
    if (config_bg_invert_enabled("files")) {
        build_variant(g_config.files_img);
        build_variant(g_config.files_img_dark);
    }
    if (config_bg_invert_enabled("toplevel")) {
        build_variant(g_config.bg_full_light);
        build_variant(g_config.bg_full_dark);
    }
    if (config_bg_invert_enabled("logo")) {
        build_variant(g_config.blog_logo);
        build_variant(g_config.blog_logo_dark);
    }
}

const char *image_invert_variant(const char *filename) {
    if (!filename || !filename[0]) return NULL;
    for (size_t i = 0; i < g_variant_count; i++) {
        if (strcmp(g_variants[i].orig, filename) == 0) return g_variants[i].variant;
    }
    return NULL;
}

/* Ring of URL buffers for inverted variants; deep enough for every hero on
 * a page to resolve both modes before appending.  Thread-local: requests are
 * rendered on a worker pool, and a shared ring let concurrent renders read
 * half-written or reused slots, baking wrong data-img-* URLs into pages. */
#define RESOLVE_RING 16
static _Thread_local char g_resolve_ring[RESOLVE_RING][320];
static _Thread_local size_t g_resolve_ring_idx;

const char *image_bg_resolve(const char *light_img, const char *dark_img, const char *target,
                             bool dark_mode, const char **out_shown_name, bool *out_css_filter) {
    const char *name = NULL;
    bool invert = false;
    config_resolve_bg(light_img, dark_img, target, dark_mode, &name, &invert);
    const char *url = image_inline_bg_url(name);
    const char *shown = name;
    bool css_filter = false;
    if (invert) {
        const char *variant = image_invert_variant(name);
        if (variant) {
            char *buf = g_resolve_ring[g_resolve_ring_idx % RESOLVE_RING];
            g_resolve_ring_idx++;
            snprintf(buf, sizeof(g_resolve_ring[0]), "/assets/img/%s", variant);
            url = buf;
            shown = variant;
        } else {
            css_filter = true;
        }
    }
    if (out_shown_name) *out_shown_name = shown;
    if (out_css_filter) *out_css_filter = css_filter;
    return url;
}

/* --- Theme-switchable hero backgrounds ---------------------------------- */

#include <cwist/core/sstring/sstring.h>
#include <cwist/image_contrast.h>

#define HERO_CSS_FILTER "invert(1) hue-rotate(180deg) saturate(0.55)"

void image_logo_resolve_modes(const char **out_l, const char **out_d,
                              bool *out_css_l, bool *out_css_d) {
    if (out_l) *out_l = image_bg_resolve(g_config.blog_logo, g_config.blog_logo_dark,
                                         "logo", false, NULL, out_css_l);
    if (out_d) *out_d = image_bg_resolve(g_config.blog_logo, g_config.blog_logo_dark,
                                         "logo", true, NULL, out_css_d);
}

void hero_bg_resolve_modes(const char *light_img, const char *dark_img, const char *target,
                           hero_bg_mode_t modes[2]) {
    for (int m = 0; m < 2; m++) {
        hero_bg_mode_t *md = &modes[m];
        memset(md, 0, sizeof(*md));
        md->url = image_bg_resolve(light_img, dark_img, target, m == 1,
                                   &md->shown_name, &md->css_filter);
        if (md->url && !md->css_filter) {
            /* Analyze the image actually shown — the inverted variant when
             * there is one — so text/overlay stay correct after inversion.
             * The CSS-filter fallback cannot be analyzed (no inverted file)
             * and pairs with the theme's own foreground instead. */
            char img_path[512];
            snprintf(img_path, sizeof(img_path), "public/img/%s", md->shown_name);
            get_image_text_style(img_path, md->url,
                                 md->shell_style, sizeof(md->shell_style),
                                 md->text_style, sizeof(md->text_style),
                                 md->logo_filter, sizeof(md->logo_filter),
                                 md->overlay_style, sizeof(md->overlay_style));
        }
    }
}

static void append_attr(cwist_sstring *b, const char *name, const char *value) {
    if (!value || !value[0]) return;
    cwist_sstring_append(b, " ");
    cwist_sstring_append(b, name);
    cwist_sstring_append(b, "=\"");
    cwist_sstring_append_escaped(b, value);
    cwist_sstring_append(b, "\"");
}

bool hero_bg_append_open(cwist_sstring *b, const hero_bg_mode_t modes[2], bool dark) {
    const hero_bg_mode_t *cur = &modes[dark ? 1 : 0];
    if (!cur->url) return false;

    char style_l[1600], style_d[1600];
    snprintf(style_l, sizeof(style_l), "%s;%s", modes[0].shell_style, modes[0].text_style);
    snprintf(style_d, sizeof(style_d), "%s;%s", modes[1].shell_style, modes[1].text_style);

    cwist_sstring_append(b, "<div data-hero-bg");
    append_attr(b, "data-style-light", style_l);
    append_attr(b, "data-style-dark", style_d);
    append_attr(b, "data-overlay-light", modes[0].overlay_style);
    append_attr(b, "data-overlay-dark", modes[1].overlay_style);
    append_attr(b, "data-logo-filter-light", modes[0].logo_filter);
    append_attr(b, "data-logo-filter-dark", modes[1].logo_filter);
    cwist_sstring_append(b, " style=\"");
    cwist_sstring_append_escaped(b, dark ? style_d : style_l);
    cwist_sstring_append(b, "\">");

    cwist_sstring_append(b, "<img class='hero-bg' fetchpriority='high'");
    append_attr(b, "data-img-light", modes[0].url);
    append_attr(b, "data-img-dark", modes[1].url);
    append_attr(b, "data-filter-light", modes[0].css_filter ? HERO_CSS_FILTER : "");
    append_attr(b, "data-filter-dark", modes[1].css_filter ? HERO_CSS_FILTER : "");
    /* Responsive variants (768/1280/1920w) cut the hero payload to roughly
     * the rendered viewport width; layout.js swaps srcset alongside src on
     * theme changes. */
    char srcset_l[1200], srcset_d[1200];
    bool has_ss_l = modes[0].url && image_inline_srcset(modes[0].url, srcset_l, sizeof(srcset_l));
    bool has_ss_d = modes[1].url && image_inline_srcset(modes[1].url, srcset_d, sizeof(srcset_d));
    if (has_ss_l || has_ss_d) {
        append_attr(b, "sizes", "100vw");
        append_attr(b, "data-srcset-light", has_ss_l ? srcset_l : NULL);
        append_attr(b, "data-srcset-dark", has_ss_d ? srcset_d : NULL);
        append_attr(b, "srcset", dark ? (has_ss_d ? srcset_d : NULL)
                                      : (has_ss_l ? srcset_l : NULL));
    }
    cwist_sstring_append(b, " src='");
    cwist_sstring_append(b, cur->url);
    cwist_sstring_append(b, "' alt='' style='position:absolute;inset:0;width:100%;height:100%;object-fit:cover;object-position:center;z-index:0");
    if (cur->css_filter) {
        cwist_sstring_append(b, ";filter:");
        cwist_sstring_append(b, HERO_CSS_FILTER);
    }
    cwist_sstring_append(b, "'>");

    if (cur->overlay_style[0]) {
        cwist_sstring_append(b, "<div class='hero-overlay' style=\"position:absolute;inset:0;z-index:1;");
        cwist_sstring_append_escaped(b, cur->overlay_style);
        cwist_sstring_append(b, "\"></div>");
    }
    return true;
}

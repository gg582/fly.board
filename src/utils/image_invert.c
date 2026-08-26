#define _POSIX_C_SOURCE 200809L
#include "image_invert.h"
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
 * photographs and gradients come out with strongly distorted colors.  A
 * perceptual L-flip (OKLCH) keeps hue but crushes contrast on low-contrast
 * art: a light-gray line on white becomes a dark-gray line on black and the
 * whole image reads as "all black".  This module therefore inverts linear
 * luminance (Y -> 1 - Y) while preserving chromaticity: white becomes black,
 * a light-gray line becomes a *light* gray line, black stays white, and
 * saturated colors desaturate toward white/black only as much as the gamut
 * forces — hue never flips and nothing turns neon. */

#define MAX_VARIANTS 8

typedef struct {
    char orig[256];
    char variant[300];
} invert_entry_t;

static invert_entry_t g_variants[MAX_VARIANTS];
static size_t g_variant_count;

static float srgb_to_linear(float c) {
    return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}

static float linear_to_srgb(float c) {
    return c <= 0.0031308f ? 12.92f * c : 1.055f * powf(c, 1.0f / 2.4f) - 0.055f;
}

/* sRGB 0..1 in/out.  Scaling linear RGB by Y'/Y preserves chromaticity
 * exactly; when that scale would leave the gamut, the pixel is first capped
 * so its brightest channel hits 1 and then blended toward white just enough
 * to reach the target luminance (hue kept, saturation reduced on demand). */
static void invert_pixel_luminance(float *pr, float *pg, float *pb) {
    float r = srgb_to_linear(*pr);
    float g = srgb_to_linear(*pg);
    float b = srgb_to_linear(*pb);

    float Y = 0.2126f * r + 0.7152f * g + 0.0722f * b;
    float target = 1.0f - Y;

    float out_r, out_g, out_b;
    if (Y < 1e-6f) {
        out_r = out_g = out_b = target; /* pure black inverts to white */
    } else {
        float k = target / Y;
        out_r = r * k;
        out_g = g * k;
        out_b = b * k;
        float maxc = fmaxf(out_r, fmaxf(out_g, out_b));
        if (maxc > 1.0f) {
            float s = 1.0f / maxc;
            out_r *= s;
            out_g *= s;
            out_b *= s;
            float Yc = 0.2126f * out_r + 0.7152f * out_g + 0.0722f * out_b;
            if (Yc < target && Yc < 1.0f) {
                float t = (target - Yc) / (1.0f - Yc);
                out_r += t * (1.0f - out_r);
                out_g += t * (1.0f - out_g);
                out_b += t * (1.0f - out_b);
            }
        }
    }

    *pr = linear_to_srgb(out_r);
    *pg = linear_to_srgb(out_g);
    *pb = linear_to_srgb(out_b);
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

    /* Prefix doubles as an algorithm version: changing it forces regeneration
     * of variants cached from an older algorithm. */
    invert_entry_t *entry = &g_variants[g_variant_count];
    int n = snprintf(entry->variant, sizeof(entry->variant), "luminv-%s.webp", orig_name);
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
        return;
    }

    int w = 0, h = 0;
    unsigned char *pixels = load_rgba(src_path, &w, &h);
    if (!pixels) {
        CWIST_LOG_WARN("bg_invert_color: cannot decode %s, inversion disabled for it", src_path);
        return;
    }

    for (long i = 0; i < (long)w * h; i++) {
        unsigned char *px = pixels + i * 4;
        float r = px[0] / 255.0f, g = px[1] / 255.0f, b = px[2] / 255.0f;
        invert_pixel_luminance(&r, &g, &b);
        px[0] = (unsigned char)(r * 255.0f + 0.5f);
        px[1] = (unsigned char)(g * 255.0f + 0.5f);
        px[2] = (unsigned char)(b * 255.0f + 0.5f);
        /* alpha (px[3]) is kept as-is */
    }

    uint8_t *webp = NULL;
    size_t webp_size = WebPEncodeRGBA(pixels, w, h, w * 4, 85.0f, &webp);
    stbi_image_free(pixels);
    if (!webp) {
        CWIST_LOG_WARN("bg_invert_color: webp encode failed for %s", src_path);
        return;
    }
    FILE *out = fopen(dst_path, "wb");
    if (out) {
        fwrite(webp, 1, webp_size, out);
        fclose(out);
        snprintf(entry->orig, sizeof(entry->orig), "%s", orig_name);
        g_variant_count++;
        CWIST_LOG_INFO("bg_invert_color: generated %s", dst_path);
    }
    WebPFree(webp);
}
#else
static void build_variant(const char *orig_name) {
    (void)orig_name;
    CWIST_LOG_WARN("bg_invert_color: built without libwebp; server-side inversion unavailable, CSS filter fallback applies");
}
#endif

void image_invert_cache_build(void) {
    if (!g_config.bg_invert_color[0]) return;
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
}

const char *image_invert_variant(const char *filename) {
    if (!filename || !filename[0]) return NULL;
    for (size_t i = 0; i < g_variant_count; i++) {
        if (strcmp(g_variants[i].orig, filename) == 0) return g_variants[i].variant;
    }
    return NULL;
}

/* Ring of URL buffers for inverted variants; deep enough for every hero on
 * a page to resolve both modes before appending. */
#define RESOLVE_RING 16
static char g_resolve_ring[RESOLVE_RING][320];
static size_t g_resolve_ring_idx;

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

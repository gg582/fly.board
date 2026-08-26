#define _POSIX_C_SOURCE 200809L
#include "image_invert.h"
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
 * module instead converts each pixel to OKLCH, flips only the perceptual
 * lightness (L -> 1 - L) while keeping chroma and hue, and clamps out-of-
 * gamut results by reducing chroma.  The result reads as "the same image,
 * darker/brighter" instead of a color-negative. */

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

/* sRGB 0..1 -> OKLCH, invert L, clamp chroma into gamut, back to sRGB. */
static void invert_pixel_oklch(float *pr, float *pg, float *pb) {
    float r = srgb_to_linear(*pr);
    float g = srgb_to_linear(*pg);
    float b = srgb_to_linear(*pb);

    float l = 0.4122214708f * r + 0.5363325363f * g + 0.0514459929f * b;
    float m = 0.2119034982f * r + 0.6806995451f * g + 0.1073969566f * b;
    float s = 0.0883024619f * r + 0.2817188376f * g + 0.6299787005f * b;

    float l_ = cbrtf(l), m_ = cbrtf(m), s_ = cbrtf(s);

    float L = 0.2104542553f * l_ + 0.7936177850f * m_ - 0.0040720468f * s_;
    float A = 1.9779984951f * l_ - 2.4285922050f * m_ + 0.4505937099f * s_;
    float B = 0.0259040371f * l_ + 0.7827717662f * m_ - 0.8086757660f * s_;

    float C = sqrtf(A * A + B * B);
    float h = atan2f(B, A);
    float Li = 1.0f - L; /* the only perceptual axis we flip */

    /* Chroma is kept, but damped: perceived colorfulness grows with
     * luminance (Hunt effect), so carrying full chroma across the lightness
     * flip makes mid-tones look neon/oversaturated.  A constant soft
     * compression keeps the inverted image believable. */
    C *= 0.82f;

    /* Binary-search the largest chroma that stays inside the sRGB gamut. */
    float lo = 0.0f, hi = C;
    float out_r = 0.0f, out_g = 0.0f, out_b = 0.0f;
    for (int i = 0; i < 16; i++) {
        float c = (lo + hi) * 0.5f;
        float a2 = c * cosf(h);
        float b2 = c * sinf(h);

        float l2_ = Li + 0.3963377774f * a2 + 0.2158037573f * b2;
        float m2_ = Li - 0.1055613458f * a2 - 0.0638541728f * b2;
        float s2_ = Li - 0.0894841775f * a2 - 1.2914855480f * b2;

        float l2 = l2_ * l2_ * l2_;
        float m2 = m2_ * m2_ * m2_;
        float s2 = s2_ * s2_ * s2_;

        float rr = +4.0767416621f * l2 - 3.3077115913f * m2 + 0.2309699292f * s2;
        float gg = -1.2684380046f * l2 + 2.6097574011f * m2 - 0.3413193965f * s2;
        float bb = -0.0041960863f * l2 - 0.7034186147f * m2 + 1.7076147010f * s2;

        if (rr >= 0.0f && rr <= 1.0f && gg >= 0.0f && gg <= 1.0f && bb >= 0.0f && bb <= 1.0f) {
            out_r = rr; out_g = gg; out_b = bb;
            lo = c;
        } else {
            hi = c;
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

    invert_entry_t *entry = &g_variants[g_variant_count];
    int n = snprintf(entry->variant, sizeof(entry->variant), "oklch-inv-%s.webp", orig_name);
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
        invert_pixel_oklch(&r, &g, &b);
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
    CWIST_LOG_WARN("bg_invert_color: built without libwebp; OKLCH inversion unavailable, CSS filter fallback applies");
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

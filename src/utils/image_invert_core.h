/**
 * @file image_invert_core.h
 * @brief Pure per-pixel color-inversion math shared by the server batch
 *        (image_invert.c) and the browser WASM module (wasm-client).
 *
 * Everything here is stateless float math on one pixel: sRGB transfer
 * functions and the two inversion algorithms (luminance-preserving flip and
 * the OKLCH perceptual flip).  No I/O, no globals, safe to compile into any
 * target.
 */

#ifndef FLYBOARD_IMAGE_INVERT_CORE_H
#define FLYBOARD_IMAGE_INVERT_CORE_H

#include <math.h>
#include <stdbool.h>

static inline float srgb_to_linear(float c) {
    return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}

static inline float linear_to_srgb(float c) {
    return c <= 0.0031308f ? 12.92f * c : 1.055f * powf(c, 1.0f / 2.4f) - 0.055f;
}

/* sRGB 0..1 in/out.  Scaling linear RGB by Y'/Y preserves chromaticity
 * exactly; when that scale would leave the gamut, the pixel is first capped
 * so its brightest channel hits 1 and then blended toward white just enough
 * to reach the target luminance (hue kept, saturation reduced on demand). */
static inline void invert_pixel_luminance(float *pr, float *pg, float *pb) {
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

/* Perceptual lightness flip in OKLCH (L -> 1 - L), keeping hue and chroma.
 * Smoother on photos and gradients, but crushes contrast on low-contrast
 * line art, hence the chroma damping to avoid neon mid-tones and gamut
 * clamping. */
static inline void invert_pixel_oklch(float *pr, float *pg, float *pb) {
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
    float Li = 1.0f - L;

    /* Chroma kept but damped: perceived colorfulness grows with luminance
     * (Hunt effect), so full chroma across the flip looks neon. */
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

#endif /* FLYBOARD_IMAGE_INVERT_CORE_H */

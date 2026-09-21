/**
 * @file image_contrast_core.h
 * @brief Pure colorimetry and band sampling shared by the server
 *        (image_contrast.c) and the browser WASM module (wasm-client).
 *
 * Stateless double-precision math: sRGB -> XYZ -> CIE Lab -> LCh, plus the
 * three-band lightness sampler the text-style heuristic uses (top 60% of the
 * image split into left/center/right thirds).  No I/O; pixels arrive as an
 * in-memory RGB buffer.
 */

#ifndef FLYBOARD_IMAGE_CONTRAST_CORE_H
#define FLYBOARD_IMAGE_CONTRAST_CORE_H

#include <math.h>
#include <stdbool.h>
#include <stddef.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static inline void fb_srgb_to_linear(double c, double *out) {
    c /= 255.0;
    *out = (c <= 0.04045) ? (c / 12.92) : pow((c + 0.055) / 1.055, 2.4);
}

static inline void fb_rgb_to_xyz(double r, double g, double b, double *X, double *Y, double *Z) {
    double lr, lg, lb;
    fb_srgb_to_linear(r, &lr);
    fb_srgb_to_linear(g, &lg);
    fb_srgb_to_linear(b, &lb);
    *X = 0.4124564 * lr + 0.3575761 * lg + 0.1804375 * lb;
    *Y = 0.2126729 * lr + 0.7151522 * lg + 0.0721750 * lb;
    *Z = 0.0193339 * lr + 0.1191920 * lg + 0.9503041 * lb;
}

static inline double fb_lab_f(double t) {
    return (t > 0.008856) ? pow(t, 1.0 / 3.0) : (7.787 * t + 16.0 / 116.0);
}

static inline void fb_xyz_to_lab(double X, double Y, double Z, double *L, double *a, double *b) {
    const double Xn = 0.95047, Yn = 1.00000, Zn = 1.08883;
    double fx = fb_lab_f(X / Xn);
    double fy = fb_lab_f(Y / Yn);
    double fz = fb_lab_f(Z / Zn);
    *L = 116.0 * fy - 16.0;
    *a = 500.0 * (fx - fy);
    *b = 200.0 * (fy - fz);
}

static inline void fb_rgb_to_lch(double r, double g, double b, double *L, double *C, double *H) {
    double X, Y, Z, a, bb;
    fb_rgb_to_xyz(r, g, b, &X, &Y, &Z);
    fb_xyz_to_lab(X, Y, Z, L, &a, &bb);
    *C = sqrt(a * a + bb * bb);
    *H = atan2(bb, a) * 180.0 / M_PI;
    if (*H < 0.0)
        *H += 360.0;
}

/**
 * Lightness (CIE L*) of the left/center/right thirds of the top 60% of an
 * RGB image, row-major 3 bytes per pixel.  Mirrors the historical
 * analyze_image sampling exactly.
 */
static inline bool fb_contrast_sample_rgb(const unsigned char *rgb, int w, int h,
                                          double L_out[3]) {
    if (!rgb || w < 1 || h < 1 || !L_out)
        return false;

    int top = h * 6 / 10;
    if (top < 1)
        top = 1;

    int x1 = w / 3;
    int x2 = w * 2 / 3;

    double sum_r[3] = {0.0}, sum_g[3] = {0.0}, sum_b[3] = {0.0};
    long count[3] = {0};

    for (int y = 0; y < top; y++) {
        for (int x = 0; x < w; x++) {
            const unsigned char *p = rgb + ((size_t)y * (size_t)w + (size_t)x) * 3;
            int idx = (x < x1) ? 0 : (x < x2) ? 1 : 2;
            sum_r[idx] += p[0];
            sum_g[idx] += p[1];
            sum_b[idx] += p[2];
            count[idx]++;
        }
    }

    for (int i = 0; i < 3; i++) {
        if (count[i] == 0)
            count[i] = 1;
        double L, C, H;
        fb_rgb_to_lch(sum_r[i] / count[i], sum_g[i] / count[i], sum_b[i] / count[i], &L, &C, &H);
        L_out[i] = L;
    }
    return true;
}

#endif /* FLYBOARD_IMAGE_CONTRAST_CORE_H */

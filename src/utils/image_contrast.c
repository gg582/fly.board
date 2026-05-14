#define _POSIX_C_SOURCE 200809L
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#include "cwist/image_contrast.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---------- Color space conversions: sRGB -> XYZ -> Lab -> LCH ---------- */

static void srgb_to_linear(double c, double *out)
{
    c /= 255.0;
    *out = (c <= 0.04045) ? (c / 12.92) : pow((c + 0.055) / 1.055, 2.4);
}

static void rgb_to_xyz(double r, double g, double b, double *X, double *Y, double *Z)
{
    double lr, lg, lb;
    srgb_to_linear(r, &lr);
    srgb_to_linear(g, &lg);
    srgb_to_linear(b, &lb);
    *X = 0.4124564 * lr + 0.3575761 * lg + 0.1804375 * lb;
    *Y = 0.2126729 * lr + 0.7151522 * lg + 0.0721750 * lb;
    *Z = 0.0193339 * lr + 0.1191920 * lg + 0.9503041 * lb;
}

static double lab_f(double t)
{
    return (t > 0.008856) ? pow(t, 1.0 / 3.0) : (7.787 * t + 16.0 / 116.0);
}

static void xyz_to_lab(double X, double Y, double Z, double *L, double *a, double *b)
{
    const double Xn = 0.95047, Yn = 1.00000, Zn = 1.08883;
    double fx = lab_f(X / Xn);
    double fy = lab_f(Y / Yn);
    double fz = lab_f(Z / Zn);
    *L = 116.0 * fy - 16.0;
    *a = 500.0 * (fx - fy);
    *b = 200.0 * (fy - fz);
}

static void rgb_to_lch(double r, double g, double b, double *L, double *C, double *H)
{
    double X, Y, Z, a, bb;
    rgb_to_xyz(r, g, b, &X, &Y, &Z);
    xyz_to_lab(X, Y, Z, L, &a, &bb);
    *C = sqrt(a * a + bb * bb);
    *H = atan2(bb, a) * 180.0 / M_PI;
    if (*H < 0.0) *H += 360.0;
}

/* ---------- Image sampling ---------- */

static int analyze_image(const char *path, double *L_left, double *L_center, double *L_right)
{
    int w, h, channels;
    unsigned char *data = stbi_load(path, &w, &h, &channels, 3);
    if (!data || w < 1 || h < 1) {
        if (data) stbi_image_free(data);
        return -1;
    }

    /* Sample the top 60 % of the image where text is expected to sit. */
    int top = h * 6 / 10;
    if (top < 1) top = h;

    int x1 = w / 3;
    int x2 = w * 2 / 3;

    double sum_r[3] = {0.0}, sum_g[3] = {0.0}, sum_b[3] = {0.0};
    long count[3] = {0};

    for (int y = 0; y < top; y++) {
        for (int x = 0; x < w; x++) {
            unsigned char *p = data + (y * w + x) * 3;
            int idx = (x < x1) ? 0 : (x < x2) ? 1 : 2;
            sum_r[idx] += p[0];
            sum_g[idx] += p[1];
            sum_b[idx] += p[2];
            count[idx]++;
        }
    }

    stbi_image_free(data);

    for (int i = 0; i < 3; i++) {
        if (count[i] == 0) count[i] = 1;
        double L, C, H;
        rgb_to_lch(sum_r[i] / count[i], sum_g[i] / count[i], sum_b[i] / count[i], &L, &C, &H);
        if (i == 0)      *L_left   = L;
        else if (i == 1) *L_center = L;
        else             *L_right  = L;
    }
    return 0;
}

/* ---------- Caching ---------- */

typedef struct {
    char path[256];
    time_t mtime;
    char bg_style[512];
    char text_style[256];
    char logo_filter[128];
    int valid;
} style_cache_t;

/* ---------- Public API ---------- */

int get_image_text_style(const char *image_path, const char *image_url,
                         char *bg_style_out, size_t bg_style_len,
                         char *text_style_out, size_t text_style_len,
                         char *logo_filter_out, size_t logo_filter_len)
{
    static style_cache_t cache = {0};

    struct stat st;
    time_t mtime = 0;
    if (stat(image_path, &st) == 0) {
        mtime = st.st_mtime;
    }

    if (cache.valid && strcmp(cache.path, image_path) == 0 && cache.mtime == mtime) {
        snprintf(bg_style_out, bg_style_len, "%s", cache.bg_style);
        snprintf(text_style_out, text_style_len, "%s", cache.text_style);
        snprintf(logo_filter_out, logo_filter_len, "%s", cache.logo_filter);
        return 0;
    }

    double L_left = 50.0, L_center = 50.0, L_right = 50.0;
    int ok = analyze_image(image_path, &L_left, &L_center, &L_right);

    if (ok != 0) {
        snprintf(bg_style_out, bg_style_len,
                 "background-image:url('%s?v=%ld');background-size:cover;background-position:center;border-radius:12px;margin-bottom:24px",
                 image_url, (long)mtime);
        snprintf(text_style_out, text_style_len,
                 "color:#ffffff;text-shadow:0 1px 3px rgba(0,0,0,0.5)");
        snprintf(logo_filter_out, logo_filter_len,
                 "drop-shadow(0 1px 2px rgba(0,0,0,0.5))");
        return -1;
    }

    const double target_contrast = 60.0;
    const double white_L = 95.0;
    const double black_L = 5.0;

    /* Minimum LCH lightness difference against white and black text candidates */
    double cw_left   = fabs(white_L - L_left);
    double cw_center = fabs(white_L - L_center);
    double cw_right  = fabs(white_L - L_right);
    double min_cw = cw_left < cw_center ? (cw_left < cw_right ? cw_left : cw_right)
                                        : (cw_center < cw_right ? cw_center : cw_right);

    double cb_left   = fabs(L_left   - black_L);
    double cb_center = fabs(L_center - black_L);
    double cb_right  = fabs(L_right  - black_L);
    double min_cb = cb_left < cb_center ? (cb_left < cb_right ? cb_left : cb_right)
                                        : (cb_center < cb_right ? cb_center : cb_right);

    const char *text_color;
    const char *shadow_rgba;
    const char *logo_rgba;
    int use_white;

    if (min_cw >= target_contrast && min_cw >= min_cb) {
        /* Dark background -> white text */
        use_white = 1;
        text_color = "#ffffff";
        shadow_rgba = "rgba(0,0,0,0.5)";
        logo_rgba   = "rgba(0,0,0,0.5)";
    } else if (min_cb >= target_contrast) {
        /* Light background -> dark text */
        use_white = 0;
        text_color = "#1a1a2e";
        shadow_rgba = "rgba(255,255,255,0.5)";
        logo_rgba   = "rgba(255,255,255,0.5)";
    } else {
        /* Mid-tone: force white text with dark overlay + heavier shadow */
        use_white = 1;
        text_color = "#ffffff";
        shadow_rgba = "rgba(0,0,0,0.85)";
        logo_rgba   = "rgba(0,0,0,0.7)";
    }

    if (use_white && min_cw < target_contrast) {
        /* Mid-tone path: darken the background with an overlay */
        double deficit = target_contrast - min_cw;
        if (deficit < 0.0) deficit = 0.0;
        if (deficit > target_contrast) deficit = target_contrast;

        double overlay_alpha = 0.2 + (deficit / target_contrast) * 0.3; /* 0.2 .. 0.5 */
        int shadow_blur = 3 + (int)((deficit / target_contrast) * 10);   /* 3 .. 13 px */

        snprintf(bg_style_out, bg_style_len,
                 "background-image:linear-gradient(rgba(0,0,0,%.2f),rgba(0,0,0,%.2f)),url('%s?v=%ld');"
                 "background-size:cover;background-position:center;border-radius:12px;margin-bottom:24px",
                 overlay_alpha, overlay_alpha, image_url, (long)mtime);

        snprintf(text_style_out, text_style_len,
                 "color:%s;text-shadow:0 1px %dpx %s,0 2px %dpx rgba(0,0,0,%.2f)",
                 text_color, shadow_blur, shadow_rgba,
                 shadow_blur + 4, 0.5 + (deficit / target_contrast) * 0.3);

        snprintf(logo_filter_out, logo_filter_len,
                 "drop-shadow(0 1px 2px %s)", logo_rgba);
    } else {
        /* Sufficient contrast without overlay */
        snprintf(bg_style_out, bg_style_len,
                 "background-image:url('%s?v=%ld');background-size:cover;background-position:center;padding:40px 20px 20px;border-radius:12px;margin-bottom:24px",
                 image_url, (long)mtime);

        snprintf(text_style_out, text_style_len,
                 "color:%s;text-shadow:0 1px 3px %s",
                 text_color, shadow_rgba);

        snprintf(logo_filter_out, logo_filter_len,
                 "drop-shadow(0 1px 2px %s)", logo_rgba);
    }

    /* Cache result */
    snprintf(cache.path, sizeof(cache.path), "%s", image_path);
    cache.mtime = mtime;
    snprintf(cache.bg_style, sizeof(cache.bg_style), "%s", bg_style_out);
    snprintf(cache.text_style, sizeof(cache.text_style), "%s", text_style_out);
    snprintf(cache.logo_filter, sizeof(cache.logo_filter), "%s", logo_filter_out);
    cache.valid = 1;

    return 0;
}

#define _POSIX_C_SOURCE 200809L
#include "stb_image.h"
#include "cwist/image_contrast.h"
#include "image_contrast_core.h"
#include <math.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <sys/stat.h>
#include <pthread.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---------- Color space conversions: sRGB -> XYZ -> Lab -> LCH ---------- */

/* ---------- Image sampling ---------- */

static int analyze_image(const char *path, double *L_left, double *L_center, double *L_right){
    int w, h, channels;
    unsigned char *data = stbi_load(path, &w, &h, &channels, 3);
    if (!data || w < 1 || h < 1) {
        if (data) stbi_image_free(data);
        return -1;
    }

    double L[3];
    bool ok = fb_contrast_sample_rgb(data, w, h, L);
    stbi_image_free(data);
    if (!ok) return -1;

    *L_left = L[0];
    *L_center = L[1];
    *L_right = L[2];
    return 0;
}

/* Vector heroes (traced line art) are transparent single/dual-color shapes
 * over the page background, which stb cannot sample.  Estimate the visible
 * blend instead: dominant hex fill blended over the theme background at a
 * fixed line-art ink coverage.  Returns 0 when at least one fill was found. */
static int analyze_svg(const char *path, bool dark_bg, double L[3]) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    struct stat st;
    if (fstat(fileno(f), &st) != 0 || st.st_size <= 0 || st.st_size > 512 * 1024) {
        fclose(f);
        return -1;
    }
    char *buf = (char *)malloc((size_t)st.st_size + 1);
    if (!buf) { fclose(f); return -1; }
    size_t n = fread(buf, 1, (size_t)st.st_size, f);
    fclose(f);
    if (n != (size_t)st.st_size) { free(buf); return -1; }
    buf[n] = '\0';

    /* Dominant #rrggbb fill in the file (first color wins ties). */
    long best_count = 0;
    unsigned int br = 0, bg = 0, bb = 0;
    for (size_t i = 0; i + 7 < n && buf[i]; i++) {
        if (buf[i] != '#') continue;
        unsigned int c[3];
        int hexlen = 0;
        if (sscanf(buf + i + 1, "%2x%2x%2x%n", &c[0], &c[1], &c[2], &hexlen) != 3 || hexlen != 6
            || (i + 7 < n && isxdigit((unsigned char)buf[i + 7]))) continue;
        long count = 0;
        char needle[8];
        snprintf(needle, sizeof(needle), "#%02x%02x%02x", c[0], c[1], c[2]);
        for (const char *p = buf; (p = strstr(p, needle)); p++) count++;
        if (count > best_count) {
            best_count = count;
            br = c[0]; bg = c[1]; bb = c[2];
        }
        i += 6;
    }
    free(buf);
    if (best_count == 0) return -1;

    const double coverage = 0.18; /* traced line-art ink share of the canvas */
    double L_ink, C, H;
    fb_rgb_to_lch((double)br, (double)bg, (double)bb, &L_ink, &C, &H);
    /* Page background behind the transparent art: near-white in light mode,
     * near-black in dark mode (matches the white_L/black_L anchors below). */
    double L_page = dark_bg ? 5.0 : 95.0;
    double sample = L_page * (1.0 - coverage) + L_ink * coverage;
    L[0] = L[1] = L[2] = sample;
    return 0;
}

static bool filename_is_svg(const char *path) {
    size_t len = path ? strlen(path) : 0;
    return len >= 4 && strcasecmp(path + len - 4, ".svg") == 0;
}

/* ---------- Caching ---------- */

typedef struct {
    char path[256];
    time_t mtime;
    char bg_style[512];
    char text_style[256];
    char logo_filter[128];
    char overlay_style[256];
    int valid;
} style_cache_t;

/* ---------- Public API ---------- */

int get_image_text_style(const char *image_path, const char *image_url, bool dark_bg,
                         char *shell_style_out, size_t shell_style_len,
                         char *text_style_out, size_t text_style_len,
                         char *logo_filter_out, size_t logo_filter_len,
                         char *overlay_style_out, size_t overlay_style_len)
{
    (void)image_url;
    static pthread_mutex_t g_image_contrast_mutex = PTHREAD_MUTEX_INITIALIZER;
    static style_cache_t cache = {0};

    struct stat st;
    time_t mtime = 0;
    if (stat(image_path, &st) == 0) {
        mtime = st.st_mtime;
    }

    pthread_mutex_lock(&g_image_contrast_mutex);
    if (cache.valid && strcmp(cache.path, image_path) == 0 && cache.mtime == mtime) {
        snprintf(shell_style_out, shell_style_len, "%s", cache.bg_style);
        snprintf(text_style_out, text_style_len, "%s", cache.text_style);
        snprintf(logo_filter_out, logo_filter_len, "%s", cache.logo_filter);
        snprintf(overlay_style_out, overlay_style_len, "%s", cache.overlay_style);
        pthread_mutex_unlock(&g_image_contrast_mutex);
        return 0;
    }
    pthread_mutex_unlock(&g_image_contrast_mutex);

    double L_left = 50.0, L_center = 50.0, L_right = 50.0;
    int ok;
    double L_svg[3];
    if (filename_is_svg(image_path) && analyze_svg(image_path, dark_bg, L_svg) == 0) {
        L_left = L_svg[0];
        L_center = L_svg[1];
        L_right = L_svg[2];
        ok = 0;
    } else {
        ok = analyze_image(image_path, &L_left, &L_center, &L_right);
    }

    if (ok != 0) {
        snprintf(shell_style_out, shell_style_len,
                 "position:relative;border-radius:0;margin-bottom:24px");
        snprintf(text_style_out, text_style_len,
                 "color:#ffffff;text-shadow:0 1px 3px rgba(0,0,0,0.5)");
        snprintf(logo_filter_out, logo_filter_len,
                 "drop-shadow(0 1px 2px rgba(0,0,0,0.5))");
        snprintf(overlay_style_out, overlay_style_len, "%s", "");
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

        snprintf(shell_style_out, shell_style_len,
                 "position:relative;border-radius:0;margin-bottom:24px");
        snprintf(overlay_style_out, overlay_style_len,
                 "background:linear-gradient(rgba(0,0,0,%.2f),rgba(0,0,0,%.2f))",
                 overlay_alpha, overlay_alpha);

        snprintf(text_style_out, text_style_len,
                 "color:%s;text-shadow:0 1px %dpx %s,0 2px %dpx rgba(0,0,0,%.2f)",
                 text_color, shadow_blur, shadow_rgba,
                 shadow_blur + 4, 0.5 + (deficit / target_contrast) * 0.3);

        snprintf(logo_filter_out, logo_filter_len,
                 "drop-shadow(0 1px 2px %s)", logo_rgba);
    } else {
        /* Sufficient contrast without overlay */
        snprintf(shell_style_out, shell_style_len,
                 "position:relative;padding:40px 20px 20px;border-radius:0;margin-bottom:24px");
        snprintf(overlay_style_out, overlay_style_len, "%s", "");

        snprintf(text_style_out, text_style_len,
                 "color:%s;text-shadow:0 1px 3px %s",
                 text_color, shadow_rgba);

        snprintf(logo_filter_out, logo_filter_len,
                 "drop-shadow(0 1px 2px %s)", logo_rgba);
    }

    /* Cache result */
    pthread_mutex_lock(&g_image_contrast_mutex);
    snprintf(cache.path, sizeof(cache.path), "%s", image_path);
    cache.mtime = mtime;
    snprintf(cache.bg_style, sizeof(cache.bg_style), "%s", shell_style_out);
    snprintf(cache.text_style, sizeof(cache.text_style), "%s", text_style_out);
    snprintf(cache.logo_filter, sizeof(cache.logo_filter), "%s", logo_filter_out);
    snprintf(cache.overlay_style, sizeof(cache.overlay_style), "%s", overlay_style_out);
    cache.valid = 1;
    pthread_mutex_unlock(&g_image_contrast_mutex);

    return 0;
}

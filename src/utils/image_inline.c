#define _POSIX_C_SOURCE 200809L
#include "image_inline.h"
#include "config/config.h"
#include "stb_image.h"
#ifdef HAVE_WEBP
#include <webp/encode.h>
#include <webp/decode.h>
#endif

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <strings.h>

static const char *const PREFIX = "data:image/webp;base64,";

typedef struct {
    char *home_bg_url;
    char *boards_bg_url;
    char *files_bg_url;
    char *home_bg_dark_url;
    char *boards_bg_dark_url;
    char *files_bg_dark_url;
    char *logo_url;
    char *logo_dark_url;
    char *favicon_url;
} inline_images_t;

static inline_images_t g_inline_images;

static bool env_on(const char *name) {
    const char *env = getenv(name);
    return env && (strcmp(env, "1") == 0 || strcasecmp(env, "true") == 0 || strcasecmp(env, "on") == 0);
}

static bool image_inline_enabled(void) {
    return env_on("FLYBOARD_INLINE_ALL_ASSETS") || env_on("FLYBOARD_INLINE_IMAGES");
}

static bool bg_images_inline_enabled(void) {
    /* Background images are usually large; require an explicit opt-in even
     * when FLYBOARD_INLINE_ALL_ASSETS is set so they do not bloat the first
     * HTML payload. */
    return env_on("FLYBOARD_INLINE_BG_IMAGES");
}

static size_t inline_max_image_size(void) {
    const char *env = getenv("FLYBOARD_INLINE_MAX_IMAGE_SIZE");
    if (env) {
        long val = strtol(env, NULL, 10);
        if (val > 0) return (size_t)val;
    }
    return 48 * 1024; /* default: keep each inlined image under ~48 KiB */
}

static char *external_image_url(const char *filename) {
    if (!filename || !filename[0]) return NULL;
    size_t len = strlen("/assets/img/") + strlen(filename) + 1;
    char *url = (char *)malloc(len);
    if (!url) return NULL;
    snprintf(url, len, "/assets/img/%s", filename);
    return url;
}

static char *base64_encode(const unsigned char *data, size_t len) {
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t out_len = 4 * ((len + 2) / 3);
    char *out = (char *)malloc(out_len + 1);
    if (!out) return NULL;

    size_t i = 0, j = 0;
    while (i + 3 <= len) {
        unsigned int v = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        out[j++] = table[(v >> 18) & 0x3f];
        out[j++] = table[(v >> 12) & 0x3f];
        out[j++] = table[(v >> 6) & 0x3f];
        out[j++] = table[v & 0x3f];
        i += 3;
    }
    if (i + 1 == len) {
        unsigned int v = data[i] << 16;
        out[j++] = table[(v >> 18) & 0x3f];
        out[j++] = table[(v >> 12) & 0x3f];
        out[j++] = '=';
        out[j++] = '=';
    } else if (i + 2 == len) {
        unsigned int v = (data[i] << 16) | (data[i + 1] << 8);
        out[j++] = table[(v >> 18) & 0x3f];
        out[j++] = table[(v >> 12) & 0x3f];
        out[j++] = table[(v >> 6) & 0x3f];
        out[j++] = '=';
    }
    out[j] = '\0';
    return out;
}

static unsigned char *read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    struct stat st;
    if (fstat(fileno(f), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
        fclose(f);
        return NULL;
    }
    unsigned char *buf = (unsigned char *)malloc((size_t)st.st_size);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, (size_t)st.st_size, f);
    fclose(f);
    if (n != (size_t)st.st_size) { free(buf); return NULL; }
    *out_len = n;
    return buf;
}

static unsigned char *load_image_any(const char *path, int *w, int *h, int *channels) {
    /* Try stb_image first (PNG/JPEG/BMP/GIF/etc). */
    unsigned char *data = stbi_load(path, w, h, channels, 0);
    if (data) return data;

    /* Fall back to libwebp for .webp inputs if available. */
#ifdef HAVE_WEBP
    size_t len = 0;
    unsigned char *file_data = read_file(path, &len);
    if (!file_data) return NULL;

    int width = 0, height = 0;
    if (!WebPGetInfo(file_data, len, &width, &height)) {
        free(file_data);
        return NULL;
    }
    unsigned char *rgba = WebPDecodeRGBA(file_data, len, &width, &height);
    free(file_data);
    if (!rgba) return NULL;
    *w = width;
    *h = height;
    *channels = 4;
    return rgba;
#else
    (void)path;
    return NULL;
#endif
}

#ifdef HAVE_WEBP
/* Bilinear downscale in place-ish: returns a new buffer when the image is
 * wider than max_w, preserving aspect ratio.  Identity when already small. */
static unsigned char *downscale_to_width(const unsigned char *px, int w, int h, int ch,
                                         int max_w, int *out_w, int *out_h) {
    if (w <= max_w) { *out_w = w; *out_h = h; return NULL; }
    int nw = max_w;
    int nh = (int)((long)h * max_w / w);
    if (nh < 1) nh = 1;
    unsigned char *out = (unsigned char *)malloc((size_t)nw * nh * ch);
    if (!out) { *out_w = w; *out_h = h; return NULL; }
    for (int y = 0; y < nh; y++) {
        float sy = (y + 0.5f) * h / nh - 0.5f;
        int y0 = (int)sy; if (y0 < 0) y0 = 0;
        int y1 = y0 + 1; if (y1 >= h) y1 = h - 1;
        float fy = sy - y0; if (fy < 0) fy = 0;
        for (int x = 0; x < nw; x++) {
            float sx = (x + 0.5f) * w / nw - 0.5f;
            int x0 = (int)sx; if (x0 < 0) x0 = 0;
            int x1 = x0 + 1; if (x1 >= w) x1 = w - 1;
            float fx = sx - x0; if (fx < 0) fx = 0;
            const unsigned char *p00 = px + ((size_t)y0 * w + x0) * ch;
            const unsigned char *p10 = px + ((size_t)y0 * w + x1) * ch;
            const unsigned char *p01 = px + ((size_t)y1 * w + x0) * ch;
            const unsigned char *p11 = px + ((size_t)y1 * w + x1) * ch;
            unsigned char *dst = out + ((size_t)y * nw + x) * ch;
            for (int c = 0; c < ch; c++) {
                float v = p00[c] * (1 - fx) * (1 - fy) + p10[c] * fx * (1 - fy)
                        + p01[c] * (1 - fx) * fy + p11[c] * fx * fy;
                dst[c] = (unsigned char)(v + 0.5f);
            }
        }
    }
    *out_w = nw; *out_h = nh;
    return out;
}

/* PageSpeed: branding/hero assets are often uploaded as multi-MB PNGs far
 * larger than their rendered size.  Re-encode them as lossy WebP, capped at
 * opt_max_width pixels wide, cached next to the source and rebuilt only when
 * the source changes (mtime), mirroring the invert-variant cache.  Returns
 * the variant URL, or NULL when the source cannot be improved. */
static char *optimized_webp_url(const char *filename, int opt_max_width) {
    if (!filename || !filename[0]) return NULL;
    if (strchr(filename, '/')) return NULL; /* expect a plain basename */

    char src_path[512];
    snprintf(src_path, sizeof(src_path), "public/img/%s", filename);
    struct stat sst;
    if (stat(src_path, &sst) != 0 || sst.st_size <= 0) return NULL;

    char variant[300];
    int n = snprintf(variant, sizeof(variant), "opt%dw-%s.webp", opt_max_width, filename);
    if (n < 0 || n >= (int)sizeof(variant)) return NULL;

    char dst_path[600];
    snprintf(dst_path, sizeof(dst_path), "public/img/%s", variant);
    struct stat dst;
    if (stat(dst_path, &dst) == 0 && dst.st_mtime >= sst.st_mtime &&
        dst.st_size > 0 && dst.st_size < sst.st_size) {
        return external_image_url(variant);
    }

    int w = 0, h = 0, ch = 0;
    unsigned char *pixels = load_image_any(src_path, &w, &h, &ch);
    if (!pixels || w <= 0 || h <= 0) { if (pixels) stbi_image_free(pixels); return NULL; }

    int ew = w, eh = h;
    unsigned char *scaled = downscale_to_width(pixels, w, h, ch, opt_max_width, &ew, &eh);
    const unsigned char *enc_px = scaled ? scaled : pixels;

    /* Already-small files (or ones we cannot shrink) keep the original. */
    uint8_t *webp = NULL;
    size_t webp_size = 0;
    if (ch == 4) {
        webp_size = WebPEncodeRGBA(enc_px, ew, eh, ew * 4, 82.0f, &webp);
    } else if (ch == 3 || ch == 1) {
        unsigned char *rgb = NULL;
        if (ch == 1) {
            rgb = (unsigned char *)malloc((size_t)ew * eh * 3);
            if (rgb) {
                for (long i = 0; i < (long)ew * eh; i++)
                    rgb[i * 3] = rgb[i * 3 + 1] = rgb[i * 3 + 2] = enc_px[i * ch];
                enc_px = rgb;
            }
        }
        webp_size = WebPEncodeRGB(enc_px, ew, eh, ew * 3, 82.0f, &webp);
        free(rgb);
    }
    free(scaled);
    stbi_image_free(pixels);

    if (!webp || webp_size >= (size_t)sst.st_size) {
        if (webp) WebPFree(webp);
        return NULL;
    }
    FILE *out = fopen(dst_path, "wb");
    if (!out) { WebPFree(webp); return NULL; }
    fwrite(webp, 1, webp_size, out);
    fclose(out);
    WebPFree(webp);
    return external_image_url(variant);
}
#else
static char *optimized_webp_url(const char *filename, int opt_max_width) {
    (void)filename; (void)opt_max_width;
    return NULL;
}
#endif

static char *encode_image_to_webp_data_url(const char *path, size_t max_bytes) {
#ifndef HAVE_WEBP
    (void)path; (void)max_bytes;
    return NULL;
#else
    /* Reject obviously oversized inputs before touching a WebP encoder. */
    struct stat st;
    if (stat(path, &st) == 0 && (size_t)st.st_size > max_bytes) return NULL;

    int w, h, channels;
    unsigned char *pixels = load_image_any(path, &w, &h, &channels);
    if (!pixels) return NULL;

    uint8_t *webp = NULL;
    size_t webp_size = 0;
    if (channels == 4) {
        webp_size = WebPEncodeRGBA(pixels, w, h, w * 4, 85.0f, &webp);
    } else if (channels == 3) {
        webp_size = WebPEncodeRGB(pixels, w, h, w * 3, 85.0f, &webp);
    } else if (channels == 1) {
        /* Grayscale: expand to RGB for WebP encoding. */
        unsigned char *rgb = (unsigned char *)malloc((size_t)w * h * 3);
        if (rgb) {
            for (int i = 0; i < w * h; i++) {
                rgb[i * 3] = rgb[i * 3 + 1] = rgb[i * 3 + 2] = pixels[i];
            }
            webp_size = WebPEncodeRGB(rgb, w, h, w * 3, 85.0f, &webp);
            free(rgb);
        }
    }
    stbi_image_free(pixels);
    if (!webp) return NULL;

    /* base64 expands by 4/3; if the encoded data URL would exceed the budget,
     * fall back to an external URL so we do not explode the HTML payload. */
    size_t encoded_estimate = 4 * ((webp_size + 2) / 3) + strlen(PREFIX) + 1;
    if (encoded_estimate > max_bytes) { WebPFree(webp); return NULL; }

    char *b64 = base64_encode(webp, webp_size);
    WebPFree(webp);
    if (!b64) return NULL;

    size_t prefix_len = strlen(PREFIX);
    char *url = (char *)malloc(prefix_len + strlen(b64) + 1);
    if (!url) { free(b64); return NULL; }
    memcpy(url, PREFIX, prefix_len);
    strcpy(url + prefix_len, b64);
    free(b64);
    return url;
#endif
}

/* Width caps for the on-disk WebP variants: heroes render full-width (cap at
 * a desktop viewport), logos/favicons render a few dozen CSS pixels tall. */
#define OPT_MAX_WIDTH_BG 1920
#define OPT_MAX_WIDTH_LOGO 512
#define OPT_MAX_WIDTH_FAVICON 128

static void build_one(const char *filename, char **out_url, bool allow_inline, size_t max_bytes,
                      int opt_max_width) {
    if (!filename || !filename[0]) return;
    if (allow_inline) {
        char path[512];
        snprintf(path, sizeof(path), "public/img/%s", filename);
        *out_url = encode_image_to_webp_data_url(path, max_bytes);
        if (*out_url) return;
    }
    *out_url = optimized_webp_url(filename, opt_max_width);
    if (!*out_url) {
        *out_url = external_image_url(filename);
    }
}

void image_inline_cache_build(void) {
    size_t max_img = inline_max_image_size();

    /* Hero/background images are large; only inline when explicitly requested
     * and still under the per-image size budget. */
    build_one(g_config.home_img,   &g_inline_images.home_bg_url,   bg_images_inline_enabled(), max_img, OPT_MAX_WIDTH_BG);
    build_one(g_config.boards_img, &g_inline_images.boards_bg_url, bg_images_inline_enabled(), max_img, OPT_MAX_WIDTH_BG);
    build_one(g_config.files_img,  &g_inline_images.files_bg_url,  bg_images_inline_enabled(), max_img, OPT_MAX_WIDTH_BG);
    build_one(g_config.home_img_dark,   &g_inline_images.home_bg_dark_url,   bg_images_inline_enabled(), max_img, OPT_MAX_WIDTH_BG);
    build_one(g_config.boards_img_dark, &g_inline_images.boards_bg_dark_url, bg_images_inline_enabled(), max_img, OPT_MAX_WIDTH_BG);
    build_one(g_config.files_img_dark,  &g_inline_images.files_bg_dark_url,  bg_images_inline_enabled(), max_img, OPT_MAX_WIDTH_BG);

    /* Logo/favicon are small identity assets; inline with the usual image flag,
     * but still respect the size cap. */
    build_one(g_config.blog_logo,  &g_inline_images.logo_url,    image_inline_enabled(), max_img, OPT_MAX_WIDTH_LOGO);
    build_one(g_config.blog_logo_dark, &g_inline_images.logo_dark_url, image_inline_enabled(), max_img, OPT_MAX_WIDTH_LOGO);
    build_one(g_config.favicon,    &g_inline_images.favicon_url, image_inline_enabled(), max_img, OPT_MAX_WIDTH_FAVICON);

    /* If no custom logo is configured, use the default logo.png. */
    if (!g_inline_images.logo_url) {
        if (image_inline_enabled()) {
            g_inline_images.logo_url = encode_image_to_webp_data_url("public/img/logo.png", max_img);
        }
        if (!g_inline_images.logo_url) {
            g_inline_images.logo_url = optimized_webp_url("logo.png", OPT_MAX_WIDTH_LOGO);
        }
        if (!g_inline_images.logo_url) {
            g_inline_images.logo_url = external_image_url("logo.png");
        }
    }

    /* Multi-width variants for hero/background srcset. */
    image_inline_build_responsive_bg();
}

const char *image_inline_home_bg(void)   { return g_inline_images.home_bg_url; }
const char *image_inline_boards_bg(void) { return g_inline_images.boards_bg_url; }
const char *image_inline_files_bg(void)  { return g_inline_images.files_bg_url; }
const char *image_inline_logo(void)      { return g_inline_images.logo_url; }
const char *image_inline_favicon(void)   { return g_inline_images.favicon_url; }

const char *image_inline_bg_url(const char *filename) {
    if (!filename || !filename[0]) return NULL;
    const struct { const char *name; const char *url; } map[] = {
        { g_config.home_img,        g_inline_images.home_bg_url },
        { g_config.boards_img,      g_inline_images.boards_bg_url },
        { g_config.files_img,       g_inline_images.files_bg_url },
        { g_config.home_img_dark,   g_inline_images.home_bg_dark_url },
        { g_config.boards_img_dark, g_inline_images.boards_bg_dark_url },
        { g_config.files_img_dark,  g_inline_images.files_bg_dark_url },
        { g_config.blog_logo,       g_inline_images.logo_url },
        { g_config.blog_logo_dark,  g_inline_images.logo_dark_url },
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (map[i].name[0] && strcmp(filename, map[i].name) == 0) return map[i].url;
    }
    return NULL;
}

/* --- Responsive (srcset) support ----------------------------------------- */

/* Intrinsic pixel width of an image file, reading only the header. */
static int width_of_file(const char *path) {
    int w = 0, h = 0, ch = 0;
    if (stbi_info(path, &w, &h, &ch)) return w;
#ifdef HAVE_WEBP
    size_t len = 0;
    unsigned char *data = read_file(path, &len);
    if (data) {
        int ww = 0, hh = 0;
        if (WebPGetInfo(data, len, &ww, &hh)) w = ww;
        free(data);
    }
#endif
    return w;
}

void image_inline_make_width_variant(const char *basename, int width) {
    (void)optimized_webp_url(basename, width);
}

void image_inline_build_responsive_bg(void) {
    const char *const names[] = {
        g_config.home_img, g_config.boards_img, g_config.files_img,
        g_config.home_img_dark, g_config.boards_img_dark, g_config.files_img_dark,
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (!names[i][0]) continue;
        image_inline_make_width_variant(names[i], 768);
        image_inline_make_width_variant(names[i], 1280);
    }
}

bool image_file_dimensions_from_url(const char *url, int *out_w, int *out_h) {
    if (!url || !out_w || !out_h) return false;
    if (strncmp(url, "/assets/img/", 12) != 0) return false;
    char name[320];
    snprintf(name, sizeof(name), "%s", url + 12);
    char *q = strchr(name, '?');
    if (q) *q = '\0';
    if (!name[0]) return false;
    char path[640];
    snprintf(path, sizeof(path), "public/img/%s", name);
    int w = 0, h = 0, ch = 0;
    if (stbi_info(path, &w, &h, &ch)) { *out_w = w; *out_h = h; return true; }
#ifdef HAVE_WEBP
    size_t len = 0;
    unsigned char *data = read_file(path, &len);
    if (data) {
        int ww = 0, hh = 0;
        int ok = WebPGetInfo(data, len, &ww, &hh);
        free(data);
        if (ok) { *out_w = ww; *out_h = hh; return true; }
    }
#endif
    return false;
}

/* Compose a srcset value for a hero/background URL from the on-disk width
 * variants (768/1280/1920 plus the file itself).  Returns false when fewer
 * than two candidates exist — then the caller keeps the plain src. */
bool image_inline_srcset(const char *url, char *out, size_t out_sz) {
    if (!url || !out || out_sz == 0) return false;
    out[0] = '\0';
    if (strncmp(url, "/assets/img/", 12) != 0) return false;

    char self[320];
    snprintf(self, sizeof(self), "%s", url + 12);
    char *q = strchr(self, '?');
    if (q) *q = '\0';
    if (!self[0]) return false;

    /* Source stem: the file the opt<w>w- variants were generated from. */
    char stem[320];
    snprintf(stem, sizeof(stem), "%s", self);
    char *s = stem;
    if (strncmp(s, "opt1920w-", 9) == 0) s += 9;
    size_t sl = strlen(s);
    if (sl > 5 && strcmp(s + sl - 5, ".webp") == 0) s[sl - 5] = '\0';

    struct { char name[352]; int w; } cand[4];
    int n_cand = 0;
    static const int widths[] = { 768, 1280, 1920 };
    for (int i = 0; i < 3; i++) {
        char var[352];
        snprintf(var, sizeof(var), "opt%dw-%s.webp", widths[i], s);
        char path[720];
        snprintf(path, sizeof(path), "public/img/%s", var);
        struct stat st;
        if (stat(path, &st) != 0 || st.st_size <= 0) continue;
        int w = width_of_file(path);
        if (w <= 0) w = widths[i];
        snprintf(cand[n_cand].name, sizeof(cand[0].name), "%s", var);
        cand[n_cand].w = w;
        n_cand++;
    }
    /* The current file itself (e.g. the full-resolution inverted variant)
     * is a candidate too, unless it is already listed as opt1920w-<stem>. */
    char opt1920[352];
    snprintf(opt1920, sizeof(opt1920), "opt1920w-%s.webp", s);
    if (strcmp(self, opt1920) != 0 && n_cand < 4) {
        char path[720];
        snprintf(path, sizeof(path), "public/img/%s", self);
        struct stat st;
        if (stat(path, &st) == 0 && st.st_size > 0) {
            int w = width_of_file(path);
            if (w > 0) {
                bool dup = false;
                for (int i = 0; i < n_cand; i++) if (cand[i].w == w) { dup = true; break; }
                if (!dup) {
                    snprintf(cand[n_cand].name, sizeof(cand[0].name), "%s", self);
                    cand[n_cand].w = w;
                    n_cand++;
                }
            }
        }
    }
    if (n_cand < 2) { out[0] = '\0'; return false; }

    /* Ascending width order. */
    for (int i = 0; i < n_cand - 1; i++)
        for (int j = i + 1; j < n_cand; j++)
            if (cand[j].w < cand[i].w) {
                typeof(cand[0]) tmp = cand[i]; cand[i] = cand[j]; cand[j] = tmp;
            }

    size_t used = 0;
    for (int i = 0; i < n_cand; i++) {
        int n = snprintf(out + used, out_sz - used, "%s/assets/img/%s %dw",
                         i ? ", " : "", cand[i].name, cand[i].w);
        if (n < 0 || (size_t)n >= out_sz - used) { out[0] = '\0'; return false; }
        used += (size_t)n;
    }
    return true;
}

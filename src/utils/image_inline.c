#define _POSIX_C_SOURCE 200809L
#include "image_inline.h"
#include "config/config.h"
#include "stb_image.h"
#include <webp/encode.h>
#include <webp/decode.h>
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
    char *logo_url;
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

    /* Fall back to libwebp for .webp inputs. */
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
}

static char *encode_image_to_webp_data_url(const char *path, size_t max_bytes) {
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
}

static void build_one(const char *filename, char **out_url, bool allow_inline, size_t max_bytes) {
    if (!filename || !filename[0]) return;
    if (!allow_inline) {
        *out_url = external_image_url(filename);
        return;
    }
    char path[512];
    snprintf(path, sizeof(path), "public/img/%s", filename);
    *out_url = encode_image_to_webp_data_url(path, max_bytes);
    if (!*out_url) {
        *out_url = external_image_url(filename);
    }
}

void image_inline_cache_build(void) {
    size_t max_img = inline_max_image_size();

    /* Hero/background images are large; only inline when explicitly requested
     * and still under the per-image size budget. */
    build_one(g_config.home_img,   &g_inline_images.home_bg_url,   bg_images_inline_enabled(), max_img);
    build_one(g_config.boards_img, &g_inline_images.boards_bg_url, bg_images_inline_enabled(), max_img);
    build_one(g_config.files_img,  &g_inline_images.files_bg_url,  bg_images_inline_enabled(), max_img);

    /* Logo/favicon are small identity assets; inline with the usual image flag,
     * but still respect the size cap. */
    build_one(g_config.blog_logo,  &g_inline_images.logo_url,    image_inline_enabled(), max_img);
    build_one(g_config.favicon,    &g_inline_images.favicon_url, image_inline_enabled(), max_img);

    /* If no custom logo is configured, use the default logo.png. */
    if (!g_inline_images.logo_url) {
        if (image_inline_enabled()) {
            g_inline_images.logo_url = encode_image_to_webp_data_url("public/img/logo.png", max_img);
        }
        if (!g_inline_images.logo_url) {
            g_inline_images.logo_url = external_image_url("logo.png");
        }
    }
}

const char *image_inline_home_bg(void)   { return g_inline_images.home_bg_url; }
const char *image_inline_boards_bg(void) { return g_inline_images.boards_bg_url; }
const char *image_inline_files_bg(void)  { return g_inline_images.files_bg_url; }
const char *image_inline_logo(void)      { return g_inline_images.logo_url; }
const char *image_inline_favicon(void)   { return g_inline_images.favicon_url; }

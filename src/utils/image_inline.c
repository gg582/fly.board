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

static bool image_inline_enabled(void) {
    const char *env = getenv("FLYBOARD_INLINE_ALL_ASSETS");
    if (env && (strcmp(env, "1") == 0 || strcasecmp(env, "true") == 0 || strcasecmp(env, "on") == 0)) return true;
    env = getenv("FLYBOARD_INLINE_IMAGES");
    if (env && (strcmp(env, "1") == 0 || strcasecmp(env, "true") == 0 || strcasecmp(env, "on") == 0)) return true;
    return false;
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

static char *encode_image_to_webp_data_url(const char *path) {
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

static void build_one(const char *filename, char **out_url) {
    if (!filename || !filename[0]) return;
    if (!image_inline_enabled()) {
        *out_url = external_image_url(filename);
        return;
    }
    char path[512];
    snprintf(path, sizeof(path), "public/img/%s", filename);
    *out_url = encode_image_to_webp_data_url(path);
}

void image_inline_cache_build(void) {
    build_one(g_config.home_img,   &g_inline_images.home_bg_url);
    build_one(g_config.boards_img, &g_inline_images.boards_bg_url);
    build_one(g_config.files_img,  &g_inline_images.files_bg_url);
    build_one(g_config.blog_logo,  &g_inline_images.logo_url);
    build_one(g_config.favicon,    &g_inline_images.favicon_url);
    /* If no custom logo is configured, use the default logo.png. Inlining is
     * controlled by FLYBOARD_INLINE_IMAGES / FLYBOARD_INLINE_ALL_ASSETS. */
    if (!g_inline_images.logo_url) {
        if (image_inline_enabled()) {
            g_inline_images.logo_url = encode_image_to_webp_data_url("public/img/logo.png");
        } else {
            g_inline_images.logo_url = external_image_url("logo.png");
        }
    }
}

const char *image_inline_home_bg(void)   { return g_inline_images.home_bg_url; }
const char *image_inline_boards_bg(void) { return g_inline_images.boards_bg_url; }
const char *image_inline_files_bg(void)  { return g_inline_images.files_bg_url; }
const char *image_inline_logo(void)      { return g_inline_images.logo_url; }
const char *image_inline_favicon(void)   { return g_inline_images.favicon_url; }

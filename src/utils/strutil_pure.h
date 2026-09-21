/**
 * @file strutil_pure.h
 * @brief Pure string utilities shared by the server (utils.c, sql_escape.c)
 *        and the WASM sandbox module (wasm/strutil_module.c).
 *
 * All functions are stateless string-in/string-out transforms.  Allocation
 * is injected through FB_STRUTIL_ALLOC/FB_STRUTIL_FREE, which default to
 * malloc/free; the server defines them to cwist_alloc/cwist_free before
 * including so returned ownership matches the existing callers.
 */

#ifndef FLYBOARD_STRUTIL_PURE_H
#define FLYBOARD_STRUTIL_PURE_H

#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef FB_STRUTIL_ALLOC
#define FB_STRUTIL_ALLOC(sz) malloc(sz)
#endif
#ifndef FB_STRUTIL_FREE
#define FB_STRUTIL_FREE(p) free(p)
#endif

static inline char *fb_generate_slug(const char *title) {
    size_t len = strlen(title);
    char *slug = (char *)FB_STRUTIL_ALLOC(len * 3 + 1);
    if (!slug)
        return NULL;
    size_t j = 0;
    for (size_t i = 0; i < len && j < len * 3; i++) {
        unsigned char c = (unsigned char)title[i];
        if (isalnum(c)) {
            slug[j++] = (char)tolower(c);
        } else if (c == ' ' || c == '-' || c == '_') {
            if (j == 0 || slug[j - 1] != '-')
                slug[j++] = '-';
        }
    }
    if (j > 0 && slug[j - 1] == '-')
        j--;
    slug[j] = '\0';
    if (j == 0) {
        slug[0] = 'p';
        slug[1] = 'o';
        slug[2] = 's';
        slug[3] = 't';
        slug[4] = '\0';
    }
    return slug;
}

static inline size_t fb_utf8_truncate_len(const char *str, size_t max_bytes) {
    if (!str)
        return 0;
    size_t len = strlen(str);
    if (len <= max_bytes)
        return len;
    size_t i = max_bytes;
    while (i > 0 && ((unsigned char)str[i] & 0xC0) == 0x80) {
        i--;
    }
    return i;
}

static inline bool fb_is_safe_public_path(const char *path) {
    if (!path || path[0] == '\0')
        return false;
    if (path[0] == '/')
        return false;
    if (path[0] == '.' && path[1] == '.')
        return false;

    static const char *allowed_prefixes[] = {
        "public/uploads/",
        "public/profile/",
        "public/img/",
        "public/media/",
        "data/tasfa/",
    };
    bool has_allowed_prefix = false;
    for (size_t i = 0; i < sizeof(allowed_prefixes) / sizeof(allowed_prefixes[0]); i++) {
        size_t plen = strlen(allowed_prefixes[i]);
        if (strncmp(path, allowed_prefixes[i], plen) == 0) {
            has_allowed_prefix = true;
            break;
        }
    }
    if (!has_allowed_prefix)
        return false;

    /* Reject any ".." segment anywhere in the path. */
    const char *p = path;
    while (*p) {
        if (p[0] == '.' && p[1] == '.' && (p[2] == '/' || p[2] == '\0'))
            return false;
        const char *slash = strchr(p, '/');
        if (!slash)
            break;
        p = slash + 1;
    }
    return true;
}

static inline char *fb_sanitize_filename(const char *filename) {
    if (!filename)
        return NULL;
    const char *base = strrchr(filename, '/');
    if (base)
        base++;
    else
        base = filename;
    const char *base2 = strrchr(base, '\\');
    if (base2)
        base = base2 + 1;
    if (!base[0])
        return NULL;

    size_t len = strlen(base);
    if (len > 255)
        len = 255;
    char *out = (char *)FB_STRUTIL_ALLOC(len + 1);
    if (!out)
        return NULL;
    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)base[i];
        if (c == '\0' || c == '/' || c == '\\' || c == '\r' || c == '\n' || c < 0x20)
            continue;
        out[j++] = (char)c;
    }
    out[j] = '\0';
    if (j == 0) {
        FB_STRUTIL_FREE(out);
        return NULL;
    }
    return out;
}

static inline char *fb_sql_escape(const char *src) {
    if (!src)
        return NULL;
    size_t len = strlen(src);
    size_t extra = 0;
    for (size_t i = 0; i < len; i++) {
        switch (src[i]) {
        case '&':
            extra += 4;
            break;
        case '<':
            extra += 3;
            break;
        case '>':
            extra += 3;
            break;
        case '"':
            extra += 5;
            break;
        case '\'':
            extra += 5;
            break;
        }
    }
    char *out = (char *)FB_STRUTIL_ALLOC(len + extra + 1);
    if (!out)
        return NULL;
    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        switch (src[i]) {
        case '&':
            memcpy(out + j, "&amp;", 5);
            j += 5;
            break;
        case '<':
            memcpy(out + j, "&lt;", 4);
            j += 4;
            break;
        case '>':
            memcpy(out + j, "&gt;", 4);
            j += 4;
            break;
        case '"':
            memcpy(out + j, "&quot;", 6);
            j += 6;
            break;
        case '\'':
            memcpy(out + j, "&#x27;", 6);
            j += 6;
            break;
        default:
            out[j++] = src[i];
            break;
        }
    }
    out[j] = '\0';
    return out;
}

static inline char *fb_sql_unescape(const char *src) {
    if (!src)
        return NULL;
    size_t len = strlen(src);
    char *out = (char *)FB_STRUTIL_ALLOC(len + 1);
    if (!out)
        return NULL;
    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        if (src[i] == '&' && strncmp(src + i, "&amp;", 5) == 0) {
            out[j++] = '&';
            i += 4;
        } else if (src[i] == '&' && strncmp(src + i, "&lt;", 4) == 0) {
            out[j++] = '<';
            i += 3;
        } else if (src[i] == '&' && strncmp(src + i, "&gt;", 4) == 0) {
            out[j++] = '>';
            i += 3;
        } else if (src[i] == '&' && strncmp(src + i, "&gtl", 4) == 0) {
            /* handle cwist append_escaped bug (&gtl instead of &gt;) */
            out[j++] = '>';
            i += 3;
        } else if (src[i] == '&' && strncmp(src + i, "&quot;", 6) == 0) {
            out[j++] = '"';
            i += 5;
        } else if (src[i] == '&' && strncmp(src + i, "&#x27;", 6) == 0) {
            out[j++] = '\'';
            i += 5;
        } else if (src[i] == '&' && strncmp(src + i, "&#39;", 5) == 0) {
            out[j++] = '\'';
            i += 4;
        } else {
            out[j++] = src[i];
        }
    }
    out[j] = '\0';
    return out;
}

#endif /* FLYBOARD_STRUTIL_PURE_H */

#define _POSIX_C_SOURCE 200809L
#include "sql_escape.h"
#include <cwist/core/mem/alloc.h>
#include <string.h>

char *sql_escape(const char *src) {
    if (!src) return NULL;
    size_t len = strlen(src);
    size_t extra = 0;
    for (size_t i = 0; i < len; i++) {
        switch (src[i]) {
            case '&': extra += 4; break;
            case '<': extra += 3; break;
            case '>': extra += 3; break;
            case '"': extra += 5; break;
            case '\'': extra += 5; break;
        }
    }
    char *out = (char *)cwist_alloc(len + extra + 1);
    if (!out) return NULL;
    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        switch (src[i]) {
            case '&': memcpy(out + j, "&amp;", 5); j += 5; break;
            case '<': memcpy(out + j, "&lt;", 4); j += 4; break;
            case '>': memcpy(out + j, "&gt;", 4); j += 4; break;
            case '"': memcpy(out + j, "&quot;", 6); j += 6; break;
            case '\'': memcpy(out + j, "&#x27;", 6); j += 6; break;
            default: out[j++] = src[i]; break;
        }
    }
    out[j] = '\0';
    return out;
}

char *sql_unescape(const char *src) {
    if (!src) return NULL;
    size_t len = strlen(src);
    char *out = (char *)cwist_alloc(len + 1);
    if (!out) return NULL;
    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        if (src[i] == '&' && strncmp(src + i, "&amp;", 5) == 0) {
            out[j++] = '&'; i += 4;
        } else if (src[i] == '&' && strncmp(src + i, "&lt;", 4) == 0) {
            out[j++] = '<'; i += 3;
        } else if (src[i] == '&' && strncmp(src + i, "&gt;", 4) == 0) {
            out[j++] = '>'; i += 3;
        } else if (src[i] == '&' && strncmp(src + i, "&gtl", 4) == 0) {
            /* handle cwist append_escaped bug (&gtl instead of &gt;) */
            out[j++] = '>'; i += 3;
        } else if (src[i] == '&' && strncmp(src + i, "&quot;", 6) == 0) {
            out[j++] = '"'; i += 5;
        } else if (src[i] == '&' && strncmp(src + i, "&#x27;", 6) == 0) {
            out[j++] = '\''; i += 5;
        } else if (src[i] == '&' && strncmp(src + i, "&#39;", 5) == 0) {
            out[j++] = '\''; i += 4;
        } else {
            out[j++] = src[i];
        }
    }
    out[j] = '\0';
    return out;
}

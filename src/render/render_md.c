#define _POSIX_C_SOURCE 200809L
#include "render.h"
#include "cwist/image_size.h"
#include <cwist/core/sstring/sstring.h>
#include <md4c-html.h>
#include <md4c.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    char **expressions;
    int count;
    int capacity;
} math_registry_t;

static void math_registry_init(math_registry_t *reg) {
    reg->expressions = NULL;
    reg->count = 0;
    reg->capacity = 0;
}

static void math_registry_add(math_registry_t *reg, const char *expr, size_t len) {
    while (len > 0 && (expr[0] == ' ' || expr[0] == '\t' || expr[0] == '\n' || expr[0] == '\r')) {
        expr++; len--;
    }
    while (len > 0 && (expr[len - 1] == ' ' || expr[len - 1] == '\t' || expr[len - 1] == '\n' || expr[len - 1] == '\r')) {
        len--;
    }
    if (len == 0) {
        expr = " ";
        len = 1;
    }
    if (reg->count >= reg->capacity) {
        reg->capacity = reg->capacity ? reg->capacity * 2 : 16;
        reg->expressions = (char **)realloc(reg->expressions, sizeof(char *) * reg->capacity);
    }
    char *copy = (char *)malloc(len + 1);
    memcpy(copy, expr, len);
    copy[len] = '\0';
    reg->expressions[reg->count++] = copy;
}

static void math_registry_free(math_registry_t *reg) {
    for (int i = 0; i < reg->count; i++) free(reg->expressions[i]);
    free(reg->expressions);
    reg->expressions = NULL;
    reg->count = 0;
    reg->capacity = 0;
}

static void append_escaped_math(cwist_sstring *out, const char *text) {
    while (*text) {
        if (*text == '<') cwist_sstring_append(out, "&lt;");
        else if (*text == '>') cwist_sstring_append(out, "&gt;");
        else if (*text == '&') cwist_sstring_append(out, "&amp;");
        else if (*text == '"') cwist_sstring_append(out, "&quot;");
        else cwist_sstring_append_len(out, text, 1);
        text++;
    }
}

static bool is_line_start(const char *s, size_t pos) {
    return pos == 0 || s[pos - 1] == '\n';
}

static bool is_code_fence_line(const char *s, size_t pos, size_t len, int *out_len) {
    size_t k = pos;
    while (k < len && (s[k] == ' ' || s[k] == '\t')) k++;
    if (k + 3 > len) return false;
    int backticks = 0;
    while (k + backticks < len && s[k + backticks] == '`') backticks++;
    if (backticks >= 3) {
        if (out_len) *out_len = backticks;
        return true;
    }
    int tildes = 0;
    while (k + tildes < len && s[k + tildes] == '~') tildes++;
    if (tildes >= 3) {
        if (out_len) *out_len = tildes;
        return true;
    }
    return false;
}

static char *protect_math(const char *md, math_registry_t *blocks, math_registry_t *inlines) {
    size_t len = strlen(md);
    cwist_sstring *out = cwist_sstring_create();
    size_t i = 0;
    int in_code_block = 0;
    int current_fence_len = 0;

    while (i < len) {
        /* Track fenced code blocks with proper backtick/tilde counts */
        int fence_len = 0;
        if (is_line_start(md, i) && is_code_fence_line(md, i, len, &fence_len)) {
            if (in_code_block) {
                if (fence_len >= current_fence_len) {
                    in_code_block = 0;
                    current_fence_len = 0;
                    /* Consume the rest of the closing fence line so backticks
                       are not misinterpreted as inline code spans. */
                    while (i < len && md[i] != '\n') {
                        cwist_sstring_append_len(out, md + i, 1);
                        i++;
                    }
                    if (i < len) {
                        cwist_sstring_append_len(out, md + i, 1);
                        i++;
                    }
                    continue;
                }
            } else {
                in_code_block = 1;
                current_fence_len = fence_len;
            }
        }

        if (in_code_block) {
            cwist_sstring_append_len(out, md + i, 1);
            i++;
            continue;
        }

        /* Skip inline code spans (backticks) */
        if (md[i] == '`') {
            size_t j = i + 1;
            while (j < len && md[j] != '`') j++;
            if (j < len) {
                while (i <= j) {
                    cwist_sstring_append_len(out, md + i, 1);
                    i++;
                }
                continue;
            }
        }

        /* Block math: $$...$$ */
        if (i + 1 < len && md[i] == '$' && md[i + 1] == '$' && (i == 0 || md[i - 1] != '\\')) {
            size_t j = i + 2;
            while (j + 1 < len && !(md[j] == '$' && md[j + 1] == '$')) j++;
            if (j + 1 < len) {
                size_t expr_len = j - (i + 2);
                math_registry_add(blocks, md + i + 2, expr_len);
                char placeholder[64];
                snprintf(placeholder, sizeof(placeholder), "@@MATH_BLOCK_%d@@", blocks->count - 1);
                cwist_sstring_append(out, placeholder);
                i = j + 2;
                continue;
            }
        }

        /* Block math: \[...\] */
        if (i + 1 < len && md[i] == '\\' && md[i + 1] == '[') {
            size_t j = i + 2;
            while (j + 1 < len && !(md[j] == '\\' && md[j + 1] == ']')) j++;
            if (j + 1 < len) {
                size_t expr_len = j - (i + 2);
                math_registry_add(blocks, md + i + 2, expr_len);
                char placeholder[64];
                snprintf(placeholder, sizeof(placeholder), "@@MATH_BLOCK_%d@@", blocks->count - 1);
                cwist_sstring_append(out, placeholder);
                i = j + 2;
                continue;
            }
        }

        /* Inline math: $...$ */
        if (md[i] == '$' && (i == 0 || md[i - 1] != '\\')) {
            size_t j = i + 1;
            while (j < len && md[j] != '$' && md[j] != '\n') j++;
            if (j < len && md[j] == '$') {
                size_t expr_len = j - (i + 1);
                if (expr_len > 0) {
                    math_registry_add(inlines, md + i + 1, expr_len);
                    char placeholder[64];
                    snprintf(placeholder, sizeof(placeholder), "@@MATH_INLINE_%d@@", inlines->count - 1);
                    cwist_sstring_append(out, placeholder);
                    i = j + 1;
                    continue;
                }
            }
        }

        /* Inline math: \(...\) */
        if (i + 1 < len && md[i] == '\\' && md[i + 1] == '(') {
            size_t j = i + 2;
            while (j + 1 < len && !(md[j] == '\\' && md[j + 1] == ')')) j++;
            if (j + 1 < len) {
                size_t expr_len = j - (i + 2);
                if (expr_len > 0) {
                    math_registry_add(inlines, md + i + 2, expr_len);
                    char placeholder[64];
                    snprintf(placeholder, sizeof(placeholder), "@@MATH_INLINE_%d@@", inlines->count - 1);
                    cwist_sstring_append(out, placeholder);
                    i = j + 2;
                    continue;
                }
            }
        }

        cwist_sstring_append_len(out, md + i, 1);
        i++;
    }

    char *result = strdup(out->data);
    cwist_sstring_destroy(out);
    return result;
}

static void restore_math(cwist_sstring *html, const math_registry_t *blocks, const math_registry_t *inlines) {
    const char *data = html->data;
    size_t len = strlen(data);
    cwist_sstring *out = cwist_sstring_create();
    size_t i = 0;

    while (i < len) {
        if (i + 13 <= len && strncmp(data + i, "@@MATH_BLOCK_", 13) == 0) {
            size_t j = i + 13;
            int idx = 0;
            while (j < len && data[j] >= '0' && data[j] <= '9') {
                idx = idx * 10 + (data[j] - '0');
                j++;
            }
            if (j + 2 <= len && strncmp(data + j, "@@", 2) == 0) {
                cwist_sstring_append(out, "<span class=\"math-block\">");
                if (idx >= 0 && idx < blocks->count) {
                    append_escaped_math(out, blocks->expressions[idx]);
                }
                cwist_sstring_append(out, "</span>");
                i = j + 2;
                continue;
            }
        } else if (i + 14 <= len && strncmp(data + i, "@@MATH_INLINE_", 14) == 0) {
            size_t j = i + 14;
            int idx = 0;
            while (j < len && data[j] >= '0' && data[j] <= '9') {
                idx = idx * 10 + (data[j] - '0');
                j++;
            }
            if (j + 2 <= len && strncmp(data + j, "@@", 2) == 0) {
                cwist_sstring_append(out, "<span class=\"math-inline\">");
                if (idx >= 0 && idx < inlines->count) {
                    append_escaped_math(out, inlines->expressions[idx]);
                }
                cwist_sstring_append(out, "</span>");
                i = j + 2;
                continue;
            }
        }
        cwist_sstring_append_len(out, data + i, 1);
        i++;
    }

    cwist_sstring_assign(html, out->data);
    cwist_sstring_destroy(out);
}

static void md_output_cb(const MD_CHAR *data, MD_SIZE size, void *userdata) {
    cwist_sstring *str = (cwist_sstring *)userdata;
    cwist_error_t err = cwist_sstring_append_len(str, data, size);
    if (err.errtype != CWIST_ERR_INT8 || err.error.err_i8 != ERR_SSTRING_OKAY) {
        size_t new_size = (size_t)(str->size * 1.25f);
        if (new_size < str->size + size) new_size = str->size + size;
        cwist_sstring_change_size(str, new_size, false);
        cwist_sstring_append_len(str, data, size);
    }
}

static void rewrite_tasfa_bootstrap(cwist_sstring *html) {
    const char *data = html->data;
    size_t len = strlen(data);
    cwist_sstring *out = cwist_sstring_create();
    size_t i = 0;
    while (i < len) {
        if (data[i] == '<') {
            const char *tag_name = NULL;
            bool is_media = false;
            bool is_link = false;
            if (i + 2 <= len && strncmp(data + i, "<a", 2) == 0) { tag_name = "a"; is_link = true; }
            else if (i + 4 <= len && strncmp(data + i, "<img", 4) == 0) { tag_name = "img"; is_media = true; }
            else if (i + 6 <= len && strncmp(data + i, "<video", 6) == 0) { tag_name = "video"; is_media = true; }
            else if (i + 6 <= len && strncmp(data + i, "<audio", 6) == 0) { tag_name = "audio"; is_media = true; }
            if (tag_name) {
                size_t j = i;
                while (j < len && data[j] != '>') j++;
                if (j < len) j++;
                size_t tag_len = j - i;
                char tag[2048];
                if (tag_len >= sizeof(tag)) tag_len = sizeof(tag) - 1;
                memcpy(tag, data + i, tag_len);
                tag[tag_len] = '\0';
                if (is_media) {
                    const char *src = strstr(tag, "src=\"");
                    if (src) {
                        src += 5;
                        if (strncmp(src, "/file/download/", 15) == 0) {
                            char rewritten[3072];
                            const char *value_end = strchr(src, '"');
                            size_t path_len = value_end ? (size_t)(value_end - src) : strlen(src);
                            char download_path[1024] = {0};
                            if (path_len < sizeof(download_path)) {
                                strncpy(download_path, src, path_len);
                            }
                            snprintf(rewritten, sizeof(rewritten), "<%s data-tasfa-download=\"%s\"", tag_name, download_path);
                            cwist_sstring_append(out, rewritten);
                            const char *after_name = data + i + strlen(tag_name) + 1;
                            const char *src_pos = strstr(after_name, "src=\"");
                            if (src_pos && src_pos < data + j) {
                                const char *before_src_end = src_pos;
                                cwist_sstring_append_len(out, after_name, (size_t)(before_src_end - after_name));
                                const char *src_end = strchr(src_pos + 5, '"');
                                if (src_end && src_end < data + j) {
                                    src_end++;
                                    cwist_sstring_append_len(out, src_end, (size_t)((data + j) - src_end));
                                }
                            } else {
                                cwist_sstring_append_len(out, data + i + strlen(tag_name) + 1, j - i - strlen(tag_name) - 1);
                            }
                            i = j;
                            continue;
                        }
                    }
                } else if (is_link) {
                    const char *href = strstr(tag, "href=\"");
                    if (href) {
                        href += 6;
                        const char *target = NULL;
                        if (strncmp(href, "/file/download/", 15) == 0) {
                            target = href;
                        } else if (strncmp(href, "https://oborona.zip/file/download/", 34) == 0) {
                            target = href + 19;
                        } else if (strncmp(href, "http://oborona.zip/file/download/", 33) == 0) {
                            target = href + 18;
                        }
                        if (target) {
                            cwist_sstring_append(out, "<a href=\"#\" data-tasfa-download-link=\"");
                            const char *value_end = strchr(target, '"');
                            if (value_end) cwist_sstring_append_len(out, target, (size_t)(value_end - target));
                            cwist_sstring_append(out, "\"");
                            const char *after_name = data + i + 2;
                            const char *href_pos = strstr(after_name, "href=\"");
                            if (href_pos && href_pos < data + j) {
                                cwist_sstring_append_len(out, after_name, (size_t)(href_pos - after_name));
                                const char *href_end = strchr(href_pos + 6, '"');
                                if (href_end && href_end < data + j) {
                                    href_end++;
                                    cwist_sstring_append_len(out, href_end, (size_t)((data + j) - href_end));
                                }
                            } else {
                                cwist_sstring_append_len(out, data + i + 2, j - i - 2);
                            }
                            i = j;
                            continue;
                        }
                    }
                }
            }
        }
        cwist_sstring_append_len(out, data + i, 1);
        i++;
    }
    cwist_sstring_assign(html, out->data);
    cwist_sstring_destroy(out);
}

static void inject_img_attrs(cwist_sstring *html) {
    const char *data = html->data;
    size_t len = strlen(data);
    if (len == 0) return;

    cwist_sstring *out = cwist_sstring_create();
    size_t i = 0;
    int img_count = 0;

    while (i < len) {
        if (i + 4 <= len && strncmp(data + i, "<img", 4) == 0) {
            size_t tag_start = i;
            size_t j = i + 4;
            while (j < len && data[j] != '>') j++;
            if (j < len) j++; /* include '>' */

            /* Check if already has width/height inside this tag */
            bool has_width = false, has_height = false;
            const char *scan = data + tag_start;
            const char *tag_end_ptr = data + j;
            while (scan < tag_end_ptr - 6) {
                if (strncmp(scan, "width=", 6) == 0) has_width = true;
                if (strncmp(scan, "height=", 7) == 0) has_height = true;
                scan++;
            }

            /* Extract src */
            char src[512] = {0};
            const char *src_attr = strstr(data + tag_start, "src=\"");
            if (src_attr && src_attr < data + j) {
                src_attr += 5;
                const char *src_end = strchr(src_attr, '"');
                if (src_end && src_end < data + j) {
                    size_t src_len = (size_t)(src_end - src_attr);
                    if (src_len >= sizeof(src)) src_len = sizeof(src) - 1;
                    strncpy(src, src_attr, src_len);
                    src[src_len] = '\0';
                }
            }

            /* Copy tag content without the closing '>' */
            size_t close_pos = j - 1;
            cwist_sstring_append_len(out, data + tag_start, close_pos - tag_start);

            if (src[0] && !has_width && !has_height) {
                char path[512] = {0};
                if (strncmp(src, "/assets/img/", 12) == 0) {
                    snprintf(path, sizeof(path), "public/img/%s", src + 12);
                } else if (strncmp(src, "/assets/uploads/", 16) == 0) {
                    snprintf(path, sizeof(path), "public/uploads/%s", src + 16);
                } else if (strncmp(src, "/assets/profile/", 16) == 0) {
                    snprintf(path, sizeof(path), "public/profile/%s", src + 16);
                }

                if (path[0]) {
                    int w, h;
                    if (get_image_dimensions(path, &w, &h)) {
                        char attrs[256];
                        snprintf(attrs, sizeof(attrs),
                                 " width=\"%d\" height=\"%d\" style=\"aspect-ratio:%d/%d;background:var(--hover)\"",
                                 w, h, w, h);
                        cwist_sstring_append(out, attrs);
                    }
                }
            }

            /* loading / decoding */
            bool has_loading = false, has_decoding = false;
            scan = data + tag_start;
            while (scan < tag_end_ptr - 9) {
                if (strncmp(scan, "loading=", 8) == 0) has_loading = true;
                if (strncmp(scan, "decoding=", 9) == 0) has_decoding = true;
                scan++;
            }

            if (!has_loading) {
                if (img_count == 0) {
                    cwist_sstring_append(out, " loading=\"eager\" fetchpriority=\"high\"");
                } else {
                    cwist_sstring_append(out, " loading=\"lazy\"");
                }
            }
            if (!has_decoding) {
                cwist_sstring_append(out, " decoding=\"async\"");
            }

            cwist_sstring_append(out, ">"); /* close tag */
            img_count++;
            i = j;
        } else {
            cwist_sstring_append_len(out, data + i, 1);
            i++;
        }
    }

    cwist_sstring_assign(html, out->data);
    cwist_sstring_destroy(out);
}

cwist_sstring *render_markdown_to_html(const char *md) {
    math_registry_t blocks = {0}, inlines = {0};
    math_registry_init(&blocks);
    math_registry_init(&inlines);
    char *protected = protect_math(md, &blocks, &inlines);

    cwist_sstring *html = cwist_sstring_create();
    if (!html) {
        free(protected);
        math_registry_free(&blocks);
        math_registry_free(&inlines);
        return NULL;
    }
    cwist_sstring_assign(html, "");
    unsigned flags = MD_DIALECT_GITHUB | MD_FLAG_TABLES | MD_FLAG_PERMISSIVEAUTOLINKS | MD_FLAG_STRIKETHROUGH | MD_FLAG_TASKLISTS;
    int rc = md_html(protected, (MD_SIZE)strlen(protected), md_output_cb, html, flags, 0);
    free(protected);
    if (rc != 0) {
        cwist_sstring_destroy(html);
        math_registry_free(&blocks);
        math_registry_free(&inlines);
        return NULL;
    }
    restore_math(html, &blocks, &inlines);
    math_registry_free(&blocks);
    math_registry_free(&inlines);
    inject_img_attrs(html);
    rewrite_tasfa_bootstrap(html);
    return html;
}

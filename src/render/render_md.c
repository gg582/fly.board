#define _POSIX_C_SOURCE 200809L
#include "render.h"
#include "cwist/image_size.h"
#include <cwist/core/sstring/sstring.h>
#include <md4c-html.h>
#include <md4c.h>
#include <string.h>
#include <stdio.h>

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
    cwist_sstring *html = cwist_sstring_create();
    if (!html) return NULL;
    cwist_sstring_assign(html, "");
    unsigned flags = MD_DIALECT_GITHUB | MD_FLAG_TABLES | MD_FLAG_PERMISSIVEAUTOLINKS | MD_FLAG_STRIKETHROUGH | MD_FLAG_TASKLISTS;
    int rc = md_html(md, (MD_SIZE)strlen(md), md_output_cb, html, flags, 0);
    if (rc != 0) {
        cwist_sstring_destroy(html);
        return NULL;
    }
    inject_img_attrs(html);
    return html;
}

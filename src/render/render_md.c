#define _POSIX_C_SOURCE 200809L
#include "render.h"
#include <cwist/core/sstring/sstring.h>
#include <md4c-html.h>
#include <md4c.h>
#include <string.h>

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
    return html;
}

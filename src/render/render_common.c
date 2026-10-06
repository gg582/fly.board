#define _POSIX_C_SOURCE 200809L
#include "render_internal.h"
#include <cwist/core/mem/alloc.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

cwist_html_element_t *nav_link(const char *href, const char *label) {
    cwist_html_element_t *a = cwist_html_element_create("a");
    cwist_html_element_add_attr(a, "href", href);
    cwist_html_element_add_attr(a, "class", "nav-item");
    cwist_html_element_set_text(a, label);
    return a;
}

cwist_sstring *build_form(const char *title, const char *action, const char *method,
                          const char *fields_html, const char *btn_text, const char *error, bool dark) {
    (void)dark;
    cwist_sstring *s = cwist_sstring_create();
    cwist_sstring_assign(s, "<div class='card' style='max-width:420px;margin:40px auto;'>");
    cwist_sstring_append(s, "<h2 style='margin-top:0'>");
    cwist_sstring_append(s, title);
    cwist_sstring_append(s, "</h2>");
    if (error && error[0]) {
        cwist_sstring_append(s, "<div class='alert'>");
        cwist_sstring_append_escaped(s, error);
        cwist_sstring_append(s, "</div>");
    }
    cwist_sstring_append(s, "<form action='");
    cwist_sstring_append(s, action);
    cwist_sstring_append(s, "' method='");
    cwist_sstring_append(s, method);
    cwist_sstring_append(s, "'>");
    cwist_sstring_append(s, fields_html);
    cwist_sstring_append(s, "<button type='submit' class='btn' style='margin-top:8px;width:100%'>");
    cwist_sstring_append(s, btn_text);
    cwist_sstring_append(s, "</button></form></div>");
    return s;
}

const char *code_copy_script =
    "<script src='/assets/js/copy.js' defer></script>";

const char *login_register_script =
    "<script src='/assets/js/auth.js' defer></script>";

char *format_join_date(const char *iso_date) {
    static char buf[128];
    if (!iso_date || !iso_date[0]) {
        snprintf(buf, sizeof(buf), "Joined recently");
        return buf;
    }
    int year, mon, day;
    if (sscanf(iso_date, "%d-%d-%d", &year, &mon, &day) == 3) {
        const char *months[] = {"January","February","March","April","May","June",
                                "July","August","September","October","November","December"};
        if (mon >= 1 && mon <= 12) {
            snprintf(buf, sizeof(buf), "Signed in %s %d, %d", months[mon - 1], day, year);
            return buf;
        }
    }
    snprintf(buf, sizeof(buf), "Joined %s", iso_date);
    return buf;
}

/* ---- Blog helpers: URL segments, plain-text excerpts, lead images ---- */

void render_append_url_segment(cwist_sstring *b, const char *s) {
    static const char hex[] = "0123456789ABCDEF";
    char buf[4];
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
        unsigned char c = *p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            cwist_sstring_append_len(b, (const char *)p, 1);
        } else {
            buf[0] = '%';
            buf[1] = hex[c >> 4];
            buf[2] = hex[c & 15];
            buf[3] = '\0';
            cwist_sstring_append(b, buf);
        }
    }
}

/* Index just past the end of the line holding @p i (past its '\n'). */
static size_t skip_line(const char *s, size_t i) {
    while (s[i] && s[i] != '\n') i++;
    return s[i] ? i + 1 : i;
}

static bool word_byte(char c) {
    unsigned char u = (unsigned char)c;
    return u >= 0x80 || (u >= '0' && u <= '9') || (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z');
}

void render_text_excerpt(const char *md, size_t max_cp, char *out, size_t out_size) {
    if (!out || out_size == 0) return;
    out[0] = '\0';
    if (!md) return;
    size_t o = 0, cps = 0;
    bool space = false, line_start = true, in_fence = false;
    for (size_t i = 0; md[i];) {
        char c = md[i];
        if (line_start) {
            size_t j = i;
            while (md[j] == ' ' || md[j] == '\t') j++;
            if (strncmp(md + j, "```", 3) == 0 || strncmp(md + j, "~~~", 3) == 0 ||
                strncmp(md + j, "$$", 2) == 0) {
                in_fence = !in_fence;
                i = skip_line(md, j);
                space = o > 0;
                continue;
            }
            if (in_fence) {
                i = skip_line(md, j);
                continue;
            }
            /* Block markers: headings, quotes, list bullets, table pipes. */
            while (md[j] == '#' || md[j] == '>' || md[j] == '|') j++;
            if ((md[j] == '-' || md[j] == '*' || md[j] == '+') && md[j + 1] == ' ') j += 2;
            i = j;
            line_start = false;
            continue;
        }
        if (c == '\n') {
            line_start = true;
            space = o > 0;
            i++;
            continue;
        }
        if (c == '!' && md[i + 1] == '[') {
            /* Images carry no prose; drop alt and URL. */
            const char *close = strstr(md + i, "](");
            const char *end = close ? strchr(close, ')') : NULL;
            i = end ? (size_t)(end - md) + 1 : i + 2;
            continue;
        }
        if (c == '[') { i++; continue; }
        if (c == ']' && md[i + 1] == '(') {
            const char *end = strchr(md + i, ')');
            i = end ? (size_t)(end - md) + 1 : i + 1;
            continue;
        }
        if (c == '<') {
            const char *end = strchr(md + i, '>');
            if (end && end - (md + i) < 200) { i = (size_t)(end - md) + 1; space = o > 0; continue; }
        }
        /* Intraword '_' is not emphasis in CommonMark (snake_case). */
        if (c == '_' && i > 0 && word_byte(md[i - 1]) && word_byte(md[i + 1])) {
            /* fall through and keep it */
        } else if (c == '*' || c == '_' || c == '`' || c == '~' || c == '$' || c == '|') { i++; continue; }
        if (c == ' ' || c == '\t' || c == '\r') { space = o > 0; i++; continue; }

        size_t n = 1;
        unsigned char u = (unsigned char)c;
        if ((u & 0xE0) == 0xC0) n = 2;
        else if ((u & 0xF0) == 0xE0) n = 3;
        else if ((u & 0xF8) == 0xF0) n = 4;
        if (cps >= max_cp || o + (space ? 1 : 0) + n + 4 >= out_size) {
            memcpy(out + o, "\xE2\x80\xA6", 3); /* … */
            o += 3;
            break;
        }
        if (space) { out[o++] = ' '; space = false; }
        for (size_t k = 0; k < n && md[i]; k++) out[o++] = md[i++];
        cps++;
    }
    out[o] = '\0';
}

bool render_find_lead_image(const char *md, char *out, size_t out_size) {
    if (!md || !out || out_size == 0) return false;
    out[0] = '\0';
    for (const char *p = strstr(md, "!["); p; p = strstr(p + 2, "![")) {
        const char *close = strstr(p, "](");
        if (!close) return false;
        const char *url = close + 2;
        while (*url == ' ' || *url == '<') url++;
        size_t len = strcspn(url, " )>\"'\n");
        if (len == 0 || len >= out_size) continue;
        /* Only our own uploads or absolute http(s) images; a download link
         * is swapped for its plain image preview. */
        if (strncmp(url, "/file/download/", 15) == 0) {
            int id = atoi(url + 15);
            if (id <= 0) continue;
            snprintf(out, out_size, "/file/preview/%d", id);
            return true;
        }
        if (url[0] == '/' || strncmp(url, "https://", 8) == 0 || strncmp(url, "http://", 7) == 0) {
            memcpy(out, url, len);
            out[len] = '\0';
            return true;
        }
    }
    return false;
}

int render_reading_minutes(const char *md) {
    int words = 0;
    bool in_word = false;
    for (const char *p = md ? md : ""; *p; p++) {
        bool ws = *p == ' ' || *p == '\t' || *p == '\n' || *p == '\r';
        if (!ws && !in_word) words++;
        in_word = !ws;
    }
    /* Same rule as the editor's counter (public/js/editor.js). */
    return words ? (words + 219) / 220 : 0;
}

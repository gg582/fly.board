#define _POSIX_C_SOURCE 200809L
#include "render_internal.h"
#include <cwist/core/mem/alloc.h>
#include <string.h>
#include <stdio.h>

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
    "<script src='/assets/js/copy.js'></script>";

const char *login_register_script =
    "<script src='/assets/js/auth.js'></script>";

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

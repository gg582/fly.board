#define _POSIX_C_SOURCE 200809L
#include "handlers_internal.h"
#include "audit/audit.h"
#include "engine/bdr.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <time.h>
#include <unistd.h>

#define FONT_UPLOAD_MAX (12 * 1024 * 1024)
#define FONT_SCRIPT_MAX (256 * 1024)

static const char *field_value(form_field_t *fields, const char *name) {
    form_field_t *field = form_find(fields, name);
    return field && field->data ? field->data : "";
}

static bool safe_url(const char *url) {
    if (!url[0]) return true;
    if (strncmp(url, "https://", 8) != 0 && strncmp(url, "http://", 7) != 0 && url[0] != '/') return false;
    for (const unsigned char *p = (const unsigned char *)url; *p; p++) {
        if (*p <= 32 || *p == '\"' || *p == '\'' || *p == '<' || *p == '>' || *p == '\\') return false;
    }
    return true;
}

static bool safe_font_css(const char *value) {
    if (!value[0] || strlen(value) >= 512) return false;
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        if (*p == '{' || *p == '}' || *p == '<' || *p == '>' || *p == ';' || *p == '\n' || *p == '\r') return false;
    }
    return true;
}

static bool safe_family(const char *value) {
    if (!value[0] || strlen(value) >= sizeof(g_font_settings.face_family)) return false;
    for (const unsigned char *p = (const unsigned char *)value; *p; p++)
        if (!isalnum(*p) && *p != ' ' && *p != '-' && *p != '_') return false;
    return true;
}

static bool safe_spacing(const char *value) {
    if (strcmp(value, "normal") == 0) return true;
    char *end = NULL;
    errno = 0;
    (void)strtod(value, &end);
    if (errno || end == value) return false;
    return strcmp(end, "em") == 0 || strcmp(end, "rem") == 0 || strcmp(end, "px") == 0;
}

static bool save_upload(const char *path, form_field_t *field, mode_t mode) {
    char temporary[PATH_MAX];
    int needed = snprintf(temporary, sizeof(temporary), "%s.tmp.%ld", path, (long)getpid());
    if (needed < 0 || needed >= (int)sizeof(temporary)) return false;
    int fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL, mode);
    if (fd < 0) return false;
    bool ok = write(fd, field->data, field->file_size) == (ssize_t)field->file_size &&
              fsync(fd) == 0 && close(fd) == 0;
    if (!ok || rename(temporary, path) != 0) {
        if (fd >= 0) close(fd);
        unlink(temporary);
        return false;
    }
    return true;
}

static const char *font_extension(form_field_t *field) {
    const char *dot = field && field->filename ? strrchr(field->filename, '.') : NULL;
    if (!dot || field->file_size == 0 || field->file_size > FONT_UPLOAD_MAX) return NULL;
    if (strcasecmp(dot, ".ttf") == 0 || strcasecmp(dot, ".otf") == 0 ||
        strcasecmp(dot, ".woff") == 0 || strcasecmp(dot, ".woff2") == 0) return dot + 1;
    return NULL;
}

static bool update_theme_modes(const char *light, const char *dark) {
    FILE *source = fopen("blog.settings", "r");
    cwist_sstring *content = cwist_sstring_create();
    char line[2048];
    bool replaced = false;
    while (source && fgets(line, sizeof(line), source)) {
        if (strncmp(line, "use_special_modes=", 18) == 0) {
            cwist_sstring_append(content, "use_special_modes=");
            cwist_sstring_append(content, light);
            cwist_sstring_append(content, ",");
            cwist_sstring_append(content, dark);
            cwist_sstring_append(content, "\n");
            replaced = true;
        } else {
            cwist_sstring_append(content, line);
        }
    }
    if (source) fclose(source);
    if (!replaced) {
        cwist_sstring_append(content, "use_special_modes=");
        cwist_sstring_append(content, light);
        cwist_sstring_append(content, ",");
        cwist_sstring_append(content, dark);
        cwist_sstring_append(content, "\n");
    }
    char temporary[64];
    snprintf(temporary, sizeof(temporary), "blog.settings.tmp.%ld", (long)getpid());
    FILE *output = fopen(temporary, "w");
    if (!output) { cwist_sstring_destroy(content); return false; }
    bool ok = fputs(content->data ? content->data : "", output) >= 0 && fflush(output) == 0 &&
              fsync(fileno(output)) == 0 && fclose(output) == 0 && rename(temporary, "blog.settings") == 0;
    if (!ok) { fclose(output); unlink(temporary); }
    cwist_sstring_destroy(content);
    if (ok) theme_modes_load("blog.settings");
    return ok;
}

void handler_admin_appearance_get(cwist_http_request *req, cwist_http_response *res) {
    if (!auth_require_admin(req, res)) return;
    int uid = 0;
    char role[32] = {0};
    auth_is_logged_in(req, &uid, role, sizeof(role));
    char *profile = get_profile_pic(req->db, uid, role);
    cwist_sstring *page = render_admin_appearance(is_dark(req), profile, is_mobile_request(req),
                                                  cwist_query_map_get(req->query_params, "msg"));
    send_html_res(res, page);
    free(profile);
}

void handler_admin_appearance_post(cwist_http_request *req, cwist_http_response *res) {
    if (!auth_require_admin(req, res)) return;
    const char *type = cwist_http_header_get(req->headers, "Content-Type");
    const char *marker = type ? strstr(type, "boundary=") : NULL;
    if (!type || !strstr(type, "multipart/form-data") || !marker || !req->body) {
        res->status_code = CWIST_HTTP_BAD_REQUEST;
        return;
    }
    marker += 9;
    if (*marker == '\"') marker++;
    size_t boundary_length = strcspn(marker, "\"\r\n; ");
    char *boundary = cwist_alloc(boundary_length + 1);
    memcpy(boundary, marker, boundary_length);
    boundary[boundary_length] = '\0';
    form_field_t *fields = multipart_parse(req->body->data, req->body->size, boundary);
    cwist_free(boundary);

    form_field_t *theme_file = form_find(fields, "theme_file");
    char theme_error[160] = {0};
    bool has_theme = theme_file && theme_file->filename && theme_file->filename[0] && theme_file->file_size > 0;
    if (has_theme && !theme_custom_validate(theme_file->data, theme_file->file_size,
                                            theme_error, sizeof(theme_error))) {
        multipart_free(fields);
        redirect(res, "/admin/appearance?msg=invalid-theme");
        return;
    }

    struct { const char *name; char *value; size_t size; } families[] = {
        {"font_body", g_font_settings.body, sizeof(g_font_settings.body)},
        {"font_heading", g_font_settings.heading, sizeof(g_font_settings.heading)},
        {"font_ui", g_font_settings.ui, sizeof(g_font_settings.ui)},
        {"font_code", g_font_settings.code, sizeof(g_font_settings.code)},
        {"font_blockquote", g_font_settings.blockquote, sizeof(g_font_settings.blockquote)},
        {"font_display", g_font_settings.display, sizeof(g_font_settings.display)}
    };
    bool valid = true;
    for (size_t i = 0; i < sizeof(families) / sizeof(families[0]); i++) {
        const char *value = field_value(fields, families[i].name);
        if (!safe_font_css(value)) { valid = false; break; }
        snprintf(families[i].value, families[i].size, "%s", value);
    }
    struct { const char *prefix; const char *names[3]; char *values[3]; } tuning[] = {
        {"body", {"letter_spacing_body", "font_weight_body", NULL}, {g_font_settings.letter_spacing_body, g_font_settings.font_weight_body, NULL}},
        {"h1", {"letter_spacing_h1", "font_weight_h1", NULL}, {g_font_settings.letter_spacing_h1, g_font_settings.font_weight_h1, NULL}},
        {"h2", {"letter_spacing_h2", "font_weight_h2", NULL}, {g_font_settings.letter_spacing_h2, g_font_settings.font_weight_h2, NULL}},
        {"h3", {"letter_spacing_h3", "font_weight_h3", NULL}, {g_font_settings.letter_spacing_h3, g_font_settings.font_weight_h3, NULL}},
        {"h4", {"letter_spacing_h4", "font_weight_h4", NULL}, {g_font_settings.letter_spacing_h4, g_font_settings.font_weight_h4, NULL}},
        {"h5h6", {"letter_spacing_h5h6", "font_weight_h5h6", NULL}, {g_font_settings.letter_spacing_h5h6, g_font_settings.font_weight_h5h6, NULL}},
        {"topbar_title", {"letter_spacing_topbar_title", "font_weight_topbar_title", NULL}, {g_font_settings.letter_spacing_topbar_title, g_font_settings.font_weight_topbar_title, NULL}},
        {"btn", {"letter_spacing_btn", "font_weight_btn", NULL}, {g_font_settings.letter_spacing_btn, g_font_settings.font_weight_btn, NULL}},
        {"board_line_title", {"letter_spacing_board_line_title", "font_weight_board_line_title", NULL}, {g_font_settings.letter_spacing_board_line_title, g_font_settings.font_weight_board_line_title, NULL}},
        {"hero_h1", {"letter_spacing_hero_h1", "font_weight_hero_h1", NULL}, {g_font_settings.letter_spacing_hero_h1, g_font_settings.font_weight_hero_h1, NULL}},
        {"md_h1", {"letter_spacing_md_h1", "font_weight_md_h1", NULL}, {g_font_settings.letter_spacing_md_h1, g_font_settings.font_weight_md_h1, NULL}},
        {"md_h2", {"letter_spacing_md_h2", "font_weight_md_h2", NULL}, {g_font_settings.letter_spacing_md_h2, g_font_settings.font_weight_md_h2, NULL}},
        {"md_h3", {"letter_spacing_md_h3", "font_weight_md_h3", NULL}, {g_font_settings.letter_spacing_md_h3, g_font_settings.font_weight_md_h3, NULL}},
        {"post_h1", {"letter_spacing_post_h1", "font_weight_post_h1", NULL}, {g_font_settings.letter_spacing_post_h1, g_font_settings.font_weight_post_h1, NULL}}
    };
    for (size_t i = 0; valid && i < sizeof(tuning) / sizeof(tuning[0]); i++) {
        const char *spacing = field_value(fields, tuning[i].names[0]);
        const char *weight = field_value(fields, tuning[i].names[1]);
        int numeric_weight = atoi(weight);
        if (!safe_spacing(spacing) || numeric_weight < 100 || numeric_weight > 900 ||
            snprintf(NULL, 0, "%d", numeric_weight) != (int)strlen(weight)) {
            valid = false;
            break;
        }
        snprintf(tuning[i].values[0], 16, "%s", spacing);
        snprintf(tuning[i].values[1], 8, "%s", weight);
    }
    const char *spacing_hero = field_value(fields, "letter_spacing_hero_p");
    if (!safe_spacing(spacing_hero)) valid = false;
    else snprintf(g_font_settings.letter_spacing_hero_p, sizeof(g_font_settings.letter_spacing_hero_p), "%s", spacing_hero);
    const char *import_url = field_value(fields, "font_import_url");
    const char *script_url = field_value(fields, "font_script_url");
    if (!safe_url(import_url) || !safe_url(script_url) || !safe_family(field_value(fields, "font_face_family"))) valid = false;
    const char *layout_names[] = {"navigation", "content_style", "density", "list_style", "content_width", "panel_style"};
    char *layout_values[] = {g_layout_settings.navigation, g_layout_settings.content_style,
                             g_layout_settings.density, g_layout_settings.list_style, g_layout_settings.content_width,
                             g_layout_settings.panel_style};
    for (size_t i = 0; i < sizeof(layout_names) / sizeof(layout_names[0]); i++) {
        const char *value = field_value(fields, layout_names[i]);
        if (!layout_value_valid(layout_names[i], value)) { valid = false; break; }
        snprintf(layout_values[i], 16, "%s", value);
    }
    const char *panel_width = field_value(fields, "panel_width");
    const char *panel_x = field_value(fields, "panel_x");
    const char *panel_y = field_value(fields, "panel_y");
    const char *panel_color = field_value(fields, "panel_color");
    if (!layout_value_valid("panel_width", panel_width) || !layout_value_valid("panel_x", panel_x) ||
        !layout_value_valid("panel_y", panel_y) || !layout_value_valid("panel_color", panel_color)) {
        valid = false;
    } else {
        g_layout_settings.panel_width = atoi(panel_width);
        g_layout_settings.panel_x = atoi(panel_x);
        g_layout_settings.panel_y = atoi(panel_y);
        snprintf(g_layout_settings.panel_color, sizeof(g_layout_settings.panel_color), "%s", panel_color);
    }

    form_field_t *font_file = form_find(fields, "font_file");
    const char *extension = font_extension(font_file);
    if (font_file && font_file->filename && font_file->filename[0] && !extension) valid = false;
    form_field_t *script_file = form_find(fields, "font_script_file");
    bool has_script = script_file && script_file->filename && script_file->filename[0];
    size_t script_name_length = has_script ? strlen(script_file->filename) : 0;
    if (has_script && (script_file->file_size == 0 || script_file->file_size > FONT_SCRIPT_MAX ||
                       script_name_length < 3 || strcmp(script_file->filename + script_name_length - 3, ".js") != 0)) valid = false;
    if (!valid) {
        multipart_free(fields);
        redirect(res, "/admin/appearance?msg=invalid-font");
        return;
    }

    snprintf(g_font_settings.import_url, sizeof(g_font_settings.import_url), "%s", import_url);
    snprintf(g_font_settings.script_url, sizeof(g_font_settings.script_url), "%s", script_url);
    bool files_ok = true;
    if (extension) {
        char path[128], source[256];
        snprintf(path, sizeof(path), "public/fonts/appearance-font.%s", extension);
        snprintf(g_font_settings.face_family, sizeof(g_font_settings.face_family), "%s", field_value(fields, "font_face_family"));
        snprintf(source, sizeof(source), "url('/assets/fonts/appearance-font.%s?v=%lld')", extension, (long long)time(NULL));
        snprintf(g_font_settings.face_src, sizeof(g_font_settings.face_src), "%s", source);
        for (size_t i = 0; i < sizeof(families) / sizeof(families[0]); i++) {
            if (strstr(families[i].value, g_font_settings.face_family)) continue;
            char stack[256];
            int needed = snprintf(stack, sizeof(stack), "'%s', %s", g_font_settings.face_family, families[i].value);
            if (needed > 0 && needed < (int)sizeof(stack)) snprintf(families[i].value, families[i].size, "%s", stack);
        }
        files_ok = save_upload(path, font_file, 0644);
    }
    if (has_script) {
        files_ok = files_ok && save_upload("public/js/appearance-webfont.js", script_file, 0644);
        if (files_ok) snprintf(g_font_settings.script_url, sizeof(g_font_settings.script_url), "/assets/js/appearance-webfont.js?v=%lld", (long long)time(NULL));
    }
    if (has_theme) files_ok = files_ok && theme_custom_save("data/custom-theme.json", theme_file->data,
                                                            theme_file->file_size, theme_error, sizeof(theme_error));
    const char *light = field_value(fields, "light_theme");
    const char *dark = field_value(fields, "dark_theme");
    const char *allowed[] = {"light", "dark", "ocean", "forest", "sepia", "paper",
                             "glacier", "rose", "midnight", "plum", "ember", "custom"};
    for (int pass = 0; pass < 2; pass++) {
        const char *selected = pass == 0 ? light : dark;
        bool found = false;
        for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++) {
            if (strcmp(selected, allowed[i]) == 0 && (strcmp(selected, "custom") != 0 || theme_custom_available())) found = true;
        }
        if (!found) files_ok = false;
    }
    bool ok = files_ok && layout_settings_save("layout.settings") &&
              font_settings_save("fonts.settings") && update_theme_modes(light, dark);
    if (ok) {
        int uid = 0;
        char role[32] = {0};
        auth_is_logged_in(req, &uid, role, sizeof(role));
        audit_record(uid, "admin.appearance.update", "site", "success", "layout, theme, and typography");
        page_cache_invalidate_all();
        engine_bdr_bump_appearance();
    }
    multipart_free(fields);
    redirect(res, ok ? "/admin/appearance?msg=saved" : "/admin/appearance?msg=error");
}

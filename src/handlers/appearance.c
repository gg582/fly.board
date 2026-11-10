#define _POSIX_C_SOURCE 200809L
#include "handlers_internal.h"
#include "audit/audit.h"
#include "engine/bdr.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <limits.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define FONT_UPLOAD_MAX (12 * 1024 * 1024)
#define FONT_SCRIPT_MAX (256 * 1024)
#define THEME_FILE_MAX (512 * 1024)
/* Appearance files are uploaded in chunks before the form is submitted, so
 * no single request comes near the transport's body limit. */
#define STAGE_DIR "data/appearance-staging"
#define STAGE_CHUNK_MAX (4 * 1024 * 1024)
#define STAGE_MAX_AGE (60 * 60)

static const struct { const char *slot; size_t max; } g_stage_slots[] = {
    {"font_file_body", FONT_UPLOAD_MAX},
    {"font_file_heading", FONT_UPLOAD_MAX},
    {"font_file_ui", FONT_UPLOAD_MAX},
    {"font_file_code", FONT_UPLOAD_MAX},
    {"font_file_blockquote", FONT_UPLOAD_MAX},
    {"font_file_display", FONT_UPLOAD_MAX},
    {"font_script_file", FONT_SCRIPT_MAX},
    {"theme_file", THEME_FILE_MAX},
};

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

/* Font stacks are stored as CSS text; uploaded families are kept at the
 * front as "'Family', <rest>". */
static void stack_remove_family(char *stack, size_t size, const char *family) {
    if (!family || !family[0]) return;
    char quoted[80];
    snprintf(quoted, sizeof(quoted), "'%s'", family);
    size_t ql = strlen(quoted);
    for (int pass = 0; pass < 4; pass++) {
        char *hit = strstr(stack, quoted);
        if (!hit) return;
        char *end = hit + ql;
        while (*end == ' ') end++;
        if (*end == ',') end++;
        while (*end == ' ') end++;
        memmove(hit, end, strlen(end) + 1);
    }
    (void)size;
}

static void stack_add_family(char *stack, size_t size, const char *family) {
    char quoted[80];
    snprintf(quoted, sizeof(quoted), "'%s'", family);
    if (strstr(stack, quoted)) return;
    char next[512];
    int needed = snprintf(next, sizeof(next), stack[0] ? "%s, %s" : "%s%s", quoted, stack);
    if (needed > 0 && (size_t)needed < size) snprintf(stack, size, "%s", next);
}

static void remove_role_fonts(const char *role) {
    DIR *dir = opendir("public/fonts");
    if (!dir) return;
    char prefix[48];
    snprintf(prefix, sizeof(prefix), "appearance-%s-", role);
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, prefix, strlen(prefix)) != 0) continue;
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "public/fonts/%s", entry->d_name);
        unlink(path);
    }
    closedir(dir);
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

static size_t stage_slot_max(const char *slot) {
    for (size_t i = 0; i < sizeof(g_stage_slots) / sizeof(g_stage_slots[0]); i++)
        if (slot && strcmp(slot, g_stage_slots[i].slot) == 0) return g_stage_slots[i].max;
    return 0;
}

static bool stage_token_valid(const char *token) {
    if (!token || strlen(token) != 32) return false;
    for (const char *p = token; *p; p++)
        if (!isxdigit((unsigned char)*p) || isupper((unsigned char)*p)) return false;
    return true;
}

static bool stage_path(char *out, size_t out_size, const char *token, const char *slot) {
    if (!stage_token_valid(token) || !stage_slot_max(slot)) return false;
    int needed = snprintf(out, out_size, STAGE_DIR "/%s-%s", token, slot);
    return needed > 0 && (size_t)needed < out_size;
}

static void stage_sweep(void) {
    DIR *dir = opendir(STAGE_DIR);
    if (!dir) return;
    time_t now = time(NULL);
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        char path[PATH_MAX];
        struct stat st;
        snprintf(path, sizeof(path), STAGE_DIR "/%s", entry->d_name);
        if (stat(path, &st) == 0 && now - st.st_mtime > STAGE_MAX_AGE) unlink(path);
    }
    closedir(dir);
}

static void stage_reply(cwist_http_response *res, int status, const char *json) {
    res->status_code = (cwist_http_status_t)status;
    cwist_http_header_add(&res->headers, "Content-Type", "application/json");
    cwist_http_header_add(&res->headers, "Cache-Control", "private, no-store");
    cwist_sstring_assign(res->body, json);
}

/* POST /admin/appearance/upload?slot=&token=&offset=&total= with a raw chunk
 * body. Chunks must arrive in order; offset 0 starts the file over. */
void handler_admin_appearance_upload_post(cwist_http_request *req, cwist_http_response *res) {
    if (!auth_require_admin(req, res)) return;
    const char *slot = cwist_query_map_get(req->query_params, "slot");
    const char *token = cwist_query_map_get(req->query_params, "token");
    const char *offset_text = cwist_query_map_get(req->query_params, "offset");
    const char *total_text = cwist_query_map_get(req->query_params, "total");
    char path[PATH_MAX];
    size_t max = stage_slot_max(slot);
    long long offset = offset_text ? atoll(offset_text) : -1;
    long long total = total_text ? atoll(total_text) : -1;
    size_t length = req->body && req->body->data ? req->body->size : 0;
    if (!stage_path(path, sizeof(path), token, slot) || offset < 0 || total <= 0 ||
        (size_t)total > max || length == 0 || length > STAGE_CHUNK_MAX ||
        (unsigned long long)offset + length > (unsigned long long)total) {
        stage_reply(res, (size_t)total > max && max ? 413 : 400, "{\"ok\":false,\"error\":\"invalid chunk\"}");
        return;
    }
    if (offset == 0) {
        mkdir("data", 0755);
        mkdir(STAGE_DIR, 0700);
        stage_sweep();
    }
    int fd = open(path, O_WRONLY | O_CREAT | (offset == 0 ? O_TRUNC : 0), 0600);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0 || st.st_size != offset) {
        if (fd >= 0) close(fd);
        stage_reply(res, 409, "{\"ok\":false,\"error\":\"chunk out of order\"}");
        return;
    }
    bool ok = pwrite(fd, req->body->data, length, (off_t)offset) == (ssize_t)length;
    ok = close(fd) == 0 && ok;
    if (!ok) {
        unlink(path);
        stage_reply(res, 500, "{\"ok\":false,\"error\":\"write failed\"}");
        return;
    }
    char reply[64];
    snprintf(reply, sizeof(reply), "{\"ok\":true,\"received\":%lld}", offset + (long long)length);
    stage_reply(res, 200, reply);
}

/* Swap staged uploads into the parsed form as if they had been sent inline.
 * Returns false when a referenced staged file is missing or oversized. */
static bool stage_attach(form_field_t **fields) {
    bool ok = true;
    for (size_t i = 0; i < sizeof(g_stage_slots) / sizeof(g_stage_slots[0]); i++) {
        const char *slot = g_stage_slots[i].slot;
        char key[64], name_key[64], path[PATH_MAX];
        snprintf(key, sizeof(key), "staged_%s", slot);
        snprintf(name_key, sizeof(name_key), "staged_%s_name", slot);
        const char *token = field_value(*fields, key);
        if (!token[0]) continue;
        const char *filename = field_value(*fields, name_key);
        if (!stage_path(path, sizeof(path), token, slot) || !filename[0]) { ok = false; continue; }
        FILE *f = fopen(path, "rb");
        struct stat st;
        if (!f || fstat(fileno(f), &st) != 0 || st.st_size <= 0 || (size_t)st.st_size > g_stage_slots[i].max) {
            if (f) fclose(f);
            unlink(path);
            ok = false;
            continue;
        }
        size_t size = (size_t)st.st_size;
        char *data = cwist_alloc(size + 1);
        bool read_ok = data && fread(data, 1, size, f) == size;
        fclose(f);
        unlink(path);
        if (!read_ok) { if (data) cwist_free(data); ok = false; continue; }
        data[size] = '\0';
        form_field_t *field = form_find(*fields, slot);
        if (!field) {
            field = cwist_alloc(sizeof(*field));
            if (!field) { cwist_free(data); ok = false; continue; }
            memset(field, 0, sizeof(*field));
            field->name = cwist_alloc(strlen(slot) + 1);
            if (field->name) strcpy(field->name, slot);
            field->next = *fields;
            *fields = field;
        }
        if (field->data) cwist_free(field->data);
        if (field->filename) cwist_free(field->filename);
        field->data = data;
        field->len = size;
        field->file_size = size;
        field->filename = cwist_alloc(strlen(filename) + 1);
        if (field->filename) strcpy(field->filename, filename);
    }
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
    if (!stage_attach(&fields)) {
        multipart_free(fields);
        redirect(res, "/admin/appearance?msg=upload-expired");
        return;
    }

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
    if (!safe_url(import_url) || !safe_url(script_url)) valid = false;
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
    const char *panel_side = field_value(fields, "panel_side");
    const char *panel_radius = field_value(fields, "panel_radius");
    const char *panel_color = field_value(fields, "panel_color");
    /* "custom" carries its pixel value in panel_radius_px. */
    if (strcmp(panel_radius, "custom") == 0) panel_radius = field_value(fields, "panel_radius_px");
    if (!layout_value_valid("panel_width", panel_width) || !layout_value_valid("panel_side", panel_side) ||
        !layout_value_valid("panel_radius", panel_radius) || !layout_value_valid("panel_color", panel_color)) {
        valid = false;
    } else {
        g_layout_settings.panel_width = atoi(panel_width);
        snprintf(g_layout_settings.panel_side, sizeof(g_layout_settings.panel_side), "%s", panel_side);
        snprintf(g_layout_settings.panel_radius, sizeof(g_layout_settings.panel_radius), "%s", panel_radius);
        snprintf(g_layout_settings.panel_color, sizeof(g_layout_settings.panel_color), "%s", panel_color);
    }
    const char *code_light = field_value(fields, "code_theme_light");
    const char *code_dark = field_value(fields, "code_theme_dark");
    if (!code_theme_valid(code_light) || !code_theme_valid(code_dark)) {
        valid = false;
    } else {
        snprintf(g_layout_settings.code_theme_light, sizeof(g_layout_settings.code_theme_light), "%s", code_light);
        snprintf(g_layout_settings.code_theme_dark, sizeof(g_layout_settings.code_theme_dark), "%s", code_dark);
    }
    if (form_find(fields, "sections_form")) {
        char disabled[sizeof(g_layout_settings.disabled_sections)] = "";
        size_t used = 0;
        for (size_t i = 0; i < g_site_section_count; i++) {
            char key[48];
            snprintf(key, sizeof(key), "section_%s", g_site_sections[i].id);
            if (form_find(fields, key)) continue;
            int w = snprintf(disabled + used, sizeof(disabled) - used, "%s%s", used ? "," : "",
                             g_site_sections[i].id);
            if (w > 0 && (size_t)w < sizeof(disabled) - used) used += (size_t)w;
        }
        snprintf(g_layout_settings.disabled_sections, sizeof(g_layout_settings.disabled_sections), "%s", disabled);
    }
    const char *default_mode = field_value(fields, "default_mode");
    if (default_mode && default_mode[0]) {
        if (!layout_value_valid("default_mode", default_mode)) valid = false;
        else snprintf(g_layout_settings.default_mode, sizeof(g_layout_settings.default_mode), "%s", default_mode);
    }
    g_layout_settings.invert_transparent_images = form_find(fields, "invert_transparent_images") != NULL ||
                                                  form_find(fields, "invert_transparent_svg") != NULL;
    g_layout_settings.transparent_image_formats[0] = '\0';
    if (form_find(fields, "transparent_format_all")) {
        snprintf(g_layout_settings.transparent_image_formats, sizeof(g_layout_settings.transparent_image_formats), "all");
    } else {
        const char *formats[] = {"svg", "png", "webp", "gif", "avif", "jpg", "bmp", "ico"};
        size_t used = 0;
        for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) {
            char name[64];
            snprintf(name, sizeof(name), "transparent_format_%s", formats[i]);
            if (!form_find(fields, name)) continue;
            int written = snprintf(g_layout_settings.transparent_image_formats + used,
                                   sizeof(g_layout_settings.transparent_image_formats) - used,
                                   "%s%s", used ? "," : "", formats[i]);
            if (written < 0 || (size_t)written >= sizeof(g_layout_settings.transparent_image_formats) - used) {
                valid = false;
                break;
            }
            used += (size_t)written;
        }
    }
    if (g_layout_settings.invert_transparent_images && !g_layout_settings.transparent_image_formats[0]) valid = false;
    if (!g_layout_settings.transparent_image_formats[0])
        snprintf(g_layout_settings.transparent_image_formats, sizeof(g_layout_settings.transparent_image_formats), "svg");

    /* One optional uploaded font per role; each only joins its own stack. */
    const char *role_ext[FONT_ROLE_COUNT] = {0};
    form_field_t *role_file[FONT_ROLE_COUNT] = {0};
    char role_family[FONT_ROLE_COUNT][64];
    for (int r = 0; r < FONT_ROLE_COUNT; r++) {
        char name[48];
        snprintf(name, sizeof(name), "font_file_%s", g_font_roles[r]);
        role_file[r] = form_find(fields, name);
        bool chosen = role_file[r] && role_file[r]->filename && role_file[r]->filename[0];
        role_ext[r] = chosen ? font_extension(role_file[r]) : NULL;
        if (chosen && !role_ext[r]) valid = false;
        snprintf(name, sizeof(name), "font_face_family_%s", g_font_roles[r]);
        const char *family = field_value(fields, name);
        if (!family[0]) family = g_font_settings.role_family[r][0] ? g_font_settings.role_family[r] : "";
        snprintf(role_family[r], sizeof(role_family[r]), "%s", family);
        if (!role_family[r][0]) snprintf(role_family[r], sizeof(role_family[r]), "Custom %s", g_font_roles[r]);
        if (!safe_family(role_family[r])) valid = false;
    }
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
    char *role_stack[FONT_ROLE_COUNT] = {g_font_settings.body, g_font_settings.heading, g_font_settings.ui,
                                         g_font_settings.code, g_font_settings.blockquote, g_font_settings.display};
    /* The legacy single upload applied to every stack; it can be removed. */
    if (form_find(fields, "font_clear_legacy") && g_font_settings.face_src[0]) {
        for (int r = 0; r < FONT_ROLE_COUNT; r++) stack_remove_family(role_stack[r], 256, g_font_settings.face_family);
        g_font_settings.face_src[0] = '\0';
    }
    for (int r = 0; files_ok && r < FONT_ROLE_COUNT; r++) {
        char name[48];
        snprintf(name, sizeof(name), "font_clear_%s", g_font_roles[r]);
        bool clear = form_find(fields, name) != NULL && !role_ext[r];
        if ((clear || role_ext[r]) && g_font_settings.role_src[r][0]) {
            stack_remove_family(role_stack[r], 256, g_font_settings.role_family[r]);
            remove_role_fonts(g_font_roles[r]);
            g_font_settings.role_src[r][0] = '\0';
            g_font_settings.role_family[r][0] = '\0';
        }
        if (role_ext[r]) {
            char path[160], source[256];
            long long stamp = (long long)time(NULL);
            snprintf(path, sizeof(path), "public/fonts/appearance-%s-%lld.%s", g_font_roles[r], stamp, role_ext[r]);
            snprintf(source, sizeof(source), "url('/assets/fonts/appearance-%s-%lld.%s')", g_font_roles[r], stamp, role_ext[r]);
            files_ok = save_upload(path, role_file[r], 0644);
            if (files_ok) {
                snprintf(g_font_settings.role_family[r], sizeof(g_font_settings.role_family[r]), "%s", role_family[r]);
                snprintf(g_font_settings.role_src[r], sizeof(g_font_settings.role_src[r]), "%s", source);
                stack_add_family(role_stack[r], 256, role_family[r]);
            }
        } else if (g_font_settings.role_src[r][0]) {
            /* Renaming an existing upload's family follows into its stack. */
            if (strcmp(g_font_settings.role_family[r], role_family[r]) != 0) {
                stack_remove_family(role_stack[r], 256, g_font_settings.role_family[r]);
                snprintf(g_font_settings.role_family[r], sizeof(g_font_settings.role_family[r]), "%s", role_family[r]);
            }
            stack_add_family(role_stack[r], 256, role_family[r]);
        }
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

#include "config.h"
#include <cwist/core/log.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

blog_config_t g_config = {0};
font_settings_t g_font_settings = {0};

static long long parse_size_bytes(const char *value, long long def) {
    if (!value || !value[0]) return def;
    char *end = NULL;
    double amount = strtod(value, &end);
    if (end == value || amount <= 0.0) return def;
    while (*end == ' ' || *end == '\t') end++;
    long long scale = 1;
    if (*end) {
        if (strcasecmp(end, "k") == 0 || strcasecmp(end, "kb") == 0) scale = 1024LL;
        else if (strcasecmp(end, "m") == 0 || strcasecmp(end, "mb") == 0) scale = 1024LL * 1024LL;
        else if (strcasecmp(end, "g") == 0 || strcasecmp(end, "gb") == 0) scale = 1024LL * 1024LL * 1024LL;
        else if (strcasecmp(end, "t") == 0 || strcasecmp(end, "tb") == 0) scale = 1024LL * 1024LL * 1024LL * 1024LL;
        else return def;
    }
    return (long long)(amount * (double)scale);
}

static int clamp_int_config(int value, int min_value, int max_value) {
    if (value < min_value) return min_value;
    if (value > max_value) return max_value;
    return value;
}

static bool is_safe_image_name(const char *name) {
    if (!name || !name[0]) return false;
    return strchr(name, '/') == NULL && strchr(name, '\\') == NULL;
}

static void validate_image_setting(char *filename, const char *setting_name) {
    if (!filename || !filename[0]) return;
    if (!is_safe_image_name(filename)) {
        CWIST_LOG_WARN("Ignoring unsafe %s value: %s", setting_name, filename);
        filename[0] = '\0';
        return;
    }
    char path[512];
    int n = snprintf(path, sizeof(path), "public/img/%s", filename);
    if (n < 0 || n >= (int)sizeof(path)) {
        CWIST_LOG_WARN("Ignoring too-long %s value: %s", setting_name, filename);
        filename[0] = '\0';
        return;
    }
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        CWIST_LOG_WARN("Configured %s not found at %s, ignoring", setting_name, path);
        filename[0] = '\0';
    }
}

static void set_default(void) {
    snprintf(g_config.title, sizeof(g_config.title), "CWIST Docker Blog");
    snprintf(g_config.subtitle, sizeof(g_config.subtitle), "Explore boards and read stories.");
    snprintf(g_config.brand_footer, sizeof(g_config.brand_footer), "Built with CWIST C Framework");
    snprintf(g_config.accent, sizeof(g_config.accent), "#3b82f6");
    g_config.port = 8443;
    g_config.use_tasfa = true;
    g_config.use_rss = false;
    g_config.use_tls = true;
    g_config.use_http2 = true;
    g_config.use_http3 = true;
    g_config.roundness = 0.0f;
    g_config.max_upload_size = 1024LL * 1024LL * 1024LL;
    g_config.max_total_parallel_uploads = 8;
    g_config.max_upload_parallel_chunks = 32;
    g_config.max_concurrent_downloads = 128;
}

static void trim_newline(char *s) {
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r')) {
        s[len - 1] = '\0';
        len--;
    }
}

bool blog_config_load(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        set_default();
        f = fopen(path, "w");
        if (f) {
            fprintf(f, "title=%s\n", g_config.title);
            fprintf(f, "subtitle=%s\n", g_config.subtitle);
            fprintf(f, "brand_footer=%s\n", g_config.brand_footer);
            fprintf(f, "accent=%s\n", g_config.accent);
            fprintf(f, "port=%d\n", g_config.port);
            fprintf(f, "home_img=%s\n", g_config.home_img);
            fprintf(f, "blog_logo=%s\n", g_config.blog_logo);
            fprintf(f, "boards_img=%s\n", g_config.boards_img);
            fprintf(f, "files_img=%s\n", g_config.files_img);
            fprintf(f, "favicon=%s\n", g_config.favicon);
            fprintf(f, "root_url=%s\n", g_config.root_url);
            fprintf(f, "use_tasfa=%s\n", g_config.use_tasfa ? "true" : "false");
            fprintf(f, "use_rss=%s\n", g_config.use_rss ? "true" : "false");
            fprintf(f, "use_tls=%s\n", g_config.use_tls ? "true" : "false");
            fprintf(f, "use_http2=%s\n", g_config.use_http2 ? "true" : "false");
            fprintf(f, "use_http3=%s\n", g_config.use_http3 ? "true" : "false");
            fprintf(f, "roundness=%.2f\n", g_config.roundness);
            fprintf(f, "max_upload_size=1G\n");
            fprintf(f, "max_total_parallel_uploads=%d\n", g_config.max_total_parallel_uploads);
            fprintf(f, "max_upload_parallel_chunks=%d\n", g_config.max_upload_parallel_chunks);
            fprintf(f, "max_concurrent_downloads=%d\n", g_config.max_concurrent_downloads);
            fclose(f);
        }
        return true;
    }
    set_default();
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        trim_newline(line);
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        const char *key = line;
        const char *val = eq + 1;
        if (strcmp(key, "title") == 0) {
            snprintf(g_config.title, sizeof(g_config.title), "%s", val);
        } else if (strcmp(key, "subtitle") == 0) {
            snprintf(g_config.subtitle, sizeof(g_config.subtitle), "%s", val);
        } else if (strcmp(key, "brand_footer") == 0) {
            snprintf(g_config.brand_footer, sizeof(g_config.brand_footer), "%s", val);
        } else if (strcmp(key, "accent") == 0) {
            snprintf(g_config.accent, sizeof(g_config.accent), "%s", val);
        } else if (strcmp(key, "port") == 0) {
            g_config.port = atoi(val);
        } else if (strcmp(key, "home_img") == 0) {
            snprintf(g_config.home_img, sizeof(g_config.home_img), "%s", val);
        } else if (strcmp(key, "blog_logo") == 0) {
            snprintf(g_config.blog_logo, sizeof(g_config.blog_logo), "%s", val);
        } else if (strcmp(key, "boards_img") == 0) {
            snprintf(g_config.boards_img, sizeof(g_config.boards_img), "%s", val);
        } else if (strcmp(key, "files_img") == 0) {
            snprintf(g_config.files_img, sizeof(g_config.files_img), "%s", val);
        } else if (strcmp(key, "favicon") == 0) {
            snprintf(g_config.favicon, sizeof(g_config.favicon), "%s", val);
        } else if (strcmp(key, "root_url") == 0) {
            snprintf(g_config.root_url, sizeof(g_config.root_url), "%s", val);
        } else if (strcmp(key, "use_tasfa") == 0) {
            g_config.use_tasfa = (strcmp(val, "true") == 0 || strcmp(val, "1") == 0);
        } else if (strcmp(key, "use_rss") == 0) {
            g_config.use_rss = (strcmp(val, "true") == 0 || strcmp(val, "1") == 0);
        } else if (strcmp(key, "use_tls") == 0) {
            g_config.use_tls = (strcmp(val, "true") == 0 || strcmp(val, "1") == 0);
        } else if (strcmp(key, "use_http2") == 0) {
            g_config.use_http2 = (strcmp(val, "true") == 0 || strcmp(val, "1") == 0);
        } else if (strcmp(key, "use_http3") == 0) {
            g_config.use_http3 = (strcmp(val, "true") == 0 || strcmp(val, "1") == 0);
        } else if (strcmp(key, "roundness") == 0) {
            g_config.roundness = strtof(val, NULL);
            if (g_config.roundness < 0.0f) g_config.roundness = 0.0f;
            if (g_config.roundness > 1.0f) g_config.roundness = 1.0f;
        } else if (strcmp(key, "max_upload_size") == 0) {
            g_config.max_upload_size = parse_size_bytes(val, g_config.max_upload_size);
        } else if (strcmp(key, "max_total_parallel_uploads") == 0) {
            g_config.max_total_parallel_uploads = atoi(val);
        } else if (strcmp(key, "max_upload_parallel_chunks") == 0) {
            g_config.max_upload_parallel_chunks = atoi(val);
        } else if (strcmp(key, "max_concurrent_downloads") == 0) {
            g_config.max_concurrent_downloads = atoi(val);
        }
    }
    fclose(f);
    if (!g_config.title[0]) snprintf(g_config.title, sizeof(g_config.title), "CWIST Docker Blog");
    if (!g_config.subtitle[0]) snprintf(g_config.subtitle, sizeof(g_config.subtitle), "Explore boards and read stories.");
    if (!g_config.brand_footer[0]) snprintf(g_config.brand_footer, sizeof(g_config.brand_footer), "Built with CWIST C Framework");
    if (!g_config.accent[0]) snprintf(g_config.accent, sizeof(g_config.accent), "#3b82f6");
    if (g_config.port <= 0 || g_config.port > 65535) g_config.port = 8443;
    if (g_config.max_upload_size <= 0) g_config.max_upload_size = 1024LL * 1024LL * 1024LL;
    g_config.max_total_parallel_uploads = clamp_int_config(g_config.max_total_parallel_uploads, 1, 512);
    g_config.max_upload_parallel_chunks = clamp_int_config(g_config.max_upload_parallel_chunks, 1, 64);
    g_config.max_concurrent_downloads = clamp_int_config(g_config.max_concurrent_downloads, 1, 512);

    /* Validate configured image assets so the renderer does not emit broken
       <img> tags that result in 404s in Firefox (and all other browsers). */
    validate_image_setting(g_config.home_img, "home_img");
    validate_image_setting(g_config.blog_logo, "blog_logo");
    validate_image_setting(g_config.boards_img, "boards_img");
    validate_image_setting(g_config.files_img, "files_img");
    validate_image_setting(g_config.favicon, "favicon");
    return true;
}

static void font_set_default(void) {
    /* Web fonts are inlined into every HTML response by render_page.c, so no
     * external @import or @font-face URLs are needed by default. Leaving these
     * empty avoids extra round trips and keeps the page self-contained. */
    g_font_settings.import_url[0] = '\0';
    snprintf(g_font_settings.face_family, sizeof(g_font_settings.face_family), "JetBrains Mono");
    g_font_settings.face_src[0] = '\0';
    snprintf(g_font_settings.body, sizeof(g_font_settings.body),
             "'Space Grotesk', 'IBM Plex Sans KR', 'Pretendard Variable', 'Pretendard', sans-serif");
    snprintf(g_font_settings.heading, sizeof(g_font_settings.heading), "'Outfit', sans-serif");
    snprintf(g_font_settings.ui, sizeof(g_font_settings.ui),
             "'Inter', 'IBM Plex Sans KR', 'Pretendard Variable', sans-serif");
    snprintf(g_font_settings.code, sizeof(g_font_settings.code),
             "'JetBrains Mono', 'Fira Code', 'D2Coding', Consolas, Monaco, 'Courier New', monospace");
    snprintf(g_font_settings.blockquote, sizeof(g_font_settings.blockquote),
             "'Source Serif 4', 'IBM Plex Sans KR', serif");
    snprintf(g_font_settings.display, sizeof(g_font_settings.display), "'Outfit', sans-serif");
    snprintf(g_font_settings.letter_spacing_body, sizeof(g_font_settings.letter_spacing_body), "-0.01em");
    snprintf(g_font_settings.letter_spacing_h1, sizeof(g_font_settings.letter_spacing_h1), "-0.05em");
    snprintf(g_font_settings.letter_spacing_h2, sizeof(g_font_settings.letter_spacing_h2), "-0.04em");
    snprintf(g_font_settings.letter_spacing_h3, sizeof(g_font_settings.letter_spacing_h3), "-0.03em");
    snprintf(g_font_settings.letter_spacing_h4, sizeof(g_font_settings.letter_spacing_h4), "-0.02em");
    snprintf(g_font_settings.letter_spacing_h5h6, sizeof(g_font_settings.letter_spacing_h5h6), "-0.01em");
    snprintf(g_font_settings.letter_spacing_topbar_title, sizeof(g_font_settings.letter_spacing_topbar_title), "-0.04em");
    snprintf(g_font_settings.letter_spacing_btn, sizeof(g_font_settings.letter_spacing_btn), "0.02em");
    snprintf(g_font_settings.letter_spacing_board_line_title, sizeof(g_font_settings.letter_spacing_board_line_title), "-0.045em");
    snprintf(g_font_settings.letter_spacing_hero_h1, sizeof(g_font_settings.letter_spacing_hero_h1), "-0.05em");
    snprintf(g_font_settings.letter_spacing_hero_p, sizeof(g_font_settings.letter_spacing_hero_p), "-0.01em");
    snprintf(g_font_settings.letter_spacing_md_h1, sizeof(g_font_settings.letter_spacing_md_h1), "-0.03em");
    snprintf(g_font_settings.letter_spacing_md_h2, sizeof(g_font_settings.letter_spacing_md_h2), "-0.02em");
    snprintf(g_font_settings.letter_spacing_md_h3, sizeof(g_font_settings.letter_spacing_md_h3), "-0.015em");
    snprintf(g_font_settings.letter_spacing_post_h1, sizeof(g_font_settings.letter_spacing_post_h1), "-0.03em");
    snprintf(g_font_settings.font_weight_body, sizeof(g_font_settings.font_weight_body), "450");
    snprintf(g_font_settings.font_weight_h1, sizeof(g_font_settings.font_weight_h1), "800");
    snprintf(g_font_settings.font_weight_h2, sizeof(g_font_settings.font_weight_h2), "750");
    snprintf(g_font_settings.font_weight_h3, sizeof(g_font_settings.font_weight_h3), "700");
    snprintf(g_font_settings.font_weight_h4, sizeof(g_font_settings.font_weight_h4), "600");
    snprintf(g_font_settings.font_weight_h5h6, sizeof(g_font_settings.font_weight_h5h6), "500");
    snprintf(g_font_settings.font_weight_topbar_title, sizeof(g_font_settings.font_weight_topbar_title), "800");
    snprintf(g_font_settings.font_weight_btn, sizeof(g_font_settings.font_weight_btn), "600");
    snprintf(g_font_settings.font_weight_board_line_title, sizeof(g_font_settings.font_weight_board_line_title), "800");
    snprintf(g_font_settings.font_weight_hero_h1, sizeof(g_font_settings.font_weight_hero_h1), "850");
    snprintf(g_font_settings.font_weight_md_h1, sizeof(g_font_settings.font_weight_md_h1), "800");
    snprintf(g_font_settings.font_weight_md_h2, sizeof(g_font_settings.font_weight_md_h2), "700");
    snprintf(g_font_settings.font_weight_md_h3, sizeof(g_font_settings.font_weight_md_h3), "700");
    snprintf(g_font_settings.font_weight_post_h1, sizeof(g_font_settings.font_weight_post_h1), "800");
}

bool font_settings_load(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        font_set_default();
        f = fopen(path, "w");
        if (f) {
            fprintf(f, "font_import_url=%s\n", g_font_settings.import_url);
            fprintf(f, "font_face_family=%s\n", g_font_settings.face_family);
            fprintf(f, "font_face_src=%s\n", g_font_settings.face_src);
            fprintf(f, "font_body=%s\n", g_font_settings.body);
            fprintf(f, "font_heading=%s\n", g_font_settings.heading);
            fprintf(f, "font_ui=%s\n", g_font_settings.ui);
            fprintf(f, "font_code=%s\n", g_font_settings.code);
            fprintf(f, "font_blockquote=%s\n", g_font_settings.blockquote);
            fprintf(f, "font_display=%s\n", g_font_settings.display);
            fprintf(f, "letter_spacing_body=%s\n", g_font_settings.letter_spacing_body);
            fprintf(f, "letter_spacing_h1=%s\n", g_font_settings.letter_spacing_h1);
            fprintf(f, "letter_spacing_h2=%s\n", g_font_settings.letter_spacing_h2);
            fprintf(f, "letter_spacing_h3=%s\n", g_font_settings.letter_spacing_h3);
            fprintf(f, "letter_spacing_h4=%s\n", g_font_settings.letter_spacing_h4);
            fprintf(f, "letter_spacing_h5h6=%s\n", g_font_settings.letter_spacing_h5h6);
            fprintf(f, "letter_spacing_topbar_title=%s\n", g_font_settings.letter_spacing_topbar_title);
            fprintf(f, "letter_spacing_btn=%s\n", g_font_settings.letter_spacing_btn);
            fprintf(f, "letter_spacing_board_line_title=%s\n", g_font_settings.letter_spacing_board_line_title);
            fprintf(f, "letter_spacing_hero_h1=%s\n", g_font_settings.letter_spacing_hero_h1);
            fprintf(f, "letter_spacing_hero_p=%s\n", g_font_settings.letter_spacing_hero_p);
            fprintf(f, "letter_spacing_md_h1=%s\n", g_font_settings.letter_spacing_md_h1);
            fprintf(f, "letter_spacing_md_h2=%s\n", g_font_settings.letter_spacing_md_h2);
            fprintf(f, "letter_spacing_md_h3=%s\n", g_font_settings.letter_spacing_md_h3);
            fprintf(f, "letter_spacing_post_h1=%s\n", g_font_settings.letter_spacing_post_h1);
            fprintf(f, "font_weight_body=%s\n", g_font_settings.font_weight_body);
            fprintf(f, "font_weight_h1=%s\n", g_font_settings.font_weight_h1);
            fprintf(f, "font_weight_h2=%s\n", g_font_settings.font_weight_h2);
            fprintf(f, "font_weight_h3=%s\n", g_font_settings.font_weight_h3);
            fprintf(f, "font_weight_h4=%s\n", g_font_settings.font_weight_h4);
            fprintf(f, "font_weight_h5h6=%s\n", g_font_settings.font_weight_h5h6);
            fprintf(f, "font_weight_topbar_title=%s\n", g_font_settings.font_weight_topbar_title);
            fprintf(f, "font_weight_btn=%s\n", g_font_settings.font_weight_btn);
            fprintf(f, "font_weight_board_line_title=%s\n", g_font_settings.font_weight_board_line_title);
            fprintf(f, "font_weight_hero_h1=%s\n", g_font_settings.font_weight_hero_h1);
            fprintf(f, "font_weight_md_h1=%s\n", g_font_settings.font_weight_md_h1);
            fprintf(f, "font_weight_md_h2=%s\n", g_font_settings.font_weight_md_h2);
            fprintf(f, "font_weight_md_h3=%s\n", g_font_settings.font_weight_md_h3);
            fprintf(f, "font_weight_post_h1=%s\n", g_font_settings.font_weight_post_h1);
            fclose(f);
        }
        return true;
    }
    font_set_default();
    char line[768];
    while (fgets(line, sizeof(line), f)) {
        trim_newline(line);
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        const char *key = line;
        const char *val = eq + 1;
        if (strcmp(key, "font_import_url") == 0) {
            snprintf(g_font_settings.import_url, sizeof(g_font_settings.import_url), "%s", val);
        } else if (strcmp(key, "font_face_family") == 0) {
            snprintf(g_font_settings.face_family, sizeof(g_font_settings.face_family), "%s", val);
        } else if (strcmp(key, "font_face_src") == 0) {
            snprintf(g_font_settings.face_src, sizeof(g_font_settings.face_src), "%s", val);
        } else if (strcmp(key, "font_body") == 0) {
            snprintf(g_font_settings.body, sizeof(g_font_settings.body), "%s", val);
        } else if (strcmp(key, "font_heading") == 0) {
            snprintf(g_font_settings.heading, sizeof(g_font_settings.heading), "%s", val);
        } else if (strcmp(key, "font_ui") == 0) {
            snprintf(g_font_settings.ui, sizeof(g_font_settings.ui), "%s", val);
        } else if (strcmp(key, "font_code") == 0) {
            snprintf(g_font_settings.code, sizeof(g_font_settings.code), "%s", val);
        } else if (strcmp(key, "font_blockquote") == 0) {
            snprintf(g_font_settings.blockquote, sizeof(g_font_settings.blockquote), "%s", val);
        } else if (strcmp(key, "font_display") == 0) {
            snprintf(g_font_settings.display, sizeof(g_font_settings.display), "%s", val);
        } else if (strcmp(key, "letter_spacing_body") == 0) {
            snprintf(g_font_settings.letter_spacing_body, sizeof(g_font_settings.letter_spacing_body), "%s", val);
        } else if (strcmp(key, "letter_spacing_h1") == 0) {
            snprintf(g_font_settings.letter_spacing_h1, sizeof(g_font_settings.letter_spacing_h1), "%s", val);
        } else if (strcmp(key, "letter_spacing_h2") == 0) {
            snprintf(g_font_settings.letter_spacing_h2, sizeof(g_font_settings.letter_spacing_h2), "%s", val);
        } else if (strcmp(key, "letter_spacing_h3") == 0) {
            snprintf(g_font_settings.letter_spacing_h3, sizeof(g_font_settings.letter_spacing_h3), "%s", val);
        } else if (strcmp(key, "letter_spacing_h4") == 0) {
            snprintf(g_font_settings.letter_spacing_h4, sizeof(g_font_settings.letter_spacing_h4), "%s", val);
        } else if (strcmp(key, "letter_spacing_h5h6") == 0) {
            snprintf(g_font_settings.letter_spacing_h5h6, sizeof(g_font_settings.letter_spacing_h5h6), "%s", val);
        } else if (strcmp(key, "letter_spacing_topbar_title") == 0) {
            snprintf(g_font_settings.letter_spacing_topbar_title, sizeof(g_font_settings.letter_spacing_topbar_title), "%s", val);
        } else if (strcmp(key, "letter_spacing_btn") == 0) {
            snprintf(g_font_settings.letter_spacing_btn, sizeof(g_font_settings.letter_spacing_btn), "%s", val);
        } else if (strcmp(key, "letter_spacing_board_line_title") == 0) {
            snprintf(g_font_settings.letter_spacing_board_line_title, sizeof(g_font_settings.letter_spacing_board_line_title), "%s", val);
        } else if (strcmp(key, "letter_spacing_hero_h1") == 0) {
            snprintf(g_font_settings.letter_spacing_hero_h1, sizeof(g_font_settings.letter_spacing_hero_h1), "%s", val);
        } else if (strcmp(key, "letter_spacing_hero_p") == 0) {
            snprintf(g_font_settings.letter_spacing_hero_p, sizeof(g_font_settings.letter_spacing_hero_p), "%s", val);
        } else if (strcmp(key, "letter_spacing_md_h1") == 0) {
            snprintf(g_font_settings.letter_spacing_md_h1, sizeof(g_font_settings.letter_spacing_md_h1), "%s", val);
        } else if (strcmp(key, "letter_spacing_md_h2") == 0) {
            snprintf(g_font_settings.letter_spacing_md_h2, sizeof(g_font_settings.letter_spacing_md_h2), "%s", val);
        } else if (strcmp(key, "letter_spacing_md_h3") == 0) {
            snprintf(g_font_settings.letter_spacing_md_h3, sizeof(g_font_settings.letter_spacing_md_h3), "%s", val);
        } else if (strcmp(key, "letter_spacing_post_h1") == 0) {
            snprintf(g_font_settings.letter_spacing_post_h1, sizeof(g_font_settings.letter_spacing_post_h1), "%s", val);
        } else if (strcmp(key, "font_weight_body") == 0) {
            snprintf(g_font_settings.font_weight_body, sizeof(g_font_settings.font_weight_body), "%s", val);
        } else if (strcmp(key, "font_weight_h1") == 0) {
            snprintf(g_font_settings.font_weight_h1, sizeof(g_font_settings.font_weight_h1), "%s", val);
        } else if (strcmp(key, "font_weight_h2") == 0) {
            snprintf(g_font_settings.font_weight_h2, sizeof(g_font_settings.font_weight_h2), "%s", val);
        } else if (strcmp(key, "font_weight_h3") == 0) {
            snprintf(g_font_settings.font_weight_h3, sizeof(g_font_settings.font_weight_h3), "%s", val);
        } else if (strcmp(key, "font_weight_h4") == 0) {
            snprintf(g_font_settings.font_weight_h4, sizeof(g_font_settings.font_weight_h4), "%s", val);
        } else if (strcmp(key, "font_weight_h5h6") == 0) {
            snprintf(g_font_settings.font_weight_h5h6, sizeof(g_font_settings.font_weight_h5h6), "%s", val);
        } else if (strcmp(key, "font_weight_topbar_title") == 0) {
            snprintf(g_font_settings.font_weight_topbar_title, sizeof(g_font_settings.font_weight_topbar_title), "%s", val);
        } else if (strcmp(key, "font_weight_btn") == 0) {
            snprintf(g_font_settings.font_weight_btn, sizeof(g_font_settings.font_weight_btn), "%s", val);
        } else if (strcmp(key, "font_weight_board_line_title") == 0) {
            snprintf(g_font_settings.font_weight_board_line_title, sizeof(g_font_settings.font_weight_board_line_title), "%s", val);
        } else if (strcmp(key, "font_weight_hero_h1") == 0) {
            snprintf(g_font_settings.font_weight_hero_h1, sizeof(g_font_settings.font_weight_hero_h1), "%s", val);
        } else if (strcmp(key, "font_weight_md_h1") == 0) {
            snprintf(g_font_settings.font_weight_md_h1, sizeof(g_font_settings.font_weight_md_h1), "%s", val);
        } else if (strcmp(key, "font_weight_md_h2") == 0) {
            snprintf(g_font_settings.font_weight_md_h2, sizeof(g_font_settings.font_weight_md_h2), "%s", val);
        } else if (strcmp(key, "font_weight_md_h3") == 0) {
            snprintf(g_font_settings.font_weight_md_h3, sizeof(g_font_settings.font_weight_md_h3), "%s", val);
        } else if (strcmp(key, "font_weight_post_h1") == 0) {
            snprintf(g_font_settings.font_weight_post_h1, sizeof(g_font_settings.font_weight_post_h1), "%s", val);
        }
    }
    fclose(f);
    return true;
}

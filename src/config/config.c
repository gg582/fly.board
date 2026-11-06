#include "config.h"
#include "../engine/flywire.h"
#include <cwist/core/log.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

blog_config_t g_config = {0};
font_settings_t g_font_settings = {0};
layout_settings_t g_layout_settings = {0};
s3_config_t g_s3_config = {0};
robots_config_t g_robots_config = {0};

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
    snprintf(g_config.language, sizeof(g_config.language), "ko");
    snprintf(g_config.subtitle, sizeof(g_config.subtitle), "Explore boards and read stories.");
    snprintf(g_config.brand_footer, sizeof(g_config.brand_footer), "Built with c4punks/CWIST | Served in Finland, Helsinki 🇫🇮");
    snprintf(g_config.accent, sizeof(g_config.accent), "#3b82f6");
    g_config.port = 8443;
    g_config.use_tasfa = true;
    g_config.use_rss = false;
    g_config.newsletter_enabled = false;
    g_config.realtime_enabled = false;
    g_config.realtime_typing = false;
    g_config.notification_aggregation = false;
    g_config.notification_window_seconds = 300;
    g_config.use_tls = true;
    g_config.use_http2 = true;
    g_config.use_http3 = true;
    g_config.roundness = 0.0f;
    g_config.max_upload_size = 1024LL * 1024LL * 1024LL;
    g_config.max_total_parallel_uploads = 8;
    g_config.max_upload_parallel_chunks = 48;
    g_config.max_concurrent_downloads = 128;
    g_config.use_special_modes[0] = '\0';
    g_config.vote_only[0] = '\0';
    snprintf(g_config.bg_invert_algo, sizeof(g_config.bg_invert_algo), "luminv");
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
            fprintf(f, "blog_logo_dark=%s\n", g_config.blog_logo_dark);
            fprintf(f, "invert_logo=%s\n", g_config.invert_logo ? "true" : "false");
            fprintf(f, "boards_img=%s\n", g_config.boards_img);
            fprintf(f, "files_img=%s\n", g_config.files_img);
            fprintf(f, "favicon=%s\n", g_config.favicon);
            fprintf(f, "bg_full_light=%s\n", g_config.bg_full_light);
            fprintf(f, "bg_full_dark=%s\n", g_config.bg_full_dark);
            fprintf(f, "home_img_dark=%s\n", g_config.home_img_dark);
            fprintf(f, "boards_img_dark=%s\n", g_config.boards_img_dark);
            fprintf(f, "files_img_dark=%s\n", g_config.files_img_dark);
            fprintf(f, "archive_img=%s\n", g_config.archive_img);
            fprintf(f, "archive_img_dark=%s\n", g_config.archive_img_dark);
            fprintf(f, "mail_img=%s\n", g_config.mail_img);
            fprintf(f, "mail_img_dark=%s\n", g_config.mail_img_dark);
            fprintf(f, "bg_invert_color=%s\n", g_config.bg_invert_color);
            fprintf(f, "bg_invert_algo=%s\n", g_config.bg_invert_algo);
            fprintf(f, "root_url=%s\n", g_config.root_url);
            fprintf(f, "mail_domain=%s\n", g_config.mail_domain);
            fprintf(f, "language=%s\n", g_config.language);
            fprintf(f, "use_tasfa=%s\n", g_config.use_tasfa ? "true" : "false");
            fprintf(f, "use_rss=%s\n", g_config.use_rss ? "true" : "false");
            fprintf(f, "newsletter_enabled=%s\n", g_config.newsletter_enabled ? "true" : "false");
            fprintf(f, "realtime_enabled=%s\n", g_config.realtime_enabled ? "true" : "false");
            fprintf(f, "realtime_typing=%s\n", g_config.realtime_typing ? "true" : "false");
            fprintf(f, "notification_aggregation=%s\n", g_config.notification_aggregation ? "true" : "false");
            fprintf(f, "notification_window_seconds=%d\n", g_config.notification_window_seconds);
            fprintf(f, "use_tls=%s\n", g_config.use_tls ? "true" : "false");
            fprintf(f, "use_http2=%s\n", g_config.use_http2 ? "true" : "false");
            fprintf(f, "use_http3=%s\n", g_config.use_http3 ? "true" : "false");
            fprintf(f, "roundness=%.2f\n", g_config.roundness);
            fprintf(f, "max_upload_size=1G\n");
            fprintf(f, "max_total_parallel_uploads=%d\n", g_config.max_total_parallel_uploads);
            fprintf(f, "max_upload_parallel_chunks=%d\n", g_config.max_upload_parallel_chunks);
            fprintf(f, "max_concurrent_downloads=%d\n", g_config.max_concurrent_downloads);
            fprintf(f, "use_special_modes=%s\n", g_config.use_special_modes);
            fprintf(f, "vote_only=%s\n", g_config.vote_only);
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
        } else if (strcmp(key, "language") == 0) {
            if (val[0]) snprintf(g_config.language, sizeof(g_config.language), "%s", val);
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
        } else if (strcmp(key, "blog_logo_dark") == 0) {
            snprintf(g_config.blog_logo_dark, sizeof(g_config.blog_logo_dark), "%s", val);
        } else if (strcmp(key, "invert_logo") == 0) {
            g_config.invert_logo = (strcmp(val, "true") == 0 || strcmp(val, "1") == 0);
        } else if (strcmp(key, "boards_img") == 0) {
            snprintf(g_config.boards_img, sizeof(g_config.boards_img), "%s", val);
        } else if (strcmp(key, "files_img") == 0) {
            snprintf(g_config.files_img, sizeof(g_config.files_img), "%s", val);
        } else if (strcmp(key, "favicon") == 0) {
            snprintf(g_config.favicon, sizeof(g_config.favicon), "%s", val);
        } else if (strcmp(key, "root_url") == 0) {
            snprintf(g_config.root_url, sizeof(g_config.root_url), "%s", val);
        } else if (strcmp(key, "mail_domain") == 0) {
            snprintf(g_config.mail_domain, sizeof(g_config.mail_domain), "%s", val);
        } else if (strcmp(key, "bg_full_light") == 0) {
            snprintf(g_config.bg_full_light, sizeof(g_config.bg_full_light), "%s", val);
        } else if (strcmp(key, "bg_full_dark") == 0) {
            snprintf(g_config.bg_full_dark, sizeof(g_config.bg_full_dark), "%s", val);
        } else if (strcmp(key, "home_img_dark") == 0) {
            snprintf(g_config.home_img_dark, sizeof(g_config.home_img_dark), "%s", val);
        } else if (strcmp(key, "boards_img_dark") == 0) {
            snprintf(g_config.boards_img_dark, sizeof(g_config.boards_img_dark), "%s", val);
        } else if (strcmp(key, "files_img_dark") == 0) {
            snprintf(g_config.files_img_dark, sizeof(g_config.files_img_dark), "%s", val);
        } else if (strcmp(key, "archive_img") == 0) {
            snprintf(g_config.archive_img, sizeof(g_config.archive_img), "%s", val);
        } else if (strcmp(key, "archive_img_dark") == 0) {
            snprintf(g_config.archive_img_dark, sizeof(g_config.archive_img_dark), "%s", val);
        } else if (strcmp(key, "mail_img") == 0) {
            snprintf(g_config.mail_img, sizeof(g_config.mail_img), "%s", val);
        } else if (strcmp(key, "mail_img_dark") == 0) {
            snprintf(g_config.mail_img_dark, sizeof(g_config.mail_img_dark), "%s", val);
        } else if (strcmp(key, "bg_invert_color") == 0) {
            snprintf(g_config.bg_invert_color, sizeof(g_config.bg_invert_color), "%s", val);
        } else if (strcmp(key, "bg_invert_algo") == 0) {
            snprintf(g_config.bg_invert_algo, sizeof(g_config.bg_invert_algo), "%s", val);
        } else if (strcmp(key, "use_tasfa") == 0) {
            g_config.use_tasfa = (strcmp(val, "true") == 0 || strcmp(val, "1") == 0);
        } else if (strcmp(key, "use_rss") == 0) {
            g_config.use_rss = (strcmp(val, "true") == 0 || strcmp(val, "1") == 0);
        } else if (strcmp(key, "newsletter_enabled") == 0) {
            g_config.newsletter_enabled = (strcmp(val, "true") == 0 || strcmp(val, "1") == 0);
        } else if (strcmp(key, "realtime_enabled") == 0) {
            g_config.realtime_enabled = (strcmp(val, "true") == 0 || strcmp(val, "1") == 0);
        } else if (strcmp(key, "realtime_typing") == 0) {
            g_config.realtime_typing = (strcmp(val, "true") == 0 || strcmp(val, "1") == 0);
        } else if (strcmp(key, "notification_aggregation") == 0) {
            g_config.notification_aggregation = (strcmp(val, "true") == 0 || strcmp(val, "1") == 0);
        } else if (strcmp(key, "notification_window_seconds") == 0) {
            g_config.notification_window_seconds = atoi(val);
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
        } else if (strcmp(key, "use_special_modes") == 0) {
            snprintf(g_config.use_special_modes, sizeof(g_config.use_special_modes), "%s", val);
        } else if (strcmp(key, "vote_only") == 0) {
            snprintf(g_config.vote_only, sizeof(g_config.vote_only), "%s", val);
        }
    }
    fclose(f);
    if (!g_config.title[0]) snprintf(g_config.title, sizeof(g_config.title), "CWIST Docker Blog");
    if (!g_config.subtitle[0]) snprintf(g_config.subtitle, sizeof(g_config.subtitle), "Explore boards and read stories.");
    if (!g_config.brand_footer[0]) snprintf(g_config.brand_footer, sizeof(g_config.brand_footer), "Built with c4punks/CWIST | Served in Finland, Helsinki 🇫🇮");
    if (!g_config.accent[0]) snprintf(g_config.accent, sizeof(g_config.accent), "#3b82f6");
    if (g_config.port <= 0 || g_config.port > 65535) g_config.port = 8443;
    if (g_config.max_upload_size <= 0) g_config.max_upload_size = 1024LL * 1024LL * 1024LL;
    g_config.max_total_parallel_uploads = clamp_int_config(g_config.max_total_parallel_uploads, 1, 512);
    g_config.max_upload_parallel_chunks = clamp_int_config(g_config.max_upload_parallel_chunks, 1, 64);
    g_config.max_concurrent_downloads = clamp_int_config(g_config.max_concurrent_downloads, 1, 512);
    g_config.notification_window_seconds = clamp_int_config(g_config.notification_window_seconds, 1, 86400);
    if (strcmp(g_config.bg_invert_algo, "oklch") != 0) {
        /* Unknown values (including empty) fall back to the default. */
        snprintf(g_config.bg_invert_algo, sizeof(g_config.bg_invert_algo), "luminv");
    }
    if (g_config.vote_only[0] && strcmp(g_config.vote_only, "all") != 0 &&
        strcmp(g_config.vote_only, "authorized") != 0 && strcmp(g_config.vote_only, "admin") != 0) {
        CWIST_LOG_WARN("Unknown vote_only value '%s', falling back to all", g_config.vote_only);
        g_config.vote_only[0] = '\0';
    }

    /* Resolve the mail domain: explicit env override wins, then the
     * mail_domain setting, then the host part of root_url, then localhost. */
    const char *md_env = getenv("FLY_MAIL_DOMAIN");
    if (md_env && md_env[0]) {
        snprintf(g_config.mail_domain, sizeof(g_config.mail_domain), "%s", md_env);
    }
    if (!g_config.mail_domain[0]) {
        const char *u = g_config.root_url;
        const char *p = strstr(u, "://");
        p = p ? p + 3 : u;
        size_t len = strcspn(p, "/:?#");
        if (len >= sizeof(g_config.mail_domain)) len = sizeof(g_config.mail_domain) - 1;
        memcpy(g_config.mail_domain, p, len);
        g_config.mail_domain[len] = '\0';
    }
    if (!g_config.mail_domain[0])
        snprintf(g_config.mail_domain, sizeof(g_config.mail_domain), "localhost");

    /* Validate configured image assets so the renderer does not emit broken
       <img> tags that result in 404s in Firefox (and all other browsers). */
    validate_image_setting(g_config.home_img, "home_img");
    validate_image_setting(g_config.blog_logo, "blog_logo");
    validate_image_setting(g_config.blog_logo_dark, "blog_logo_dark");
    validate_image_setting(g_config.boards_img, "boards_img");
    validate_image_setting(g_config.files_img, "files_img");
    validate_image_setting(g_config.favicon, "favicon");
    validate_image_setting(g_config.bg_full_light, "bg_full_light");
    validate_image_setting(g_config.bg_full_dark, "bg_full_dark");
    validate_image_setting(g_config.home_img_dark, "home_img_dark");
    validate_image_setting(g_config.boards_img_dark, "boards_img_dark");
    validate_image_setting(g_config.files_img_dark, "files_img_dark");
    validate_image_setting(g_config.archive_img, "archive_img");
    validate_image_setting(g_config.archive_img_dark, "archive_img_dark");
    validate_image_setting(g_config.mail_img, "mail_img");
    validate_image_setting(g_config.mail_img_dark, "mail_img_dark");
    return true;
}

bool s3_config_load(const char *path) {
    memset(&g_s3_config, 0, sizeof(g_s3_config));
    FILE *f = fopen(path, "r");
    if (!f) {
        /* s3.settings is optional: write an empty template so operators can
         * discover the keys, and run with S3 disabled. */
        f = fopen(path, "w");
        if (f) {
            fprintf(f, "# S3-compatible object storage for uploaded files (optional)\n");
            fprintf(f, "# Fill every value below to enable; leave empty to keep local disk storage.\n");
            fprintf(f, "endpoint=\n");
            fprintf(f, "region=\n");
            fprintf(f, "bucket=\n");
            fprintf(f, "access_key=\n");
            fprintf(f, "secret_key=\n");
            fprintf(f, "prefix=\n");
            fprintf(f, "use_path_style=\n");
            fprintf(f, "# mode: mirror (keep local copy, also store in S3) or offload (move to S3)\n");
            fprintf(f, "mode=\n");
            fclose(f);
        }
        return true;
    }
    char line[768];
    while (fgets(line, sizeof(line), f)) {
        trim_newline(line);
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        const char *key = line;
        const char *val = eq + 1;
        if (strcmp(key, "endpoint") == 0) {
            snprintf(g_s3_config.endpoint, sizeof(g_s3_config.endpoint), "%s", val);
        } else if (strcmp(key, "region") == 0) {
            snprintf(g_s3_config.region, sizeof(g_s3_config.region), "%s", val);
        } else if (strcmp(key, "bucket") == 0) {
            snprintf(g_s3_config.bucket, sizeof(g_s3_config.bucket), "%s", val);
        } else if (strcmp(key, "access_key") == 0) {
            snprintf(g_s3_config.access_key, sizeof(g_s3_config.access_key), "%s", val);
        } else if (strcmp(key, "secret_key") == 0) {
            snprintf(g_s3_config.secret_key, sizeof(g_s3_config.secret_key), "%s", val);
        } else if (strcmp(key, "prefix") == 0) {
            snprintf(g_s3_config.prefix, sizeof(g_s3_config.prefix), "%s", val);
        } else if (strcmp(key, "use_path_style") == 0) {
            g_s3_config.use_path_style = (strcmp(val, "true") == 0 || strcmp(val, "1") == 0);
        } else if (strcmp(key, "mode") == 0) {
            snprintf(g_s3_config.mode, sizeof(g_s3_config.mode), "%s", val);
        }
    }
    fclose(f);
    if (s3_config_enabled() && g_s3_config.mode[0] &&
        strcmp(g_s3_config.mode, "mirror") != 0 && strcmp(g_s3_config.mode, "offload") != 0) {
        CWIST_LOG_WARN("Unknown s3 mode '%s', falling back to mirror", g_s3_config.mode);
        g_s3_config.mode[0] = '\0';
    }
    return true;
}

bool s3_config_enabled(void) {
    return g_s3_config.endpoint[0] && g_s3_config.bucket[0] &&
           g_s3_config.access_key[0] && g_s3_config.secret_key[0];
}

bool s3_config_offload(void) {
    return s3_config_enabled() && strcmp(g_s3_config.mode, "offload") == 0;
}

/* ---- robots.txt / llms.txt policy (robots.settings) ---- */

static const char *normalize_level(const char *val) {
    if (val && (strcmp(val, "restrict") == 0 || strcmp(val, "block") == 0)) return val;
    return "allow";
}

bool robots_config_load(const char *path) {
    memset(&g_robots_config, 0, sizeof(g_robots_config));
    FILE *f = fopen(path, "r");
    if (!f) {
        /* Optional file: write a commented template and run fully open. */
        f = fopen(path, "w");
        if (f) {
            fprintf(f, "# robots.txt and llms.txt access policy.\n");
            fprintf(f, "# Edit, save, then restart the service (systemctl restart flyboard).\n");
            fprintf(f, "#\n");
            fprintf(f, "# robots.level - policy for regular crawlers (robots.txt):\n");
            fprintf(f, "#   allow    : every crawler may fetch everything (default)\n");
            fprintf(f, "#   restrict : allow everything except admin/private paths\n");
            fprintf(f, "#              (/admin, /api/, /login, /logout, /register, /profile, /notifications)\n");
            fprintf(f, "#   block    : disallow all crawlers (Disallow: /, no sitemap)\n");
            fprintf(f, "#\n");
            fprintf(f, "# llms.level - policy for AI/LLM agents (/llms.txt and AI crawlers):\n");
            fprintf(f, "#   allow    : serve /llms.txt; AI crawlers are welcome (default)\n");
            fprintf(f, "#   restrict : serve /llms.txt, but disallow well-known training\n");
            fprintf(f, "#              crawlers (GPTBot, ClaudeBot, CCBot, Google-Extended, ...)\n");
            fprintf(f, "#              in robots.txt\n");
            fprintf(f, "#   block    : do not serve /llms.txt (404) and disallow AI crawlers\n");
            fprintf(f, "#\n");
            fprintf(f, "# Unknown values fall back to allow.\n");
            fprintf(f, "robots.level=allow\n");
            fprintf(f, "llms.level=allow\n");
            fclose(f);
        }
        return true;
    }
    char line[768];
    while (fgets(line, sizeof(line), f)) {
        trim_newline(line);
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        if (strcmp(line, "robots.level") == 0) {
            snprintf(g_robots_config.robots_level, sizeof(g_robots_config.robots_level), "%s", eq + 1);
        } else if (strcmp(line, "llms.level") == 0) {
            snprintf(g_robots_config.llms_level, sizeof(g_robots_config.llms_level), "%s", eq + 1);
        }
    }
    fclose(f);
    /* normalize_level() may return a pointer into the same buffer, so go
     * through a temporary (snprintf with overlapping src/dst is UB). */
    char tmp[16];
    snprintf(tmp, sizeof(tmp), "%s", normalize_level(g_robots_config.robots_level));
    snprintf(g_robots_config.robots_level, sizeof(g_robots_config.robots_level), "%s", tmp);
    snprintf(tmp, sizeof(tmp), "%s", normalize_level(g_robots_config.llms_level));
    snprintf(g_robots_config.llms_level, sizeof(g_robots_config.llms_level), "%s", tmp);
    return true;
}

const char *robots_level(void) { return g_robots_config.robots_level[0] ? g_robots_config.robots_level : "allow"; }
const char *llms_level(void)   { return g_robots_config.llms_level[0] ? g_robots_config.llms_level : "allow"; }

/* ---- Upload MIME allow/deny policy (upload.settings) ---- */

upload_policy_t g_upload_policy;

/* Conservative default: block executables and script interpreters, plus
 * Usenet news (frequently used to smuggle spam binaries).  Everything else,
 * including message/rfc822 (.eml), is allowed. */
static const char *const UPLOAD_DEFAULT_DENYLIST =
    "application/x-dosexec, application/x-msdownload, application/x-msdos-program, "
    "application/x-elf, application/x-executable, application/x-sh, application/x-bash, "
    "application/x-csh, application/x-perl, application/x-python, application/x-php, "
    "message/news";

static void upload_policy_set_defaults(void) {
    memset(&g_upload_policy, 0, sizeof(g_upload_policy));
    snprintf(g_upload_policy.mode, sizeof(g_upload_policy.mode), "%s", UPLOAD_POLICY_MODE_DENY);
    snprintf(g_upload_policy.denylist, sizeof(g_upload_policy.denylist), "%s", UPLOAD_DEFAULT_DENYLIST);
}

bool upload_policy_load(const char *path) {
    upload_policy_set_defaults();
    FILE *f = fopen(path, "r");
    if (!f) {
        /* Optional file: write a commented template with the defaults. */
        f = fopen(path, "w");
        if (f) {
            fprintf(f, "# Upload MIME allow/deny policy.\n");
            fprintf(f, "# Detection is by libmagic content sniffing (file magic), not extension.\n");
            fprintf(f, "#\n");
            fprintf(f, "# mode - denylist (default): everything is allowed except the denylist;\n");
            fprintf(f, "#        allowlist: only the allowlist is accepted.\n");
            fprintf(f, "# List entries are comma-separated MIME types. An entry ending in \"/*\"\n");
            fprintf(f, "# or \"/\" (e.g. image/) matches every type below it; other entries match\n");
            fprintf(f, "# exactly (case-insensitive).\n");
            fprintf(f, "mode=%s\n", g_upload_policy.mode);
            fprintf(f, "denylist=%s\n", g_upload_policy.denylist);
            fprintf(f, "allowlist=\n");
            fclose(f);
        }
        return true;
    }
    char line[2304];
    while (fgets(line, sizeof(line), f)) {
        trim_newline(line);
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        if (strcmp(line, "mode") == 0) {
            snprintf(g_upload_policy.mode, sizeof(g_upload_policy.mode), "%s", eq + 1);
        } else if (strcmp(line, "denylist") == 0) {
            snprintf(g_upload_policy.denylist, sizeof(g_upload_policy.denylist), "%s", eq + 1);
        } else if (strcmp(line, "allowlist") == 0) {
            snprintf(g_upload_policy.allowlist, sizeof(g_upload_policy.allowlist), "%s", eq + 1);
        }
    }
    fclose(f);
    if (strcmp(g_upload_policy.mode, UPLOAD_POLICY_MODE_DENY) != 0 &&
        strcmp(g_upload_policy.mode, UPLOAD_POLICY_MODE_ALLOW) != 0) {
        CWIST_LOG_WARN("Unknown upload policy mode '%s', falling back to denylist", g_upload_policy.mode);
        snprintf(g_upload_policy.mode, sizeof(g_upload_policy.mode), "%s", UPLOAD_POLICY_MODE_DENY);
    }
    return true;
}

bool upload_policy_save(const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) return false;
    fprintf(f, "# Upload MIME allow/deny policy. Managed from the admin dashboard.\n");
    fprintf(f, "# Detection is by libmagic content sniffing (file magic), not extension.\n");
    fprintf(f, "mode=%s\n", g_upload_policy.mode);
    fprintf(f, "denylist=%s\n", g_upload_policy.denylist);
    fprintf(f, "allowlist=%s\n", g_upload_policy.allowlist);
    bool ok = fclose(f) == 0;
    if (!ok) CWIST_LOG_ERROR("Failed to write %s", path);
    return ok;
}

const char *upload_policy_mode(void) {
    return strcmp(g_upload_policy.mode, UPLOAD_POLICY_MODE_ALLOW) == 0 ? UPLOAD_POLICY_MODE_ALLOW
                                                                       : UPLOAD_POLICY_MODE_DENY;
}

const char *upload_policy_active_list(void) {
    return strcmp(upload_policy_mode(), UPLOAD_POLICY_MODE_ALLOW) == 0 ? g_upload_policy.allowlist
                                                                       : g_upload_policy.denylist;
}

/* ---- FlyWire primary/replica synchronization (flywire.settings) ---- */

flywire_config_t g_flywire;

bool flywire_config_load(const char *path) {
    memset(&g_flywire, 0, sizeof(g_flywire));
    g_flywire.poll_seconds = 2;
    g_flywire.self_idx = -1;
    g_flywire.failover_seconds = 20;
    FILE *f = fopen(path, "r");
    if (!f) {
        /* Optional file: write a commented template and run with FlyWire off. */
        f = fopen(path, "w");
        if (f) {
            fprintf(f, "# FlyWire site synchronization (primary/replica).\n");
            fprintf(f, "# Edit, save, then restart the service.\n");
            fprintf(f, "#\n");
            fprintf(f, "# mode - \"\" (off, default), \"primary\" (this site serves the\n");
            fprintf(f, "#        /flywire/feed delta feed and journals mutations),\n");
            fprintf(f, "#        \"replica\" (this site is a read-only copy kept in sync\n");
            fprintf(f, "#        from the primary; first bootstrap it with\n");
            fprintf(f, "#        fly_board --restore <archive> from the primary's backup)\n");
            fprintf(f, "# primary_url - where a replica polls, e.g. https://oborona.zip\n");
            fprintf(f, "# token - shared secret; replicas send it as X-FlyWire-Token\n");
            fprintf(f, "# poll_seconds - 1..60, default 2\n");
            fprintf(f, "# auto_upgrade - \"1\" lets a replica run ./flywire-upgrade.sh\n");
            fprintf(f, "#        (git fetch/reset + build + service restart) when the\n");
            fprintf(f, "#        primary advertises a different version; default off\n");
            fprintf(f, "#\n");
            fprintf(f, "# Failover (optional, needs at least 3 sites for a majority):\n");
            fprintf(f, "# nodes - comma-separated sites in failover priority order\n");
            fprintf(f, "#        (IPs, or full base URLs), e.g.\n");
            fprintf(f, "#        80.47.226.102,173.231.53.9,59.23.169.247\n");
            fprintf(f, "# self - which nodes entry this site is\n");
            fprintf(f, "# failover - \"priority\" (default: the first live site in\n");
            fprintf(f, "#        nodes order takes over, and a higher-priority site\n");
            fprintf(f, "#        takes the role back once it has caught up) or \"auto\"\n");
            fprintf(f, "#        (the most up-to-date live site, then the fastest)\n");
            fprintf(f, "# site_host - public hostname for TLS SNI/Host when nodes are\n");
            fprintf(f, "#        IPs, e.g. oborona.zip\n");
            fprintf(f, "# failover_seconds - primary silence before a takeover, default 20\n");
            fprintf(f, "mode=\n");
            fprintf(f, "primary_url=\n");
            fprintf(f, "token=\n");
            fprintf(f, "poll_seconds=2\n");
            fprintf(f, "auto_upgrade=\n");
            fprintf(f, "nodes=\n");
            fprintf(f, "self=\n");
            fprintf(f, "failover=\n");
            fprintf(f, "site_host=\n");
            fprintf(f, "failover_seconds=\n");
            fclose(f);
        }
        return true;
    }
    char line[768];
    while (fgets(line, sizeof(line), f)) {
        trim_newline(line);
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        const char *key = line;
        const char *val = eq + 1;
        if (strcmp(key, "mode") == 0) {
            snprintf(g_flywire.mode, sizeof(g_flywire.mode), "%s", val);
        } else if (strcmp(key, "primary_url") == 0) {
            snprintf(g_flywire.primary_url, sizeof(g_flywire.primary_url), "%s", val);
        } else if (strcmp(key, "token") == 0) {
            snprintf(g_flywire.token, sizeof(g_flywire.token), "%s", val);
        } else if (strcmp(key, "poll_seconds") == 0) {
            g_flywire.poll_seconds = atoi(val);
        } else if (strcmp(key, "auto_upgrade") == 0) {
            g_flywire.auto_upgrade = (strcmp(val, "1") == 0);
        } else if (strcmp(key, "nodes") == 0) {
            char list[768];
            snprintf(list, sizeof(list), "%s", val);
            char *save = NULL;
            for (char *tok = strtok_r(list, ", ", &save); tok && g_flywire.n_nodes < FLYWIRE_MAX_NODES;
                 tok = strtok_r(NULL, ", ", &save)) {
                size_t n = strlen(tok);
                while (n > 0 && tok[n - 1] == '/') tok[--n] = '\0';
                if (n) snprintf(g_flywire.nodes[g_flywire.n_nodes++], sizeof(g_flywire.nodes[0]), "%s", tok);
            }
        } else if (strcmp(key, "self") == 0) {
            snprintf(g_flywire.self, sizeof(g_flywire.self), "%s", val);
        } else if (strcmp(key, "failover") == 0) {
            g_flywire.failover_auto = (strcmp(val, "auto") == 0);
        } else if (strcmp(key, "site_host") == 0) {
            snprintf(g_flywire.site_host, sizeof(g_flywire.site_host), "%s", val);
        } else if (strcmp(key, "failover_seconds") == 0) {
            if (atoi(val) > 0) g_flywire.failover_seconds = atoi(val);
        }
    }
    fclose(f);
    {
        size_t n = strlen(g_flywire.self);
        while (n > 0 && g_flywire.self[n - 1] == '/') g_flywire.self[--n] = '\0';
        for (int i = 0; i < g_flywire.n_nodes; i++) {
            if (g_flywire.self[0] && strcmp(g_flywire.nodes[i], g_flywire.self) == 0) g_flywire.self_idx = i;
        }
        if (g_flywire.n_nodes > 0 && g_flywire.self_idx < 0) {
            CWIST_LOG_WARN("FlyWire: self=%s is not in nodes=; failover disabled", g_flywire.self);
        }
        if (g_flywire.failover_seconds < 3) g_flywire.failover_seconds = 3;
    }
    if (strcmp(g_flywire.mode, "primary") != 0 && strcmp(g_flywire.mode, "replica") != 0 &&
        g_flywire.mode[0]) {
        CWIST_LOG_WARN("Unknown flywire mode '%s', FlyWire disabled", g_flywire.mode);
        g_flywire.mode[0] = '\0';
    }
    if (g_flywire.poll_seconds < 1) g_flywire.poll_seconds = 1;
    if (g_flywire.poll_seconds > 60) g_flywire.poll_seconds = 60;
    return true;
}

const char *flywire_mode(void) {
    return g_flywire.mode;
}

bool flywire_cluster_configured(void) {
    return g_flywire.n_nodes >= 2 && g_flywire.self_idx >= 0 && g_flywire.token[0];
}

/* In a failover cluster the role is runtime state shared by every worker
 * process (flywire_cluster.c); otherwise it is the static mode= setting. */
bool flywire_is_primary(void) {
    if (flywire_cluster_configured()) return flywire_cluster_current_primary() == g_flywire.self_idx;
    return strcmp(g_flywire.mode, "primary") == 0 && g_flywire.token[0];
}

bool flywire_is_replica(void) {
    if (flywire_cluster_configured()) {
        int p = flywire_cluster_current_primary();
        return p >= 0 && p != g_flywire.self_idx;
    }
    return strcmp(g_flywire.mode, "replica") == 0 && g_flywire.primary_url[0] && g_flywire.token[0];
}

const char *flywire_token(void) {
    return g_flywire.token;
}

const char *flywire_primary_url(void) {
    /* Public base URL of the site's primary. In a cluster with site_host the
     * public name stays the same whichever node holds the role (requests are
     * routed to the node with flywire_curl_target()). */
    if (flywire_cluster_configured() && g_flywire.site_host[0]) {
        static char url[300];
        snprintf(url, sizeof(url), "https://%s", g_flywire.site_host);
        return url;
    }
    return g_flywire.primary_url;
}

int flywire_poll_seconds(void) {
    return g_flywire.poll_seconds > 0 ? g_flywire.poll_seconds : 2;
}

bool flywire_auto_upgrade(void) {
    return g_flywire.auto_upgrade;
}


bool config_bg_invert_enabled(const char *target) {
    if (!target || !target[0]) return false;
    /* The logo uses its own boolean flag rather than the bg_invert_color
     * target list. */
    if (strcasecmp(target, "logo") == 0) return g_config.invert_logo;
    if (!g_config.bg_invert_color[0]) return false;
    char buf[128];
    snprintf(buf, sizeof(buf), "%s", g_config.bg_invert_color);
    char *save = NULL;
    for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        while (*tok == ' ' || *tok == '\t') tok++;
        size_t len = strlen(tok);
        while (len > 0 && (tok[len - 1] == ' ' || tok[len - 1] == '\t')) tok[--len] = '\0';
        if (strcasecmp(tok, target) == 0) return true;
    }
    return false;
}

void config_resolve_bg(const char *light_img, const char *dark_img, const char *target,
                       bool dark_mode, const char **out_img, bool *out_invert) {
    bool has_light = light_img && light_img[0];
    bool has_dark = dark_img && dark_img[0];
    const char *img = dark_mode ? (has_dark ? dark_img : light_img)
                                : (has_light ? light_img : dark_img);
    if (!img || !img[0]) img = NULL;
    bool invert = false;
    /* Inversion only fills the mode that has no image of its own; when both
     * light and dark are explicitly configured the invert setting is void
     * for this target. */
    if (img && !(has_light && has_dark) && config_bg_invert_enabled(target)) {
        invert = dark_mode ? !has_dark : !has_light;
    }
    if (out_img) *out_img = img;
    if (out_invert) *out_invert = invert;
}

bool config_vote_allowed(bool logged_in, const char *role) {
    const char *mode = g_config.vote_only;
    if (!mode[0] || strcmp(mode, "all") == 0) return true;
    if (strcmp(mode, "authorized") == 0) return logged_in;
    if (strcmp(mode, "admin") == 0)
        return logged_in && role && strcmp(role, "admin") == 0;
    return true;
}

static void font_set_default(void) {
    /* Web fonts are inlined into every HTML response by render_page.c, so no
     * external @import or @font-face URLs are needed by default. Leaving these
     * empty avoids extra round trips and keeps the page self-contained. */
    g_font_settings.import_url[0] = '\0';
    g_font_settings.script_url[0] = '\0';
    snprintf(g_font_settings.face_family, sizeof(g_font_settings.face_family), "JetBrains Mono");
    g_font_settings.face_src[0] = '\0';
    g_font_settings.download_count = 0;
    snprintf(g_font_settings.body, sizeof(g_font_settings.body),
             "'IBM Plex Sans KR', 'IBM Plex Sans', 'Pretendard Variable', 'Pretendard', sans-serif");
    snprintf(g_font_settings.heading, sizeof(g_font_settings.heading), "'Outfit', 'IBM Plex Sans KR', 'IBM Plex Sans', sans-serif");
    snprintf(g_font_settings.ui, sizeof(g_font_settings.ui),
             "'Inter', 'IBM Plex Sans KR', 'IBM Plex Sans', 'Pretendard Variable', sans-serif");
    snprintf(g_font_settings.code, sizeof(g_font_settings.code),
             "'JetBrains Mono', 'Fira Code', 'D2Coding', Consolas, Monaco, 'Courier New', monospace");
    snprintf(g_font_settings.blockquote, sizeof(g_font_settings.blockquote),
             "'Source Serif 4', 'IBM Plex Sans KR', 'Pretendard Variable', serif");
    snprintf(g_font_settings.display, sizeof(g_font_settings.display), "'Outfit', 'IBM Plex Sans KR', 'IBM Plex Sans', sans-serif");
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
            fprintf(f, "font_script_url=%s\n", g_font_settings.script_url);
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
        } else if (strcmp(key, "font_script_url") == 0) {
            snprintf(g_font_settings.script_url, sizeof(g_font_settings.script_url), "%s", val);
        } else if (strcmp(key, "font_face_family") == 0) {
            snprintf(g_font_settings.face_family, sizeof(g_font_settings.face_family), "%s", val);
        } else if (strcmp(key, "font_face_src") == 0) {
            snprintf(g_font_settings.face_src, sizeof(g_font_settings.face_src), "%s", val);
        } else if (strcmp(key, "font_file") == 0) {
            /* format: font_file=<filename> <url>. The server downloads the
             * file into public/fonts/<filename> at startup when it is
             * missing locally. The filename must be a plain basename. */
            const char *sp = strchr(val, ' ');
            if (sp && g_font_settings.download_count < FONT_FILE_MAX) {
                size_t name_len = (size_t)(sp - val);
                const char *url = sp + 1;
                while (*url == ' ') url++;
                bool name_ok = name_len > 0 && name_len < FONT_FILE_NAME_LEN && val[0] != '.';
                for (size_t i = 0; name_ok && i < name_len; i++) {
                    if (val[i] == '/' || val[i] == '\\') name_ok = false;
                }
                bool url_ok = (strncmp(url, "https://", 8) == 0 || strncmp(url, "http://", 7) == 0) &&
                              strlen(url) < FONT_FILE_URL_LEN;
                if (name_ok && url_ok) {
                    size_t i = g_font_settings.download_count++;
                    snprintf(g_font_settings.download_name[i], FONT_FILE_NAME_LEN, "%.*s", (int)name_len, val);
                    snprintf(g_font_settings.download_url[i], FONT_FILE_URL_LEN, "%s", url);
                }
            }
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

static void layout_set_default(void) {
    snprintf(g_layout_settings.navigation, sizeof(g_layout_settings.navigation), "topbar");
    snprintf(g_layout_settings.content_style, sizeof(g_layout_settings.content_style), "blog");
    snprintf(g_layout_settings.density, sizeof(g_layout_settings.density), "comfortable");
    snprintf(g_layout_settings.list_style, sizeof(g_layout_settings.list_style), "feed");
    snprintf(g_layout_settings.content_width, sizeof(g_layout_settings.content_width), "standard");
}

bool layout_value_valid(const char *key, const char *value) {
    static const char *keys[] = {"navigation", "content_style", "density", "list_style", "content_width"};
    static const char *values[][4] = {
        {"topbar", "hamburger", NULL, NULL},
        {"blog", "community", NULL, NULL},
        {"compact", "comfortable", "spacious", NULL},
        {"feed", "cards", "table", NULL},
        {"narrow", "standard", "wide", NULL}
    };
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        if (strcmp(key, keys[i]) != 0) continue;
        for (size_t j = 0; j < 4 && values[i][j]; j++)
            if (strcmp(value, values[i][j]) == 0) return true;
        return false;
    }
    return false;
}

bool layout_settings_save(const char *path) {
    char temporary[256];
    int needed = snprintf(temporary, sizeof(temporary), "%s.tmp.%ld", path, (long)getpid());
    if (needed < 0 || needed >= (int)sizeof(temporary)) return false;
    FILE *f = fopen(temporary, "w");
    if (!f) return false;
    bool ok = fprintf(f,
        "navigation=%s\ncontent_style=%s\ndensity=%s\nlist_style=%s\ncontent_width=%s\n",
        g_layout_settings.navigation, g_layout_settings.content_style, g_layout_settings.density,
        g_layout_settings.list_style, g_layout_settings.content_width) >= 0 &&
        fflush(f) == 0 && fsync(fileno(f)) == 0 && fclose(f) == 0 && rename(temporary, path) == 0;
    if (!ok) { fclose(f); unlink(temporary); }
    return ok;
}

bool layout_settings_load(const char *path) {
    layout_set_default();
    FILE *f = fopen(path, "r");
    if (!f) return layout_settings_save(path);
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        trim_newline(line);
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        const char *value = eq + 1;
        if (!layout_value_valid(line, value)) continue;
        if (strcmp(line, "navigation") == 0)
            snprintf(g_layout_settings.navigation, sizeof(g_layout_settings.navigation), "%s", value);
        else if (strcmp(line, "content_style") == 0)
            snprintf(g_layout_settings.content_style, sizeof(g_layout_settings.content_style), "%s", value);
        else if (strcmp(line, "density") == 0)
            snprintf(g_layout_settings.density, sizeof(g_layout_settings.density), "%s", value);
        else if (strcmp(line, "list_style") == 0)
            snprintf(g_layout_settings.list_style, sizeof(g_layout_settings.list_style), "%s", value);
        else if (strcmp(line, "content_width") == 0)
            snprintf(g_layout_settings.content_width, sizeof(g_layout_settings.content_width), "%s", value);
    }
    fclose(f);
    return true;
}

char *layout_build_css(void) {
    const char *row_padding = strcmp(g_layout_settings.density, "compact") == 0 ? "10px 0" :
                              strcmp(g_layout_settings.density, "spacious") == 0 ? "42px 0" : "28px 0";
    const char *item_padding = strcmp(g_layout_settings.density, "compact") == 0 ? "8px 12px" :
                               strcmp(g_layout_settings.density, "spacious") == 0 ? "28px 20px" : "20px 18px";
    const char *board_gap = strcmp(g_layout_settings.density, "compact") == 0 ? "12px" :
                            strcmp(g_layout_settings.density, "spacious") == 0 ? "40px" : "28px";
    const char *width = strcmp(g_layout_settings.content_width, "narrow") == 0 ? "1100px" :
                        strcmp(g_layout_settings.content_width, "wide") == 0 ? "2200px" : "1800px";
    const char *format =
        "body.width-narrow .shell,body.width-standard .shell,body.width-wide .shell{max-width:%s;}"
        "body.density-compact .post-row,body.density-comfortable .post-row,body.density-spacious .post-row,"
        "body.density-compact .post-row.featured,body.density-comfortable .post-row.featured,body.density-spacious .post-row.featured{padding:%s;}"
        "body.density-compact .board-post-item,body.density-comfortable .board-post-item,body.density-spacious .board-post-item{padding:%s;}"
        "body.density-compact .board-list,body.density-comfortable .board-list,body.density-spacious .board-list{gap:%s;}"
        "body.density-compact .board-line{padding:20px 24px;}body.density-compact .post-row-meta{padding-top:6px;}"
        "body.density-spacious .board-line{padding:44px;}body.density-spacious .post-row-meta{padding-top:16px;}"
        "body.layout-community .hero{padding-top:28px;padding-bottom:28px;}"
        "body.layout-community .hero h1{font-size:clamp(1.8rem,3vw,2.8rem);}"
        "body.layout-community .post-row-title,body.layout-community .post-row.featured .post-row-title{font-size:clamp(1.15rem,1.7vw,1.5rem);font-weight:750;line-height:1.35;border-bottom:0;padding-bottom:0;width:auto;}"
        "body.layout-community .post-row-summary{font-size:0.95rem;line-height:1.45;padding-top:4px;}"
        "body.layout-community .board-line-title{font-size:clamp(1.25rem,2vw,1.75rem);}"
        "body.list-cards .post-list{gap:16px;}body.list-cards .post-row,body.list-cards .post-row.featured{padding:24px;border:1px solid var(--glass-border);border-radius:12px;background:var(--glass-bg);}"
        "body.list-table .post-row,body.list-table .post-row.featured{padding-top:10px;padding-bottom:10px;}"
        "body.list-table .post-row-title,body.list-table .post-row.featured .post-row-title{display:inline;font-size:1rem;font-weight:650;line-height:1.4;border-bottom:0;padding-bottom:0;}"
        "body.list-table .post-row-summary{display:none;}body.list-table .post-row-head{display:inline-flex;padding:0 8px 0 0;}"
        "body.list-table .post-row-meta{display:inline-flex;padding:0 0 0 10px;font-size:12px;}"
        "body.list-table .board-post-summary{display:none;}"
        "@media(min-width:769px){body.nav-hamburger .burger-btn{display:inline-flex;}"
        "body.nav-hamburger:not(.mobile) .nav-links{display:none;position:fixed;top:60px;right:0;left:auto;width:min(380px,100vw);max-height:calc(100vh - 60px);overflow-y:auto;box-sizing:border-box;flex-direction:column;align-items:stretch;gap:0;padding:16px 0;z-index:101;background:color-mix(in srgb,var(--glass-bg) 96%,transparent);border:1px solid var(--glass-border);backdrop-filter:blur(16px) saturate(140%);}"
        "body.nav-hamburger:not(.mobile) .nav-links.open{display:flex;}"
        "body.nav-hamburger:not(.mobile) .nav-item{display:block;padding:14px 24px;border-bottom:1px solid var(--border);}"
        "body.nav-hamburger:not(.mobile) .desktop-only{display:none;}"
        "body.nav-hamburger:not(.mobile) .mobile-account-header{display:flex;padding:16px 24px;gap:12px;align-items:center;border-bottom:1px solid var(--border);order:-1;}"
        "body.nav-hamburger:not(.mobile) .nav-board-dropdown,body.nav-hamburger:not(.mobile) .nav-admin-dropdown{display:block;width:100%;padding:0;margin:0;}"
        "body.nav-hamburger:not(.mobile) .nav-board-menu,body.nav-hamburger:not(.mobile) .nav-admin-menu{position:static;min-width:0;box-shadow:none;}"
        "body.list-table .post-row-meta{float:right;padding-left:16px;}}"
        "@media(max-width:600px){body.list-cards .post-row,body.list-cards .post-row.featured{padding:18px;}body.list-table .post-row-meta{display:flex;float:none;padding:4px 0 0;}body.density-spacious .board-line{padding:26px;}}";
    int needed = snprintf(NULL, 0, format, width, row_padding, item_padding, board_gap);
    if (needed < 0) return NULL;
    char *css = malloc((size_t)needed + 1);
    if (css) snprintf(css, (size_t)needed + 1, format, width, row_padding, item_padding, board_gap);
    return css;
}

/* Resolved mail domain for the local mail stack (see blog_config_load). */
const char *fly_mail_domain(void) {
    return g_config.mail_domain[0] ? g_config.mail_domain : "localhost";
}


bool font_settings_save(const char *path) {
    char temporary[512];
    int needed = snprintf(temporary, sizeof(temporary), "%s.tmp.%ld", path, (long)getpid());
    if (needed < 0 || (size_t)needed >= sizeof(temporary)) return false;
    FILE *file = fopen(temporary, "w");
    if (!file) return false;
#define WRITE_FONT_SETTING(key, value) fprintf(file, key "=%s\n", value)
    WRITE_FONT_SETTING("font_import_url", g_font_settings.import_url);
    WRITE_FONT_SETTING("font_script_url", g_font_settings.script_url);
    WRITE_FONT_SETTING("font_face_family", g_font_settings.face_family);
    WRITE_FONT_SETTING("font_face_src", g_font_settings.face_src);
    WRITE_FONT_SETTING("font_body", g_font_settings.body);
    WRITE_FONT_SETTING("font_heading", g_font_settings.heading);
    WRITE_FONT_SETTING("font_ui", g_font_settings.ui);
    WRITE_FONT_SETTING("font_code", g_font_settings.code);
    WRITE_FONT_SETTING("font_blockquote", g_font_settings.blockquote);
    WRITE_FONT_SETTING("font_display", g_font_settings.display);
    WRITE_FONT_SETTING("letter_spacing_body", g_font_settings.letter_spacing_body);
    WRITE_FONT_SETTING("letter_spacing_h1", g_font_settings.letter_spacing_h1);
    WRITE_FONT_SETTING("letter_spacing_h2", g_font_settings.letter_spacing_h2);
    WRITE_FONT_SETTING("letter_spacing_h3", g_font_settings.letter_spacing_h3);
    WRITE_FONT_SETTING("letter_spacing_h4", g_font_settings.letter_spacing_h4);
    WRITE_FONT_SETTING("letter_spacing_h5h6", g_font_settings.letter_spacing_h5h6);
    WRITE_FONT_SETTING("letter_spacing_topbar_title", g_font_settings.letter_spacing_topbar_title);
    WRITE_FONT_SETTING("letter_spacing_btn", g_font_settings.letter_spacing_btn);
    WRITE_FONT_SETTING("letter_spacing_board_line_title", g_font_settings.letter_spacing_board_line_title);
    WRITE_FONT_SETTING("letter_spacing_hero_h1", g_font_settings.letter_spacing_hero_h1);
    WRITE_FONT_SETTING("letter_spacing_hero_p", g_font_settings.letter_spacing_hero_p);
    WRITE_FONT_SETTING("letter_spacing_md_h1", g_font_settings.letter_spacing_md_h1);
    WRITE_FONT_SETTING("letter_spacing_md_h2", g_font_settings.letter_spacing_md_h2);
    WRITE_FONT_SETTING("letter_spacing_md_h3", g_font_settings.letter_spacing_md_h3);
    WRITE_FONT_SETTING("letter_spacing_post_h1", g_font_settings.letter_spacing_post_h1);
    WRITE_FONT_SETTING("font_weight_body", g_font_settings.font_weight_body);
    WRITE_FONT_SETTING("font_weight_h1", g_font_settings.font_weight_h1);
    WRITE_FONT_SETTING("font_weight_h2", g_font_settings.font_weight_h2);
    WRITE_FONT_SETTING("font_weight_h3", g_font_settings.font_weight_h3);
    WRITE_FONT_SETTING("font_weight_h4", g_font_settings.font_weight_h4);
    WRITE_FONT_SETTING("font_weight_h5h6", g_font_settings.font_weight_h5h6);
    WRITE_FONT_SETTING("font_weight_topbar_title", g_font_settings.font_weight_topbar_title);
    WRITE_FONT_SETTING("font_weight_btn", g_font_settings.font_weight_btn);
    WRITE_FONT_SETTING("font_weight_board_line_title", g_font_settings.font_weight_board_line_title);
    WRITE_FONT_SETTING("font_weight_hero_h1", g_font_settings.font_weight_hero_h1);
    WRITE_FONT_SETTING("font_weight_md_h1", g_font_settings.font_weight_md_h1);
    WRITE_FONT_SETTING("font_weight_md_h2", g_font_settings.font_weight_md_h2);
    WRITE_FONT_SETTING("font_weight_md_h3", g_font_settings.font_weight_md_h3);
    WRITE_FONT_SETTING("font_weight_post_h1", g_font_settings.font_weight_post_h1);
#undef WRITE_FONT_SETTING
    for (size_t i = 0; i < g_font_settings.download_count; i++)
        fprintf(file, "font_file=%s %s\n", g_font_settings.download_name[i], g_font_settings.download_url[i]);
    bool ok = fflush(file) == 0 && fsync(fileno(file)) == 0 && fclose(file) == 0;
    if (!ok || rename(temporary, path) != 0) {
        fclose(file);
        unlink(temporary);
        return false;
    }
    return true;
}

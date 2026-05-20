#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

blog_config_t g_config = {0};

static void set_default(void) {
    snprintf(g_config.title, sizeof(g_config.title), "CWIST Docker Blog");
    snprintf(g_config.subtitle, sizeof(g_config.subtitle), "Explore boards and read stories.");
    snprintf(g_config.brand_footer, sizeof(g_config.brand_footer), "Built with CWIST C Framework");
    snprintf(g_config.accent, sizeof(g_config.accent), "#3b82f6");
    g_config.port = 8443;
    g_config.use_tasfa = true;
    g_config.use_rss = false;
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
            fclose(f);
        }
        return true;
    }
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
        } else if (strcmp(key, "root_url") == 0) {
            snprintf(g_config.root_url, sizeof(g_config.root_url), "%s", val);
        } else if (strcmp(key, "use_tasfa") == 0) {
            g_config.use_tasfa = (strcmp(val, "true") == 0 || strcmp(val, "1") == 0);
        } else if (strcmp(key, "use_rss") == 0) {
            g_config.use_rss = (strcmp(val, "true") == 0 || strcmp(val, "1") == 0);
        }
    }
    fclose(f);
    if (!g_config.title[0]) snprintf(g_config.title, sizeof(g_config.title), "CWIST Docker Blog");
    if (!g_config.subtitle[0]) snprintf(g_config.subtitle, sizeof(g_config.subtitle), "Explore boards and read stories.");
    if (!g_config.brand_footer[0]) snprintf(g_config.brand_footer, sizeof(g_config.brand_footer), "Built with CWIST C Framework");
    if (!g_config.accent[0]) snprintf(g_config.accent, sizeof(g_config.accent), "#3b82f6");
    if (g_config.port <= 0 || g_config.port > 65535) g_config.port = 8443;
    return true;
}

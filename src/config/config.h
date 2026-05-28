#ifndef BLOG_CONFIG_H
#define BLOG_CONFIG_H

#include <stdbool.h>

typedef struct {
    char title[128];
    char subtitle[256];
    char brand_footer[256];
    char accent[16];
    int port;
    char home_img[256];
    char blog_logo[256];
    char boards_img[256];
    char files_img[256];
    char favicon[256];
    char root_url[256];
    bool use_tasfa;
    bool use_rss;
    long long max_upload_size;
    int max_total_parallel_uploads;
    int max_upload_parallel_chunks;
} blog_config_t;

extern blog_config_t g_config;

bool blog_config_load(const char *path);

typedef struct {
    char import_url[512];
    char face_family[64];
    char face_src[512];
    char body[256];
    char heading[256];
    char ui[256];
    char code[256];
    char blockquote[256];
    char display[256];
} font_settings_t;

extern font_settings_t g_font_settings;

bool font_settings_load(const char *path);

#endif

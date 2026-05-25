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

#endif

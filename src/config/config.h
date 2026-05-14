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
} blog_config_t;

extern blog_config_t g_config;

bool blog_config_load(const char *path);

#endif

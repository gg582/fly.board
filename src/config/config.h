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
    char letter_spacing_body[16];
    char letter_spacing_h1[16];
    char letter_spacing_h2[16];
    char letter_spacing_h3[16];
    char letter_spacing_h4[16];
    char letter_spacing_h5h6[16];
    char letter_spacing_topbar_title[16];
    char letter_spacing_btn[16];
    char letter_spacing_board_line_title[16];
    char letter_spacing_hero_h1[16];
    char letter_spacing_hero_p[16];
    char letter_spacing_md_h1[16];
    char letter_spacing_md_h2[16];
    char letter_spacing_md_h3[16];
    char letter_spacing_post_h1[16];
    char font_weight_body[8];
    char font_weight_h1[8];
    char font_weight_h2[8];
    char font_weight_h3[8];
    char font_weight_h4[8];
    char font_weight_h5h6[8];
    char font_weight_topbar_title[8];
    char font_weight_btn[8];
    char font_weight_board_line_title[8];
    char font_weight_hero_h1[8];
    char font_weight_md_h1[8];
    char font_weight_md_h2[8];
    char font_weight_md_h3[8];
    char font_weight_post_h1[8];
} font_settings_t;

extern font_settings_t g_font_settings;

bool font_settings_load(const char *path);

#endif

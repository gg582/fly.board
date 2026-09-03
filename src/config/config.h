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
    char blog_logo_dark[256];
    char boards_img[256];
    char files_img[256];
    char favicon[256];
    char root_url[256];
    char bg_full_light[256];
    char bg_full_dark[256];
    /* Optional dark-mode variants of the hero backgrounds.  When empty, the
     * base image is used in dark mode as well. */
    char home_img_dark[256];
    char boards_img_dark[256];
    char files_img_dark[256];
    /* Comma-separated targets ("home", "boards", "files", "toplevel") whose
     * missing light/dark counterpart is filled with the inverted image. */
    char bg_invert_color[128];
    /* Inversion algorithm for bg_invert_color: "luminv" (default, keeps
     * line-art contrast) or "oklch" (perceptual L-flip, smoother on
     * photos). */
    char bg_invert_algo[16];
    /* When true and only one of blog_logo/blog_logo_dark is set, the other
     * mode's logo is filled with the inverted variant.  Ignored when both
     * are set. */
    bool invert_logo;
    bool use_tasfa;
    bool use_rss;
    bool use_tls;
    bool use_http2;
    bool use_http3;
    float roundness;
    long long max_upload_size;
    int max_total_parallel_uploads;
    int max_upload_parallel_chunks;
    int max_concurrent_downloads;
    char use_special_modes[128];
    /* Who may vote on posts: "" or "all" (anyone, current behavior),
     * "authorized" (logged-in users only), "admin" (admins only). */
    char vote_only[16];
} blog_config_t;

extern blog_config_t g_config;

bool blog_config_load(const char *path);

/* True when target ("home", "boards", "files", "toplevel") is listed in the
 * bg_invert_color setting. */
bool config_bg_invert_enabled(const char *target);

/* Resolve the effective background image for a mode.  Picks the dark variant
 * in dark mode when present, otherwise falls back to whichever variant is
 * configured.  *out_invert is set when the target is listed in
 * bg_invert_color, only one of light/dark is configured, and the current
 * mode is the one without its own image — i.e. the image must be shown with
 * inverted colors.  When both variants are explicitly configured, inversion
 * is void and *out_invert is always false. */
void config_resolve_bg(const char *light_img, const char *dark_img, const char *target,
                       bool dark_mode, const char **out_img, bool *out_invert);

/* True when a user with the given login state/role may cast a vote,
 * according to the vote_only setting. */
bool config_vote_allowed(bool logged_in, const char *role);

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

typedef struct {
    /* All fields empty means S3 is disabled (the default).  S3 is used only
     * when endpoint, bucket, access_key and secret_key are all set. */
    char endpoint[256];     /* e.g. https://s3.amazonaws.com or https://minio.local:9000 */
    char region[64];        /* e.g. us-east-1 */
    char bucket[128];
    char access_key[128];
    char secret_key[256];
    char prefix[256];       /* optional key prefix, e.g. "uploads/" */
    bool use_path_style;    /* path-style (bucket in URL path) vs virtual-host style */
    /* "mirror" (default when enabled): keep the local file and also copy it
     * to S3; downloads keep using the local file.  "offload": move the file
     * to S3, delete the local copy, and serve downloads via presigned
     * redirects.  Anything else behaves as "mirror". */
    char mode[16];
} s3_config_t;

extern s3_config_t g_s3_config;

bool s3_config_load(const char *path);
/* True when the S3 settings are complete enough to use. */
bool s3_config_enabled(void);
/* True when S3 is enabled with mode=offload. */
bool s3_config_offload(void);

/* robots.txt / llms.txt access policy (robots.settings).
 * Each level is "allow" (default), "restrict", or "block":
 *  - robots.level: general crawler policy.  allow = full access;
 *    restrict = full access except admin/private paths; block = no crawlers.
 *  - llms.level: AI/LLM policy.  allow = serve /llms.txt and allow AI
 *    crawlers; restrict = serve /llms.txt but disallow training crawlers in
 *    robots.txt; block = no /llms.txt (404) and AI crawlers disallowed. */
typedef struct {
    char robots_level[16];
    char llms_level[16];
} robots_config_t;

extern robots_config_t g_robots_config;

bool robots_config_load(const char *path);
/* Normalized level accessors ("allow" for anything unrecognized). */
const char *robots_level(void);
const char *llms_level(void);

#endif

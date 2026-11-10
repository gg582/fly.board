/* Unit test: src/config/config.c blog.settings parsing: defaults, key/value
 * parsing, FLY_MAIL_DOMAIN env override, and root_url host fallback for the
 * resolved mail domain.
 *
 * Build:
 *   gcc -O1 -Iinclude -Isrc -I/home/yjlee/cwist/include -I/home/yjlee/cwist/lib \
 *       tests/test_config.c src/config/config.o \
 *       /home/yjlee/cwist/libcwist.a /home/yjlee/cwist/lib/libttak/lib/libttak.a \
 *       -lpthread -lm -ldl -o /tmp/test_config
 * Run: /tmp/test_config
 */
#include "config/config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures = 0;
#define CHECK(cond, name) do { \
    if (cond) printf("PASS: %s\n", name); \
    else { printf("FAIL: %s (line %d)\n", name, __LINE__); failures++; } \
} while (0)

#define PATH "/tmp/flytest_blog.settings"

static void write_settings(const char *content) {
    FILE *f = fopen(PATH, "w");
    fputs(content, f);
    fclose(f);
}

int main(void) {
    unsetenv("FLY_MAIL_DOMAIN");
    remove(PATH);

    /* missing file -> defaults written and loaded */
    CHECK(blog_config_load(PATH), "load with missing file succeeds");
    CHECK(access(PATH, F_OK) == 0, "missing file created with defaults");
    CHECK(g_config.title[0] != '\0', "default title set");
    CHECK(g_config.port > 0, "default port set");
    CHECK(!g_config.realtime_enabled, "realtime disabled by default");
    CHECK(!g_config.realtime_typing, "typing disabled by default");
    CHECK(!g_config.notification_aggregation, "notification aggregation disabled by default");
    CHECK(g_config.notification_window_seconds == 300, "notification window defaults to five minutes");
    CHECK(strcmp(g_config.language, "ko") == 0, "default language is ko");
    /* a just-created file is written with defaults but not parsed until the
     * next load; mail_domain resolves on the reload */
    CHECK(g_config.mail_domain[0] == '\0', "fresh defaults leave mail_domain unresolved");
    CHECK(blog_config_load(PATH), "reload freshly created defaults");
    CHECK(strcmp(g_config.mail_domain, "localhost") == 0,
          "mail domain falls back to localhost");

    /* key/value parsing */
    write_settings(
        "title=My Blog\n"
        "subtitle=Sub here\n"
        "port=9999\n"
        "language=en\n"
        "invert_logo=true\n"
        "use_tasfa=true\n"
        "realtime_enabled=true\n"
        "realtime_typing=true\n"
        "roundness=0.75\n"
        "max_upload_size=5M\n"
        "vote_only=authorized\n"
        "notification_aggregation=true\n"
        "notification_window_seconds=120\n"
        "root_url=https://blog.example.com:8443/path\nmail_domain=\n"
    );
    CHECK(blog_config_load(PATH), "load custom settings");
    CHECK(strcmp(g_config.title, "My Blog") == 0, "title parsed");
    CHECK(strcmp(g_config.subtitle, "Sub here") == 0, "subtitle parsed");
    CHECK(g_config.port == 9999, "port parsed");
    CHECK(strcmp(g_config.language, "en") == 0, "language parsed");
    CHECK(g_config.invert_logo == true, "invert_logo bool parsed");
    CHECK(g_config.use_tasfa == true, "use_tasfa bool parsed");
    CHECK(g_config.realtime_enabled && g_config.realtime_typing, "realtime flags parsed");
    CHECK(g_config.roundness > 0.7f && g_config.roundness < 0.8f, "roundness parsed");
    CHECK(g_config.max_upload_size == 5LL * 1024 * 1024, "max_upload_size parsed");
    CHECK(strcmp(g_config.vote_only, "authorized") == 0, "vote_only parsed");
    CHECK(g_config.notification_aggregation, "notification aggregation opt-in parsed");
    CHECK(g_config.notification_window_seconds == 120, "notification window parsed");
    CHECK(strcmp(g_config.mail_domain, "blog.example.com") == 0,
          "mail domain derived from root_url host (port/path stripped)");

    /* explicit mail_domain setting wins over root_url host */
    write_settings("root_url=https://blog.example.com\nmail_domain=mail.example.org\n");
    CHECK(blog_config_load(PATH), "load with explicit mail_domain");
    CHECK(strcmp(g_config.mail_domain, "mail.example.org") == 0,
          "explicit mail_domain setting wins");

    /* FLY_MAIL_DOMAIN env override wins over everything */
    setenv("FLY_MAIL_DOMAIN", "env.example.net", 1);
    write_settings("root_url=https://blog.example.com\nmail_domain=mail.example.org\n");
    CHECK(blog_config_load(PATH), "load with env override present");
    CHECK(strcmp(g_config.mail_domain, "env.example.net") == 0,
          "FLY_MAIL_DOMAIN env overrides setting");
    unsetenv("FLY_MAIL_DOMAIN");

    /* vote_only normalization */
    write_settings("vote_only=bogus\n");
    CHECK(blog_config_load(PATH), "load with bogus vote_only");
    CHECK(g_config.vote_only[0] == '\0', "bogus vote_only reset to default");

    /* accessor helpers */
    CHECK(config_vote_allowed(false, NULL), "vote allowed for all by default");
    write_settings("vote_only=authorized\n");
    CHECK(blog_config_load(PATH), "reload vote_only=authorized");
    CHECK(!config_vote_allowed(false, NULL), "anonymous vote denied when authorized");
    CHECK(config_vote_allowed(true, "user"), "logged-in vote allowed when authorized");

    remove(PATH);
    CHECK(font_settings_load(PATH), "load default font settings");
    snprintf(g_font_settings.script_url, sizeof(g_font_settings.script_url), "/assets/js/appearance-webfont.js?v=1");
    snprintf(g_font_settings.body, sizeof(g_font_settings.body), "'Custom Font', sans-serif");
    CHECK(font_settings_save(PATH), "save font settings from appearance editor");
    CHECK(font_settings_load(PATH), "reload saved font settings");
    CHECK(strcmp(g_font_settings.script_url, "/assets/js/appearance-webfont.js?v=1") == 0, "webfont script URL round trip");
    CHECK(strcmp(g_font_settings.body, "'Custom Font', sans-serif") == 0, "font stack round trip");

    remove(PATH);
    CHECK(layout_settings_load(PATH), "load default layout settings");
    CHECK(strcmp(g_layout_settings.navigation, "topbar") == 0, "responsive top bar is the default");
    CHECK(strcmp(g_layout_settings.content_style, "blog") == 0, "blog presentation is the default");
    CHECK(strcmp(g_layout_settings.density, "comfortable") == 0, "comfortable density is the default");
    CHECK(g_layout_settings.invert_transparent_images, "transparent image inversion enabled by default");
    CHECK(strcmp(g_layout_settings.transparent_image_formats, "svg") == 0, "SVG is the only default format");
    snprintf(g_layout_settings.navigation, sizeof(g_layout_settings.navigation), "hamburger");
    snprintf(g_layout_settings.content_style, sizeof(g_layout_settings.content_style), "community");
    snprintf(g_layout_settings.density, sizeof(g_layout_settings.density), "compact");
    snprintf(g_layout_settings.list_style, sizeof(g_layout_settings.list_style), "table");
    snprintf(g_layout_settings.content_width, sizeof(g_layout_settings.content_width), "wide");
    snprintf(g_layout_settings.panel_style, sizeof(g_layout_settings.panel_style), "bubble");
    snprintf(g_layout_settings.panel_color, sizeof(g_layout_settings.panel_color), "#12345680");
    g_layout_settings.panel_width = 460;
    g_layout_settings.upload_chunk_kib = 512;
    snprintf(g_layout_settings.panel_side, sizeof(g_layout_settings.panel_side), "right");
    snprintf(g_layout_settings.panel_radius, sizeof(g_layout_settings.panel_radius), "24");
    snprintf(g_layout_settings.code_theme_light, sizeof(g_layout_settings.code_theme_light), "monodraw-light");
    snprintf(g_layout_settings.code_theme_dark, sizeof(g_layout_settings.code_theme_dark), "tokyo-night-dark");
    snprintf(g_layout_settings.default_mode, sizeof(g_layout_settings.default_mode), "dark");
    g_layout_settings.invert_transparent_images = false;
    snprintf(g_layout_settings.transparent_image_formats, sizeof(g_layout_settings.transparent_image_formats), "png,webp");
    CHECK(layout_settings_save(PATH), "save layout settings from appearance editor");
    memset(&g_layout_settings, 0, sizeof(g_layout_settings));
    CHECK(layout_settings_load(PATH), "reload saved layout settings");
    CHECK(strcmp(g_layout_settings.navigation, "hamburger") == 0, "navigation round trip");
    CHECK(strcmp(g_layout_settings.content_style, "community") == 0, "content style round trip");
    CHECK(strcmp(g_layout_settings.density, "compact") == 0, "density round trip");
    CHECK(strcmp(g_layout_settings.list_style, "table") == 0, "list style round trip");
    CHECK(strcmp(g_layout_settings.content_width, "wide") == 0, "content width round trip");
    CHECK(strcmp(g_layout_settings.panel_style, "bubble") == 0, "panel style round trip");
    CHECK(strcmp(g_layout_settings.panel_color, "#12345680") == 0, "RGBA panel color round trip");
    CHECK(strcmp(g_layout_settings.code_theme_light, "monodraw-light") == 0 &&
          strcmp(g_layout_settings.code_theme_dark, "tokyo-night-dark") == 0, "code themes round trip");
    CHECK(strcmp(g_layout_settings.default_mode, "dark") == 0, "default mode round trip");
    CHECK(!layout_value_valid("default_mode", "ocean"), "default mode rejects other values");
    {
        char saved[128];
        snprintf(saved, sizeof(saved), "%s", g_layout_settings.disabled_sections);
        snprintf(g_layout_settings.disabled_sections, sizeof(g_layout_settings.disabled_sections), "archive,rss,tags");
        CHECK(!site_section_enabled("archive") && site_section_enabled("files") && site_section_enabled("series"),
              "section list parses");
        CHECK(site_section_path_disabled("/archive") && site_section_path_disabled("/archive/2026-01") &&
              site_section_path_disabled("/rss.xml") && site_section_path_disabled("/board/dev/rss.xml") &&
              site_section_path_disabled("/tag/c") && site_section_path_disabled("/tags") &&
              site_section_path_disabled("/api/tag-graph?board=1"),
              "disabled sections block their paths");
        CHECK(!site_section_path_disabled("/archives") && !site_section_path_disabled("/files") &&
              !site_section_path_disabled("/post/hello") && !site_section_path_disabled("/api/tags") &&
              !site_section_path_disabled("/series/3"),
              "enabled sections and look-alike paths stay reachable");
        CHECK(layout_value_valid("disabled_sections", "mail,guestbook") &&
              !layout_value_valid("disabled_sections", "mail,boards"), "only known sections are accepted");
        CHECK(layout_settings_save(PATH), "save with disabled sections");
        memset(&g_layout_settings, 0, sizeof(g_layout_settings));
        CHECK(layout_settings_load(PATH) && strcmp(g_layout_settings.disabled_sections, "archive,rss,tags") == 0,
              "disabled sections round trip");
        snprintf(g_layout_settings.disabled_sections, sizeof(g_layout_settings.disabled_sections), "%s", saved);
    }
    {
        char href[192];
        code_theme_href("monodraw-dark", href, sizeof(href));
        CHECK(strcmp(href, "/assets/css/hljs-monodraw-dark.css?v=1") == 0, "Monodraw is served locally");
        code_theme_href("atom-one-light", href, sizeof(href));
        CHECK(strcmp(href, "https://cdnjs.cloudflare.com/ajax/libs/highlight.js/11.12.0/styles/atom-one-light.min.css") == 0,
              "highlight.js themes come from the CDN");
        CHECK(code_theme_local_path("monodraw-light") && access(code_theme_local_path("monodraw-light"), R_OK) == 0 &&
              access(code_theme_local_path("monodraw-dark"), R_OK) == 0, "Monodraw stylesheets exist");
        CHECK(code_theme_valid("seoulism") && code_theme_valid("seoulism-intense") &&
              access(code_theme_local_path("seoulism"), R_OK) == 0 &&
              access(code_theme_local_path("seoulism-intense"), R_OK) == 0, "Seoulism stylesheets exist");
        CHECK(layout_value_valid("code_theme_dark", "github-dark") && !layout_value_valid("code_theme_dark", "../x") &&
              !layout_value_valid("code_theme_light", ""), "code theme ids are whitelisted");
    }
    CHECK(g_layout_settings.panel_width == 460 && strcmp(g_layout_settings.panel_side, "right") == 0,
          "panel width and side round trip");
    CHECK(strcmp(g_layout_settings.panel_radius, "24") == 0, "custom panel radius round trip");
    CHECK(g_layout_settings.upload_chunk_kib == 512, "upload chunk size round trip");
    CHECK(layout_value_valid("upload_chunk_kib", "8192") && !layout_value_valid("upload_chunk_kib", "8193") &&
          !layout_value_valid("upload_chunk_kib", "128"), "upload chunk size is bounded");
    char *panel_css = layout_build_css();
    CHECK(panel_css && strstr(panel_css, "width:min(460px,100vw);right:0;left:auto;border-radius:24px;"),
          "right panel anchors to the right edge with the custom radius");
    CHECK(panel_css && strstr(panel_css, "body.panel-side-right .burger-btn{order:99;}"),
          "right side moves the hamburger to the right");
    CHECK(panel_css && !strstr(panel_css, "translate("), "free-form panel offset is gone");
    free(panel_css);
    char radius[16];
    snprintf(g_layout_settings.panel_radius, sizeof(g_layout_settings.panel_radius), "auto");
    layout_panel_radius_css(radius, sizeof(radius));
    CHECK(strcmp(radius, "32px") == 0, "auto radius follows the bubble style");
    snprintf(g_layout_settings.panel_radius, sizeof(g_layout_settings.panel_radius), "site");
    float saved_roundness = g_config.roundness;
    g_config.roundness = 0.5f;
    layout_panel_radius_css(radius, sizeof(radius));
    CHECK(strcmp(radius, "16.0px") == 0, "site radius follows the site roundness");
    g_config.roundness = 0.0f;
    layout_panel_radius_css(radius, sizeof(radius));
    CHECK(strcmp(radius, "0") == 0, "zero site roundness gives square corners");
    g_config.roundness = saved_roundness;
    snprintf(g_layout_settings.panel_radius, sizeof(g_layout_settings.panel_radius), "auto");
    CHECK(layout_value_valid("panel_side", "left") && layout_value_valid("panel_side", "right") &&
          !layout_value_valid("panel_side", "center"), "panel side is left or right only");
    CHECK(layout_value_valid("panel_radius", "auto") && layout_value_valid("panel_radius", "site") &&
          layout_value_valid("panel_radius", "48") && !layout_value_valid("panel_radius", "49") &&
          !layout_value_valid("panel_radius", "-1"), "panel radius accepts auto, site, 0..48");
    CHECK(!g_layout_settings.invert_transparent_images, "disabled image inversion round trip");
    CHECK(strcmp(g_layout_settings.transparent_image_formats, "png,webp") == 0, "selected image formats round trip");
    CHECK(!layout_value_valid("density", "invalid"), "invalid layout value rejected");
    CHECK(!layout_value_valid("panel_color", "#12345"), "invalid panel color rejected");
    CHECK(!layout_value_valid("panel_width", "900"), "out-of-range panel width rejected");
    CHECK(!layout_value_valid("invert_transparent_images", "sometimes"), "invalid image inversion value rejected");
    CHECK(layout_value_valid("transparent_image_formats", "all"), "all image formats accepted");
    CHECK(layout_value_valid("transparent_image_formats", "svg,png,webp,gif,avif,jpg,bmp,ico"),
          "every documented extension accepted");
    CHECK(!layout_value_valid("transparent_image_formats", "svg,all"), "all cannot be mixed with extensions");
    CHECK(!layout_value_valid("transparent_image_formats", "svg,,png"), "empty format rejected");
    CHECK(!layout_value_valid("transparent_image_formats", "exe"), "unknown format rejected");
    {
        /* A layout.settings written before panel_side still loads: the old
         * free-form offsets are ignored and the side defaults to left. */
        const char *legacy = "/tmp/flytest_layout_legacy.settings";
        FILE *lf = fopen(legacy, "w");
        if (lf) {
            fputs("navigation=hamburger\npanel_style=round\npanel_width=400\npanel_x=-120\npanel_y=80\n", lf);
            fclose(lf);
        }
        layout_settings_t keep = g_layout_settings;
        CHECK(layout_settings_load(legacy), "legacy layout settings load");
        CHECK(g_layout_settings.panel_width == 400 && strcmp(g_layout_settings.panel_side, "left") == 0 &&
              strcmp(g_layout_settings.panel_radius, "auto") == 0, "legacy offsets ignored, new fields default");
        CHECK(strcmp(g_layout_settings.code_theme_light, "github") == 0 &&
              strcmp(g_layout_settings.code_theme_dark, "github-dark") == 0, "code themes default to GitHub");
        remove(legacy);
        g_layout_settings = keep;
    }
    char *layout_css = layout_build_css();
    CHECK(layout_css && strstr(layout_css, "nav-hamburger") && strstr(layout_css, "density-compact"),
          "layout CSS reflects saved navigation and density");
    CHECK(layout_css && strstr(layout_css, "width:min(460px,100vw)") &&
          strstr(layout_css, "right:0;left:auto;") && strstr(layout_css, "#12345680") &&
          strstr(layout_css, "border-radius:32px"), "panel side, RGBA color, and bubble style rendered");
    CHECK(layout_css && !strstr(layout_css, "data-svg-transparent"), "disabled image inversion emits no filter");
    free(layout_css);
    g_layout_settings.invert_transparent_images = true;
    layout_css = layout_build_css();
    CHECK(layout_css && strstr(layout_css, "html[data-theme-base=dark]") &&
          strstr(layout_css, "body.transparent-image-invert-enabled") && strstr(layout_css, "invert(1)"),
          "enabled image inversion renders the dark-only filter");
    free(layout_css);
    snprintf(g_layout_settings.transparent_image_formats, sizeof(g_layout_settings.transparent_image_formats), "all");
    CHECK(layout_settings_save(PATH), "save all-format setting");
    memset(&g_layout_settings, 0, sizeof(g_layout_settings));
    CHECK(layout_settings_load(PATH), "reload all-format setting");
    CHECK(g_layout_settings.invert_transparent_images && strcmp(g_layout_settings.transparent_image_formats, "all") == 0,
          "all-format setting round trip");

    write_settings("invert_transparent_svg=false\n");
    CHECK(layout_settings_load(PATH), "load legacy disabled SVG setting");
    CHECK(!g_layout_settings.invert_transparent_images, "legacy disabled state migrated");
    CHECK(strcmp(g_layout_settings.transparent_image_formats, "svg") == 0, "legacy migration keeps SVG default formats");
    write_settings("invert_transparent_svg=true\ntransparent_image_formats=png,gif\n");
    CHECK(layout_settings_load(PATH), "load legacy enabled SVG setting with new formats");
    CHECK(g_layout_settings.invert_transparent_images && strcmp(g_layout_settings.transparent_image_formats, "png,gif") == 0,
          "legacy enable migrates with selected formats");
    write_settings("invert_transparent_svg=false\ninvert_transparent_images=true\n");
    CHECK(layout_settings_load(PATH), "load both legacy and new enable settings");
    CHECK(g_layout_settings.invert_transparent_images, "new enable setting takes precedence over legacy");

    remove(PATH);
    printf("%s\n", failures == 0 ? "ALL PASS" : "SOME FAILED");
    {
        const char *fpath = "/tmp/flytest_fonts.settings";
        remove(fpath);
        CHECK(font_settings_load(fpath), "font settings created with defaults");
        snprintf(g_font_settings.role_family[3], sizeof(g_font_settings.role_family[3]), "Iosevka Test");
        snprintf(g_font_settings.role_src[3], sizeof(g_font_settings.role_src[3]), "url('/assets/fonts/appearance-code-1.woff2')");
        CHECK(font_settings_save(fpath), "font settings with a code font saved");
        memset(&g_font_settings, 0, sizeof(g_font_settings));
        CHECK(font_settings_load(fpath), "font settings reloaded");
        CHECK(strcmp(g_font_roles[3], "code") == 0 && strcmp(g_font_settings.role_family[3], "Iosevka Test") == 0 &&
              strstr(g_font_settings.role_src[3], "appearance-code-1.woff2") != NULL, "code role font round trip");
        CHECK(g_font_settings.role_src[0][0] == '\0' && g_font_settings.role_src[1][0] == '\0',
              "other roles stay without an uploaded font");
        CHECK(strstr(g_font_settings.body, "Iosevka") == NULL, "body stack untouched by the code font");
        remove(fpath);
    }

    return failures == 0 ? 0 : 1;
}

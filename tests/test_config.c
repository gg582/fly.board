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
    CHECK(g_layout_settings.invert_transparent_svg, "transparent SVG inversion enabled by default");
    snprintf(g_layout_settings.navigation, sizeof(g_layout_settings.navigation), "hamburger");
    snprintf(g_layout_settings.content_style, sizeof(g_layout_settings.content_style), "community");
    snprintf(g_layout_settings.density, sizeof(g_layout_settings.density), "compact");
    snprintf(g_layout_settings.list_style, sizeof(g_layout_settings.list_style), "table");
    snprintf(g_layout_settings.content_width, sizeof(g_layout_settings.content_width), "wide");
    snprintf(g_layout_settings.panel_style, sizeof(g_layout_settings.panel_style), "bubble");
    snprintf(g_layout_settings.panel_color, sizeof(g_layout_settings.panel_color), "#12345680");
    g_layout_settings.panel_width = 460;
    g_layout_settings.panel_x = -120;
    g_layout_settings.panel_y = 80;
    g_layout_settings.invert_transparent_svg = false;
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
    CHECK(g_layout_settings.panel_width == 460 && g_layout_settings.panel_x == -120 && g_layout_settings.panel_y == 80,
          "panel geometry round trip");
    CHECK(!g_layout_settings.invert_transparent_svg, "disabled SVG inversion round trip");
    CHECK(!layout_value_valid("density", "invalid"), "invalid layout value rejected");
    CHECK(!layout_value_valid("panel_color", "#12345"), "invalid panel color rejected");
    CHECK(!layout_value_valid("panel_width", "900"), "out-of-range panel width rejected");
    CHECK(!layout_value_valid("invert_transparent_svg", "sometimes"), "invalid SVG inversion value rejected");
    char *layout_css = layout_build_css();
    CHECK(layout_css && strstr(layout_css, "nav-hamburger") && strstr(layout_css, "density-compact"),
          "layout CSS reflects saved navigation and density");
    CHECK(layout_css && strstr(layout_css, "width:min(460px,100vw)") &&
          strstr(layout_css, "translate(-120px,80px)") && strstr(layout_css, "#12345680") &&
          strstr(layout_css, "border-radius:32px"), "panel geometry, RGBA color, and bubble style rendered");
    CHECK(layout_css && !strstr(layout_css, "data-svg-transparent"), "disabled SVG inversion emits no filter");
    free(layout_css);
    g_layout_settings.invert_transparent_svg = true;
    layout_css = layout_build_css();
    CHECK(layout_css && strstr(layout_css, "html[data-theme-base=dark]") &&
          strstr(layout_css, "img[data-svg-transparent=true]") && strstr(layout_css, "invert(1)"),
          "enabled SVG inversion renders the dark-only filter");
    free(layout_css);

    remove(PATH);
    printf("%s\n", failures == 0 ? "ALL PASS" : "SOME FAILED");
    return failures == 0 ? 0 : 1;
}

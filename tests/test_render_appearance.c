#include "config/config.h"
#include "render/render.h"
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond, name) do { \
    if (cond) printf("PASS: %s\n", name); \
    else { printf("FAIL: %s (line %d)\n", name, __LINE__); failures++; } \
} while (0)

int main(void) {
    const char *path = "/tmp/flyboard-appearance-layout.settings";
    const char *checked = "name='invert_transparent_svg' value='true' data-appearance-setting checked";
    remove(path);
    CHECK(layout_settings_load(path), "appearance fixture loads defaults");

    cwist_sstring *page = render_admin_appearance(true, NULL, false, NULL);
    CHECK(page != NULL, "appearance page renders");
    if (page) {
        CHECK(strstr(page->data, checked) != NULL, "SVG inversion checkbox is checked by default");
        CHECK(strstr(page->data, "class=\"dark svg-invert-enabled nav-") != NULL,
              "dark page enables SVG inversion");
        CHECK(strstr(page->data, "data-theme-base=\"dark\"") != NULL,
              "page exposes its initial theme family");
        CHECK(strstr(page->data, "html[data-theme-base=dark] body.svg-invert-enabled") != NULL,
              "enabled page contains the dark SVG filter");
        cwist_sstring_destroy(page);
    }

    g_layout_settings.invert_transparent_svg = false;
    page = render_admin_appearance(true, NULL, false, NULL);
    CHECK(page != NULL, "appearance page renders with inversion disabled");
    if (page) {
        CHECK(strstr(page->data, checked) == NULL, "disabled SVG inversion is unchecked");
        CHECK(strstr(page->data, "class=\"dark svg-invert-enabled nav-") == NULL,
              "disabled page omits the inversion body class");
        CHECK(strstr(page->data, "html[data-theme-base=dark] body.svg-invert-enabled") == NULL,
              "disabled page omits the SVG filter");
        cwist_sstring_destroy(page);
    }

    remove(path);
    printf("%s\n", failures == 0 ? "ALL PASS" : "SOME FAILED");
    return failures ? 1 : 0;
}

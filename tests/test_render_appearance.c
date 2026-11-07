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
    const char *checked = "name='invert_transparent_images' value='true' data-appearance-setting checked";
    remove(path);
    CHECK(layout_settings_load(path), "appearance fixture loads defaults");

    cwist_sstring *page = render_admin_appearance(true, NULL, false, NULL);
    CHECK(page != NULL, "appearance page renders");
    if (page) {
        CHECK(strstr(page->data, checked) != NULL, "image inversion checkbox is checked by default");
        CHECK(strstr(page->data, "class=\"dark transparent-image-invert-enabled nav-") != NULL,
              "dark page enables transparent image inversion");
        CHECK(strstr(page->data, "data-invert-transparent-formats=\"svg\"") != NULL,
              "SVG is the only default client format");
        CHECK(strstr(page->data, "name='transparent_format_svg' value='svg' data-transparent-format data-appearance-setting checked") != NULL,
              "SVG format checkbox is checked by default");
        CHECK(strstr(page->data, "name='transparent_format_png' value='png' data-transparent-format data-appearance-setting>") != NULL,
              "PNG format is opt-in");
        CHECK(strstr(page->data, "data-theme-base=\"dark\"") != NULL,
              "page exposes its initial theme family");
        CHECK(strstr(page->data, "html[data-theme-base=dark] body.transparent-image-invert-enabled") != NULL,
              "enabled page contains the dark image filter");
        cwist_sstring_destroy(page);
    }

    g_layout_settings.invert_transparent_images = false;
    page = render_admin_appearance(true, NULL, false, NULL);
    CHECK(page != NULL, "appearance page renders with inversion disabled");
    if (page) {
        CHECK(strstr(page->data, checked) == NULL, "disabled image inversion is unchecked");
        CHECK(strstr(page->data, "class=\"dark transparent-image-invert-enabled nav-") == NULL,
              "disabled page omits the inversion body class");
        CHECK(strstr(page->data, "html[data-theme-base=dark] body.transparent-image-invert-enabled") == NULL,
              "disabled page omits the image filter");
        cwist_sstring_destroy(page);
    }

    g_layout_settings.invert_transparent_images = true;
    snprintf(g_layout_settings.transparent_image_formats, sizeof(g_layout_settings.transparent_image_formats), "all");
    page = render_admin_appearance(true, NULL, false, NULL);
    CHECK(page != NULL, "appearance page renders in all-format mode");
    if (page) {
        CHECK(strstr(page->data, "data-invert-transparent-formats=\"all\"") != NULL,
              "all-format mode reaches the client");
        CHECK(strstr(page->data, "name='transparent_format_all' value='all' data-transparent-format-all data-appearance-setting checked") != NULL,
              "all-format checkbox is checked");
        cwist_sstring_destroy(page);
    }

    remove(path);
    printf("%s\n", failures == 0 ? "ALL PASS" : "SOME FAILED");
    return failures ? 1 : 0;
}

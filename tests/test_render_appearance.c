#include "config/config.h"
#include "render/render.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

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
    if (page && getenv("DUMP_APPEARANCE")) {
        FILE *dump = fopen(getenv("DUMP_APPEARANCE"), "w");
        if (dump) { fputs(page->data, dump); fclose(dump); }
    }
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
        CHECK(strstr(page->data, "name='panel_side' value='left' data-appearance-setting checked") != NULL &&
              strstr(page->data, "name='panel_side' value='right' data-appearance-setting>") != NULL,
              "panel side offers left (default) and right");
        CHECK(strstr(page->data, "name='panel_x'") == NULL && strstr(page->data, "name='panel_y'") == NULL,
              "free-form panel offsets are gone");
        CHECK(strstr(page->data, "type='hidden' name='panel_width' value='380'") != NULL,
              "panel width is set from the preview, not a number field");
        CHECK(strstr(page->data, "name='panel_radius' value='auto' data-appearance-setting checked") != NULL &&
              strstr(page->data, "name='panel_radius_px' min='0' max='48'") != NULL &&
              strstr(page->data, "data-panel-radius disabled") != NULL,
              "panel roundness defaults to the style and keeps the slider disabled");
        CHECK(strstr(page->data, "data-site-roundness='") != NULL, "site roundness is exposed to the preview");
        CHECK(strstr(page->data, "panel-side-left") != NULL, "page body carries the panel side");
        CHECK(strstr(page->data, "<select name='code_theme_light'") != NULL &&
              strstr(page->data, "<select name='code_theme_dark'") != NULL, "code theme selects render");
        CHECK(strstr(page->data, "<option value='github' data-href='https://cdnjs.cloudflare.com/ajax/libs/highlight.js/11.12.0/styles/github.min.css' selected>GitHub</option>") != NULL,
              "light code theme defaults to GitHub");
        CHECK(strstr(page->data, ">Monodraw Light</option>") != NULL && strstr(page->data, ">Monodraw Dark</option>") != NULL,
              "Monodraw themes are offered");
        CHECK(strstr(page->data, "data-hl-dark=\"https://cdnjs.cloudflare.com/ajax/libs/highlight.js/11.12.0/styles/github-dark.min.css\"") != NULL,
              "page exposes both code theme URLs");
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

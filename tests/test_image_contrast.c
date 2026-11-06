#include "cwist/image_contrast.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    const char *path = "/tmp/fly-image-contrast.svg";
    FILE *f = fopen(path, "wb");
    if (!f) return 1;
    fputs("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"10\" height=\"10\">"
          "<path fill=\"#000000\" d=\"M0 0h10v10z\"/></svg>", f);
    fclose(f);

    char shell[512], light_text[256], dark_text[256], logo[128], overlay[256];
    if (get_image_text_style(path, "/assets/img/test.svg", false,
                             shell, sizeof(shell), light_text, sizeof(light_text),
                             logo, sizeof(logo), overlay, sizeof(overlay)) != 0)
        return 1;
    if (get_image_text_style(path, "/assets/img/test.svg", true,
                             shell, sizeof(shell), dark_text, sizeof(dark_text),
                             logo, sizeof(logo), overlay, sizeof(overlay)) != 0)
        return 1;

    unlink(path);
    if (!strstr(light_text, "color:#1a1a2e") || !strstr(dark_text, "color:#ffffff") ||
        strcmp(light_text, dark_text) == 0)
        return 1;

    puts("image contrast: light and dark SVG results have independent cache entries");
    return 0;
}

#include "render/render.h"
#include "config/config.h"
#include <stdio.h>
#include <string.h>

int main(void) {
    int failures = 0;
    cJSON *user = cJSON_CreateObject();
    cJSON_AddStringToObject(user, "username", "root");
    cJSON_AddStringToObject(user, "email", "root@example.com");
    snprintf(g_config.title, sizeof(g_config.title), "오보로나의 집");

    cwist_sstring *page = render_account_settings(user, false, "admin", NULL, NULL,
        false, 0, false, true, "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ", NULL, NULL);

#define CHECK(condition, name) do { \
    if (condition) printf("PASS: %s\n", name); \
    else { printf("FAIL: %s\n", name); failures++; } \
} while (0)

    CHECK(page != NULL, "account settings rendered");
    CHECK(page && strstr(page->data, "<svg") != NULL &&
          strstr(page->data, "TOTP setup QR code") != NULL,
        "Korean issuer produces a QR code");
    CHECK(page && strstr(page->data, "QR unavailable") == NULL,
        "Korean issuer does not hit the QR fallback");

    cwist_sstring_destroy(page);
    cJSON_Delete(user);
    printf("%s\n", failures ? "SOME TESTS FAILED" : "ALL TESTS PASSED");
    return failures ? 1 : 0;
}

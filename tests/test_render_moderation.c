#include "render/render.h"
#include <stdio.h>
#include <string.h>

cwist_sstring *render_moderation_queue(cJSON *items, const char *msg, bool dark,
                                       const char *role, const char *profile_pic, bool is_mobile);

cwist_sstring *render_page(const char *title, const char *body_html, bool dark, const char *user_role,
                           const char *profile_pic, bool is_mobile) {
    (void)title; (void)dark; (void)user_role; (void)profile_pic; (void)is_mobile;
    cwist_sstring *out = cwist_sstring_create();
    cwist_sstring_assign(out, body_html);
    return out;
}

int main(void) {
    cJSON *items = cJSON_CreateArray();
    cJSON *item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "target_type", "comment");
    cJSON_AddNumberToObject(item, "target_id", 42);
    cJSON_AddStringToObject(item, "author_name", "<author>");
    cJSON_AddStringToObject(item, "reason", "duplicate_content");
    cJSON_AddStringToObject(item, "content", "queued <body>");
    cJSON_AddItemToArray(items, item);
    cwist_sstring *page = render_moderation_queue(items, NULL, false, "moderator", NULL, false);
    int failures = 0;
#define CHECK(condition, name) do { \
    if (condition) printf("PASS: %s\n", name); \
    else { printf("FAIL: %s\n", name); failures++; } \
} while (0)
    CHECK(page && strstr(page->data, "queued &lt;body&gt;") != NULL, "comment body escaped");
    CHECK(page && strstr(page->data, "&lt;author&gt;") != NULL, "author escaped");
    CHECK(page && strstr(page->data, "name='target_id' value='42'") != NULL, "action identifies item");
    CHECK(page && strstr(page->data, "value='approve'") && strstr(page->data, "value='reject'"), "approve and reject actions rendered");
    cwist_sstring_destroy(page);
    cJSON_Delete(items);

    items = cJSON_CreateArray();
    page = render_moderation_queue(items, NULL, false, "admin", NULL, false);
    CHECK(page && strstr(page->data, "No content is waiting") != NULL, "empty queue rendered");
    cwist_sstring_destroy(page);
    cJSON_Delete(items);
    printf("%s\n", failures ? "SOME TESTS FAILED" : "ALL TESTS PASSED");
    return failures ? 1 : 0;
}

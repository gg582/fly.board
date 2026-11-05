#include "render/render.h"
#include <stdio.h>
#include <string.h>

cwist_sstring *render_page(const char *title, const char *body_html, bool dark, const char *user_role,
                           const char *profile_pic, bool is_mobile) {
    (void)title; (void)dark; (void)user_role; (void)profile_pic; (void)is_mobile;
    cwist_sstring *out = cwist_sstring_create();
    cwist_sstring_assign(out, body_html);
    return out;
}

int main(void) {
    int failures = 0;
#define CHECK(condition, name) do { \
    if (condition) printf("PASS: %s\n", name); \
    else { printf("FAIL: %s\n", name); failures++; } \
} while (0)
    cJSON *items = cJSON_CreateArray();
    cJSON *item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "actor_name", "<Alice>");
    cJSON_AddNumberToObject(item, "actor_count", 100);
    cJSON_AddStringToObject(item, "kind", "comment");
    cJSON_AddStringToObject(item, "post_slug", "direct-target");
    cJSON_AddStringToObject(item, "excerpt", "latest <excerpt>");
    cJSON_AddNumberToObject(item, "is_read", 0);
    cJSON_AddItemToArray(items, item);
    cwist_sstring *page = render_notifications_page(items, false, "user", NULL, false);
    CHECK(page && strstr(page->data, "&lt;Alice&gt; and 99 others commented on your post") != NULL,
          "aggregate count rendered and actor escaped");
    CHECK(page && strstr(page->data, "href='/post/direct-target'") != NULL, "direct post link retained");
    CHECK(page && strstr(page->data, "latest &lt;excerpt&gt;") != NULL, "latest excerpt rendered escaped");
    cwist_sstring_destroy(page);
    cJSON_Delete(items);

    items = cJSON_CreateArray();
    item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "actor_name", "Alice");
    cJSON_AddStringToObject(item, "kind", "reply");
    cJSON_AddNumberToObject(item, "is_read", 1);
    cJSON_AddItemToArray(items, item);
    page = render_notifications_page(items, false, "user", NULL, false);
    CHECK(page && strstr(page->data, "Alice replied to your comment") != NULL, "single actor rendering unchanged");
    CHECK(page && strstr(page->data, "others") == NULL, "missing count does not add aggregate suffix");
    cwist_sstring_destroy(page);
    cJSON_Delete(items);
    printf("%s\n", failures ? "SOME TESTS FAILED" : "ALL TESTS PASSED");
    return failures ? 1 : 0;
}

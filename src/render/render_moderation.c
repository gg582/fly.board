#define _POSIX_C_SOURCE 200809L
#include "render.h"
#include "db/moderation.h"
#include <stdio.h>
#include <string.h>

static const char *field(cJSON *item, const char *name) {
    cJSON *value = cJSON_GetObjectItem(item, name);
    return cJSON_IsString(value) ? value->valuestring : "";
}

static int number(cJSON *item, const char *name) {
    cJSON *value = cJSON_GetObjectItem(item, name);
    return cJSON_IsNumber(value) ? value->valueint : 0;
}

cwist_sstring *render_moderation_queue(cJSON *items, const char *msg, bool dark,
                                       const char *role, const char *profile_pic, bool is_mobile) {
    cwist_sstring *body = cwist_sstring_create();
    cwist_sstring_assign(body, "<div class='hero'><h1>Approval queue</h1></div><div class='card'>");
    if (msg && msg[0]) {
        cwist_sstring_append(body, "<p class='alert'>");
        cwist_sstring_append_escaped(body, msg);
        cwist_sstring_append(body, "</p>");
    }
    if (!items || cJSON_GetArraySize(items) == 0) {
        cwist_sstring_append(body, "<p>No content is waiting for approval.</p>");
    }
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, items) {
        char id[32];
        snprintf(id, sizeof(id), "%d", number(item, "target_id"));
        cwist_sstring_append(body, "<article style='border-top:1px solid var(--border);padding:16px 0'><h2 style='margin-top:0'>");
        if (strcmp(field(item, "target_type"), MODERATION_POST) == 0) {
            cwist_sstring_append(body, "Post: ");
            cwist_sstring_append_escaped(body, field(item, "post_title"));
        } else {
            cwist_sstring_append(body, "Comment by ");
            cwist_sstring_append_escaped(body, field(item, "author_name"));
        }
        cwist_sstring_append(body, "</h2><p><strong>Reason:</strong> ");
        cwist_sstring_append_escaped(body, field(item, "reason"));
        cwist_sstring_append(body, "</p>");
        cwist_sstring_append(body, "<pre style='white-space:pre-wrap'>");
        cwist_sstring_append_escaped(body, strcmp(field(item, "target_type"), MODERATION_COMMENT) == 0
                                     ? field(item, "content") : field(item, "post_content"));
        cwist_sstring_append(body, "</pre>");
        cwist_sstring_append(body, "<form method='post' action='/admin/moderation/action' style='display:inline'>"
                             "<input type='hidden' name='target_type' value='");
        cwist_sstring_append_escaped(body, field(item, "target_type"));
        cwist_sstring_append(body, "'><input type='hidden' name='target_id' value='");
        cwist_sstring_append(body, id);
        cwist_sstring_append(body, "'><button class='btn' name='action' value='approve'>Approve</button> "
                             "<button class='btn btn-outline' name='action' value='reject'>Reject</button></form></article>");
    }
    cwist_sstring_append(body, "</div>");
    cwist_sstring *page = render_page("Approval queue", body->data, dark, role, profile_pic, is_mobile);
    cwist_sstring_destroy(body);
    return page;
}

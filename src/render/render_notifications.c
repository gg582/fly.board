#define _POSIX_C_SOURCE 200809L
#include "render.h"
#include "render_internal.h"
#include <cwist/core/sstring/sstring.h>
#include <stdio.h>
#include <string.h>

cwist_sstring *render_notifications_page(cJSON *notifs, bool dark, const char *user_role, const char *profile_pic, bool is_mobile) {
    cwist_sstring *b = cwist_sstring_create();
    cwist_sstring_assign(b, "<div class='hero'><h1>Notifications</h1></div>");
    cwist_sstring_append(b, "<div class='card notif-card'>");

    int n = notifs ? cJSON_GetArraySize(notifs) : 0;
    if (n == 0) {
        cwist_sstring_append(b, "<p style='color:var(--muted)'>No notifications.</p>");
    }
    for (int i = 0; i < n; i++) {
        cJSON *it = cJSON_GetArrayItem(notifs, i);
        cJSON *actor = cJSON_GetObjectItem(it, "actor_name");
        cJSON *kind = cJSON_GetObjectItem(it, "kind");
        cJSON *slug = cJSON_GetObjectItem(it, "post_slug");
        cJSON *excerpt = cJSON_GetObjectItem(it, "excerpt");
        cJSON *created = cJSON_GetObjectItem(it, "created_at");
        cJSON *is_read = cJSON_GetObjectItem(it, "is_read");
        const char *actor_s = (actor && cJSON_IsString(actor) && actor->valuestring && actor->valuestring[0]) ? actor->valuestring : "Anonymous";
        const char *kind_s = (kind && cJSON_IsString(kind) && kind->valuestring) ? kind->valuestring : "";
        const char *slug_s = (slug && cJSON_IsString(slug) && slug->valuestring) ? slug->valuestring : "";
        bool unread = !(is_read && cJSON_IsNumber(is_read) && is_read->valueint);

        cwist_sstring_append(b, "<div class='notif-row' style='padding:10px 0;border-bottom:1px solid var(--border,#e5e5e5)'>");
        cwist_sstring_append(b, "<div class='notif-head'>");
        if (unread) cwist_sstring_append(b, "<strong>");
        if (slug_s[0]) {
            cwist_sstring_append(b, "<a href='/post/");
            cwist_sstring_append_escaped(b, slug_s);
            cwist_sstring_append(b, "'>");
        }
        cwist_sstring_append_escaped(b, actor_s);
        cwist_sstring_append(b, strcmp(kind_s, "reply") == 0 ? " replied to your comment" : " commented on your post");
        if (slug_s[0]) cwist_sstring_append(b, "</a>");
        if (unread) cwist_sstring_append(b, "</strong>");
        cwist_sstring_append(b, "</div>");
        if (excerpt && cJSON_IsString(excerpt) && excerpt->valuestring && excerpt->valuestring[0]) {
            cwist_sstring_append(b, "<div class='notif-excerpt' style='color:var(--muted)'>");
            cwist_sstring_append_escaped(b, excerpt->valuestring);
            cwist_sstring_append(b, "</div>");
        }
        if (created && cJSON_IsString(created) && created->valuestring) {
            cwist_sstring_append(b, "<div class='notif-date' style='color:var(--muted);font-size:12px'>");
            cwist_sstring_append_escaped(b, created->valuestring);
            cwist_sstring_append(b, "</div>");
        }
        cwist_sstring_append(b, "</div>");
    }

    cwist_sstring_append(b, "</div>");
    cwist_sstring *page = render_page("Notifications", b->data, dark, user_role, profile_pic, is_mobile);
    cwist_sstring_destroy(b);
    return page;
}

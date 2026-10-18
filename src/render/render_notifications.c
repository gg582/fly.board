#define _POSIX_C_SOURCE 200809L
#include "render.h"
#include "render_internal.h"
#include <cwist/core/sstring/sstring.h>
#include <cwist/core/template/template.h>
#include <stdio.h>
#include <string.h>

cwist_sstring *render_notifications_page(cJSON *notifs, bool dark, const char *user_role, const char *profile_pic, bool is_mobile) {
    const char *tmpl =
        "<div class='hero'><h1>Notifications</h1></div>"
        "<div class='card notif-card'>"
        "{% if not items %}<p style='color:var(--muted)'>No notifications.</p>{% endif %}"
        "{% for it in items %}"
        "<div class='notif-row' style='padding:10px 0;border-bottom:1px solid var(--border,#e5e5e5)'>"
        "<div class='notif-head'>"
        "{% if it.unread %}<strong>{% endif %}"
        "{% if it.slug %}<a href='/post/{{ it.slug|escape }}'>{% endif %}"
        "{{ it.actor|escape }}{{ it.action_label }}"
        "{% if it.slug %}</a>{% endif %}"
        "{% if it.unread %}</strong>{% endif %}"
        "</div>"
        "{% if it.excerpt %}<div class='notif-excerpt' style='color:var(--muted)'>{{ it.excerpt|escape }}</div>{% endif %}"
        "{% if it.created %}<div class='notif-date' style='color:var(--muted);font-size:12px'>{{ it.created|escape }}</div>{% endif %}"
        "</div>"
        "{% endfor %}"
        "</div>";

    cJSON *ctx = cJSON_CreateObject();
    cJSON *items = cJSON_CreateArray();
    int n = notifs ? cJSON_GetArraySize(notifs) : 0;
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

        cJSON *row = cJSON_CreateObject();
        cJSON_AddStringToObject(row, "actor", actor_s);
        cJSON_AddStringToObject(row, "action_label",
            strcmp(kind_s, "reply") == 0 ? " replied to your comment" : " commented on your post");
        cJSON_AddStringToObject(row, "slug", slug_s);
        cJSON_AddBoolToObject(row, "unread", unread);
        if (excerpt && cJSON_IsString(excerpt) && excerpt->valuestring && excerpt->valuestring[0]) {
            cJSON_AddStringToObject(row, "excerpt", excerpt->valuestring);
        }
        if (created && cJSON_IsString(created) && created->valuestring) {
            cJSON_AddStringToObject(row, "created", created->valuestring);
        }
        cJSON_AddItemToArray(items, row);
    }
    cJSON_AddItemToObject(ctx, "items", items);

    cwist_sstring *body = cwist_template_render(tmpl, ctx);
    cJSON_Delete(ctx);
    if (!body) {
        body = cwist_sstring_create();
        cwist_sstring_assign(body, "Template render error");
    }

    cwist_sstring *page = render_page("Notifications", body->data, dark, user_role, profile_pic, is_mobile);
    cwist_sstring_destroy(body);
    return page;
}

#define _POSIX_C_SOURCE 200809L
#include "render_guestbook.h"
#include "../db/guestbook.h"
#include "../utils/spam_guard.h"
#include <cwist/core/sstring/sstring.h>
#include <cwist/core/template/template.h>
#include <cjson/cJSON.h>
#include <stdio.h>
#include <string.h>

#define GUESTBOOK_PAGE_SIZE 20

/* Append text with HTML escaping, turning newlines into <br>. */
static void append_multiline(cwist_sstring *b, const char *text) {
    if (!text) return;
    for (const char *p = text; *p; p++) {
        if (*p == '\n') {
            cwist_sstring_append(b, "<br>");
        } else if (*p == '\r') {
            /* skip */
        } else {
            char tmp[2] = { *p, '\0' };
            cwist_sstring_append_escaped(b, tmp);
        }
    }
}

static int json_int_or(cJSON *obj, const char *key, int def) {
    cJSON *v = cJSON_GetObjectItem(obj, key);
    return (v && cJSON_IsNumber(v)) ? v->valueint : def;
}

static const char *json_str_or(cJSON *obj, const char *key, const char *def) {
    cJSON *v = cJSON_GetObjectItem(obj, key);
    return (v && cJSON_IsString(v) && v->valuestring) ? v->valuestring : def;
}

/* Partial HTML for one profile's guestbook: entries (oldest first within the
 * page, like a classic guestbook), pager, post form, owner toggle. */
cwist_sstring *render_guestbook_section(cwist_db *db, int owner_uid, int viewer_uid,
                                        const char *viewer_role, int page) {
    const char *tmpl =
        "<h3 style='margin-top:24px'>Guestbook</h3>"
        "<p style='color:var(--muted);font-size:13px;margin-top:0'>{{ total }} entr{{ entry_suffix }}</p>"
        "{% for e in entries %}"
        "<div style='border-top:1px solid var(--border);padding:10px 0;text-align:left'>"
        "<div style='display:flex;justify-content:space-between;gap:8px;flex-wrap:wrap'>"
        "<strong>"
        "{% if e.author_uid %}"
        "<a href='/user/{{ e.author_uid }}'>{{ e.author_name|escape }}</a>"
        "{% else %}"
        "{{ e.author_name|escape }} <small style='color:var(--muted)'>(guest)</small>"
        "{% endif %}"
        "</strong><small style='color:var(--muted)'>{{ e.created_at|escape }}</small></div>"
        "<p style='margin:6px 0 0'>{{ e.content_html }}</p>"
        "{% if e.can_delete %}"
        "<form action='/guestbook/delete' method='POST' style='margin:6px 0 0'>"
        "<input type='hidden' name='id' value='{{ e.id }}'>"
        "<input type='hidden' name='page' value='{{ page }}'>"
        "<button type='submit' class='btn btn-outline' style='font-size:12px;padding:4px 10px'>Delete</button>"
        "</form>"
        "{% endif %}"
        "</div>"
        "{% endfor %}"
        "{% if pages %}"
        "<div style='margin-top:12px;display:flex;gap:10px;justify-content:center'>"
        "{% for p in pages %}"
        "{% if p.current %}<strong>{{ p.num }}</strong>{% else %}<a href='/user/{{ owner_uid }}?page={{ p.num }}'>{{ p.num }}</a>{% endif %}"
        "{% if not p.last %} {% endif %}"
        "{% endfor %}"
        "</div>"
        "{% endif %}"
        "{% if is_owner %}"
        "<hr style='margin:20px 0;border:0;border-top:1px solid var(--border)'>"
        "<form action='/guestbook/settings' method='POST' style='text-align:left'>"
        "<label style='display:flex;align-items:center;gap:8px;font-weight:normal'>"
        "<input type='checkbox' name='guestbook_anon' value='1'{% if anon_allowed %} checked{% endif %}> Allow anonymous guestbook posts</label>"
        "<button type='submit' class='btn btn-outline' style='margin-top:8px'>Save</button>"
        "</form>"
        "{% endif %}"
        "<hr style='margin:20px 0;border:0;border-top:1px solid var(--border)'>"
        "<form action='/guestbook/post' method='POST' style='text-align:left'>"
        "<input type='hidden' name='owner' value='{{ owner_uid }}'>"
        "{% if show_anon_name %}"
        "<label>Name</label><input name='name' maxlength='64' placeholder='Anonymous'>"
        "{% endif %}"
        "{% if can_post %}"
        "<label>Message</label><textarea name='content' rows='4' maxlength='2000' "
        "placeholder='Leave a message' style='width:100%;max-width:100%'></textarea>"
        "{{ spam_fields }}"
        "<button type='submit' class='btn' style='margin-top:8px'>Sign Guestbook</button>"
        "{% else %}"
        "<p style='color:var(--muted);margin:0'><a href='/login'>Log in</a> to write in this guestbook.</p>"
        "{% endif %}"
        "</form>";

    bool is_owner = (viewer_uid > 0 && viewer_uid == owner_uid);
    bool is_admin = (viewer_role && strcmp(viewer_role, "admin") == 0);
    bool anon_allowed = db_guestbook_anon_allowed(db, owner_uid);

    int total = db_guestbook_count(db, owner_uid);
    int total_pages = (total + GUESTBOOK_PAGE_SIZE - 1) / GUESTBOOK_PAGE_SIZE;
    if (total_pages < 1) total_pages = 1;
    if (page < 1) page = 1;
    if (page > total_pages) page = total_pages;

    cJSON *ctx = cJSON_CreateObject();
    cJSON_AddNumberToObject(ctx, "total", total);
    cJSON_AddStringToObject(ctx, "entry_suffix", total == 1 ? "y" : "ies");
    cJSON_AddNumberToObject(ctx, "page", page);
    cJSON_AddNumberToObject(ctx, "owner_uid", owner_uid);
    cJSON_AddBoolToObject(ctx, "is_owner", is_owner);
    cJSON_AddBoolToObject(ctx, "anon_allowed", anon_allowed);
    cJSON_AddBoolToObject(ctx, "show_anon_name", viewer_uid <= 0 && anon_allowed);
    cJSON_AddBoolToObject(ctx, "can_post", viewer_uid > 0 || anon_allowed);

    cwist_sstring *spam_fields = cwist_sstring_create();
    spam_guard_append_fields(spam_fields);
    cJSON_AddStringToObject(ctx, "spam_fields", spam_fields->data ? spam_fields->data : "");
    cwist_sstring_destroy(spam_fields);

    cJSON *entry_list = cJSON_CreateArray();
    cJSON *entries = db_guestbook_list(db, owner_uid, (page - 1) * GUESTBOOK_PAGE_SIZE, GUESTBOOK_PAGE_SIZE);
    if (entries && cJSON_IsArray(entries)) {
        int n = cJSON_GetArraySize(entries);
        for (int i = n - 1; i >= 0; i--) {
            cJSON *e = cJSON_GetArrayItem(entries, i);
            int id = json_int_or(e, "id", 0);
            int author_uid = json_int_or(e, "author_uid", 0);
            const char *author_name = json_str_or(e, "author_name", "Anonymous");
            const char *content = json_str_or(e, "content", "");
            const char *created_at = json_str_or(e, "created_at", "");
            bool can_delete = is_owner || is_admin || (author_uid > 0 && author_uid == viewer_uid);

            cwist_sstring *content_html = cwist_sstring_create();
            append_multiline(content_html, content);

            cJSON *item = cJSON_CreateObject();
            cJSON_AddNumberToObject(item, "id", id);
            cJSON_AddNumberToObject(item, "author_uid", author_uid);
            cJSON_AddStringToObject(item, "author_name", author_name);
            cJSON_AddStringToObject(item, "content_html", content_html->data ? content_html->data : "");
            cJSON_AddStringToObject(item, "created_at", created_at);
            cJSON_AddBoolToObject(item, "can_delete", can_delete);
            cJSON_AddItemToArray(entry_list, item);

            cwist_sstring_destroy(content_html);
        }
    }
    if (entries) cJSON_Delete(entries);
    cJSON_AddItemToObject(ctx, "entries", entry_list);

    cJSON *pages = cJSON_CreateArray();
    if (total_pages > 1) {
        for (int p = 1; p <= total_pages; p++) {
            cJSON *pitem = cJSON_CreateObject();
            cJSON_AddNumberToObject(pitem, "num", p);
            cJSON_AddBoolToObject(pitem, "current", p == page);
            cJSON_AddBoolToObject(pitem, "last", p == total_pages);
            cJSON_AddItemToArray(pages, pitem);
        }
    }
    cJSON_AddItemToObject(ctx, "pages", pages);

    cwist_sstring *b = cwist_template_render(tmpl, ctx);
    cJSON_Delete(ctx);
    return b;
}

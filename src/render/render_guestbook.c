#define _POSIX_C_SOURCE 200809L
#include "render_guestbook.h"
#include "../db/guestbook.h"
#include "../utils/spam_guard.h"
#include <cwist/core/sstring/sstring.h>
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
    cwist_sstring *b = cwist_sstring_create();
    if (!b) return NULL;
    bool is_owner = (viewer_uid > 0 && viewer_uid == owner_uid);
    bool is_admin = (viewer_role && strcmp(viewer_role, "admin") == 0);
    bool anon_allowed = db_guestbook_anon_allowed(db, owner_uid);

    int total = db_guestbook_count(db, owner_uid);
    int total_pages = (total + GUESTBOOK_PAGE_SIZE - 1) / GUESTBOOK_PAGE_SIZE;
    if (total_pages < 1) total_pages = 1;
    if (page < 1) page = 1;
    if (page > total_pages) page = total_pages;

    char buf[512];
    snprintf(buf, sizeof(buf),
             "<h3 style='margin-top:24px'>Guestbook</h3>"
             "<p style='color:var(--muted);font-size:13px;margin-top:0'>%d entr%s</p>",
             total, total == 1 ? "y" : "ies");
    cwist_sstring_append(b, buf);

    cJSON *entries = db_guestbook_list(db, owner_uid, (page - 1) * GUESTBOOK_PAGE_SIZE, GUESTBOOK_PAGE_SIZE);
    if (entries && cJSON_IsArray(entries)) {
        /* The query is newest-first; render the page oldest-first so the
         * newest entry sits at the bottom, classic guestbook style. */
        int n = cJSON_GetArraySize(entries);
        for (int i = n - 1; i >= 0; i--) {
            cJSON *e = cJSON_GetArrayItem(entries, i);
            int id = json_int_or(e, "id", 0);
            int author_uid = json_int_or(e, "author_uid", 0);
            const char *author_name = json_str_or(e, "author_name", "Anonymous");
            const char *content = json_str_or(e, "content", "");
            const char *created_at = json_str_or(e, "created_at", "");
            bool can_delete = is_owner || is_admin || (author_uid > 0 && author_uid == viewer_uid);

            cwist_sstring_append(b,
                "<div style='border-top:1px solid var(--border);padding:10px 0;text-align:left'>"
                "<div style='display:flex;justify-content:space-between;gap:8px;flex-wrap:wrap'>"
                "<strong>");
            if (author_uid > 0) {
                snprintf(buf, sizeof(buf), "<a href='/user/%d'>", author_uid);
                cwist_sstring_append(b, buf);
                cwist_sstring_append_escaped(b, author_name);
                cwist_sstring_append(b, "</a>");
            } else {
                cwist_sstring_append_escaped(b, author_name);
                cwist_sstring_append(b, " <small style='color:var(--muted)'>(guest)</small>");
            }
            cwist_sstring_append(b, "</strong><small style='color:var(--muted)'>");
            cwist_sstring_append_escaped(b, created_at);
            cwist_sstring_append(b, "</small></div><p style='margin:6px 0 0'>");
            append_multiline(b, content);
            cwist_sstring_append(b, "</p>");
            if (can_delete) {
                snprintf(buf, sizeof(buf),
                         "<form action='/guestbook/delete' method='POST' style='margin:6px 0 0'>"
                         "<input type='hidden' name='id' value='%d'>"
                         "<input type='hidden' name='page' value='%d'>"
                         "<button type='submit' class='btn btn-outline' style='font-size:12px;padding:4px 10px'>Delete</button>"
                         "</form>",
                         id, page);
                cwist_sstring_append(b, buf);
            }
            cwist_sstring_append(b, "</div>");
        }
    }
    if (entries) cJSON_Delete(entries);

    /* Pager: profile URL carries ?page=, so these links re-render the whole
     * profile and the embedded fetch reloads this section. */
    if (total_pages > 1) {
        cwist_sstring_append(b, "<div style='margin-top:12px;display:flex;gap:10px;justify-content:center'>");
        for (int p = 1; p <= total_pages; p++) {
            if (p == page) {
                snprintf(buf, sizeof(buf), "<strong>%d</strong>", p);
            } else {
                snprintf(buf, sizeof(buf), "<a href='/user/%d?page=%d'>%d</a>", owner_uid, p, p);
            }
            cwist_sstring_append(b, buf);
            if (p < total_pages) cwist_sstring_append(b, " ");
        }
        cwist_sstring_append(b, "</div>");
    }

    /* Owner toggle: who may post anonymously. */
    if (is_owner) {
        cwist_sstring_append(b,
            "<hr style='margin:20px 0;border:0;border-top:1px solid var(--border)'>"
            "<form action='/guestbook/settings' method='POST' style='text-align:left'>"
            "<label style='display:flex;align-items:center;gap:8px;font-weight:normal'>"
            "<input type='checkbox' name='guestbook_anon' value='1'");
        if (anon_allowed) cwist_sstring_append(b, " checked");
        cwist_sstring_append(b,
            "> Allow anonymous guestbook posts</label>"
            "<button type='submit' class='btn btn-outline' style='margin-top:8px'>Save</button>"
            "</form>");
    }

    /* Post form / login hint. */
    cwist_sstring_append(b,
        "<hr style='margin:20px 0;border:0;border-top:1px solid var(--border)'>"
        "<form action='/guestbook/post' method='POST' style='text-align:left'>"
        "<input type='hidden' name='owner' value='");
    snprintf(buf, sizeof(buf), "%d", owner_uid);
    cwist_sstring_append(b, buf);
    cwist_sstring_append(b, "'>");
    if (viewer_uid <= 0 && anon_allowed) {
        cwist_sstring_append(b,
            "<label>Name</label><input name='name' maxlength='64' placeholder='Anonymous'>");
    }
    if (viewer_uid > 0 || anon_allowed) {
        cwist_sstring_append(b,
            "<label>Message</label><textarea name='content' rows='4' maxlength='2000' "
            "placeholder='Leave a message' style='width:100%;max-width:100%'></textarea>");
        spam_guard_append_fields(b);
        cwist_sstring_append(b, "<button type='submit' class='btn' style='margin-top:8px'>Sign Guestbook</button>");
    } else {
        cwist_sstring_append(b,
            "<p style='color:var(--muted);margin:0'><a href='/login'>Log in</a> to write in this guestbook.</p>");
    }
    cwist_sstring_append(b, "</form>");
    return b;
}

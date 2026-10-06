#define _POSIX_C_SOURCE 200809L
#include "render.h"
#include "render_internal.h"
#include "config/write_policy.h"
#include "db/db.h"
#include <cwist/core/sstring/sstring.h>
#include <stdio.h>
#include <string.h>

cwist_sstring *render_user_admin(cJSON *users, bool dark, const char *profile_pic, bool is_mobile) {
    cwist_sstring *b = cwist_sstring_create();
    cwist_sstring_assign(b, "<div class='hero'><h1>User Admin</h1></div>");
    cwist_sstring_append(b, "<div class='card admin-user-card'><div class='table-scroll'><table class='admin-user-table' style='width:100%;border-collapse:collapse'>");
    cwist_sstring_append(b, "<thead><tr><th style='text-align:left;padding:8px'>User</th><th style='text-align:left;padding:8px'>Email</th><th class='admin-role-col' style='text-align:left;padding:8px'>Role</th><th class='admin-action-col' style='text-align:left;padding:8px'>Action</th></tr></thead><tbody>");
    if (users) {
        int n = cJSON_GetArraySize(users);
        for (int i = 0; i < n; i++) {
            cJSON *u = cJSON_GetArrayItem(users, i);
            cJSON *uid = cJSON_GetObjectItem(u, "id");
            cJSON *uname = cJSON_GetObjectItem(u, "username");
            cJSON *email = cJSON_GetObjectItem(u, "email");
            cJSON *role = cJSON_GetObjectItem(u, "role");
            cwist_sstring_append(b, "<tr><td style='padding:8px'>");
            cwist_sstring_append_escaped(b, uname->valuestring);
            cwist_sstring_append(b, "</td><td style='padding:8px'>");
            cwist_sstring_append_escaped(b, email->valuestring);
            cwist_sstring_append(b, "</td><td class='admin-role-cell' style='padding:8px'>");
            cwist_sstring_append_escaped(b, role->valuestring);
            cwist_sstring_append(b, "</td><td class='admin-action-cell' style='padding:8px'>");
            cwist_sstring_append(b, "<form class='admin-role-form' action='/admin/user/role' method='post'>");
            cwist_sstring_append(b, "<input type='hidden' name='id' value='");
            char uid_buf[32];
            snprintf(uid_buf, sizeof(uid_buf), "%d", uid->valueint);
            cwist_sstring_append(b, uid_buf);
            cwist_sstring_append(b, "'>");
            cwist_sstring_append(b, "<select class='admin-role-select' name='role'><option value='user'>user</option><option value='admin'>admin</option></select>");
            cwist_sstring_append(b, "<button type='submit' class='btn' style='font-size:12px;padding:4px 10px;margin-left:6px'>Set</button></form>");
            cwist_sstring_append(b, " <a href='/account/settings?id=");
            cwist_sstring_append(b, uid_buf);
            cwist_sstring_append(b, "' class='btn btn-outline' style='font-size:12px;padding:4px 10px;text-decoration:none'>Edit</a>");
            cwist_sstring_append(b, " <form class='admin-delete-form' action='/unregister' method='post' style='display:inline'>");
            cwist_sstring_append(b, "<input type='hidden' name='id' value='");
            cwist_sstring_append(b, uid_buf);
            cwist_sstring_append(b, "'>");
            cwist_sstring_append(b, "<button type='submit' class='btn btn-outline' style='font-size:12px;padding:4px 10px' data-confirm='Delete?'>Delete</button></form>");
            cwist_sstring_append(b, "</td></tr>");
        }
    }
    cwist_sstring_append(b, "</tbody></table></div></div>");
    cwist_sstring_append(b, "<div class='card' style='margin-top:20px'><h3 style='margin-top:0'>File Admin</h3>");
    cwist_sstring_append(b, "<form action='/admin/files/drop' method='post' data-confirm='Drop ALL files? This cannot be undone.'>");
    cwist_sstring_append(b, "<button type='submit' class='btn btn-outline' style='color:#c00;border-color:#c00'>Drop All Files</button></form></div>");
    cwist_sstring *page = render_page("User Admin", b->data, dark, "admin", profile_pic, is_mobile);
    cwist_sstring_destroy(b);
    return page;
}

static void append_scope_select(cwist_sstring *b, const char *name, write_scope_t current) {
    static const struct { write_scope_t scope; const char *label; } options[] = {
        {WRITE_SCOPE_GUEST, "Guest: anyone, no account needed"},
        {WRITE_SCOPE_MEMBER, "Member: registered members and admins"},
        {WRITE_SCOPE_ADMIN, "Admin: admins only"},
    };
    cwist_sstring_append(b, "<select name='");
    cwist_sstring_append(b, name);
    cwist_sstring_append(b, "'>");
    for (size_t i = 0; i < sizeof(options) / sizeof(options[0]); i++) {
        cwist_sstring_append(b, "<option value='");
        cwist_sstring_append(b, write_scope_name(options[i].scope));
        cwist_sstring_append(b, options[i].scope == current ? "' selected>" : "'>");
        cwist_sstring_append(b, options[i].label);
        cwist_sstring_append(b, "</option>");
    }
    cwist_sstring_append(b, "</select>");
}

static void append_write_policy_section(cwist_sstring *b, const char *msg) {
    write_policy_t policy = write_policy_get();
    cwist_sstring_append(b, "<section class='board-line fade-in' style='animation-delay:0.15s'><div class='board-line-head'><h2 class='board-line-title'>Write Policy</h2></div>");
    cwist_sstring_append(b, "<p class='board-card-desc'>Pick the lowest tier (Guest, Member, Admin) that may write posts and comments, and whether every post needs a board.</p>");
    if (msg && strcmp(msg, "saved") == 0) {
        cwist_sstring_append(b, "<div class='alert'>Write policy saved.</div>");
    } else if (msg && strcmp(msg, "error") == 0) {
        cwist_sstring_append(b, "<div class='alert'>Write policy could not be saved.</div>");
    }
    cwist_sstring_append(b, "<form action='/admin/write-policy' method='post'>");
    cwist_sstring_append(b, "<label>Who can write posts</label>");
    append_scope_select(b, "post_scope", policy.post);
    cwist_sstring_append(b, "<label>Who can write comments</label>");
    append_scope_select(b, "comment_scope", policy.comment);
    cwist_sstring_append(b, "<label class='check-row'><input type='checkbox' name='require_board' value='1'");
    if (policy.require_board) cwist_sstring_append(b, " checked");
    cwist_sstring_append(b, "> Require a board for every post</label>");
    cwist_sstring_append(b, "<h3 style='margin:20px 0 4px'>Spam defense (optional)</h3>");
    cwist_sstring_append(b, "<p class='board-card-desc'>Both are off by default. Neither stores anything about writers in the database: the honeypot only checks the submitted form, and the rate limit keeps keyed hashes of accounts or addresses in memory for 10 minutes, under a key that changes at every restart. Admins are never limited. HTTP/3 requests carry no address, so anonymous writers on HTTP/3 are not rate limited.</p>");
    cwist_sstring_append(b, "<label class='check-row'><input type='checkbox' name='spam_honeypot' value='1'");
    if (policy.spam_honeypot) cwist_sstring_append(b, " checked");
    cwist_sstring_append(b, "> Honeypot: reject forms that fill a hidden field or are sent within 3 seconds</label>");
    char limit_buf[16];
    snprintf(limit_buf, sizeof(limit_buf), "%d", policy.spam_rate_limit);
    cwist_sstring_append(b, "<label for='spam-rate-limit'>Rate limit: posts, comments, reports and sign-ups per writer per 10 minutes (0 = off)</label>");
    cwist_sstring_append(b, "<input id='spam-rate-limit' type='number' name='spam_rate_limit' min='0' max='1000' value='");
    cwist_sstring_append(b, limit_buf);
    cwist_sstring_append(b, "' style='max-width:120px'>");
    cwist_sstring_append(b, "<div style='margin-top:12px'><button type='submit' class='btn'>Save Write Policy</button></div>");
    cwist_sstring_append(b, "</form></section>");
}

cwist_sstring *render_admin_dashboard(bool dark, const char *profile_pic, bool is_mobile, const char *msg, int open_reports) {
    cwist_sstring *b = cwist_sstring_create();
    cwist_sstring_assign(b, "<div class='hero'><h1>Dashboard</h1></div>");
    cwist_sstring_append(b, "<div class='board-list stagger'>");
    char reports_line[160];
    snprintf(reports_line, sizeof(reports_line), "<p class='board-card-desc'>%d open report%s waiting for review.</p>",
             open_reports, open_reports == 1 ? "" : "s");
    cwist_sstring_append(b, "<section class='board-line fade-in'><div class='board-line-head'><h2 class='board-line-title'>Reports</h2></div>");
    cwist_sstring_append(b, reports_line);
    cwist_sstring_append(b, "<a href='/admin/reports' class='btn'>Go to Reports</a></section>");
    cwist_sstring_append(b, "<section class='board-line fade-in'><div class='board-line-head'><h2 class='board-line-title'>Users</h2></div>");
    cwist_sstring_append(b, "<p class='board-card-desc'>Manage user accounts and roles.</p>");
    cwist_sstring_append(b, "<a href='/admin/users' class='btn'>Go to Users</a></section>");
    cwist_sstring_append(b, "<section class='board-line fade-in' style='animation-delay:0.05s'><div class='board-line-head'><h2 class='board-line-title'>Manage Boards</h2></div>");
    cwist_sstring_append(b, "<p class='board-card-desc'>Organize board hierarchy and parent-child relationships.</p>");
    cwist_sstring_append(b, "<a href='/admin/boards' class='btn'>Go to Manage Boards</a></section>");
    cwist_sstring_append(b, "<section class='board-line fade-in' style='animation-delay:0.10s'><div class='board-line-head'><h2 class='board-line-title'>File Admin</h2></div>");
    cwist_sstring_append(b, "<p class='board-card-desc'>Drop all uploaded files. This action cannot be undone.</p>");
    cwist_sstring_append(b, "<form action='/admin/files/drop' method='post' data-confirm='Drop ALL files? This cannot be undone.'>");
    cwist_sstring_append(b, "<button type='submit' class='btn btn-outline' style='color:#c00;border-color:#c00'>Drop All Files</button></form></section>");
    append_write_policy_section(b, msg);
    cwist_sstring_append(b, "</div>");
    cwist_sstring *page = render_page("Dashboard", b->data, dark, "admin", profile_pic, is_mobile);
    cwist_sstring_destroy(b);
    return page;
}

cwist_sstring *render_admin_boards(cJSON *boards, cJSON *tree, bool dark, const char *profile_pic, bool is_mobile) {
    cwist_sstring *b = cwist_sstring_create();
    cwist_sstring_assign(b, "<div class='hero'><h1>Manage Boards</h1></div>");
    cwist_sstring_append(b, "<div class='card' style='overflow-x:auto'><table style='width:100%;border-collapse:collapse'>");
    cwist_sstring_append(b, "<thead><tr><th style='text-align:left;padding:8px'>ID</th><th style='text-align:left;padding:8px'>Name</th><th style='text-align:left;padding:8px'>Slug</th><th style='text-align:left;padding:8px'>Parent</th><th style='text-align:left;padding:8px'>Action</th></tr></thead><tbody>");
    if (boards) {
        int n = cJSON_GetArraySize(boards);
        for (int i = 0; i < n; i++) {
            cJSON *bo = cJSON_GetArrayItem(boards, i);
            int bid = json_int(bo, "id", 0);
            cJSON *name = cJSON_GetObjectItem(bo, "name");
            cJSON *slug = cJSON_GetObjectItem(bo, "slug");
            int parent_id = 0;
            const char *parent_name = "None";
            if (tree) {
                int tn = cJSON_GetArraySize(tree);
                for (int j = 0; j < tn; j++) {
                    cJSON *node = cJSON_GetArrayItem(tree, j);
                    if (json_int(node, "board_id", 0) == bid) {
                        parent_id = json_int(node, "parent_board_id", 0);
                        break;
                    }
                }
            }
            if (parent_id > 0 && boards) {
                int pn = cJSON_GetArraySize(boards);
                for (int k = 0; k < pn; k++) {
                    cJSON *pb = cJSON_GetArrayItem(boards, k);
                    if (json_int(pb, "id", 0) == parent_id) {
                        cJSON *pname = cJSON_GetObjectItem(pb, "name");
                        if (pname && pname->valuestring) parent_name = pname->valuestring;
                        break;
                    }
                }
            }
            int depth = json_int(bo, "depth", 0);
            char bid_buf[32];
            snprintf(bid_buf, sizeof(bid_buf), "%d", bid);
            cwist_sstring_append(b, "<tr><td style='padding:8px'>");
            cwist_sstring_append(b, bid_buf);
            cwist_sstring_append(b, "</td><td style='padding:8px'>");
            if (depth > 0) {
                for (int d = 0; d < depth; d++) cwist_sstring_append(b, "&mdash; ");
            }
            cwist_sstring_append_escaped(b, name && name->valuestring ? name->valuestring : "");
            cwist_sstring_append(b, "</td><td style='padding:8px'>");
            cwist_sstring_append_escaped(b, slug && slug->valuestring ? slug->valuestring : "");
            cwist_sstring_append(b, "</td><td style='padding:8px'>");
            cwist_sstring_append_escaped(b, parent_name);
            cwist_sstring_append(b, "</td><td style='padding:8px'>");
            cwist_sstring_append(b, "<a href='/board/");
            cwist_sstring_append(b, bid_buf);
            cwist_sstring_append(b, "/edit' class='btn btn-outline' style='font-size:12px;padding:4px 10px;text-decoration:none'>Edit</a>");
            cwist_sstring_append(b, "</td></tr>");
        }
    }
    cwist_sstring_append(b, "</tbody></table></div>");
    cwist_sstring *page = render_page("Manage Boards", b->data, dark, "admin", profile_pic, is_mobile);
    cwist_sstring_destroy(b);
    return page;
}

/* ---- Report queue ---- */

static const char *report_reason_label(const char *reason) {
    if (strcmp(reason, "spam") == 0) return "Spam or advertising";
    if (strcmp(reason, "abuse") == 0) return "Harassment or hate";
    if (strcmp(reason, "illegal") == 0) return "Illegal content";
    if (strcmp(reason, "privacy") == 0) return "Personal information";
    return "Other";
}

static const char *group_str(cJSON *o, const char *key) {
    cJSON *v = cJSON_GetObjectItem(o, key);
    return cJSON_IsString(v) ? v->valuestring : "";
}

static void append_report_action(cwist_sstring *b, cJSON *g, const char *action, const char *label,
                                 const char *confirm, bool danger) {
    char id_buf[32];
    snprintf(id_buf, sizeof(id_buf), "%d", json_int(g, "target_id", 0));
    cwist_sstring_append(b, "<form action='/admin/reports/action' method='post' style='display:inline'");
    if (confirm) {
        cwist_sstring_append(b, " data-confirm='");
        cwist_sstring_append_escaped(b, confirm);
        cwist_sstring_append(b, "'");
    }
    cwist_sstring_append(b, "><input type='hidden' name='target_type' value='");
    cwist_sstring_append_escaped(b, group_str(g, "target_type"));
    cwist_sstring_append(b, "'><input type='hidden' name='target_id' value='");
    cwist_sstring_append(b, id_buf);
    cwist_sstring_append(b, "'><input type='hidden' name='action' value='");
    cwist_sstring_append(b, action);
    cwist_sstring_append(b, danger ? "'><button type='submit' class='btn btn-outline' style='color:#c00;border-color:#c00'>"
                                   : "'><button type='submit' class='btn btn-outline'>");
    cwist_sstring_append(b, label);
    cwist_sstring_append(b, "</button></form>");
}

cwist_sstring *render_admin_reports(cJSON *groups, const char *status, const char *msg, bool dark,
                                    const char *profile_pic, bool is_mobile) {
    cwist_sstring *b = cwist_sstring_create();
    cwist_sstring_assign(b, "<div class='hero'><h1>Reports</h1></div>");

    static const char *const tabs[][2] = {{"open", "Open"}, {"resolved", "Resolved"}, {"dismissed", "Dismissed"}, {"all", "All"}};
    cwist_sstring_append(b, "<nav style='display:flex;gap:8px;flex-wrap:wrap;margin-bottom:16px'>");
    for (size_t i = 0; i < sizeof(tabs) / sizeof(tabs[0]); i++) {
        bool cur = strcmp(status, tabs[i][0]) == 0;
        cwist_sstring_append(b, cur ? "<a class='btn' aria-current='page' href='/admin/reports?status=" : "<a class='btn btn-outline' href='/admin/reports?status=");
        cwist_sstring_append(b, tabs[i][0]);
        cwist_sstring_append(b, "'>");
        cwist_sstring_append(b, tabs[i][1]);
        cwist_sstring_append(b, "</a>");
    }
    cwist_sstring_append(b, "</nav>");

    if (msg) {
        const char *text = strcmp(msg, "deleted") == 0 ? "Content deleted and its reports resolved."
                         : strcmp(msg, "dismissed") == 0 ? "Reports dismissed."
                         : strcmp(msg, "resolved") == 0 ? "Reports resolved; the content was kept."
                         : "The action failed.";
        cwist_sstring_append(b, "<div class='alert'>");
        cwist_sstring_append(b, text);
        cwist_sstring_append(b, "</div>");
    }

    if (!cJSON_IsArray(groups) || cJSON_GetArraySize(groups) == 0) {
        cwist_sstring_append(b, "<p style='color:var(--muted);text-align:center;padding:40px 0'>No reports here.</p>");
    }

    cJSON *g = NULL;
    cJSON_ArrayForEach(g, groups) {
        const char *type = group_str(g, "target_type");
        bool is_comment = strcmp(type, "comment") == 0;
        cJSON *reports = cJSON_GetObjectItem(g, "reports");
        int n = cJSON_GetArraySize(reports);
        int open = 0;
        cJSON *r = NULL;
        cJSON_ArrayForEach(r, reports) {
            if (strcmp(group_str(r, "status"), REPORT_STATUS_OPEN) == 0) open++;
        }
        bool exists = cJSON_IsTrue(cJSON_GetObjectItem(g, "exists"));

        cwist_sstring_append(b, "<section class='card' style='margin-bottom:16px'>");
        cwist_sstring_append(b, "<div style='display:flex;justify-content:space-between;gap:12px;flex-wrap:wrap;align-items:baseline'><div>");
        cwist_sstring_append(b, is_comment ? "<span class='tag'>Comment</span> " : "<span class='tag'>Post</span> ");
        const char *slug = group_str(g, "post_slug");
        if (slug[0]) {
            cwist_sstring_append(b, "<a href='/post/");
            cwist_sstring_append_escaped(b, slug);
            cwist_sstring_append(b, "'>");
            cwist_sstring_append_escaped(b, group_str(g, "post_title")[0] ? group_str(g, "post_title") : "(untitled)");
            cwist_sstring_append(b, "</a>");
        } else {
            cwist_sstring_append(b, "<span style='color:var(--muted)'>(post deleted)</span>");
        }
        if (!exists) cwist_sstring_append(b, " <span style='color:var(--muted)'>&middot; content already removed</span>");
        cwist_sstring_append(b, "</div><div style='color:var(--muted);font-size:13px'>");
        char counts[96];
        snprintf(counts, sizeof(counts), "%d report%s, %d open", n, n == 1 ? "" : "s", open);
        cwist_sstring_append(b, counts);
        cwist_sstring_append(b, "</div></div>");

        /* What was reported, as plain text: never render reported markup. */
        const char *excerpt = group_str(g, "excerpt");
        if (exists && excerpt[0]) {
            char text[600];
            render_text_excerpt(excerpt, 280, text, sizeof(text));
            cwist_sstring_append(b, "<blockquote style='margin:12px 0;padding:8px 12px;border-left:3px solid var(--border);color:var(--muted);white-space:pre-wrap'>");
            cwist_sstring_append_escaped(b, text);
            cwist_sstring_append(b, "<div style='margin-top:6px;font-size:12px'>by ");
            cwist_sstring_append_escaped(b, group_str(g, "author")[0] ? group_str(g, "author") : "anonymous");
            cwist_sstring_append(b, "</div></blockquote>");
        }

        cwist_sstring_append(b, "<ul style='margin:8px 0;padding-left:20px'>");
        cJSON_ArrayForEach(r, reports) {
            cwist_sstring_append(b, "<li style='margin-bottom:6px'><strong>");
            cwist_sstring_append(b, report_reason_label(group_str(r, "reason")));
            cwist_sstring_append(b, "</strong> <span style='color:var(--muted);font-size:13px'>&middot; ");
            const char *who = group_str(r, "reporter_name");
            cwist_sstring_append_escaped(b, who[0] ? who : "anonymous");
            cwist_sstring_append(b, " &middot; ");
            cwist_sstring_append_escaped(b, group_str(r, "created_at"));
            const char *st = group_str(r, "status");
            if (strcmp(st, REPORT_STATUS_OPEN) != 0) {
                cwist_sstring_append(b, " &middot; ");
                cwist_sstring_append_escaped(b, st);
                const char *resolution = group_str(r, "resolution");
                if (resolution[0]) {
                    cwist_sstring_append(b, " (");
                    cwist_sstring_append_escaped(b, resolution);
                    cwist_sstring_append(b, ")");
                }
                const char *by = group_str(r, "resolver_name");
                if (by[0]) {
                    cwist_sstring_append(b, " by ");
                    cwist_sstring_append_escaped(b, by);
                }
            }
            cwist_sstring_append(b, "</span>");
            const char *detail = group_str(r, "detail");
            if (detail[0]) {
                cwist_sstring_append(b, "<div style='white-space:pre-wrap'>");
                cwist_sstring_append_escaped(b, detail);
                cwist_sstring_append(b, "</div>");
            }
            cwist_sstring_append(b, "</li>");
        }
        cwist_sstring_append(b, "</ul>");

        if (open > 0) {
            cwist_sstring_append(b, "<div style='display:flex;gap:8px;flex-wrap:wrap'>");
            if (exists) {
                append_report_action(b, g, "delete", is_comment ? "Delete comment" : "Delete post",
                                     is_comment ? "Delete this comment?" : "Delete this post with its files and comments?", true);
            }
            append_report_action(b, g, "resolve", exists ? "Keep content, resolve" : "Resolve", NULL, false);
            append_report_action(b, g, "dismiss", "Dismiss", NULL, false);
            cwist_sstring_append(b, "</div>");
        }
        cwist_sstring_append(b, "</section>");
    }

    cwist_sstring *page = render_page("Reports", b->data, dark, "admin", profile_pic, is_mobile);
    cwist_sstring_destroy(b);
    return page;
}

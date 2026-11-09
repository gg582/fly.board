/* Comments: Markdown rendering (and its escaping), thread linking, reply
 * targets, the comments.db index, and the create handler's validation and
 * redirect. */
#include "db/db.h"
#include "db/db_internal.h"
#include "auth/auth.h"
#include "handlers/handlers.h"
#include "handlers/handlers_internal.h"
#include "render/render.h"
#include "render/render_internal.h"
#include "utils/cache.h"
#include "utils/post_schedule.h"
#include "utils/reqshare.h"
#include "utils/spam_guard.h"
#include "config/write_policy.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(condition, name) do { \
    if (condition) printf("PASS: %s\n", name); \
    else { printf("FAIL: %s (line %d)\n", name, __LINE__); failures++; } \
} while (0)

static cwist_db g_db;

static cJSON *comment_row(int id, int parent, const char *author, const char *content) {
    cJSON *c = cJSON_CreateObject();
    cJSON_AddNumberToObject(c, "id", id);
    cJSON_AddNumberToObject(c, "parent_id", parent);
    cJSON_AddNumberToObject(c, "user_id", 0);
    cJSON_AddStringToObject(c, "author_name", author);
    cJSON_AddStringToObject(c, "content", content);
    cJSON_AddStringToObject(c, "created_at", "2026-11-09 00:00:00");
    cJSON_AddNumberToObject(c, "deleted", 0);
    return c;
}

static const char *pos(const char *hay, const char *needle) {
    return hay ? strstr(hay, needle) : NULL;
}

typedef void (*handler_fn)(cwist_http_request *, cwist_http_response *);

static cwist_http_response *call_handler(handler_fn fn, const char *body, bool fragment, const char *cookie) {
    cwist_http_request *req = cwist_http_request_create();
    cwist_http_response *res = cwist_http_response_create();
    req->db = &g_db;
    req->method = CWIST_HTTP_POST;
    req->client_fd = -1;
    if (!req->path_params) req->path_params = cwist_query_map_create();
    if (!req->query_params) req->query_params = cwist_query_map_create();
    if (fragment) cwist_http_header_add(&req->headers, "X-Fly-Comment", "fragment");
    if (cookie) cwist_http_header_add(&req->headers, "Cookie", cookie);
    cwist_sstring_assign(req->body, body);
    fn(req, res);
    cwist_http_request_destroy(req);
    return res;
}

static cwist_http_response *post_comment(const char *body) {
    cwist_http_request *req = cwist_http_request_create();
    cwist_http_response *res = cwist_http_response_create();
    req->db = &g_db;
    req->method = CWIST_HTTP_POST;
    req->client_fd = -1;
    if (!req->path_params) req->path_params = cwist_query_map_create();
    if (!req->query_params) req->query_params = cwist_query_map_create();
    cwist_sstring_assign(req->body, body);
    handler_comment_new_post(req, res);
    cwist_http_request_destroy(req);
    return res;
}

int main(void) {
    const char *main_path = "/tmp/flytest_comment_format.db";
    const char *comments_path = "/tmp/flytest_comment_format_comments.db";
    remove(main_path);
    remove(comments_path);
    remove("/tmp/flytest_comment_format_secret");
    sqlite3 *conn = NULL;
    CHECK(sqlite3_open(main_path, &conn) == SQLITE_OK, "open main database");
    g_db.conn = conn;
    bool fts5 = sqlite3_compileoption_used("ENABLE_FTS5") != 0;
    CHECK(db_init(&g_db) || !fts5, "initialize schema");
    CHECK(db_comment_init(comments_path), "initialize comments schema");
    CHECK(auth_jwt_init("/tmp/flytest_comment_format_secret"), "jwt secret");
    CHECK(spam_guard_init(), "spam guard");
    write_policy_init(&g_db);
    post_schedule_init(&g_db);
    page_cache_init();
    reqshare_init();

    /* ---- index on the table comments actually live in ---- */
    sqlite3_stmt *st = NULL;
    bool has_index = false;
    if (sqlite3_prepare_v2(db_comment_conn(), "SELECT 1 FROM sqlite_master WHERE type='index' AND name='idx_comments_target'",
                           -1, &st, NULL) == SQLITE_OK) {
        has_index = sqlite3_step(st) == SQLITE_ROW;
        sqlite3_finalize(st);
    }
    CHECK(has_index, "comments.db has the target index");
    bool uses_index = false;
    if (sqlite3_prepare_v2(db_comment_conn(),
                           "EXPLAIN QUERY PLAN SELECT id FROM comments WHERE target_type='post' AND target_id=1 ORDER BY created_at ASC",
                           -1, &st, NULL) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            const char *d = (const char *)sqlite3_column_text(st, 3);
            if (d && strstr(d, "idx_comments_target")) uses_index = true;
        }
        sqlite3_finalize(st);
    }
    CHECK(uses_index, "thread listing uses the index");

    /* ---- Markdown rendering ---- */
    cJSON *list = cJSON_CreateArray();
    cJSON_AddItemToArray(list, comment_row(1, 0, "한글이", "**bold** and `code`\n\n- one\n- two"));
    cJSON_AddItemToArray(list, comment_row(2, 0, "mallory",
                                           "<script>alert(1)</script> <img src=x onerror=alert(1)> [x](javascript:alert(1))"));
    cJSON_AddItemToArray(list, comment_row(3, 1, "bob", "reply to one"));
    cJSON_AddItemToArray(list, comment_row(4, 99, "orphan", "parent missing"));
    cJSON_AddItemToArray(list, comment_row(5, 5, "loop", "self parent"));
    cJSON_AddItemToArray(list, comment_row(6, 3, "carol", "$x^2$"));
    cwist_sstring *html = render_post_comment_list(list, "", 0, 42, NULL);
    const char *h = html ? html->data : "";
    CHECK(pos(h, "<strong>bold</strong>"), "bold renders");
    CHECK(pos(h, "<code>code</code>"), "inline code renders");
    CHECK(pos(h, "<li>one</li>"), "list renders");
    CHECK(pos(h, "class='markdown-body comment-markdown'"), "comment body uses the markdown container");
    CHECK(pos(h, "class=\"math-inline\"") || pos(h, "math-inline"), "inline math is marked for KaTeX");
    CHECK(!pos(h, "<script>alert"), "raw script is escaped");
    CHECK(!pos(h, "<img src=x onerror") && !pos(h, "<img src=\"x\" onerror"), "raw img handler is escaped");
    if (getenv("DUMP")) printf("%s\n", h);
    CHECK(!pos(h, "href=\"javascript:"), "javascript: link is not emitted");
    CHECK(pos(h, "<div class='comment-avatar'>한</div>"), "Hangul initial is a whole character");

    /* Link schemes (shared with post bodies). */
    const char *cases[][2] = {
        {"[a](javascript:alert(1))", NULL},
        {"[a]( JaVaScRiPt:alert(1))", NULL},
        {"[a](vbscript:x)", NULL},
        {"[a](data:text/html,<b>)", NULL},
        {"![i](data:image/svg+xml,<svg>)", NULL},
        {"[a](https://example.com/x)", "href=\"https://example.com/x\""},
        {"[a](/post/foo#bar)", "href=\"/post/foo#bar\""},
        {"[a](mailto:me@example.com)", "href=\"mailto:me@example.com\""},
        {"![i](data:image/png;base64,AAAA)", "src=\"data:image/png;base64,AAAA\""},
        {"<https://example.com/auto>", "href=\"https://example.com/auto\""},
    };
    for (size_t k = 0; k < sizeof(cases) / sizeof(cases[0]); k++) {
        cwist_sstring *m = render_markdown_to_html(cases[k][0]);
        const char *mh = m ? m->data : "";
        bool ok = cases[k][1] ? strstr(mh, cases[k][1]) != NULL
                              : (strstr(mh, "href=\"#\"") || strstr(mh, "src=\"#\""));
        char name[160];
        snprintf(name, sizeof(name), "link scheme: %s", cases[k][0]);
        CHECK(ok, name);
        if (!ok) printf("  got: %s\n", mh);
        if (m) cwist_sstring_destroy(m);
    }

    /* Thread order: 1, 3 (reply), 6 (reply of reply), 2, 4 (orphan at top). */
    const char *p1 = pos(h, "id='comment-1'"), *p3 = pos(h, "id='comment-3'"), *p6 = pos(h, "id='comment-6'");
    const char *p2 = pos(h, "id='comment-2'"), *p4 = pos(h, "id='comment-4'");
    CHECK(p1 && p3 && p6 && p2 && p4 && p1 < p3 && p3 < p6 && p6 < p2 && p2 < p4, "thread order kept");
    CHECK(pos(h, "id='comment-3' style='margin-left:20px") && pos(h, "id='comment-6' style='margin-left:40px"),
          "replies are indented by depth");
    CHECK(pos(h, "id='comment-4' style='margin-left:0px"), "orphan reply is shown at the top level");
    const char *p5 = pos(h, "id='comment-5'");
    CHECK(p5 && !pos(p5 + 1, "id='comment-5'"), "self-parented row renders once without looping");
    CHECK(pos(h, "name='target_type' value='post'"), "post reply targets the post");
    CHECK(pos(h, "Markdown supported"), "format hint is shown");
    if (html) cwist_sstring_destroy(html);

    cwist_sstring *fb = cwist_sstring_create();
    render_comment_tree(fb, list, "file", 7, 0, "", NULL);
    CHECK(pos(fb->data, "name='target_type' value='file'") && !pos(fb->data, "value='post'"),
          "file comment replies target the file, not a post");
    cwist_sstring_destroy(fb);
    cJSON_Delete(list);

    /* Same pipeline as the body, byte for byte: Markdown with CRLF line
     * endings (as browsers submit textareas; this is post54's comment),
     * code fences, display/inline math and TikZ. */
    const char *samples[] = {
        "## \xea\xb0\x99\xec\x9d\x80 \xea\xb2\x83\r\n\r\n```python\r\nimport math\r\nprint(f\"{'N':>8}\")\r\n```",
        "Inline $a^2+b^2=c^2$ and display:\n\n$$\\int_0^1 x\\,dx$$\n",
        "```tikz\n\\begin{tikzpicture}\\draw (0,0) -- (1,1);\\end{tikzpicture}\n```",
        "\\[ e^{i\\pi}+1=0 \\]",
        "| a | b |\n|---|---|\n| 1 | 2 |\n\n- [x] done\n\n> quote\n> two",
    };
    for (size_t k = 0; k < sizeof(samples) / sizeof(samples[0]); k++) {
        cJSON *one = cJSON_CreateArray();
        cJSON_AddItemToArray(one, comment_row(1, 0, "a", samples[k]));
        cwist_sstring *thread = render_post_comment_list(one, "", 0, 1, NULL);
        cwist_sstring *body_html = render_markdown_to_html(samples[k]);
        cwist_sstring *want = cwist_sstring_create();
        cwist_sstring_assign(want, "<div class='markdown-body comment-markdown'>");
        cwist_sstring_append(want, body_html ? body_html->data : "");
        cwist_sstring_append(want, "</div>");
        char name[64];
        snprintf(name, sizeof(name), "comment HTML equals body HTML (sample %zu)", k);
        CHECK(body_html && strstr(thread->data, want->data) != NULL, name);
        cwist_sstring_destroy(want);
        if (body_html) cwist_sstring_destroy(body_html);
        cwist_sstring_destroy(thread);
        cJSON_Delete(one);
    }
    {
        cJSON *one = cJSON_CreateArray();
        cJSON_AddItemToArray(one, comment_row(1, 0, "a", "## T\r\n\r\n```python\r\nx = 1\r\n```"));
        cwist_sstring *t = render_post_comment_list(one, "", 0, 1, NULL);
        CHECK(strstr(t->data, "<h2>T</h2>") && strstr(t->data, "<code class=\"language-python\">"),
              "CRLF comment gets heading and highlighted code fence");
        cwist_sstring_destroy(t);
        cJSON_Delete(one);

        one = cJSON_CreateArray();
        cJSON_AddItemToArray(one, comment_row(1, 0, "a", "listen: [song](/file/download/5)"));
        cJSON *files = cJSON_CreateArray();
        cJSON *f = cJSON_CreateObject();
        cJSON_AddNumberToObject(f, "id", 5);
        cJSON_AddStringToObject(f, "mime_type", "audio/mpeg");
        cJSON_AddStringToObject(f, "filename", "song.mp3");
        cJSON_AddItemToArray(files, f);
        t = render_post_comment_list(one, "", 0, 1, files);
        CHECK(strstr(t->data, "<audio") != NULL, "attachment link in a comment becomes a player, as in the body");
        cwist_sstring_destroy(t);
        cJSON_Delete(files);
        cJSON_Delete(one);
    }

    cwist_sstring *empty = render_post_comment_list(NULL, "", 0, 1, NULL);
    CHECK(pos(empty->data, "No comments yet."), "empty thread message");
    cwist_sstring_destroy(empty);

    /* ---- create handler ---- */
    CHECK(db_user_create(&g_db, "writer", "w@example.com", "hash"), "create author");
    cJSON *u = db_user_get_by_username(&g_db, "writer");
    int author = json_int(u, "id", 0);
    cJSON_Delete(u);
    char *slug = NULL;
    int post_id = db_post_create_with_auto_slug(&g_db, 0, author, "Thread", "thread", "Body", "", "", 0, 0, "",
                                                NULL, "2020-01-01 00:00:00", &slug);
    char *other_slug = NULL;
    int other_id = db_post_create_with_auto_slug(&g_db, 0, author, "Other", "other", "Body", "", "", 0, 0, "",
                                                 NULL, "2020-01-02 00:00:00", &other_slug);
    CHECK(post_id > 0 && other_id > 0, "create posts");

    if (!write_policy_can_comment("")) {
        printf("SKIP: handler checks (guests may not comment under the default policy)\n");
    } else {
        char body[512];
        snprintf(body, sizeof(body), "target_type=post&target_id=%d&author_name=Guest&content=**hi**", post_id);
        cwist_http_response *res = post_comment(body);
        const char *loc = cwist_http_header_get(res->headers, "Location");
        char want[128];
        snprintf(want, sizeof(want), "/post/%s#comment-", slug);
        CHECK((int)res->status_code == 303 && loc && strncmp(loc, want, strlen(want)) == 0,
              "new comment answers 303 to its anchor");
        int first_id = loc ? atoi(strrchr(loc, '-') + 1) : 0;
        cwist_http_response_destroy(res);
        cJSON *saved = db_comment_get_by_id(&g_db, first_id);
        CHECK(saved && strcmp(json_string_or_empty(saved, "content"), "**hi**") == 0, "Markdown source is stored as typed");
        if (saved) cJSON_Delete(saved);

        snprintf(body, sizeof(body), "target_type=post&target_id=%d&parent_id=%d&author_name=Guest&content=x", other_id, first_id);
        res = post_comment(body);
        loc = cwist_http_header_get(res->headers, "Location");
        int cross_id = loc && strrchr(loc, '-') ? atoi(strrchr(loc, '-') + 1) : 0;
        cwist_http_response_destroy(res);
        cJSON *cross = db_comment_get_by_id(&g_db, cross_id);
        CHECK(cross && json_int(cross, "parent_id", 0) == 0, "reply to another post's comment is detached");
        if (cross) cJSON_Delete(cross);

        snprintf(body, sizeof(body), "target_type=post&target_id=%d&author_name=Guest&content=", post_id);
        res = post_comment(body);
        CHECK((int)res->status_code == 400, "empty comment is a 400, not a silent redirect");
        cwist_http_response_destroy(res);

        size_t big = COMMENT_CONTENT_MAX + 64;
        char *huge = malloc(big + 128);
        int n = snprintf(huge, 128, "target_type=post&target_id=%d&content=", post_id);
        memset(huge + n, 'a', COMMENT_CONTENT_MAX + 1);
        huge[n + COMMENT_CONTENT_MAX + 1] = '\0';
        res = post_comment(huge);
        CHECK((int)res->status_code == 413, "oversized comment is refused");
        cwist_http_response_destroy(res);
        free(huge);

        res = post_comment("target_type=post&target_id=999999&content=hi");
        CHECK((int)res->status_code == 404, "comment on a missing post is a 404");
        cwist_http_response_destroy(res);

        /* In-page submit: JSON with the re-rendered thread instead of a redirect. */
        snprintf(body, sizeof(body), "target_type=post&target_id=%d&author_name=Guest&content=_frag_", post_id);
        res = call_handler(handler_comment_new_post, body, true, NULL);
        const char *ctype = cwist_http_header_get(res->headers, "Content-Type");
        cJSON *frag = cJSON_Parse(res->body->data);
        int frag_id = json_int(frag, "id", 0);
        char anchor[64];
        snprintf(anchor, sizeof(anchor), "id='comment-%d'", frag_id);
        const char *fh = json_string_or_empty(frag, "html");
        CHECK((int)res->status_code == 200 && ctype && strncmp(ctype, "application/json", 16) == 0, "fragment submit answers JSON");
        CHECK(frag_id > 0 && strstr(fh, anchor) && strstr(fh, "<em>frag</em>"), "fragment holds the rendered new comment");
        CHECK(!cwist_http_header_get(res->headers, "Location"), "fragment submit does not redirect");
        if (frag) cJSON_Delete(frag);
        cwist_http_response_destroy(res);

        snprintf(body, sizeof(body), "target_type=post&target_id=%d&content=", post_id);
        res = call_handler(handler_comment_new_post, body, true, NULL);
        CHECK((int)res->status_code == 400, "fragment submit reports errors by status");
        cwist_http_response_destroy(res);

        /* Edits: author, stranger, admin. */
        CHECK(db_user_create(&g_db, "stranger", "s@example.com", "hash"), "create stranger");
        cJSON *su = db_user_get_by_username(&g_db, "stranger");
        int stranger = json_int(su, "id", 0);
        cJSON_Delete(su);
        int own = db_comment_create(&g_db, "post", post_id, author, "writer", 0, "before");
        char *tok_author = auth_jwt_issue(author, "writer", "user");
        char *tok_stranger = auth_jwt_issue(stranger, "stranger", "user");
        char *tok_admin = auth_jwt_issue(author, "writer", "admin");
        char ck[1024];
        snprintf(body, sizeof(body), "id=%d&content=**after**", own);
        snprintf(ck, sizeof(ck), "jwt_access=%s", tok_stranger ? tok_stranger : "");
        res = call_handler(handler_comment_edit_post, body, true, ck);
        CHECK((int)res->status_code == 403, "stranger edit is refused, not reported as saved");
        cwist_http_response_destroy(res);
        cJSON *still = db_comment_get_by_id(&g_db, own);
        CHECK(still && strcmp(json_string_or_empty(still, "content"), "before") == 0, "stranger edit changed nothing");
        if (still) cJSON_Delete(still);

        snprintf(ck, sizeof(ck), "jwt_access=%s", tok_author ? tok_author : "");
        res = call_handler(handler_comment_edit_post, body, true, ck);
        CHECK((int)res->status_code == 200 && strstr(res->body->data, "<strong>after</strong>"), "author edit returns the updated thread");
        cwist_http_response_destroy(res);

        CHECK(db_user_create(&g_db, "boss", "b@example.com", "hash"), "create admin user");
        cJSON *bu = db_user_get_by_username(&g_db, "boss");
        int boss = json_int(bu, "id", 0);
        cJSON_Delete(bu);
        db_user_update_role(&g_db, boss, "admin");
        free(tok_admin);
        tok_admin = auth_jwt_issue(boss, "boss", "admin");
        snprintf(body, sizeof(body), "id=%d&content=edited-by-admin", own);
        snprintf(ck, sizeof(ck), "jwt_access=%s", tok_admin ? tok_admin : "");
        res = call_handler(handler_comment_edit_post, body, true, ck);
        if ((int)res->status_code != 200) printf("  admin edit status=%d body=%s\n", (int)res->status_code, res->body->data);
        cwist_http_response_destroy(res);
        still = db_comment_get_by_id(&g_db, own);
        CHECK(still && strcmp(json_string_or_empty(still, "content"), "edited-by-admin") == 0, "admin edit is applied");
        if (still) cJSON_Delete(still);
        free(tok_author); free(tok_stranger); free(tok_admin);

        cwist_http_request *rq = cwist_http_request_create();
        rq->db = &g_db;
        rq->client_fd = -1;
        if (!rq->path_params) rq->path_params = cwist_query_map_create();
        if (!rq->query_params) rq->query_params = cwist_query_map_create();
        cJSON *thread = db_comment_list_by_target(&g_db, "post", post_id);
        CHECK(cJSON_GetArraySize(thread) == 3, "thread holds the comments");
        if (thread) cJSON_Delete(thread);
        cwist_http_request_destroy(rq);
    }

    char key[512];
    page_cache_key_post(key, sizeof(key), "thread", false, false, "", 0);
    CHECK(strstr(key, ":v=") != NULL, "post cache key carries the shared post version");

    free(slug);
    free(other_slug);
    db_comment_close();
    sqlite3_close(conn);
    printf("%s: %d failure(s)\n", failures ? "FAILED" : "OK", failures);
    return failures ? 1 : 0;
}

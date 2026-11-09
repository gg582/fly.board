/* Where a comment submission spends its time: the POST handler, its parts,
 * and the page render that the redirect triggers. Not part of `make check`. */
#include "db/db.h"
#include "db/db_internal.h"
#include "db/moderation.h"
#include "auth/auth.h"
#include "handlers/handlers.h"
#include "handlers/handlers_internal.h"
#include "render/render.h"
#include "utils/cache.h"
#include "utils/post_schedule.h"
#include "utils/reqshare.h"
#include "utils/spam_guard.h"
#include "config/write_policy.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static cwist_db g_db;
static double now_ms(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}
static cwist_http_request *mkreq(const char *body) {
    cwist_http_request *req = cwist_http_request_create();
    req->db = &g_db; req->client_fd = -1;
    req->method = body ? CWIST_HTTP_POST : CWIST_HTTP_GET;
    if (!req->path_params) req->path_params = cwist_query_map_create();
    if (!req->query_params) req->query_params = cwist_query_map_create();
    if (body) cwist_sstring_assign(req->body, body);
    return req;
}
#define TIME(label, iters, stmt) do { double t0 = now_ms(); for (int _i = 0; _i < (iters); _i++) { stmt; } \
    printf("%-34s %8.3f ms/op\n", label, (now_ms() - t0) / (iters)); } while (0)

int main(int argc, char **argv) {
    int ncomments = argc > 1 ? atoi(argv[1]) : 100;
    remove("/tmp/flybench.db"); remove("/tmp/flybench_c.db"); remove("/tmp/flybench_s");
    sqlite3 *conn = NULL; sqlite3_open("/tmp/flybench.db", &conn); g_db.conn = conn;
    db_init(&g_db); db_comment_init("/tmp/flybench_c.db"); auth_jwt_init("/tmp/flybench_s");
    spam_guard_init(); write_policy_init(&g_db); post_schedule_init(&g_db); page_cache_init(); reqshare_init();
    db_user_create(&g_db, "writer", "w@example.com", "hash");
    cJSON *u = db_user_get_by_username(&g_db, "writer"); int author = json_int(u, "id", 0); cJSON_Delete(u);
    cwist_sstring *body = cwist_sstring_create(); cwist_sstring_assign(body, "");
    for (int i = 0; i < 60; i++) cwist_sstring_append(body, "## Section\n\nSome **text** with `code` and a [link](https://example.com) $x^2$.\n\n```c\nint main(void) { return 0; }\n```\n\n- a\n- b\n\n");
    char *slug = NULL;
    int pid = db_post_create_with_auto_slug(&g_db, 0, author, "Bench", "bench", body->data, "", "", 0, 0, "", NULL, "2020-01-01 00:00:00", &slug);
    for (int i = 0; i < ncomments; i++)
        db_comment_create(&g_db, "post", pid, 0, "guest", i % 3 ? (i - 1 > 0 ? i : 0) : 0,
                          "A **formatted** comment with `code`, a list:\n\n- one\n- two\n\nand $a+b$.");
    printf("post body %zu bytes, %d comments\n", body->size, ncomments);

    char form[256];
    snprintf(form, sizeof(form), "target_type=post&target_id=%d&author_name=Guest&content=**hi** there", pid);
    TIME("POST /comment/new (handler)", 50, {
        cwist_http_request *rq = mkreq(form); cwist_http_response *rs = cwist_http_response_create();
        handler_comment_new_post(rq, rs); cwist_http_request_destroy(rq); cwist_http_response_destroy(rs); });
    TIME("  db_post_get_by_id", 200, { cJSON *p = db_post_get_by_id(&g_db, pid); cJSON_Delete(p); });
    TIME("  moderation_check_submission", 200, { (void)moderation_check_submission(&g_db, 0, "", MODERATION_COMMENT, "**hi** there"); });
    TIME("  db_comment_create", 50, { db_comment_create(&g_db, "post", pid, 0, "g", 0, "x"); });
    TIME("  page_cache_invalidate_post", 200, { page_cache_invalidate_post(slug); });
    TIME("db_comment_list_by_target", 200, { cJSON *l = db_comment_list_by_target(&g_db, "post", pid); cJSON_Delete(l); });
    cJSON *list = db_comment_list_by_target(&g_db, "post", pid);
    TIME("render_post_comment_list", 20, { cwist_sstring *h = render_post_comment_list(list, "", 0, pid, NULL); cwist_sstring_destroy(h); });
    { cwist_sstring *h = render_post_comment_list(list, "", 0, pid, NULL); printf("  list bytes=%zu\n", h->size); cwist_sstring_destroy(h); }
    cJSON_Delete(list);
    TIME("render one comment markdown", 200, { cwist_sstring *h = render_markdown_to_html("A **formatted** comment with `code`, a list:\n\n- one\n- two\n\nand $a+b$."); cwist_sstring_destroy(h); });
    TIME("render plain markdown 'hi'", 200, { cwist_sstring *h = render_markdown_to_html("hi"); cwist_sstring_destroy(h); });
    TIME("render body markdown", 20, { cwist_sstring *h = render_markdown_to_html(body->data); cwist_sstring_destroy(h); });
    TIME("GET /post/:slug after comment", 3, {
        page_cache_clear();
        cwist_http_request *rq = mkreq(NULL); cwist_query_map_set(rq->path_params, "slug", slug);
        cwist_http_header_add(&rq->headers, "X-Fly-Head-Rewrite", "1");
        cwist_http_response *rs = cwist_http_response_create();
        handler_post_get(rq, rs);
        if (1) {
            const char *bd = rs->is_ptr_body ? (const char *)rs->ptr_body : rs->body->data; int nodes = 0;
            for (const char *q = bd; q && (q = strstr(q, "class='comment-node'")); q++) nodes++;
            printf("  GET status=%d bytes=%zu nodes=%d cache=%s\n", (int)rs->status_code, rs->is_ptr_body ? rs->ptr_body_len : rs->body->size,
                   nodes, cwist_http_header_get(rs->headers, "X-Fly-Cache-Status") ? cwist_http_header_get(rs->headers, "X-Fly-Cache-Status") : "-");
        }
        cwist_http_request_destroy(rq); cwist_http_response_destroy(rs); });
    return 0;
}

/* Password-locked posts: storage, redaction on every list path, search,
 * unlock tokens, attempt limits, file gating, and the post page handlers
 * end to end (prompt, wrong password, grant cookie, unlocked render). */
#include "db/db.h"
#include "db/db_internal.h"
#include "db/search.h"
#include "auth/auth.h"
#include "auth/post_unlock.h"
#include "handlers/handlers.h"
#include "handlers/handlers_internal.h"
#include "utils/cache.h"
#include "utils/post_schedule.h"
#include "utils/reqshare.h"
#include "utils/spam_guard.h"
#include "config/write_policy.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int failures;
#define CHECK(condition, name) do { \
    if (condition) printf("PASS: %s\n", name); \
    else { printf("FAIL: %s (line %d)\n", name, __LINE__); failures++; } \
} while (0)

static cwist_db g_db;

static cJSON *find_by_id(cJSON *rows, int id) {
    cJSON *r = NULL;
    cJSON_ArrayForEach(r, rows) {
        if (json_int(r, "id", 0) == id) return r;
    }
    return NULL;
}

static bool row_leaks(cJSON *row) {
    if (!row) return false;
    if (cJSON_GetObjectItem(row, "access_hash")) return true;
    const char *c = json_string_or_empty(row, "content");
    const char *s = json_string_or_empty(row, "summary");
    return strstr(c, "zebracorn") || strstr(s, "SECRETSUM");
}

static bool search_hits(const char *term, const char *type, int id) {
    cJSON *rows = db_post_list_search(&g_db, 0, term, type, NULL, 20, 0);
    bool hit = find_by_id(rows, id) != NULL;
    if (rows) cJSON_Delete(rows);
    return hit;
}

/* A connected loopback TCP pair, so the limiter sees a real peer address. */
static int loopback_client_fd(int *other) {
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t al = sizeof(a);
    if (ls < 0 || bind(ls, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(ls, 1) != 0 ||
        getsockname(ls, (struct sockaddr *)&a, &al) != 0) return -1;
    int c = socket(AF_INET, SOCK_STREAM, 0);
    if (c < 0 || connect(c, (struct sockaddr *)&a, sizeof(a)) != 0) return -1;
    int s = accept(ls, NULL, NULL);
    close(ls);
    *other = c;
    return s;
}

typedef struct {
    cwist_http_request *req;
    cwist_http_response *res;
} call_t;

static call_t call_new(const char *method_post, const char *slug, const char *cookie, const char *body) {
    call_t c;
    c.req = cwist_http_request_create();
    c.res = cwist_http_response_create();
    c.req->db = &g_db;
    c.req->method = method_post ? CWIST_HTTP_POST : CWIST_HTTP_GET;
    c.req->client_fd = -1;
    if (!c.req->path_params) c.req->path_params = cwist_query_map_create();
    if (!c.req->query_params) c.req->query_params = cwist_query_map_create();
    cwist_query_map_set(c.req->path_params, "slug", slug);
    if (cookie) cwist_http_header_add(&c.req->headers, "Cookie", cookie);
    if (body) cwist_sstring_assign(c.req->body, body);
    return c;
}

static const char *header_of(cwist_http_response *res, const char *name) {
    return cwist_http_header_get(res->headers, name);
}

static const char *body_of(cwist_http_response *res) {
    if (res->is_ptr_body) return (const char *)res->ptr_body;
    return res->body && res->body->data ? res->body->data : "";
}

int main(void) {
    const char *main_path = "/tmp/flytest_post_lock.db";
    const char *comments_path = "/tmp/flytest_post_lock_comments.db";
    remove(main_path);
    remove(comments_path);
    remove("/tmp/flytest_post_lock_secret");
    sqlite3 *conn = NULL;
    CHECK(sqlite3_open(main_path, &conn) == SQLITE_OK, "open main database");
    g_db.conn = conn;
    /* Some local cwist builds ship SQLite without FTS5; db_init then stops at
     * the search index, after the posts columns exist. Search checks are
     * skipped there instead of failing the whole file. */
    bool fts5 = sqlite3_compileoption_used("ENABLE_FTS5") != 0;
    bool inited = db_init(&g_db);
    CHECK(inited || !fts5, "initialize schema with access columns");
    if (!fts5) printf("SKIP: search checks (SQLite built without FTS5)\n");
    CHECK(db_comment_init(comments_path), "initialize comments schema");
    CHECK(auth_jwt_init("/tmp/flytest_post_lock_secret"), "jwt secret for grants");
    CHECK(spam_guard_init(), "spam guard shared table");
    write_policy_init(&g_db);
    post_schedule_init(&g_db);
    page_cache_init();
    reqshare_init();

    CHECK(db_user_create(&g_db, "writer", "w@example.com", "hash"), "create author");
    cJSON *u = db_user_get_by_username(&g_db, "writer");
    int author = json_int(u, "id", 0);
    cJSON_Delete(u);

    char *slug = NULL;
    int id = db_post_create_with_auto_slug(&g_db, 0, author, "Locked diary", "locked-diary",
                                           "Body with zebracorn inside", "SECRETSUM", "", 0, 0, "",
                                           NULL, "2020-01-01 00:00:00", &slug);
    CHECK(id > 0 && slug, "create post");
    char *open_slug = NULL;
    int open_id = db_post_create_with_auto_slug(&g_db, 0, author, "Open note", "open-note",
                                                "Plain zebracorn too", "open summary", "", 0, 0, "",
                                                NULL, "2020-01-02 00:00:00", &open_slug);
    CHECK(open_id > 0, "create unlocked neighbour");
    db_tag_set_for_post(&g_db, id, "diary");

    char hash[512];
    CHECK(auth_hash_password("hunter2", hash, sizeof(hash)), "hash post password");
    CHECK(db_post_set_access(&g_db, id, hash), "lock post");

    char got[512];
    int ver = -1;
    CHECK(db_post_access_get(&g_db, id, got, sizeof(got), &ver) && strcmp(got, hash) == 0 && ver == 1,
          "stored hash and first version");
    CHECK(auth_verify_password("hunter2", got) && !auth_verify_password("hunter3", got),
          "stored hash verifies only the password");

    cJSON *single = db_post_get_by_id(&g_db, id);
    CHECK(single && post_is_locked(single), "single read flags locked");
    CHECK(single && !cJSON_GetObjectItem(single, "access_hash"), "single read strips access_hash");
    CHECK(single && strstr(json_string_or_empty(single, "content"), "zebracorn"),
          "single read keeps the body for the gate");
    CHECK(single && json_int(single, "access_ver", 0) == 1, "single read carries access_ver");
    if (single) cJSON_Delete(single);
    single = db_post_get_by_slug(&g_db, slug);
    CHECK(single && post_is_locked(single) && !cJSON_GetObjectItem(single, "access_hash"),
          "slug read flags and strips");
    if (single) cJSON_Delete(single);

    cJSON *rows = db_post_list(&g_db, 0, 20, 0);
    cJSON *row = find_by_id(rows, id);
    CHECK(row && post_is_locked(row) && !row_leaks(row), "listing redacts the locked row");
    cJSON *open_row = find_by_id(rows, open_id);
    CHECK(open_row && !post_is_locked(open_row) &&
          strstr(json_string_or_empty(open_row, "content"), "zebracorn"), "listing keeps open rows");
    cJSON_Delete(rows);

    rows = db_post_recent_by_boards_batch(&g_db, 5);
    CHECK(!row_leaks(find_by_id(rows, id)), "board batch redacts");
    if (rows) cJSON_Delete(rows);
    rows = db_post_feed(&g_db, 0, 20);
    row = find_by_id(rows, id);
    CHECK(row && !row_leaks(row), "feed keeps the title but not the body");
    if (rows) cJSON_Delete(rows);
    rows = db_post_list_by_tag(&g_db, "diary", 20, 0);
    CHECK(find_by_id(rows, id) && !row_leaks(find_by_id(rows, id)), "tag listing redacts");
    if (rows) cJSON_Delete(rows);
    rows = db_post_list_by_month(&g_db, "2020-01", 20, 0);
    CHECK(find_by_id(rows, id) && !row_leaks(find_by_id(rows, id)), "archive listing redacts");
    if (rows) cJSON_Delete(rows);
    rows = db_post_claim_unannounced(&g_db);
    row = find_by_id(rows, id);
    CHECK(row && !row_leaks(row), "publish announcement carries no summary");
    if (rows) cJSON_Delete(rows);

    if (fts5) CHECK(!search_hits("zebracorn", NULL, id), "body search misses the locked post");
    if (fts5) CHECK(!search_hits("zebracorn", "body", id), "body-only search misses the locked post");
    if (fts5) CHECK(search_hits("zebracorn", NULL, open_id), "body search still finds open posts");
    if (fts5) CHECK(search_hits("diary", "title", id), "title search finds the locked post");
    if (fts5) CHECK(!search_hits("zeb", NULL, id), "short LIKE-only terms miss the locked body");

    /* Attachments. */
    int fid = db_file_create_volume_get_id(&g_db, id, author, "pic.png", "image/png",
                                           "public/uploads/abc_pic.png", 10);
    CHECK(fid > 0, "attach a file");
    CHECK(db_file_locked_post_for_path(&g_db, "public/uploads/abc_pic.png") == id,
          "path lookup finds the locked owner");
    CHECK(db_file_locked_post_for_path(&g_db, "public/uploads/other.png") == 0,
          "unrelated path is open");

    /* Grant tokens. */
    const char *secret = auth_jwt_secret();
    long now = (long)time(NULL);
    char tok[POST_UNLOCK_TOKEN_MAX];
    CHECK(post_unlock_token_make(secret, id, 1, now + 100, tok, sizeof(tok)), "make grant");
    CHECK(post_unlock_token_check(secret, id, 1, now, tok), "grant verifies");
    CHECK(!post_unlock_token_check(secret, id, 2, now, tok), "grant dies with a new version");
    CHECK(!post_unlock_token_check(secret, id + 1, 1, now, tok), "grant is bound to its post");
    CHECK(!post_unlock_token_check(secret, id, 1, now + 101, tok), "grant expires");
    CHECK(!post_unlock_token_check("other-secret", id, 1, now, tok), "grant is bound to the secret");
    CHECK(!post_unlock_token_check(NULL, id, 1, now, tok), "no secret verifies nothing");
    char forged[POST_UNLOCK_TOKEN_MAX];
    snprintf(forged, sizeof(forged), "%s", tok);
    forged[strlen(forged) - 1] = forged[strlen(forged) - 1] == '0' ? '1' : '0';
    CHECK(!post_unlock_token_check(secret, id, 1, now, forged), "tampered grant fails");
    CHECK(post_unlock_token_make(secret, id, 1, now + 10L * POST_UNLOCK_LIFETIME, tok, sizeof(tok)) &&
          !post_unlock_token_check(secret, id, 1, now, tok), "far-future expiry is refused");

    /* Attempt limits. */
    int peer = -1;
    int fd = loopback_client_fd(&peer);
    CHECK(fd >= 0, "loopback client");
    cwist_http_request lreq = {0};
    lreq.client_fd = fd;
    int allowed = 0;
    for (int i = 0; i < SPAM_UNLOCK_PER_POST + 5; i++) allowed += spam_guard_unlock_allow(&lreq, 0, 9001) ? 1 : 0;
    CHECK(allowed == SPAM_UNLOCK_PER_POST, "per-client per-post attempts are capped");
    CHECK(spam_guard_unlock_allow(&lreq, 0, 9002), "another post is still allowed");
    cwist_http_request noaddr = {0};
    noaddr.client_fd = -1;
    allowed = 0;
    for (int i = 0; i < SPAM_UNLOCK_PER_TARGET + 5; i++) allowed += spam_guard_unlock_allow(&noaddr, 0, 9003) ? 1 : 0;
    CHECK(allowed == SPAM_UNLOCK_PER_TARGET, "per-post total is capped for unknown clients");

    /* Handlers end to end. */
    call_t c = call_new(NULL, slug, NULL, NULL);
    handler_post_get(c.req, c.res);
    const char *html = body_of(c.res);
    CHECK(c.res->status_code == CWIST_HTTP_OK || c.res->status_code == 0, "prompt page renders");
    CHECK(strstr(html, "Locked diary") && strstr(html, "/unlock"), "prompt shows title and form");
    CHECK(!strstr(html, "zebracorn") && !strstr(html, "SECRETSUM"), "prompt leaks neither body nor summary");
    cwist_http_request_destroy(c.req);
    cwist_http_response_destroy(c.res);

    c = call_new("post", slug, NULL, "password=wrong");
    handler_post_unlock_post(c.req, c.res);
    CHECK(c.res->status_code == CWIST_HTTP_FORBIDDEN, "wrong password is refused");
    CHECK(!header_of(c.res, "Set-Cookie"), "wrong password gets no grant");
    CHECK(!strstr(body_of(c.res), "zebracorn"), "refusal leaks no body");
    cwist_http_request_destroy(c.req);
    cwist_http_response_destroy(c.res);

    c = call_new("post", slug, NULL, "password=hunter2");
    handler_post_unlock_post(c.req, c.res);
    const char *set_cookie = header_of(c.res, "Set-Cookie");
    CHECK((int)c.res->status_code == 303, "right password redirects with 303");
    CHECK(set_cookie && strncmp(set_cookie, POST_UNLOCK_COOKIE_PREFIX, strlen(POST_UNLOCK_COOKIE_PREFIX)) == 0 &&
          strstr(set_cookie, "HttpOnly"), "right password sets an HttpOnly grant");
    char cookie[256] = {0};
    if (set_cookie) {
        size_t n = strcspn(set_cookie, ";");
        if (n < sizeof(cookie)) { memcpy(cookie, set_cookie, n); cookie[n] = '\0'; }
    }
    cwist_http_request_destroy(c.req);
    cwist_http_response_destroy(c.res);

    c = call_new(NULL, slug, cookie, NULL);
    CHECK(post_unlock_has_any_cookie(c.req), "grant cookie is recognised for cache bypass");
    handler_post_get(c.req, c.res);
    CHECK(strstr(body_of(c.res), "zebracorn") != NULL, "grant opens the body");
    const char *cs = header_of(c.res, "X-Fly-Cache-Status");
    CHECK(cs && strcmp(cs, "private") == 0, "unlocked render is marked private for the BDR");
    cwist_http_request_destroy(c.req);
    cwist_http_response_destroy(c.res);

    /* Same anonymous cache key without the cookie: still the prompt. */
    c = call_new(NULL, slug, NULL, NULL);
    handler_post_get(c.req, c.res);
    CHECK(!strstr(body_of(c.res), "zebracorn"), "no grant after a granted view: still locked");
    cwist_http_request_destroy(c.req);
    cwist_http_response_destroy(c.res);

    /* Clearing the lock: version moves, old grants die, body is listed again. */
    CHECK(db_post_set_access(&g_db, id, ""), "remove the lock");
    CHECK(db_post_access_get(&g_db, id, got, sizeof(got), &ver) && got[0] == '\0' && ver == 2,
          "cleared hash and bumped version");
    c = call_new(NULL, slug, cookie, NULL);
    cJSON *again = db_post_get_by_id(&g_db, id);
    CHECK(again && !post_is_locked(again), "post reads as unlocked");
    if (again) cJSON_Delete(again);
    cwist_http_request_destroy(c.req);
    cwist_http_response_destroy(c.res);
    if (fts5) CHECK(search_hits("zebracorn", NULL, id), "body is searchable again");
    CHECK(db_file_locked_post_for_path(&g_db, "public/uploads/abc_pic.png") == 0, "attachment is open again");
    CHECK(db_post_set_access(&g_db, id, hash), "lock again");
    c = call_new(NULL, slug, cookie, NULL);
    handler_post_get(c.req, c.res);
    CHECK(!strstr(body_of(c.res), "zebracorn"), "grant from an older version no longer opens it");
    cwist_http_request_destroy(c.req);
    cwist_http_response_destroy(c.res);

    free(slug);
    free(open_slug);
    if (peer >= 0) close(peer);
    if (fd >= 0) close(fd);
    sqlite3_close(conn);
    remove(main_path);
    remove(comments_path);
    printf(failures ? "\n%d failure(s)\n" : "\nall passed\n", failures);
    return failures ? 1 : 0;
}

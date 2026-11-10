/* Bookmarks, personal blocks and password reset: storage, journaling for
 * FlyWire, the block filter, and the reset handler end to end including
 * session revocation. */
#include "db/db.h"
#include "db/db_internal.h"
#include "auth/auth.h"
#include "handlers/handlers.h"
#include "handlers/handlers_internal.h"
#include "utils/cache.h"
#include "utils/post_schedule.h"
#include "utils/reqshare.h"
#include "utils/spam_guard.h"
#include "config/write_policy.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;
#define CHECK(condition, name) do { \
    if (condition) printf("PASS: %s\n", name); \
    else { printf("FAIL: %s (line %d)\n", name, __LINE__); failures++; } \
} while (0)

static cwist_db g_db;

static int user_id(const char *name) {
    cJSON *u = db_user_get_by_username(&g_db, name);
    int id = json_int(u, "id", 0);
    if (u) cJSON_Delete(u);
    return id;
}

static int journal_count(const char *entity, const char *op) {
    sqlite3_stmt *st = NULL;
    int n = 0;
    if (sqlite3_prepare_v2(g_db.conn, "SELECT COUNT(*) FROM sync_journal WHERE entity=? AND op=?", -1, &st, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(st, 1, entity, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, op, -1, SQLITE_STATIC);
    if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return n;
}

static bool session_ok(int uid, const char *name) {
    char *token = auth_jwt_issue(uid, name, "user");
    if (!token) return false;
    cwist_http_request *req = cwist_http_request_create();
    req->db = &g_db;
    char cookie[4200];
    snprintf(cookie, sizeof(cookie), "%s=%s", SESSION_COOKIE_NAME, token);
    cwist_http_header_add(&req->headers, "Cookie", cookie);
    int out = 0;
    char role[32];
    bool ok = auth_jwt_verify_from_request(req, &out, role, sizeof(role)) && out == uid;
    cwist_http_request_destroy(req);
    free(token);
    return ok;
}

static bool session_ok_token(const char *token, int uid) {
    cwist_http_request *req = cwist_http_request_create();
    req->db = &g_db;
    char cookie[4200];
    snprintf(cookie, sizeof(cookie), "%s=%s", SESSION_COOKIE_NAME, token);
    cwist_http_header_add(&req->headers, "Cookie", cookie);
    int out = 0;
    char role[32];
    bool ok = auth_jwt_verify_from_request(req, &out, role, sizeof(role)) && out == uid;
    cwist_http_request_destroy(req);
    return ok;
}

static const char *body_of(cwist_http_response *res) {
    if (res->is_ptr_body) return (const char *)res->ptr_body;
    return res->body && res->body->data ? res->body->data : "";
}

int main(void) {
    const char *main_path = "/tmp/flytest_social.db";
    const char *comments_path = "/tmp/flytest_social_comments.db";
    remove(main_path);
    remove(comments_path);
    remove("/tmp/flytest_social_secret");
    sqlite3 *conn = NULL;
    CHECK(sqlite3_open(main_path, &conn) == SQLITE_OK, "open main database");
    g_db.conn = conn;
    bool fts5 = sqlite3_compileoption_used("ENABLE_FTS5") != 0;
    CHECK(db_init(&g_db) || !fts5, "initialize schema");
    CHECK(db_comment_init(comments_path), "initialize comments schema");
    CHECK(auth_jwt_init("/tmp/flytest_social_secret"), "jwt secret");
    CHECK(spam_guard_init(), "spam guard");
    write_policy_init(&g_db);
    post_schedule_init(&g_db);
    page_cache_init();
    reqshare_init();

    char hash[256];
    CHECK(auth_hash_password("oldpass1", hash, sizeof(hash)), "hash password");
    CHECK(db_user_create(&g_db, "alice", "alice@example.com", hash), "create alice");
    CHECK(db_user_create(&g_db, "bob", "bob@example.com", hash), "create bob");
    int alice = user_id("alice"), bob = user_id("bob");

    char *slug = NULL;
    int post = db_post_create_with_auto_slug(&g_db, 0, bob, "Bob writes", "bob-writes", "body", "", NULL,
                                             0, 0, NULL, NULL, NULL, &slug);
    free(slug);
    int draft = db_post_create_with_auto_slug(&g_db, 0, bob, "Bob draft", "bob-draft", "body", "", NULL,
                                              0, 0, NULL, "draft", NULL, &slug);
    free(slug);
    CHECK(post > 0 && draft > 0, "create posts");

    /* Bookmarks */
    CHECK(db_bookmark_set(&g_db, alice, post, true) && db_bookmark_set(&g_db, alice, post, true),
          "bookmark is idempotent");
    CHECK(db_bookmark_set(&g_db, alice, draft, true), "bookmark a draft row");
    CHECK(db_bookmark_has(&g_db, alice, post) && !db_bookmark_has(&g_db, bob, post), "bookmarks are per user");
    cJSON *list = db_bookmark_list(&g_db, alice, 20, 0);
    CHECK(cJSON_GetArraySize(list) == 1 && json_int(cJSON_GetArrayItem(list, 0), "id", 0) == post &&
          db_bookmark_count(&g_db, alice) == 1, "only published posts are listed for others' drafts");
    if (list) cJSON_Delete(list);
    CHECK(journal_count("bookmark", "upsert") == 2, "bookmark rows are journaled");
    CHECK(db_bookmark_set(&g_db, alice, post, false) && !db_bookmark_has(&g_db, alice, post) &&
          journal_count("bookmark", "delete") == 1, "unbookmark is journaled as a delete");

    /* Blocks */
    CHECK(!db_user_block_set(&g_db, alice, alice, true), "cannot block yourself");
    CHECK(db_user_block_set(&g_db, alice, bob, true) && db_user_blocks(&g_db, alice, bob) &&
          !db_user_blocks(&g_db, bob, alice), "blocks are one-way");
    CHECK(journal_count("user_block", "upsert") == 1, "block rows are journaled");
    cJSON *posts = db_post_recent(&g_db, 12);
    int before = cJSON_GetArraySize(posts);
    social_apply_blocks(&g_db, alice, posts, "user_id", true);
    CHECK(before >= 1 && cJSON_GetArraySize(posts) == 0, "blocked author's posts leave alice's lists");
    if (posts) cJSON_Delete(posts);
    posts = db_post_recent(&g_db, 12);
    social_apply_blocks(&g_db, bob, posts, "user_id", true);
    CHECK(cJSON_GetArraySize(posts) == before, "other viewers keep the posts");
    if (posts) cJSON_Delete(posts);
    cJSON *comments = cJSON_CreateArray();
    cJSON *c1 = cJSON_CreateObject();
    cJSON_AddNumberToObject(c1, "user_id", bob);
    cJSON_AddItemToArray(comments, c1);
    social_apply_blocks(&g_db, alice, comments, "user_id", false);
    CHECK(cJSON_GetArraySize(comments) == 1 && cJSON_IsTrue(cJSON_GetObjectItem(c1, "blocked")),
          "blocked comments are marked, not dropped");
    cJSON_Delete(comments);
    cJSON *blocks = db_user_block_list(&g_db, alice);
    CHECK(cJSON_GetArraySize(blocks) == 1 && json_int(cJSON_GetArrayItem(blocks, 0), "blocked_id", 0) == bob,
          "block list names the blocked user");
    if (blocks) cJSON_Delete(blocks);
    CHECK(db_user_block_set(&g_db, alice, bob, false) && !db_user_blocks(&g_db, alice, bob) &&
          journal_count("user_block", "delete") == 1, "unblock is journaled as a delete");

    /* Password reset */
    const char *token = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    CHECK(db_password_reset_create(&g_db, alice, token, (long)time(NULL) + 3600), "store reset token");
    {
        sqlite3_stmt *st = NULL;
        int plain = 0;
        sqlite3_prepare_v2(g_db.conn, "SELECT COUNT(*) FROM password_resets WHERE token_hash=?", -1, &st, NULL);
        sqlite3_bind_text(st, 1, token, -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW) plain = sqlite3_column_int(st, 0);
        sqlite3_finalize(st);
        CHECK(plain == 0, "token is stored hashed");
    }
    CHECK(journal_count("password_reset", "upsert") == 0, "reset tokens are not journaled");
    CHECK(db_password_reset_peek(&g_db, token) == alice, "token resolves to its account");
    CHECK(db_password_reset_age(&g_db, alice) >= 0 && db_password_reset_age(&g_db, alice) < 5, "token age");

    char *old_token = auth_jwt_issue(alice, "alice", "user");
    CHECK(old_token && session_ok_token(old_token, alice), "session works before the reset");
    sleep(1); /* the revocation instant must be after the old token's issue second */

    cwist_http_request *req = cwist_http_request_create();
    cwist_http_response *res = cwist_http_response_create();
    req->db = &g_db;
    req->method = CWIST_HTTP_POST;
    req->client_fd = -1;
    char body[256];
    snprintf(body, sizeof(body), "token=%s&new_password=short&confirm_password=short", token);
    cwist_sstring_assign(req->body, body);
    handler_reset_password_post(req, res);
    CHECK(strstr(body_of(res), "at least 6") != NULL && db_password_reset_peek(&g_db, token) == alice,
          "short password is refused and keeps the link");
    cwist_http_request_destroy(req);
    cwist_http_response_destroy(res);

    req = cwist_http_request_create();
    res = cwist_http_response_create();
    req->db = &g_db;
    req->method = CWIST_HTTP_POST;
    req->client_fd = -1;
    snprintf(body, sizeof(body), "token=%s&new_password=newpass99&confirm_password=newpass99", token);
    cwist_sstring_assign(req->body, body);
    handler_reset_password_post(req, res);
    CHECK(strstr(body_of(res), "Password changed") != NULL, "reset succeeds");
    cwist_http_request_destroy(req);
    cwist_http_response_destroy(res);

    cJSON *u = db_user_get_by_id(&g_db, alice);
    const char *stored = json_string_or_empty(u, "password_hash");
    CHECK(auth_verify_password("newpass99", stored) && !auth_verify_password("oldpass1", stored),
          "new password replaces the old one");
    CHECK(json_int(u, "sessions_valid_after", 0) > 0, "sessions are revoked");
    cJSON_Delete(u);
    CHECK(db_password_reset_peek(&g_db, token) == 0, "token works only once");
    CHECK(old_token && !session_ok_token(old_token, alice), "a session from before the reset is refused");
    CHECK(session_ok(alice, "alice"), "a new session after the reset works");
    CHECK(session_ok(bob, "bob"), "other accounts stay signed in");
    free(old_token);

    CHECK(db_password_reset_create(&g_db, bob, token, (long)time(NULL) - 1) && db_password_reset_peek(&g_db, token) == 0,
          "expired tokens are refused");
    cJSON *by_mail = db_user_get_by_email(&g_db, "BOB@example.com");
    CHECK(json_int(by_mail, "id", 0) == bob, "email lookup ignores case");
    if (by_mail) cJSON_Delete(by_mail);

    printf(failures ? "%d FAILURES\n" : "ALL PASS\n", failures);
    return failures ? 1 : 0;
}

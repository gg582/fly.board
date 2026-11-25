#include "auth/auth.h"
#include "auth/totp.h"
#include "db/db.h"
#include "db/db_internal.h"
#include "handlers/handlers_internal.h"
#include "utils/cache.h"
#include "utils/ipban.h"
#include "utils/reqshare.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;
static cwist_db g_db;

#define CHECK(condition, name) do { \
    if (condition) printf("PASS: %s\n", name); \
    else { printf("FAIL: %s (line %d)\n", name, __LINE__); failures++; } \
} while (0)

typedef struct {
    cwist_http_request *req;
    cwist_http_response *res;
} call_t;

static call_t call_new(const char *body) {
    call_t call = {0};
    call.req = cwist_http_request_create();
    call.res = cwist_http_response_create();
    call.req->db = &g_db;
    call.req->method = CWIST_HTTP_POST;
    call.req->client_fd = -1;
    if (body) cwist_sstring_assign(call.req->body, body);
    return call;
}

static void call_free(call_t *call) {
    cwist_http_request_destroy(call->req);
    cwist_http_response_destroy(call->res);
}

static const char *body_of(cwist_http_response *res) {
    if (res->is_ptr_body) return (const char *)res->ptr_body;
    return res->body && res->body->data ? res->body->data : "";
}

static int user_id(const char *username) {
    cJSON *user = db_user_get_by_username(&g_db, username);
    int id = user ? json_int(user, "id", 0) : 0;
    if (user) cJSON_Delete(user);
    return id;
}

static int scalar_for_user(sqlite3 *conn, const char *sql, int id) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(conn, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int(stmt, 1, id);
    int value = sqlite3_step(stmt) == SQLITE_ROW ? sqlite3_column_int(stmt, 0) : -1;
    sqlite3_finalize(stmt);
    return value;
}

static void exec_for_user(sqlite3 *conn, const char *sql, int id) {
    char expanded[768];
    snprintf(expanded, sizeof(expanded), sql, id);
    sqlite3_exec(conn, expanded, NULL, NULL, NULL);
}

int main(void) {
    char original_cwd[PATH_MAX];
    char temp_dir[] = "/tmp/flytest-local-admin-XXXXXX";
    CHECK(getcwd(original_cwd, sizeof(original_cwd)) != NULL, "save working directory");
    CHECK(mkdtemp(temp_dir) != NULL, "create isolated test directory");
    char data_dir[PATH_MAX];
    snprintf(data_dir, sizeof(data_dir), "%s/data", temp_dir);
    CHECK(mkdir(data_dir, 0700) == 0, "create isolated data directory");
    CHECK(chdir(temp_dir) == 0, "enter isolated test directory");

    FILE *settings = fopen("admin.settings", "w");
    CHECK(settings != NULL, "create test admin.settings");
    if (settings) {
        fputs("admin\nfly.board\n", settings);
        fclose(settings);
    }
    CHECK(auth_admin_load("admin.settings"), "load Local Admin credential");
    CHECK(auth_jwt_init("data/test-jwt-secret"), "initialize test JWT secret");
    CHECK(ipban_init(), "initialize login attempt tracking");
    page_cache_init();
    reqshare_init();
    snprintf(g_config.title, sizeof(g_config.title), "Local Admin Test");

    CHECK(sqlite3_open("data/blog.db", &g_db.conn) == SQLITE_OK, "open isolated main database");
    bool fts5 = sqlite3_compileoption_used("ENABLE_FTS5") != 0;
    bool inited = db_init(&g_db);
    CHECK(inited || !fts5, "initialize isolated schema");
    if (!inited && !fts5) {
        const char *remaining_schema =
            "ALTER TABLE users ADD COLUMN totp_secret TEXT NOT NULL DEFAULT '';"
            "ALTER TABLE users ADD COLUMN totp_enabled INTEGER NOT NULL DEFAULT 0;"
            "ALTER TABLE users ADD COLUMN totp_recovery TEXT NOT NULL DEFAULT '';"
            "ALTER TABLE users ADD COLUMN ghosted INTEGER NOT NULL DEFAULT 0;"
            "ALTER TABLE users ADD COLUMN sessions_valid_after INTEGER NOT NULL DEFAULT 0;"
            "CREATE TABLE IF NOT EXISTS emails (id INTEGER PRIMARY KEY AUTOINCREMENT, owner_id INTEGER NOT NULL, folder TEXT NOT NULL DEFAULT 'INBOX', body_text TEXT);"
            "CREATE TABLE IF NOT EXISTS password_resets (id INTEGER PRIMARY KEY AUTOINCREMENT, user_id INTEGER NOT NULL, token_hash TEXT, expires_at INTEGER);"
            "CREATE TABLE IF NOT EXISTS guestbook (id INTEGER PRIMARY KEY AUTOINCREMENT, owner_uid INTEGER NOT NULL, author_uid INTEGER, author_name TEXT NOT NULL, content TEXT NOT NULL, created_at DATETIME DEFAULT CURRENT_TIMESTAMP);"
            "CREATE TABLE IF NOT EXISTS post_revisions (id INTEGER PRIMARY KEY AUTOINCREMENT, post_id INTEGER NOT NULL, title TEXT NOT NULL DEFAULT '', content TEXT NOT NULL DEFAULT '', summary TEXT NOT NULL DEFAULT '', editor_id INTEGER NOT NULL DEFAULT 0, created_at DATETIME DEFAULT CURRENT_TIMESTAMP);"
            "CREATE TABLE IF NOT EXISTS sync_journal (seq INTEGER PRIMARY KEY AUTOINCREMENT, entity TEXT NOT NULL, entity_id INTEGER NOT NULL, op TEXT NOT NULL, payload TEXT NOT NULL DEFAULT '', created_at DATETIME DEFAULT CURRENT_TIMESTAMP);";
        CHECK(db_exec_sql(&g_db, remaining_schema), "complete non-FTS5 test schema");
    }
    CHECK(db_comment_init("data/comments.db"), "initialize isolated comments database");
    sqlite3 *conn = fly_db_conn(&g_db);
    int source = db_user_ensure_site_admin(&g_db, "admin");
    CHECK(source > 0, "create Local Admin users row");
    auth_site_admin_set_uid(source);
    CHECK(db_user_create(&g_db, "gg582", "gg582@example.com", "target-hash"), "create target administrator");
    int target = user_id("gg582");
    CHECK(db_user_update_role(&g_db, target, "admin"), "promote target administrator");
    const char *secret = "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ";
    CHECK(db_user_update_totp(&g_db, target, secret, true), "protect target with TOTP");
    CHECK(db_user_create(&g_db, "unsafe-admin", "unsafe@example.com", "unsafe-hash"), "create unprotected administrator");
    int unsafe = user_id("unsafe-admin");
    CHECK(db_user_update_role(&g_db, unsafe, "admin"), "promote unprotected administrator");
    CHECK(!db_user_disable_site_admin(&g_db, source, unsafe, true), "reject target without TOTP");
    CHECK(!db_user_disable_site_admin(&g_db, source, source, true), "reject self transfer");
    CHECK(scalar_for_user(conn, "SELECT active FROM users WHERE id=?", source) == 1,
          "rejected transfers leave Local Admin active");

    CHECK(db_user_update_totp(&g_db, source, secret, true), "enable Local Admin TOTP");
    cJSON *users = db_user_list(&g_db);
    cwist_sstring *admin_page = render_user_admin(users, false, NULL, false, NULL);
    char target_option[96];
    snprintf(target_option, sizeof(target_option), "value='%d'>gg582</option>", target);
    CHECK(admin_page && strstr(admin_page->data, "/admin/user/local-admin") &&
          strstr(admin_page->data, target_option) && strstr(admin_page->data, "Transfer content and disable"),
          "User Admin offers a protected transfer target");
    if (admin_page) cwist_sstring_destroy(admin_page);
    if (users) cJSON_Delete(users);

    call_t call = call_new("username=admin&password=fly.board");
    handler_login_post(call.req, call.res);
    CHECK(strstr(body_of(call.res), "Two-Step Verification") != NULL,
          "Local Admin password alone does not issue a session");
    CHECK(cwist_http_header_get(call.res->headers, "Set-Cookie") == NULL,
          "Local Admin TOTP challenge sets no session cookie");
    const char *marker = "name='token' value='";
    const char *start = strstr(body_of(call.res), marker);
    char pending[2048] = {0};
    if (start) {
        start += strlen(marker);
        size_t len = strcspn(start, "'");
        if (len < sizeof(pending)) {
            memcpy(pending, start, len);
            pending[len] = '\0';
        }
    }
    call_free(&call);
    CHECK(pending[0] != '\0', "Local Admin receives a pending TOTP token");

    char code[TOTP_CODE_DIGITS + 1];
    CHECK(totp_code_now(secret, code), "generate current Local Admin TOTP");
    char second_step[4096];
    snprintf(second_step, sizeof(second_step), "token=%s&code=%s", pending, code);
    call = call_new(second_step);
    handler_login_2fa_post(call.req, call.res);
    bool session = false;
    for (cwist_http_header_node *h = call.res->headers; h; h = h->next) {
        if (h->key && h->value && strcasecmp(h->key->data, "Set-Cookie") == 0 &&
            strncmp(h->value->data, SESSION_COOKIE_NAME "=", strlen(SESSION_COOKIE_NAME) + 1) == 0) {
            session = true;
        }
    }
    CHECK(session, "valid Local Admin TOTP completes login");
    call_free(&call);

    int post = db_post_create(&g_db, 0, source, "Local post", "local-post", "body", "summary", "", 0, 0, "");
    int file = db_file_create_volume_get_id(&g_db, post, source, "local.txt", "text/plain",
                                            "public/uploads/local-test.txt", 12);
    int comment = db_comment_create(&g_db, "post", post, source, "admin", 0, "Local comment");
    CHECK(post > 0 && file > 0 && comment > 0, "create Local Admin authored content");
    exec_for_user(conn, "INSERT INTO emails(owner_id,folder,body_text) VALUES (%d,'INBOX','local mail')", source);
    exec_for_user(conn, "INSERT INTO guestbook(owner_uid,author_uid,author_name,content) VALUES (%d,NULL,'admin','guestbook entry')", source);
    exec_for_user(conn, "INSERT INTO post_revisions(post_id,title,content,editor_id) VALUES (1,'old','old body',%d)", source);

    CHECK(db_user_disable_site_admin(&g_db, source, target, true), "transfer content and disable Local Admin");
    cJSON *source_row = db_user_get_by_id(&g_db, source);
    CHECK(source_row && json_int(source_row, "active", 1) == 0 &&
          strcmp(json_string_or_empty(source_row, "role"), "user") == 0 &&
          json_int(source_row, "totp_enabled", 1) == 0 &&
          json_int(source_row, "sessions_valid_after", 0) > 0,
          "Local Admin is demoted, disabled and its sessions are revoked");
    cJSON *target_row = db_user_get_by_id(&g_db, target);
    CHECK(target_row && strcmp(json_string_or_empty(target_row, "password_hash"), "target-hash") == 0 &&
          json_int(target_row, "totp_enabled", 0) == 1 &&
          strcmp(json_string_or_empty(target_row, "totp_secret"), secret) == 0,
          "target credentials and TOTP remain unchanged");
    CHECK(scalar_for_user(conn, "SELECT user_id FROM posts WHERE id=?", post) == target &&
          scalar_for_user(conn, "SELECT user_id FROM files WHERE id=?", file) == target,
          "posts and files move to the target administrator");
    sqlite3 *comments = db_comment_conn();
    CHECK(scalar_for_user(comments, "SELECT user_id FROM comments WHERE id=?", comment) == target,
          "comments move to the target administrator");
    CHECK(scalar_for_user(conn, "SELECT owner_id FROM emails WHERE body_text='local mail' LIMIT 1", 0) == target,
          "mail moves to the target administrator");
    CHECK(scalar_for_user(conn, "SELECT owner_uid FROM guestbook WHERE content='guestbook entry' LIMIT 1", 0) == target,
          "guestbook ownership moves to the target administrator");
    CHECK(scalar_for_user(conn, "SELECT editor_id FROM post_revisions WHERE title='old' LIMIT 1", 0) == target,
          "revision ownership moves to the target administrator");

    CHECK(db_user_ensure_site_admin(&g_db, "admin") == source, "restart keeps the original Local Admin mapping");
    CHECK(scalar_for_user(conn, "SELECT active FROM users WHERE id=?", source) == 0 &&
          scalar_for_user(conn, "SELECT role='user' FROM users WHERE id=?", source) == 1,
          "restart does not reactivate or promote Local Admin");
    call = call_new("username=admin&password=fly.board");
    handler_login_post(call.req, call.res);
    CHECK(strstr(body_of(call.res), "Invalid credentials") != NULL &&
          cwist_http_header_get(call.res->headers, "Set-Cookie") == NULL,
          "disabled admin.settings credential cannot sign in");
    call_free(&call);

    CHECK(db_user_create(&g_db, "replica-admin", "replica@example.com", "replica-hash"), "create replica security test row");
    int replica = user_id("replica-admin");
    CHECK(db_user_apply_replicated_security(conn, replica, source_row), "apply replicated Local Admin security");
    cJSON *replica_row = db_user_get_by_id(&g_db, replica);
    CHECK(replica_row && strcmp(json_string_or_empty(replica_row, "username"), "replica-admin") == 0 &&
          strcmp(json_string_or_empty(replica_row, "email"), "replica@example.com") == 0 &&
          strcmp(json_string_or_empty(replica_row, "password_hash"), "replica-hash") == 0 &&
          json_int(replica_row, "active", 1) == 0 && strcmp(json_string_or_empty(replica_row, "role"), "user") == 0,
          "replica security sync preserves node-local identity and password");
    if (replica_row) cJSON_Delete(replica_row);
    if (target_row) cJSON_Delete(target_row);
    if (source_row) cJSON_Delete(source_row);

    CHECK(db_user_create(&g_db, "second-local", "second@example.com", "!"), "create disable-only test account");
    int second = user_id("second-local");
    CHECK(db_user_update_role(&g_db, second, "admin"), "promote disable-only test account");
    int second_post = db_post_create(&g_db, 0, second, "Keep owner", "keep-owner", "body", "", "", 0, 0, "");
    CHECK(db_user_disable_site_admin(&g_db, second, target, false), "disable without transferring content");
    CHECK(scalar_for_user(conn, "SELECT user_id FROM posts WHERE id=?", second_post) == second &&
          scalar_for_user(conn, "SELECT active FROM users WHERE id=?", second) == 0,
          "disable-only preserves authorship while revoking the account");

    fly_db_close_thread_conns();
    db_comment_close();
    sqlite3_close(g_db.conn);
    g_db.conn = NULL;
    chdir(original_cwd);
    const char *files[] = {
        "admin.settings", "data/test-jwt-secret", "data/blog.db", "data/blog.db-wal", "data/blog.db-shm",
        "data/comments.db", "data/comments.db-wal", "data/comments.db-shm",
    };
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", temp_dir, files[i]);
        remove(path);
    }
    rmdir(data_dir);
    rmdir(temp_dir);
    printf("%s\n", failures ? "SOME TESTS FAILED" : "ALL TESTS PASSED");
    return failures ? 1 : 0;
}

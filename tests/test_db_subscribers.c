/* Unit test: src/db/subscriber.c lifecycle.
 *
 * Build:
 *   gcc -O1 -Iinclude -Isrc -I/home/yjlee/cwist/include -I/home/yjlee/cwist/lib \
 *       -I/home/yjlee/cwist/lib/cjson -I/home/yjlee/cwist/lib/sqlite3 \
 *       tests/test_db_subscribers.c tests/stubs/db_test_stubs.c \
 *       src/db/db.o src/db/user.o src/db/board.o src/db/post.o src/db/tag.o \
 *       src/db/series.o src/db/db_email.o src/db/db_sync.o src/db/sql_escape.o \
 *       src/db/orm.o src/db/fts5_mecab_tokenizer.o src/db/subscriber.o \
 *       /home/yjlee/cwist/libcwist.a /home/yjlee/cwist/lib/libttak/lib/libttak.a \
 *       /home/yjlee/cwist/lib/cjson/libcjson.a \
 *       -lpthread -lm -ldl -lmecab -o /tmp/test_db_subscribers
 * Run: /tmp/test_db_subscribers
 */
#include "db/db.h"
#include "db/db_internal.h"
#include "db/subscriber.h"
#include <cjson/cJSON.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int failures = 0;
#define CHECK(cond, name) do { \
    if (cond) printf("PASS: %s\n", name); \
    else { printf("FAIL: %s (line %d)\n", name, __LINE__); failures++; } \
} while (0)

static cwist_db g_db;

static cwist_db *open_test_db(const char *path) {
    remove(path);
    sqlite3 *conn = NULL;
    if (sqlite3_open(path, &conn) != SQLITE_OK) return NULL;
    g_db.conn = conn;
    g_db.pool_slot = 0;
    if (!db_init(&g_db)) { sqlite3_close(conn); g_db.conn = NULL; }
    return g_db.conn ? &g_db : NULL;
}

static int list_size(cwist_db *db) {
    cJSON *l = db_subscriber_list(db, 100, 0);
    int n = l ? cJSON_GetArraySize(l) : -1;
    if (l) cJSON_Delete(l);
    return n;
}

int main(void) {
    cwist_db *db = open_test_db("/tmp/flytest_subscribers.db");
    if (!db) { printf("FAIL: db_init\n"); return 1; }

    /* Schema exists after migration. */
    CHECK(db_exec_sql(db, "SELECT 1 FROM subscribers LIMIT 1"), "subscribers table created");

    /* Add + confirm lifecycle. */
    CHECK(db_subscriber_add(db, "reader@example.com", "tok1"), "add pending subscriber");
    CHECK(db_subscriber_count(db) == 0, "pending not counted as active");
    CHECK(!db_subscriber_confirm(db, "bogus"), "confirm with unknown token fails");
    CHECK(db_subscriber_confirm(db, "tok1"), "confirm with valid token");
    CHECK(db_subscriber_count(db) == 1, "active count after confirm");
    CHECK(!db_subscriber_confirm(db, "tok1"), "second confirm of same token fails");

    /* Active list exposes email + token for the unsubscribe link. */
    cJSON *active = db_subscriber_list_active(db);
    CHECK(active && cJSON_GetArraySize(active) == 1, "active list size");
    if (active && cJSON_GetArraySize(active) == 1) {
        cJSON *row = cJSON_GetArrayItem(active, 0);
        CHECK(strcmp(cJSON_GetObjectItem(row, "email")->valuestring, "reader@example.com") == 0, "active email");
        CHECK(strcmp(cJSON_GetObjectItem(row, "token")->valuestring, "tok1") == 0, "active token");
    }
    if (active) cJSON_Delete(active);

    /* Re-add resets to pending with the new token (re-confirm flow). */
    CHECK(db_subscriber_add(db, "reader@example.com", "tok2"), "re-add existing address");
    CHECK(db_subscriber_count(db) == 0, "re-add resets to pending");
    CHECK(list_size(db) == 1, "still one row after re-add");
    CHECK(db_subscriber_confirm(db, "tok2"), "confirm with reissued token");

    /* Email uniqueness is case-insensitive. */
    CHECK(db_subscriber_add(db, "READER@EXAMPLE.COM", "tok3"), "add same address different case");
    CHECK(list_size(db) == 1, "case-insensitive unique email");
    CHECK(!db_subscriber_confirm(db, "tok2"), "old token dead after re-add");
    CHECK(db_subscriber_confirm(db, "tok3"), "confirm with newest token");

    /* Unsubscribe deletes the row; tokens are single-use. */
    CHECK(db_subscriber_unsubscribe(db, "tok3"), "unsubscribe by token");
    CHECK(db_subscriber_count(db) == 0, "no active after unsubscribe");
    CHECK(!db_subscriber_unsubscribe(db, "tok3"), "unsubscribe token is single-use");

    /* Admin delete by id. */
    db_subscriber_add(db, "a@example.com", "tka");
    db_subscriber_add(db, "b@example.com", "tkb");
    cJSON *l = db_subscriber_list(db, 10, 0);
    int id_a = 0;
    if (l) {
        cJSON *row = NULL;
        cJSON_ArrayForEach(row, l) {
            if (strcmp(cJSON_GetObjectItem(row, "email")->valuestring, "a@example.com") == 0)
                id_a = cJSON_GetObjectItem(row, "id")->valueint;
        }
    }
    if (l) cJSON_Delete(l);
    CHECK(id_a > 0 && db_subscriber_delete(db, id_a), "admin delete by id");
    CHECK(list_size(db) == 1, "one row left after admin delete");

    /* Purge drops stale pending rows only. */
    db_subscriber_add(db, "stale@example.com", "tkstale");
    db_exec_sql(db, "UPDATE subscribers SET created_at=datetime('now','-2 day') WHERE email='stale@example.com'");
    db_subscriber_purge_pending(db);
    CHECK(list_size(db) == 1, "purge removes stale pending only");

    sqlite3_close(g_db.conn);
    g_db.conn = NULL;
    remove("/tmp/flytest_subscribers.db");
    printf("%s\n", failures ? "SOME TESTS FAILED" : "ALL TESTS PASSED");
    return failures ? 1 : 0;
}

/* Unit test: src/db users + boards + post auto-slug collision suffix.
 *
 * Build:
 *   gcc -O1 -Iinclude -Isrc -I/home/yjlee/cwist/include -I/home/yjlee/cwist/lib \
 *       -I/home/yjlee/cwist/lib/cjson -I/home/yjlee/cwist/lib/sqlite3 \
 *       tests/test_db_users_boards.c tests/stubs/db_test_stubs.c \
 *       src/db/db.o src/db/user.o src/db/board.o src/db/post.o src/db/tag.o \
 *       src/db/series.o src/db/db_email.o src/db/db_sync.o src/db/sql_escape.o \
 *       src/db/orm.o src/db/fts5_mecab_tokenizer.o \
 *       /home/yjlee/cwist/libcwist.a /home/yjlee/cwist/lib/libttak/lib/libttak.a \
 *       /home/yjlee/cwist/lib/cjson/libcjson.a \
 *       -lpthread -lm -ldl -lmecab -o /tmp/test_db_users_boards
 * Run: /tmp/test_db_users_boards
 */
#include "db/db.h"
#include "db/db_internal.h"
#include <cjson/cJSON.h>
#include <stdio.h>
#include <stdlib.h>
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

int main(void) {
    cwist_db *db = open_test_db("/tmp/flytest_users_boards.db");
    if (!db) { printf("FAIL: db_init\n"); return 1; }

    /* ---- users ---- */
    CHECK(db_user_create(db, "alice", "alice@example.com", "hash1"), "user create alice");
    CHECK(!db_user_create(db, "alice", "other@example.com", "hash2"), "duplicate username rejected");
    CHECK(!db_user_create(db, "bob", "alice@example.com", "hash3"), "duplicate email rejected");
    CHECK(db_user_create(db, "bob", "bob@example.com", "hash3"), "user create bob");

    cJSON *alice = db_user_get_by_username(db, "alice");
    CHECK(alice != NULL, "get user by username");
    if (alice) {
        cJSON *id = cJSON_GetObjectItem(alice, "id");
        cJSON *role = cJSON_GetObjectItem(alice, "role");
        CHECK(cJSON_IsNumber(id) && id->valueint >= 1, "user id assigned");
        CHECK(cJSON_IsString(role) && strcmp(role->valuestring, "user") == 0, "default role is 'user'");
    }
    cJSON_Delete(alice);

    cJSON *users = db_user_list(db);
    CHECK(users && cJSON_GetArraySize(users) == 2, "user list has 2 users");
    cJSON_Delete(users);

    /* ---- boards ---- */
    CHECK(db_board_create(db, "General", "general", "desc", false, 0, 0, 0), "board create general");
    CHECK(!db_board_create(db, "General2", "general", "dup slug", false, 0, 0, 0),
          "board duplicate slug rejected");
    CHECK(!db_board_create(db, "General", "other-slug", "dup name", false, 0, 0, 0),
          "board duplicate name rejected");

    cJSON *b = db_board_get_by_slug(db, "general");
    CHECK(b != NULL, "board get by slug");
    int board_id = 0;
    if (b) {
        cJSON *id = cJSON_GetObjectItem(b, "id");
        board_id = cJSON_IsNumber(id) ? id->valueint : 0;
        CHECK(board_id > 0, "board id present");
        cJSON *desc = cJSON_GetObjectItem(b, "description");
        CHECK(cJSON_IsString(desc) && strcmp(desc->valuestring, "desc") == 0, "board description stored");
    }
    cJSON_Delete(b);

    cJSON *blist = db_board_list(db);
    CHECK(blist && cJSON_GetArraySize(blist) == 1, "board list has 1 board");
    cJSON_Delete(blist);

    /* ---- post auto-slug collision suffix ---- */
    int uid = 0;
    cJSON *u = db_user_get_by_username(db, "alice");
    if (u) { cJSON *id = cJSON_GetObjectItem(u, "id"); uid = id->valueint; }
    cJSON_Delete(u);

    char *slug1 = NULL, *slug2 = NULL, *slug3 = NULL;
    int p1 = db_post_create_with_auto_slug(db, board_id, uid, "T", "my-post", "c", "s", NULL, 0, 0, NULL, NULL, NULL, &slug1);
    int p2 = db_post_create_with_auto_slug(db, board_id, uid, "T", "my-post", "c", "s", NULL, 0, 0, NULL, NULL, NULL, &slug2);
    int p3 = db_post_create_with_auto_slug(db, board_id, uid, "T", "my-post", "c", "s", NULL, 0, 0, NULL, NULL, NULL, &slug3);
    CHECK(p1 > 0 && slug1 && strcmp(slug1, "my-post") == 0, "first post keeps base slug");
    CHECK(p2 > 0 && slug2 && strcmp(slug2, "my-post1") == 0, "collision gets suffix 1");
    CHECK(p3 > 0 && slug3 && strcmp(slug3, "my-post2") == 0, "second collision gets suffix 2");
    free(slug1); free(slug2); free(slug3);

    cJSON *post = db_post_get_by_slug(db, "my-post1");
    CHECK(post != NULL, "collision-suffixed post retrievable by slug");
    cJSON_Delete(post);

    sqlite3_close(g_db.conn);
    g_db.conn = NULL;
    remove("/tmp/flytest_users_boards.db");
    remove("/tmp/flytest_users_boards.db-wal");
    remove("/tmp/flytest_users_boards.db-shm");

    printf("%s\n", failures == 0 ? "ALL PASS" : "SOME FAILED");
    return failures == 0 ? 0 : 1;
}

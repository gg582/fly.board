/* Unit test: src/db/revision.c revision history lifecycle.
 *
 * Build:
 *   gcc -O1 -Iinclude -Isrc -I/home/yjlee/cwist/include -I/home/yjlee/cwist/lib \
 *       -I/home/yjlee/cwist/lib/cjson -I/home/yjlee/cwist/lib/sqlite3 \
 *       tests/test_db_revisions.c tests/stubs/db_test_stubs.c \
 *       src/db/db.o src/db/user.o src/db/board.o src/db/post.o src/db/tag.o \
 *       src/db/series.o src/db/db_email.o src/db/db_sync.o src/db/sql_escape.o \
 *       src/db/orm.o src/db/fts5_mecab_tokenizer.o src/db/subscriber.o \
 *       src/db/revision.o \
 *       /home/yjlee/cwist/libcwist.a /home/yjlee/cwist/lib/libttak/lib/libttak.a \
 *       /home/yjlee/cwist/lib/cjson/libcjson.a \
 *       -lpthread -lm -ldl -lmecab -o /tmp/test_db_revisions
 * Run: /tmp/test_db_revisions
 */
#include "db/db.h"
#include "db/db_internal.h"
#include "db/revision.h"
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

int main(void) {
    cwist_db *db = open_test_db("/tmp/flytest_revisions.db");
    if (!db) { printf("FAIL: db_init\n"); return 1; }

    CHECK(db_exec_sql(db, "SELECT 1 FROM post_revisions LIMIT 1"), "post_revisions table created");

    CHECK(db_user_create(db, "writer", "w@example.com", "h"), "user create");
    cJSON *u = db_user_get_by_username(db, "writer");
    int uid = cJSON_GetObjectItem(u, "id")->valueint;
    cJSON_Delete(u);
    CHECK(db_board_create(db, "B", "b", "", false, 0, 0, 0), "board create");

    int pid = db_post_create(db, 1, uid, "V1", "v1", "first body", "s1", NULL, 0, 0, "");
    CHECK(pid > 0, "post created");

    /* Each edit stores the version it replaced. */
    CHECK(db_revision_save(db, pid, "V1", "first body", "s1", uid), "save revision 1");
    CHECK(db_revision_save(db, pid, "V2", "second body", "s2", uid), "save revision 2");
    CHECK(db_revision_count(db, pid) == 2, "two revisions stored");
    CHECK(!db_revision_save(db, 0, "x", "y", "z", uid), "save with invalid post id fails");

    /* List is newest first, no content payload. */
    cJSON *list = db_revision_list(db, pid, 10);
    CHECK(list && cJSON_GetArraySize(list) == 2, "list size");
    if (list && cJSON_GetArraySize(list) == 2) {
        cJSON *latest = cJSON_GetArrayItem(list, 0);
        CHECK(strcmp(cJSON_GetObjectItem(latest, "title")->valuestring, "V2") == 0, "list newest first");
        CHECK(cJSON_GetObjectItem(latest, "content") == NULL, "list omits content");
        CHECK(strcmp(cJSON_GetObjectItem(latest, "editor_name")->valuestring, "writer") == 0, "editor name joined");
    }
    if (list) cJSON_Delete(list);

    /* Get returns the full snapshot. */
    list = db_revision_list(db, pid, 1);
    int first_rev_id = 0;
    if (list && cJSON_GetArraySize(list) == 1) first_rev_id = cJSON_GetObjectItem(cJSON_GetArrayItem(list, 0), "id")->valueint;
    if (list) cJSON_Delete(list);
    cJSON *rev = db_revision_get(db, first_rev_id);
    CHECK(rev != NULL, "revision get");
    if (rev) {
        CHECK(strcmp(cJSON_GetObjectItem(rev, "title")->valuestring, "V2") == 0, "revision title");
        CHECK(strcmp(cJSON_GetObjectItem(rev, "content")->valuestring, "second body") == 0, "revision content");
        CHECK(cJSON_GetObjectItem(rev, "post_id")->valueint == pid, "revision post id");
    }
    if (rev) cJSON_Delete(rev);
    CHECK(db_revision_get(db, 99999) == NULL, "unknown revision id returns NULL");

    /* Pruning keeps the newest DB_REVISION_KEEP rows. */
    for (int i = 3; i <= 35; i++) {
        char t[16], c[24];
        snprintf(t, sizeof(t), "V%d", i);
        snprintf(c, sizeof(c), "body %d", i);
        CHECK(db_revision_save(db, pid, t, c, "", uid), "save for prune");
    }
    CHECK(db_revision_count(db, pid) == DB_REVISION_KEEP, "history pruned to keep limit");

    /* Revisions are per-post. */
    int pid2 = db_post_create(db, 1, uid, "Other", "other", "body", "", NULL, 0, 0, "");
    db_revision_save(db, pid2, "Other", "body", "", uid);
    CHECK(db_revision_count(db, pid) == DB_REVISION_KEEP, "other post does not touch history");
    CHECK(db_revision_count(db, pid2) == 1, "other post has its own history");

    sqlite3_close(g_db.conn);
    g_db.conn = NULL;
    remove("/tmp/flytest_revisions.db");
    printf("%s\n", failures ? "SOME TESTS FAILED" : "ALL TESTS PASSED");
    return failures ? 1 : 0;
}

/* Unit test: src/db post CRUD + tag link consistency + series lifecycle.
 *
 * Build:
 *   gcc -O1 -Iinclude -Isrc -I/home/yjlee/cwist/include -I/home/yjlee/cwist/lib \
 *       -I/home/yjlee/cwist/lib/cjson -I/home/yjlee/cwist/lib/sqlite3 \
 *       tests/test_db_posts_tags_series.c tests/stubs/db_test_stubs.c \
 *       src/db/db.o src/db/user.o src/db/board.o src/db/post.o src/db/tag.o \
 *       src/db/series.o src/db/db_email.o src/db/db_sync.o src/db/sql_escape.o \
 *       src/db/orm.o src/db/fts5_mecab_tokenizer.o \
 *       /home/yjlee/cwist/libcwist.a /home/yjlee/cwist/lib/libttak/lib/libttak.a \
 *       /home/yjlee/cwist/lib/cjson/libcjson.a \
 *       -lpthread -lm -ldl -lmecab -o /tmp/test_db_posts_tags_series
 * Run: /tmp/test_db_posts_tags_series
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

static int json_array_count(cJSON *arr) { return arr ? cJSON_GetArraySize(arr) : -1; }

int main(void) {
    cwist_db *db = open_test_db("/tmp/flytest_posts.db");
    if (!db) { printf("FAIL: db_init\n"); return 1; }

    CHECK(db_user_create(db, "writer", "w@example.com", "h"), "user create");
    cJSON *u = db_user_get_by_username(db, "writer");
    int uid = cJSON_GetObjectItem(u, "id")->valueint;
    cJSON_Delete(u);
    CHECK(db_board_create(db, "B", "b", "", false, 0, 0, 0), "board create");

    /* ---- post CRUD ---- */
    int pid = db_post_create(db, 1, uid, "Hello", "hello", "body text", "sum", NULL, 0, 0, "tech");
    CHECK(pid > 0, "post create");

    cJSON *p = db_post_get_by_id(db, pid);
    CHECK(p != NULL, "post get by id");
    if (p) {
        CHECK(strcmp(cJSON_GetObjectItem(p, "title")->valuestring, "Hello") == 0, "post title stored");
        CHECK(strcmp(cJSON_GetObjectItem(p, "slug")->valuestring, "hello") == 0, "post slug stored");
        CHECK(strcmp(cJSON_GetObjectItem(p, "content")->valuestring, "body text") == 0, "post content stored");
    }
    cJSON_Delete(p);

    p = db_post_get_by_slug(db, "hello");
    CHECK(p != NULL, "post get by slug");
    cJSON_Delete(p);

    CHECK(db_post_update(db, pid, 1, "Hello v2", "new body", "sum2", NULL, 0, 0, "tech", NULL, NULL),
          "post update");
    p = db_post_get_by_id(db, pid);
    CHECK(p && strcmp(cJSON_GetObjectItem(p, "title")->valuestring, "Hello v2") == 0, "post title updated");
    cJSON_Delete(p);

    CHECK(db_post_count(db, 1) == 1, "post count");
    cJSON *list = db_post_list(db, 1, 10, 0);
    CHECK(json_array_count(list) == 1, "post list");
    cJSON_Delete(list);

    /* ---- tags ---- */
    int t1 = db_tag_get_or_create(db, "c");
    int t1b = db_tag_get_or_create(db, "c");
    int t2 = db_tag_get_or_create(db, "rust");
    CHECK(t1 > 0 && t1 == t1b, "tag get_or_create is idempotent");
    CHECK(t2 > 0 && t2 != t1, "distinct tag gets distinct id");

    CHECK(db_tag_link(db, pid, t1), "tag link c");
    CHECK(db_tag_link(db, pid, t2), "tag link rust");
    CHECK(db_tag_link(db, pid, t1), "duplicate tag link tolerated");
    cJSON *tags = db_tag_list_by_post(db, pid);
    CHECK(json_array_count(tags) == 2, "post has 2 tags after duplicate link");
    cJSON_Delete(tags);

    /* csv setter replaces the whole set */
    CHECK(db_tag_set_for_post(db, pid, "go, c"), "tag_set_for_post csv");
    tags = db_tag_list_by_post(db, pid);
    CHECK(json_array_count(tags) == 2, "csv set yields 2 tags");
    if (tags) {
        bool has_go = false, has_rust = false;
        for (cJSON *it = tags->child; it; it = it->next) {
            cJSON *n = cJSON_GetObjectItem(it, "name");
            if (n && strcmp(n->valuestring, "go") == 0) has_go = true;
            if (n && strcmp(n->valuestring, "rust") == 0) has_rust = true;
        }
        CHECK(has_go && !has_rust, "csv set replaced old tags");
    }
    cJSON_Delete(tags);

    CHECK(db_tag_clear_by_post(db, pid), "tag clear");
    tags = db_tag_list_by_post(db, pid);
    CHECK(json_array_count(tags) == 0, "no tags after clear");
    cJSON_Delete(tags);

    /* ---- series ---- */
    int sid = db_series_create(db, "My Series", uid);
    CHECK(sid > 0, "series create");
    CHECK(db_series_find(db, "My Series", uid) == sid, "series find by title");
    CHECK(db_series_create(db, "My Series", uid) > 0, "duplicate series title allowed per creator");

    int pid2 = db_post_create(db, 1, uid, "P2", "p2", "c2", "", NULL, 0, 0, NULL);
    CHECK(db_post_set_series(db, pid, sid, 0), "post 1 joins series");
    CHECK(db_post_set_series(db, pid2, sid, 0), "post 2 joins series");
    int order_ids[2] = {pid2, pid};
    CHECK(db_series_set_order(db, sid, order_ids, 2), "series set order");
    cJSON *sp = db_series_posts(db, sid, false);
    CHECK(json_array_count(sp) == 2, "series has 2 posts");
    if (sp && cJSON_GetArraySize(sp) == 2) {
        cJSON *first = cJSON_GetArrayItem(sp, 0);
        CHECK(cJSON_GetObjectItem(first, "id")->valueint == pid2, "series order respected (first)");
    }
    cJSON_Delete(sp);

    cJSON *sget = db_series_get(db, sid);
    CHECK(sget != NULL, "series get");
    cJSON_Delete(sget);

    CHECK(db_series_update(db, sid, "Renamed", "desc"), "series update");
    sget = db_series_get(db, sid);
    CHECK(sget && strcmp(cJSON_GetObjectItem(sget, "title")->valuestring, "Renamed") == 0, "series title updated");
    cJSON_Delete(sget);

    CHECK(db_series_delete(db, sid), "series delete");
    CHECK(db_series_get(db, sid) == NULL, "deleted series gone");

    /* ---- post delete ---- */
    CHECK(db_post_delete(db, pid), "post delete");
    CHECK(db_post_get_by_id(db, pid) == NULL, "deleted post gone");
    tags = db_tag_list_by_post(db, pid);
    CHECK(json_array_count(tags) == 0 || tags == NULL, "tags of deleted post gone");
    if (tags) cJSON_Delete(tags);

    sqlite3_close(g_db.conn);
    g_db.conn = NULL;
    remove("/tmp/flytest_posts.db");
    remove("/tmp/flytest_posts.db-wal");
    remove("/tmp/flytest_posts.db-shm");

    printf("%s\n", failures == 0 ? "ALL PASS" : "SOME FAILED");
    return failures == 0 ? 0 : 1;
}

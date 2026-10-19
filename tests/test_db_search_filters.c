/* Unit test: search filters (tag / series / date range) on top of the FTS5
 * search path in src/db/search.c + src/db/post.c.
 *
 * Build:
 *   gcc -O1 -DFLY_TEST_REAL_SEARCH -Iinclude -Isrc -I/home/yjlee/cwist/include -I/home/yjlee/cwist/lib \
 *       -I/home/yjlee/cwist/lib/cjson -I/home/yjlee/cwist/lib/sqlite3 \
 *       tests/test_db_search_filters.c tests/stubs/db_test_stubs.c \
 *       src/db/db.o src/db/user.o src/db/board.o src/db/post.o src/db/tag.o \
 *       src/db/series.o src/db/db_email.o src/db/db_sync.o src/db/sql_escape.o \
 *       src/db/orm.o src/db/fts5_mecab_tokenizer.o src/db/subscriber.o \
 *       src/db/search.o \
 *       /home/yjlee/cwist/libcwist.a /home/yjlee/cwist/lib/libttak/lib/libttak.a \
 *       /home/yjlee/cwist/lib/cjson/libcjson.a \
 *       -lpthread -lm -ldl -lmecab -o /tmp/test_db_search_filters
 * Run: /tmp/test_db_search_filters
 */
#include "db/db.h"
#include "db/db_internal.h"
#include "db/search.h"
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

static int count_search(cwist_db *db, const char *term, const search_filters *f) {
    return db_post_count_search(db, 0, term, NULL, f);
}

static bool result_has(cJSON *rows, const char *slug) {
    if (!rows) return false;
    cJSON *r = NULL;
    cJSON_ArrayForEach(r, rows) {
        cJSON *s = cJSON_GetObjectItem(r, "slug");
        if (s && s->valuestring && strcmp(s->valuestring, slug) == 0) return true;
    }
    return false;
}

int main(void) {
    cwist_db *db = open_test_db("/tmp/flytest_search_filters.db");
    if (!db) { printf("FAIL: db_init\n"); return 1; }

    CHECK(db_user_create(db, "writer", "w@example.com", "h"), "user create");
    cJSON *u = db_user_get_by_username(db, "writer");
    int uid = cJSON_GetObjectItem(u, "id")->valueint;
    cJSON_Delete(u);
    CHECK(db_board_create(db, "B", "b", "", false, 0, 0, 0), "board create");

    int p1 = db_post_create(db, 1, uid, "Alpha post", "alpha", "apple banana", "sum", NULL, 0, 0, "");
    int p2 = db_post_create(db, 1, uid, "Beta post", "beta", "cherry date", "sum", NULL, 0, 0, "");
    int p3 = db_post_create(db, 1, uid, "Gamma post", "gamma", "apple pie", "sum", NULL, 0, 0, "");
    CHECK(p1 > 0 && p2 > 0 && p3 > 0, "posts created");

    CHECK(db_tag_set_for_post(db, p1, "red,green"), "tags on alpha");
    CHECK(db_tag_set_for_post(db, p2, "blue"), "tags on beta");
    CHECK(db_tag_set_for_post(db, p3, "red"), "tags on gamma");

    int sid = db_series_create(db, "Saga", uid);
    CHECK(sid > 0, "series created");
    char sql[128];
    snprintf(sql, sizeof(sql), "UPDATE posts SET series_id=%d WHERE id=%d", sid, p1);
    db_exec_sql(db, sql);
    snprintf(sql, sizeof(sql), "UPDATE posts SET series_id=%d WHERE id=%d", sid, p3);
    db_exec_sql(db, sql);

    /* Baseline: no filters. */
    CHECK(count_search(db, "apple", NULL) == 2, "term search unfiltered count");
    search_filters none = {0};
    CHECK(count_search(db, NULL, &none) == 3, "filter-only search lists all");

    /* Tag filter. */
    search_filters tag_red = { .tag = "red" };
    CHECK(count_search(db, NULL, &tag_red) == 2, "tag filter count");
    CHECK(count_search(db, "apple", &tag_red) == 2, "term + tag count");
    cJSON *rows = db_post_list_search(db, 0, "apple", NULL, &tag_red, 20, 0);
    CHECK(rows && cJSON_GetArraySize(rows) == 2, "term + tag list size");
    if (rows) cJSON_Delete(rows);

    search_filters tag_blue = { .tag = "blue" };
    CHECK(count_search(db, "apple", &tag_blue) == 0, "term excluded by tag filter");
    search_filters tag_missing = { .tag = "nope" };
    CHECK(count_search(db, NULL, &tag_missing) == 0, "unknown tag matches nothing");

    /* Series filter. */
    search_filters saga = { .series = "Saga" };
    CHECK(count_search(db, NULL, &saga) == 2, "series filter count");
    CHECK(result_has(db_post_list_search(db, 0, NULL, NULL, &saga, 20, 0), "gamma"), "series filter contains gamma");
    search_filters other_series = { .series = "Other" };
    CHECK(count_search(db, NULL, &other_series) == 0, "unknown series matches nothing");

    /* Date range filter. */
    search_filters wide = { .from = "2000-01-01", .to = "2100-01-01" };
    CHECK(count_search(db, NULL, &wide) == 3, "wide date range keeps all");
    search_filters past = { .from = "2000-01-01", .to = "2000-01-02" };
    CHECK(count_search(db, NULL, &past) == 0, "past date range matches nothing");
    search_filters future_only = { .from = "2100-01-01" };
    CHECK(count_search(db, NULL, &future_only) == 0, "future-only range matches nothing");

    /* Board scoping composes with filters. */
    CHECK(db_post_count_search(db, 1, NULL, NULL, &tag_red) == 2, "board scope + tag filter");
    CHECK(db_post_count_search(db, 999, NULL, NULL, &tag_red) == 0, "unknown board scope is empty");

    /* Related posts: tag-based neighbours, and the same-board fallback for
     * posts that share no tag. */
    cJSON *rel = db_post_related_by_tags(db, p1, 5);
    CHECK(rel && cJSON_GetArraySize(rel) == 1 && result_has(rel, "gamma"), "tag-based related finds gamma");
    if (rel) cJSON_Delete(rel);
    rel = db_post_related_by_tags(db, p2, 5);
    CHECK(!rel || cJSON_GetArraySize(rel) == 0, "beta shares no tag with anyone");
    if (rel) cJSON_Delete(rel);
    rel = db_post_related_by_board(db, p2, 1, 5);
    CHECK(rel && cJSON_GetArraySize(rel) == 2, "board fallback fills untagged post");
    if (rel) cJSON_Delete(rel);
    rel = db_post_related_by_board(db, p2, 0, 5);
    CHECK(rel == NULL, "board fallback needs a board");
    if (rel) cJSON_Delete(rel);

    sqlite3_close(g_db.conn);
    g_db.conn = NULL;
    remove("/tmp/flytest_search_filters.db");
    printf("%s\n", failures ? "SOME TESTS FAILED" : "ALL TESTS PASSED");
    return failures ? 1 : 0;
}

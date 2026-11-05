#define _POSIX_C_SOURCE 200809L
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;
static char active_path[512];
static char archive_path[512];

#define CHECK(c, name) do { if (c) printf("PASS: %s\n", name); \
    else { printf("FAIL: %s (line %d)\n", name, __LINE__); failures++; } } while (0)

static int exec_sql(sqlite3 *conn, const char *sql) {
    char *error = NULL;
    int rc = sqlite3_exec(conn, sql, NULL, NULL, &error);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL failed (%d): %s\n%s\n", rc, error ? error : "unknown", sql);
    }
    sqlite3_free(error);
    return rc;
}

static sqlite3 *open_database(const char *path) {
    sqlite3 *conn = NULL;
    if (sqlite3_open_v2(path, &conn, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL) != SQLITE_OK) {
        fprintf(stderr, "open failed: %s\n", conn ? sqlite3_errmsg(conn) : "unknown");
        if (conn) sqlite3_close(conn);
        return NULL;
    }
    return conn;
}

static long long scalar(sqlite3 *conn, const char *sql) {
    sqlite3_stmt *stmt = NULL;
    long long value = -1;
    if (sqlite3_prepare_v2(conn, sql, -1, &stmt, NULL) == SQLITE_OK &&
        sqlite3_step(stmt) == SQLITE_ROW) {
        value = sqlite3_column_int64(stmt, 0);
    } else {
        fprintf(stderr, "scalar query failed: %s\n", sqlite3_errmsg(conn));
    }
    sqlite3_finalize(stmt);
    return value;
}

static int migrate_year(sqlite3 *conn, int commit) {
    int rc = exec_sql(conn,
        "BEGIN IMMEDIATE;"
        "INSERT INTO archive_2023.posts(id, board_id, post_year, title) "
        "SELECT id, board_id, post_year, title FROM main.posts WHERE board_id=7 AND post_year=2023;"
        "DELETE FROM main.posts WHERE board_id=7 AND post_year=2023;");
    if (rc != SQLITE_OK) return rc;
    return exec_sql(conn, commit ? "COMMIT" : "ROLLBACK");
}

static int collect_ids(sqlite3 *conn, const char *sql, long long *ids, int capacity) {
    sqlite3_stmt *stmt = NULL;
    int count = 0;
    if (sqlite3_prepare_v2(conn, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (count >= capacity) {
            sqlite3_finalize(stmt);
            return -1;
        }
        ids[count++] = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? count : -1;
}

static int ids_equal(const long long *actual, int actual_count,
                     const long long *expected, int expected_count) {
    return actual_count == expected_count &&
           memcmp(actual, expected, (size_t)expected_count * sizeof(*expected)) == 0;
}

static void cleanup(void) {
    char path[600];
    unlink(active_path);
    unlink(archive_path);
    snprintf(path, sizeof(path), "%s-journal", active_path);
    unlink(path);
    snprintf(path, sizeof(path), "%s-journal", archive_path);
    unlink(path);
    if (active_path[0]) {
        char *slash = strrchr(active_path, '/');
        if (slash) {
            *slash = '\0';
            rmdir(active_path);
        }
    }
}

int main(void) {
    char directory[] = "/tmp/flyboard-sqlite-partition-poc.XXXXXX";
    CHECK(mkdtemp(directory) != NULL, "create isolated temporary directory");
    if (!directory[0] || access(directory, F_OK) != 0) return 1;
    snprintf(active_path, sizeof(active_path), "%s/active.sqlite", directory);
    snprintf(archive_path, sizeof(archive_path), "%s/board-7-year-2023.sqlite", directory);

    sqlite3 *active = open_database(active_path);
    sqlite3 *archive = open_database(archive_path);
    CHECK(active != NULL && archive != NULL, "open standalone active and archive databases");
    if (!active || !archive) {
        if (active) sqlite3_close(active);
        if (archive) sqlite3_close(archive);
        cleanup();
        return 1;
    }

    const char *schema =
        "PRAGMA journal_mode=DELETE;"
        "CREATE TABLE posts(id INTEGER PRIMARY KEY, board_id INTEGER NOT NULL,"
        "post_year INTEGER NOT NULL, title TEXT NOT NULL);";
    CHECK(exec_sql(active, schema) == SQLITE_OK, "create isolated active schema");
    CHECK(exec_sql(archive, schema) == SQLITE_OK, "create isolated archive schema");
    CHECK(exec_sql(active,
        "INSERT INTO posts VALUES"
        "(1,7,2023,'one'),(2,7,2023,'two'),(3,7,2023,'three'),"
        "(4,7,2024,'four'),(5,7,2025,'five'),(9,8,2023,'other board');") == SQLITE_OK,
        "seed active rows across boards and years");
    sqlite3_close(archive);

    char attach_rw[700];
    snprintf(attach_rw, sizeof(attach_rw), "ATTACH DATABASE '%s' AS archive_2023", archive_path);
    CHECK(exec_sql(active, attach_rw) == SQLITE_OK, "attach archive for offline migration");
    CHECK(migrate_year(active, 0) == SQLITE_OK, "roll back migration transaction");
    CHECK(scalar(active, "SELECT count(*) FROM main.posts WHERE board_id=7 AND post_year=2023") == 3 &&
          scalar(active, "SELECT count(*) FROM archive_2023.posts") == 0,
          "rollback restores every active row and leaves archive empty");
    CHECK(migrate_year(active, 1) == SQLITE_OK, "commit migration transaction");
    CHECK(scalar(active, "SELECT count(*) FROM main.posts WHERE board_id=7 AND post_year=2023") == 0 &&
          scalar(active, "SELECT count(*) FROM archive_2023.posts") == 3,
          "commit transfers ownership of exactly the selected board/year");
    CHECK(exec_sql(active, "DETACH DATABASE archive_2023") == SQLITE_OK, "detach writable archive");
    sqlite3_close(active);

    active = open_database(active_path);
    CHECK(active != NULL, "reopen active database for read-only archive mount");
    char attach_ro[700];
    snprintf(attach_ro, sizeof(attach_ro),
             "ATTACH DATABASE 'file:%s?mode=ro' AS archive_2023", archive_path);
    CHECK(active && exec_sql(active, attach_ro) == SQLITE_OK, "attach standard SQLite archive in read-only mode");
    CHECK(active && exec_sql(active, "PRAGMA query_only=ON") == SQLITE_OK, "enable read-only query guard");
    if (!active) {
        cleanup();
        return 1;
    }

    const char *global_read =
        "SELECT id FROM main.posts "
        "UNION ALL SELECT id FROM archive_2023.posts ORDER BY id";
    long long ids[16];
    const long long initial_ids[] = {1, 2, 3, 4, 5, 9};
    int count = collect_ids(active, global_read, ids, 16);
    CHECK(ids_equal(ids, count, initial_ids, 6), "global UNION ALL read has no duplicate or omitted post ids");
    CHECK(scalar(active,
        "SELECT count(*)-count(DISTINCT id) FROM ("
        "SELECT id FROM main.posts UNION ALL SELECT id FROM archive_2023.posts)") == 0,
        "global read contains no duplicate ids");

    const long long archive_ids[] = {1, 2, 3};
    count = collect_ids(active,
        "SELECT id FROM archive_2023.posts WHERE board_id=7 AND post_year=2023 ORDER BY id",
        ids, 16);
    CHECK(ids_equal(ids, count, archive_ids, 3), "board/year route reads only its deterministic archive owner");
    CHECK(scalar(active, "SELECT count(*) FROM main.posts WHERE board_id=7 AND post_year=2023") == 0,
          "active owner excludes the archived board/year");

    int rc = sqlite3_exec(active, "INSERT INTO archive_2023.posts VALUES(10,7,2023,'forbidden')",
                          NULL, NULL, NULL);
    CHECK(rc == SQLITE_READONLY, "SQLite rejects writes through read-only archive mount");
    CHECK(exec_sql(active, "PRAGMA query_only=OFF") == SQLITE_OK, "allow active fixture mutation");
    CHECK(exec_sql(active, "INSERT INTO main.posts VALUES(6,7,2025,'six')") == SQLITE_OK,
          "write-active database still accepts new posts");
    CHECK(exec_sql(active, "PRAGMA query_only=ON") == SQLITE_OK, "restore read-only query guard");
    const long long final_ids[] = {1, 2, 3, 4, 5, 6, 9};
    count = collect_ids(active, global_read, ids, 16);
    CHECK(ids_equal(ids, count, final_ids, 7), "new active write appears once without changing archive results");

    sqlite3_close(active);
    cleanup();
    printf("%s\n", failures ? "SOME FAILED" : "ALL PASS");
    return failures != 0;
}

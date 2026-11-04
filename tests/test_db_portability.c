#define _POSIX_C_SOURCE 200809L
#include "db/db_portability.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;
#define CHECK(c, name) do { if (c) printf("PASS: %s\n", name); \
    else { printf("FAIL: %s (line %d)\n", name, __LINE__); failures++; } } while (0)

int main(void) {
    unsetenv("FLY_DB_BACKEND");
    unsetenv("FLY_DB_LOCATION");
    CHECK(strcmp(fly_db_main_location(), "data/blog.db") == 0, "default path unchanged");
    char error[256];
    CHECK(fly_db_backend_supported(error, sizeof(error)), "default backend is sqlite");
    setenv("FLY_DB_BACKEND", "postgres", 1);
    CHECK(!fly_db_backend_supported(error, sizeof(error)) && strstr(error, "only 'sqlite'") != NULL,
          "unimplemented server backend rejected");
    setenv("FLY_DB_BACKEND", "sqlite", 1);
    CHECK(fly_db_backend_supported(error, sizeof(error)), "explicit sqlite accepted");

    const char *path = "/tmp/flytest_portability.db";
    unlink(path);
    setenv("FLY_DB_LOCATION", path, 1);
    sqlite3 *conn = NULL;
    CHECK(fly_db_open_main(&conn) == SQLITE_OK && conn != NULL, "custom path opens");
    if (conn) sqlite3_close(conn);
    CHECK(access(path, F_OK) == 0, "custom path creates selected database");

    unlink(path);
    setenv("FLY_DB_LOCATION", "file:/tmp/flytest_portability.db?mode=rwc", 1);
    conn = NULL;
    CHECK(fly_db_open_main(&conn) == SQLITE_OK && conn != NULL, "SQLite URI opens");
    if (conn) sqlite3_close(conn);
    unlink(path);
    printf("%s\n", failures ? "SOME FAILED" : "ALL PASS");
    return failures != 0;
}

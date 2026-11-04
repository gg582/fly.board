/* Focused test for optional community roles and users.active persistence.
 *
 * Build from the repository root:
 *   gcc -O1 -ffunction-sections -fdata-sections -Iinclude -Isrc \
 *       -I/home/yjlee/cwist/include -I/home/yjlee/cwist/lib/cjson \
 *       -I/home/yjlee/cwist/lib/sqlite3 tests/test_community_roles.c \
 *       src/engine/community.c src/db/user.c \
 *       /home/yjlee/cwist/lib/cjson/libcjson.a -lsqlite3 \
 *       -Wl,--gc-sections -o /tmp/test_community_roles
 * Run: /tmp/test_community_roles
 */
#include "engine/community.h"
#include "db/db.h"
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond, name) do { \
    if (cond) printf("PASS: %s\n", name); \
    else { printf("FAIL: %s (line %d)\n", name, __LINE__); failures++; } \
} while (0)

sqlite3 *fly_db_conn(cwist_db *db) {
    return db->conn;
}

cJSON *db_sqlite3_row_to_json(sqlite3_stmt *stmt) {
    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return NULL;
    }
    cJSON *row = cJSON_CreateObject();
    for (int i = 0; i < sqlite3_column_count(stmt); i++) {
        const char *name = sqlite3_column_name(stmt, i);
        if (sqlite3_column_type(stmt, i) == SQLITE_INTEGER)
            cJSON_AddNumberToObject(row, name, sqlite3_column_int(stmt, i));
        else
            cJSON_AddStringToObject(row, name, (const char *)sqlite3_column_text(stmt, i));
    }
    sqlite3_finalize(stmt);
    return row;
}

bool db_sync_journal_row(cwist_db *db, const char *table, const char *entity, int id) {
    (void)db; (void)table; (void)entity; (void)id;
    return true;
}

bool db_site_setting_get(cwist_db *db, const char *key, char *out, size_t out_len) {
    (void)db; (void)key; (void)out; (void)out_len;
    return false;
}

bool db_site_setting_set(cwist_db *db, const char *key, const char *value) {
    (void)db; (void)key; (void)value;
    return false;
}

static void write_settings(const char *path, const char *text) {
    FILE *f = fopen(path, "w");
    if (!f) return;
    fputs(text, f);
    fclose(f);
}

static int user_active(cwist_db *db, int id) {
    cJSON *user = db_user_get_by_id(db, id);
    int active = user ? cJSON_GetObjectItem(user, "active")->valueint : -1;
    if (user) cJSON_Delete(user);
    return active;
}

int main(void) {
    const char *settings = "/tmp/flytest_community.settings";
    const char *database = "/tmp/flytest_community.db";
    remove(settings);
    remove(database);

    CHECK(community_config_load(settings), "missing settings are optional");
    CHECK(!community_roles_enabled(), "roles default to false");
    CHECK(community_role_can_moderate("admin"), "admin moderates with roles off");
    CHECK(!community_role_can_moderate("moderator"), "moderator is inert with roles off");
    CHECK(!community_role_can_moderate("user"), "user cannot moderate");

    write_settings(settings, "# optional operations\nroles = true\n");
    CHECK(community_config_load(settings) && community_roles_enabled(), "roles=true enables the model");
    CHECK(community_role_can_moderate("moderator"), "moderator can process reports");
    CHECK(community_role_can_moderate("admin"), "admin retains report access");

    write_settings(settings, "roles=maybe\n");
    CHECK(!community_config_load(settings), "invalid boolean is reported");
    CHECK(!community_roles_enabled() && !community_role_can_moderate("moderator"),
          "invalid configuration fails closed");

    write_settings(settings, "roles=false\n");
    CHECK(community_config_load(settings) && !community_roles_enabled(), "explicit false disables roles");
    write_settings(settings, "roles=1\n");
    CHECK(community_config_load(settings) && community_roles_enabled(), "boolean one enables roles");

    sqlite3 *conn = NULL;
    CHECK(sqlite3_open(database, &conn) == SQLITE_OK, "open test database");
    cwist_db db = {.conn = conn};
    const char *schema = "CREATE TABLE users (id INTEGER PRIMARY KEY, username TEXT, email TEXT,"
                         "password_hash TEXT, role TEXT DEFAULT 'user', active INTEGER DEFAULT 1)";
    CHECK(conn && sqlite3_exec(conn, schema, NULL, NULL, NULL) == SQLITE_OK, "create users table");
    CHECK(db_user_create(&db, "alice", "alice@example.com", "hash"), "create active user");
    cJSON *user = db_user_get_by_username(&db, "alice");
    int uid = user ? cJSON_GetObjectItem(user, "id")->valueint : 0;
    if (user) cJSON_Delete(user);
    CHECK(uid > 0 && user_active(&db, uid) == 1, "existing active column defaults to active");
    CHECK(community_user_set_active(&db, uid, false) && user_active(&db, uid) == 0, "suspend persists active=0");
    CHECK(community_user_set_active(&db, uid, true) && user_active(&db, uid) == 1, "restore persists active=1");
    CHECK(!community_user_set_active(&db, 99999, false), "unknown user is not reported as updated");

    if (conn) sqlite3_close(conn);
    remove(settings);
    remove(database);
    printf("%s\n", failures ? "SOME TESTS FAILED" : "ALL TESTS PASSED");
    return failures ? 1 : 0;
}

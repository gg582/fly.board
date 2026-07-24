#define _POSIX_C_SOURCE 200809L
#include "cwist/board_tree.h"
#include "db/db_internal.h"
#include <cwist/core/log.h>
#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>

static sqlite3 *g_board_tree_db = NULL;

bool db_board_tree_init(const char *path) {
    if (sqlite3_open(path, &g_board_tree_db) != SQLITE_OK) {
        CWIST_LOG_ERROR("Failed to open board_tree db: %s", sqlite3_errmsg(g_board_tree_db));
        return false;
    }
    if (!db_configure_connection(g_board_tree_db)) {
        CWIST_LOG_ERROR("Failed to configure board_tree db: %s", sqlite3_errmsg(g_board_tree_db));
        sqlite3_close(g_board_tree_db);
        g_board_tree_db = NULL;
        return false;
    }
    const char *schema =
        "CREATE TABLE IF NOT EXISTS board_tree ("
        "  board_id INTEGER PRIMARY KEY,"
        "  parent_board_id INTEGER DEFAULT 0,"
        "  created_at DATETIME DEFAULT CURRENT_TIMESTAMP"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_board_tree_parent ON board_tree(parent_board_id);";
    char *err = NULL;
    if (sqlite3_exec(g_board_tree_db, schema, NULL, NULL, &err) != SQLITE_OK) {
        CWIST_LOG_ERROR("Board tree schema failed: %s", err ? err : "unknown");
        sqlite3_free(err);
        return false;
    }
    CWIST_LOG_INFO("Board tree db initialized: %s", path);
    return true;
}

void db_board_tree_close(void) {
    if (g_board_tree_db) {
        sqlite3_close(g_board_tree_db);
        g_board_tree_db = NULL;
    }
}

void db_board_tree_reopen(void) {
    if (g_board_tree_db) {
        sqlite3_close(g_board_tree_db);
        g_board_tree_db = NULL;
    }
    db_board_tree_init("data/board_tree.db");
}

bool db_board_tree_set_parent(int board_id, int parent_board_id) {
    if (!g_board_tree_db || board_id <= 0) return false;
    const char *sql =
        "INSERT INTO board_tree (board_id, parent_board_id) VALUES (?, ?)"
        " ON CONFLICT(board_id) DO UPDATE SET parent_board_id=excluded.parent_board_id;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(g_board_tree_db, sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_int(stmt, 1, board_id);
    sqlite3_bind_int(stmt, 2, parent_board_id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool db_board_tree_remove(int board_id) {
    if (!g_board_tree_db || board_id <= 0) return false;
    const char *sql = "DELETE FROM board_tree WHERE board_id=?;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(g_board_tree_db, sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_int(stmt, 1, board_id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool db_board_tree_promote_children(int parent_board_id) {
    if (!g_board_tree_db || parent_board_id <= 0) return false;
    const char *sql = "UPDATE board_tree SET parent_board_id=0 WHERE parent_board_id=?;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(g_board_tree_db, sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_int(stmt, 1, parent_board_id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

int db_board_tree_get_parent(int board_id) {
    if (!g_board_tree_db || board_id <= 0) return 0;
    const char *sql = "SELECT parent_board_id FROM board_tree WHERE board_id=?;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(g_board_tree_db, sql, -1, &stmt, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int(stmt, 1, board_id);
    int parent = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        parent = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return parent;
}

cJSON *db_board_tree_get_children(int parent_board_id) {
    if (!g_board_tree_db) return NULL;
    const char *sql = "SELECT board_id FROM board_tree WHERE parent_board_id=? ORDER BY board_id;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(g_board_tree_db, sql, -1, &stmt, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_int(stmt, 1, parent_board_id);
    cJSON *arr = cJSON_CreateArray();
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        cJSON_AddItemToArray(arr, cJSON_CreateNumber(sqlite3_column_int(stmt, 0)));
    }
    sqlite3_finalize(stmt);
    return arr;
}

cJSON *db_board_tree_get_roots(void) {
    if (!g_board_tree_db) return NULL;
    const char *sql =
        "SELECT board_id FROM board_tree WHERE parent_board_id=0 OR parent_board_id IS NULL"
        " ORDER BY board_id;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(g_board_tree_db, sql, -1, &stmt, NULL) != SQLITE_OK) return NULL;
    cJSON *arr = cJSON_CreateArray();
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        cJSON_AddItemToArray(arr, cJSON_CreateNumber(sqlite3_column_int(stmt, 0)));
    }
    sqlite3_finalize(stmt);
    return arr;
}

cJSON *db_board_tree_get_all(void) {
    if (!g_board_tree_db) return NULL;
    const char *sql = "SELECT board_id, parent_board_id FROM board_tree ORDER BY board_id;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(g_board_tree_db, sql, -1, &stmt, NULL) != SQLITE_OK) return NULL;
    cJSON *arr = cJSON_CreateArray();
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddNumberToObject(obj, "board_id", sqlite3_column_int(stmt, 0));
        cJSON_AddNumberToObject(obj, "parent_board_id", sqlite3_column_int(stmt, 1));
        cJSON_AddItemToArray(arr, obj);
    }
    sqlite3_finalize(stmt);
    return arr;
}

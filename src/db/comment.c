#define _POSIX_C_SOURCE 200809L
#include "db.h"
#include "db_internal.h"
#include <cwist/core/mem/alloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static sqlite3 *g_comments_db = NULL;

bool db_comment_init(const char *path) {
    if (sqlite3_open(path, &g_comments_db) != SQLITE_OK) return false;
    db_configure_connection(g_comments_db);
    const char *schema =
        "CREATE TABLE IF NOT EXISTS comments ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  target_type TEXT NOT NULL,"
        "  target_id INTEGER NOT NULL,"
        "  user_id INTEGER NOT NULL,"
        "  author_name TEXT,"
        "  parent_id INTEGER DEFAULT NULL,"
        "  content TEXT NOT NULL,"
        "  created_at DATETIME DEFAULT CURRENT_TIMESTAMP,"
        "  updated_at DATETIME DEFAULT CURRENT_TIMESTAMP,"
        "  deleted INTEGER DEFAULT 0"
        ");";
    char *err = NULL;
    sqlite3_exec(g_comments_db, schema, NULL, NULL, &err);
    if (err) { sqlite3_free(err); }
    return true;
}

void db_comment_close(void) {
    if (g_comments_db) { sqlite3_close(g_comments_db); g_comments_db = NULL; }
}

void db_comment_reopen(void) {
    if (g_comments_db) {
        sqlite3_close(g_comments_db);
        g_comments_db = NULL;
    }
    db_comment_init("data/comments.db");
}

bool db_comment_create(cwist_db *db, const char *target_type, int target_id, int user_id, const char *author_name, int parent_id, const char *content) {
    (void)db;
    if (!g_comments_db) return false;
    const char *sql = "INSERT INTO comments (target_type, target_id, user_id, author_name, parent_id, content) VALUES (?,?,?,?,?,?)";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(g_comments_db, sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, target_type, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, target_id);
    sqlite3_bind_int(stmt, 3, user_id);
    sqlite3_bind_text(stmt, 4, author_name ? author_name : "", -1, SQLITE_TRANSIENT);
    if (parent_id > 0) {
        sqlite3_bind_int(stmt, 5, parent_id);
    } else {
        sqlite3_bind_null(stmt, 5);
    }
    sqlite3_bind_text(stmt, 6, content, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool db_comment_update(cwist_db *db, int id, int user_id, const char *content) {
    (void)db;
    if (!g_comments_db) return false;
    const char *sql = "UPDATE comments SET content=?, updated_at=CURRENT_TIMESTAMP WHERE id=? AND user_id=? AND deleted=0";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(g_comments_db, sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, content, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, id);
    sqlite3_bind_int(stmt, 3, user_id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool db_comment_delete(cwist_db *db, int id, int user_id) {
    (void)db;
    if (!g_comments_db) return false;
    const char *sql = "UPDATE comments SET deleted=1, content='', updated_at=CURRENT_TIMESTAMP WHERE id=? AND user_id=? AND deleted=0";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(g_comments_db, sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_int(stmt, 1, id);
    sqlite3_bind_int(stmt, 2, user_id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

cJSON *db_comment_get_by_id(cwist_db *db, int id) {
    (void)db;
    if (!g_comments_db) return NULL;
    const char *sql = "SELECT * FROM comments WHERE id=? LIMIT 1";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(g_comments_db, sql, -1, &stmt, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_int(stmt, 1, id);
    return db_sqlite3_row_to_json(stmt);
}

cJSON *db_comment_list_by_target(cwist_db *db, const char *target_type, int target_id) {
    (void)db;
    if (!g_comments_db) return NULL;
    const char *sql = "SELECT id, target_type, target_id, user_id, author_name, parent_id, content, created_at, updated_at, deleted FROM comments WHERE target_type=? AND target_id=? ORDER BY created_at ASC";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(g_comments_db, sql, -1, &stmt, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_text(stmt, 1, target_type, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, target_id);
    return db_sqlite3_rows_to_json(stmt);
}

bool db_comment_delete_by_target(const char *target_type, int target_id) {
    if (!g_comments_db) return false;
    const char *sql = "DELETE FROM comments WHERE target_type=? AND target_id=?";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(g_comments_db, sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, target_type, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, target_id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

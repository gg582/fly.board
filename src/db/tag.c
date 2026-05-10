#define _POSIX_C_SOURCE 200809L
#include "db.h"
#include "db_internal.h"
#include <cwist/core/mem/alloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int db_tag_get_or_create(cwist_db *db, const char *name) {
    const char *sql_sel = "SELECT id FROM tags WHERE name=? LIMIT 1";
    sqlite3_stmt *stmt = NULL;
    int tag_id = 0;
    if (sqlite3_prepare_v2(db->conn, sql_sel, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            tag_id = sqlite3_column_int(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }
    if (tag_id > 0) return tag_id;
    const char *sql_ins = "INSERT INTO tags (name) VALUES (?)";
    if (sqlite3_prepare_v2(db->conn, sql_ins, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_DONE) {
            tag_id = (int)sqlite3_last_insert_rowid(db->conn);
        }
        sqlite3_finalize(stmt);
    }
    return tag_id;
}

bool db_tag_link(cwist_db *db, int post_id, int tag_id) {
    const char *sql = "INSERT OR IGNORE INTO post_tags (post_id, tag_id) VALUES (?,?)";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_int(stmt, 1, post_id);
    sqlite3_bind_int(stmt, 2, tag_id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

cJSON *db_tag_list_by_post(cwist_db *db, int post_id) {
    const char *sql = "SELECT t.name FROM tags t JOIN post_tags pt ON t.id=pt.tag_id WHERE pt.post_id=? ORDER BY t.name";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_int(stmt, 1, post_id);
    return db_sqlite3_rows_to_json(stmt);
}

bool db_tag_clear_by_post(cwist_db *db, int post_id) {
    const char *sql = "DELETE FROM post_tags WHERE post_id=?";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_int(stmt, 1, post_id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

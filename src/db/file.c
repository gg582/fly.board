#define _POSIX_C_SOURCE 200809L
#include "db.h"
#include "db_internal.h"
#include <cwist/core/mem/alloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

bool db_file_create_volume(cwist_db *db, int post_id, int user_id, const char *filename, const char *mime_type, const char *file_path, size_t len) {
    const char *sql = "INSERT INTO files (post_id, user_id, filename, mime_type, file_path, size) VALUES (?,?,?,?,?,?)";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_int(stmt, 1, post_id);
    sqlite3_bind_int(stmt, 2, user_id);
    sqlite3_bind_text(stmt, 3, filename, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, mime_type ? mime_type : "application/octet-stream", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 5, file_path, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 6, (sqlite3_int64)len);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

int db_file_create_volume_get_id(cwist_db *db, int post_id, int user_id, const char *filename, const char *mime_type, const char *file_path, size_t len) {
    const char *sql = "INSERT INTO files (post_id, user_id, filename, mime_type, file_path, size) VALUES (?,?,?,?,?,?)";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int(stmt, 1, post_id);
    sqlite3_bind_int(stmt, 2, user_id);
    sqlite3_bind_text(stmt, 3, filename, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, mime_type ? mime_type : "application/octet-stream", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 5, file_path, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 6, (sqlite3_int64)len);
    int rc = sqlite3_step(stmt);
    int id = rc == SQLITE_DONE ? (int)sqlite3_last_insert_rowid(db->conn) : 0;
    sqlite3_finalize(stmt);
    return id;
}

cJSON *db_file_get(cwist_db *db, int id) {
    const char *sql = "SELECT * FROM files WHERE id=? LIMIT 1";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_int(stmt, 1, id);
    return db_sqlite3_row_to_json(stmt);
}

cJSON *db_file_list_by_post(cwist_db *db, int post_id) {
    const char *sql =
        "SELECT a.id, a.filename, a.mime_type, a.file_path, a.size, a.created_at "
        "FROM files a "
        "LEFT JOIN files b ON a.post_id = b.post_id AND a.filename = b.filename AND a.id < b.id "
        "WHERE a.post_id = ? AND b.id IS NULL "
        "ORDER BY a.id DESC";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_int(stmt, 1, post_id);
    return db_sqlite3_rows_to_json(stmt);
}

bool db_file_delete(cwist_db *db, int id) {
    const char *sql = "DELETE FROM files WHERE id=?";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_int(stmt, 1, id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool db_file_increment_download(cwist_db *db, int id) {
    const char *sql = "UPDATE files SET download_count = download_count + 1 WHERE id=?";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_int(stmt, 1, id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool db_file_set_delete_pin_hash(cwist_db *db, int id, const char *delete_pin_hash) {
    const char *sql = "UPDATE files SET delete_pin_hash=? WHERE id=?";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, delete_pin_hash ? delete_pin_hash : "", -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool db_file_attach_to_post(cwist_db *db, int id, int post_id, int is_inline) {
    const char *sql = "UPDATE files SET post_id=?, is_inline=? WHERE id=?";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_int(stmt, 1, post_id);
    sqlite3_bind_int(stmt, 2, is_inline ? 1 : 0);
    sqlite3_bind_int(stmt, 3, id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

void db_file_replace_for_post(cwist_db *db, int post_id, const char *filename) {
    const char *sql = "SELECT id, file_path FROM files WHERE post_id=? AND filename=? LIMIT 1";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL) != SQLITE_OK) return;
    sqlite3_bind_int(stmt, 1, post_id);
    sqlite3_bind_text(stmt, 2, filename, -1, SQLITE_STATIC);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        int existing_id = sqlite3_column_int(stmt, 0);
        const char *existing_path = (const char *)sqlite3_column_text(stmt, 1);
        if (existing_path && existing_path[0]) unlink(existing_path);
        sqlite3_finalize(stmt);
        db_file_delete(db, existing_id);
    } else {
        sqlite3_finalize(stmt);
    }
}

void db_file_cleanup_duplicates(cwist_db *db) {
    while (1) {
        const char *sql =
            "SELECT a.id, a.file_path FROM files a "
            "JOIN files b ON (a.post_id = b.post_id OR (a.post_id IS NULL AND b.post_id IS NULL)) "
            "AND a.filename = b.filename AND a.id < b.id "
            "LIMIT 1";
        sqlite3_stmt *stmt = NULL;
        if (sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL) != SQLITE_OK) break;
        if (sqlite3_step(stmt) != SQLITE_ROW) {
            sqlite3_finalize(stmt);
            break;
        }
        int id = sqlite3_column_int(stmt, 0);
        const char *path = (const char *)sqlite3_column_text(stmt, 1);
        sqlite3_finalize(stmt);
        if (path && path[0]) unlink(path);
        db_file_delete(db, id);
    }
}

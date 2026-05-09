#define _POSIX_C_SOURCE 200809L
#include "db.h"
#include <cwist/core/mem/alloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool db_comment_create(cwist_db *db, const char *target_type, int target_id, int user_id, int parent_id, const char *content) {
    sqlite3_stmt *stmt = NULL;
    const char *sql = "INSERT INTO comments (target_type, target_id, user_id, parent_id, content) VALUES (?,?,?,?,?)";
    if (sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, target_type, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, target_id);
    sqlite3_bind_int(stmt, 3, user_id);
    sqlite3_bind_int(stmt, 4, parent_id);
    sqlite3_bind_text(stmt, 5, content, -1, SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool db_comment_update(cwist_db *db, int id, int user_id, const char *content) {
    sqlite3_stmt *stmt = NULL;
    const char *sql = "UPDATE comments SET content=?, updated_at=CURRENT_TIMESTAMP WHERE id=? AND user_id=? AND deleted=0";
    if (sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, content, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, id);
    sqlite3_bind_int(stmt, 3, user_id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool db_comment_delete(cwist_db *db, int id, int user_id) {
    sqlite3_stmt *stmt = NULL;
    const char *sql = "UPDATE comments SET deleted=1, content='', updated_at=CURRENT_TIMESTAMP WHERE id=? AND user_id=? AND deleted=0";
    if (sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_int(stmt, 1, id);
    sqlite3_bind_int(stmt, 2, user_id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

cJSON *db_comment_get_by_id(cwist_db *db, int id) {
    char sql[256];
    snprintf(sql, sizeof(sql), "SELECT * FROM comments WHERE id=%d LIMIT 1", id);
    cJSON *res = NULL;
    cwist_db_query(db, sql, &res);
    if (res && cJSON_GetArraySize(res) > 0) {
        cJSON *row = cJSON_GetArrayItem(res, 0);
        cJSON *cpy = cJSON_Duplicate(row, 1);
        cJSON_Delete(res);
        return cpy;
    }
    if (res) cJSON_Delete(res);
    return NULL;
}

cJSON *db_comment_list_by_target(cwist_db *db, const char *target_type, int target_id) {
    char sql[1024];
    snprintf(sql, sizeof(sql),
        "SELECT c.*, u.username FROM comments c "
        "JOIN users u ON c.user_id = u.id "
        "WHERE c.target_type='%s' AND c.target_id=%d "
        "ORDER BY c.created_at ASC",
        target_type, target_id);
    cJSON *res = NULL;
    cwist_db_query(db, sql, &res);
    return res;
}

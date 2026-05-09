#define _POSIX_C_SOURCE 200809L
#include "db.h"
#include <cwist/core/mem/alloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool db_post_vote(cwist_db *db, int post_id, int user_id, int vote_type) {
    sqlite3_stmt *stmt = NULL;
    const char *sql = "INSERT INTO post_votes (post_id, user_id, vote_type) VALUES (?,?,?) ON CONFLICT(post_id, user_id) DO UPDATE SET vote_type=excluded.vote_type";
    if (sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_int(stmt, 1, post_id);
    sqlite3_bind_int(stmt, 2, user_id);
    sqlite3_bind_int(stmt, 3, vote_type);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool db_post_vote_remove(cwist_db *db, int post_id, int user_id) {
    char sql[256];
    snprintf(sql, sizeof(sql), "DELETE FROM post_votes WHERE post_id=%d AND user_id=%d", post_id, user_id);
    return db_exec_sql(db, sql);
}

cJSON *db_post_vote_counts(cwist_db *db, int post_id) {
    char sql[512];
    snprintf(sql, sizeof(sql), "SELECT SUM(CASE WHEN vote_type=1 THEN 1 ELSE 0 END) as up, SUM(CASE WHEN vote_type=-1 THEN 1 ELSE 0 END) as down FROM post_votes WHERE post_id=%d", post_id);
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

int db_post_user_vote(cwist_db *db, int post_id, int user_id) {
    char sql[256];
    snprintf(sql, sizeof(sql), "SELECT vote_type FROM post_votes WHERE post_id=%d AND user_id=%d LIMIT 1", post_id, user_id);
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL) != SQLITE_OK) return 0;
    int vote = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        vote = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return vote;
}


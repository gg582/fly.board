#define _POSIX_C_SOURCE 200809L
#include "db.h"
#include "db_internal.h"
#include <cwist/core/mem/alloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool db_post_vote(cwist_db *db, int post_id, int user_id, int vote_type) {
    const char *sql = "INSERT INTO post_votes (post_id, user_id, vote_type) VALUES (?,?,?) ON CONFLICT(post_id, user_id) DO UPDATE SET vote_type=excluded.vote_type";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_int(stmt, 1, post_id);
    sqlite3_bind_int(stmt, 2, user_id);
    sqlite3_bind_int(stmt, 3, vote_type);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool db_post_vote_remove(cwist_db *db, int post_id, int user_id) {
    const char *sql = "DELETE FROM post_votes WHERE post_id=? AND user_id=?";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_int(stmt, 1, post_id);
    sqlite3_bind_int(stmt, 2, user_id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

cJSON *db_post_vote_counts(cwist_db *db, int post_id) {
    const char *sql = "SELECT SUM(CASE WHEN vote_type=1 THEN 1 ELSE 0 END) as up, SUM(CASE WHEN vote_type=-1 THEN 1 ELSE 0 END) as down FROM post_votes WHERE post_id=?";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_int(stmt, 1, post_id);
    return db_sqlite3_row_to_json(stmt);
}

int db_post_user_vote(cwist_db *db, int post_id, int user_id) {
    const char *sql = "SELECT vote_type FROM post_votes WHERE post_id=? AND user_id=? LIMIT 1";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int(stmt, 1, post_id);
    sqlite3_bind_int(stmt, 2, user_id);
    int vote = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        vote = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return vote;
}

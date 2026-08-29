#define _POSIX_C_SOURCE 200809L
#include "db.h"
#include "db_internal.h"
#include <cwist/core/mem/alloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

cJSON *db_user_get_by_username(cwist_db *db, const char *username) {
    const char *sql = "SELECT * FROM users WHERE username=? LIMIT 1";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(fly_db_conn(db), sql, -1, &stmt, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);
    return db_sqlite3_row_to_json(stmt);
}

cJSON *db_user_get_by_id(cwist_db *db, int id) {
    const char *sql = "SELECT * FROM users WHERE id=? LIMIT 1";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(fly_db_conn(db), sql, -1, &stmt, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_int(stmt, 1, id);
    return db_sqlite3_row_to_json(stmt);
}

bool db_user_create(cwist_db *db, const char *username, const char *email, const char *password_hash) {
    const char *sql = "INSERT INTO users (username, email, password_hash) VALUES (?,?,?)";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(fly_db_conn(db), sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, email, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, password_hash, -1, SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool db_user_delete(cwist_db *db, int id) {
    const char *sql = "DELETE FROM users WHERE id=?";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(fly_db_conn(db), sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_int(stmt, 1, id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool db_user_update_role(cwist_db *db, int id, const char *role) {
    const char *sql = "UPDATE users SET role=? WHERE id=?";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(fly_db_conn(db), sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, role, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool db_user_update_profile_pic(cwist_db *db, int id, const char *profile_pic) {
    const char *sql = "UPDATE users SET profile_pic=? WHERE id=?";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(fly_db_conn(db), sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, profile_pic, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool db_user_update_profile(cwist_db *db, int id, const char *nickname, const char *bio, const char *profile_pic) {
    const char *sql = "UPDATE users SET nickname=?, bio=?, profile_pic=? WHERE id=?";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(fly_db_conn(db), sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, nickname ? nickname : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, bio ? bio : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, profile_pic ? profile_pic : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 4, id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool db_user_update_password(cwist_db *db, int id, const char *password_hash) {
    const char *sql = "UPDATE users SET password_hash=? WHERE id=?";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(fly_db_conn(db), sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, password_hash, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool db_user_set_email_verified(cwist_db *db, int id, bool verified) {
    const char *sql = "UPDATE users SET email_verified=? WHERE id=?";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(fly_db_conn(db), sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_int(stmt, 1, verified ? 1 : 0);
    sqlite3_bind_int(stmt, 2, id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool db_email_token_create(cwist_db *db, int user_id, const char *token, long expires_at) {
    const char *sql = "INSERT INTO email_tokens (user_id, token, expires_at) VALUES (?,?,?)";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(fly_db_conn(db), sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_int(stmt, 1, user_id);
    sqlite3_bind_text(stmt, 2, token, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, (sqlite3_int64)expires_at);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

/* Validate a verification token.  On success marks the user verified,
 * deletes all of that user's tokens, and returns the user id; 0 otherwise. */
int db_email_token_consume(cwist_db *db, const char *token) {
    const char *sql = "SELECT user_id, expires_at FROM email_tokens WHERE token=? LIMIT 1";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(fly_db_conn(db), sql, -1, &stmt, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_text(stmt, 1, token, -1, SQLITE_TRANSIENT);
    int user_id = 0;
    long expires_at = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        user_id = sqlite3_column_int(stmt, 0);
        expires_at = (long)sqlite3_column_int64(stmt, 1);
    }
    sqlite3_finalize(stmt);
    if (user_id <= 0 || expires_at < (long)time(NULL)) return 0;

    db_user_set_email_verified(db, user_id, true);
    const char *del = "DELETE FROM email_tokens WHERE user_id=?";
    stmt = NULL;
    if (sqlite3_prepare_v2(fly_db_conn(db), del, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, user_id);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
    return user_id;
}

cJSON *db_user_list(cwist_db *db) {
    const char *sql = "SELECT id, username, email, role, profile_pic, created_at, active FROM users ORDER BY id";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(fly_db_conn(db), sql, -1, &stmt, NULL) != SQLITE_OK) return NULL;
    return db_sqlite3_rows_to_json(stmt);
}

bool db_user_delete_with_cascade(cwist_db *db, int id, bool delete_replies) {
    if (delete_replies) {
        const char *sql = "DELETE FROM comments WHERE user_id=? OR parent_id IN (SELECT id FROM comments WHERE user_id=?)";
        sqlite3_stmt *stmt = NULL;
        if (sqlite3_prepare_v2(fly_db_conn(db), sql, -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, id);
            sqlite3_bind_int(stmt, 2, id);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    } else {
        const char *sql = "UPDATE comments SET content='', deleted=1 WHERE user_id=?";
        sqlite3_stmt *stmt = NULL;
        if (sqlite3_prepare_v2(fly_db_conn(db), sql, -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, id);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    }
    {
        const char *sql = "DELETE FROM posts WHERE user_id=?";
        sqlite3_stmt *stmt = NULL;
        if (sqlite3_prepare_v2(fly_db_conn(db), sql, -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, id);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    }
    {
        const char *sql = "DELETE FROM files WHERE user_id=?";
        sqlite3_stmt *stmt = NULL;
        if (sqlite3_prepare_v2(fly_db_conn(db), sql, -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, id);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    }
    {
        const char *sql = "DELETE FROM users WHERE id=?";
        sqlite3_stmt *stmt = NULL;
        if (sqlite3_prepare_v2(fly_db_conn(db), sql, -1, &stmt, NULL) != SQLITE_OK) return false;
        sqlite3_bind_int(stmt, 1, id);
        int rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        return rc == SQLITE_DONE;
    }
}

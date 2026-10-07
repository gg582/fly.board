#define _POSIX_C_SOURCE 200809L
#include "db.h"
#include "db_internal.h"
#include <cwist/core/mem/alloc.h>
#include <cwist/core/log.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* FlyWire user sync: every account mutation journals a full-row snapshot of
 * the users table (entity "user"), so role/profile/password changes made on
 * the primary propagate to replicas. The snapshot intentionally includes
 * password_hash and email: replicas are the operator's own servers and the
 * feed is protected by TLS + the shared token.
 *
 * UPSERTS ONLY: user deletions (db_user_delete, db_user_delete_with_cascade,
 * expired-unverified sweeps) are deliberately NOT journaled — a replica keeps
 * its local accounts even when the primary deletes them, and the replica's
 * own admin.settings account must survive regardless. */

static void journal_user_row(cwist_db *db, int id) {
    if (id > 0) db_sync_journal_row(db, "users", "user", id);
}

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
    if (rc == SQLITE_DONE) journal_user_row(db, (int)sqlite3_last_insert_rowid(fly_db_conn(db)));
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
    if (rc == SQLITE_DONE) journal_user_row(db, id);
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
    if (rc == SQLITE_DONE) journal_user_row(db, id);
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
    if (rc == SQLITE_DONE) journal_user_row(db, id);
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
    if (rc == SQLITE_DONE) journal_user_row(db, id);
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
    if (rc == SQLITE_DONE) journal_user_row(db, id);
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

/* Drop all outstanding verification tokens for one account (used before
 * issuing a fresh token from the resend flow). */
bool db_email_token_delete_for_user(cwist_db *db, int user_id) {
    const char *sql = "DELETE FROM email_tokens WHERE user_id=?";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(fly_db_conn(db), sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_int(stmt, 1, user_id);
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

/* Remove unverified accounts whose verification token was created more than
 * 24 hours ago.  Tokens normally disappear through ON DELETE CASCADE, but the
 * FK pragma is per-connection, so any orphaned token rows are swept anyway to
 * keep the table from growing without bound. */
int db_user_delete_unverified_expired(cwist_db *db) {
    sqlite3 *conn = fly_db_conn(db);
    const char *sql = "DELETE FROM users WHERE email_verified=0 AND id IN ("
                      "SELECT user_id FROM email_tokens WHERE created_at < datetime('now','-24 hours'))";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(conn, sql, -1, &stmt, NULL) != SQLITE_OK) return 0;
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return 0;
    int deleted = sqlite3_changes(conn);
    const char *sweep = "DELETE FROM email_tokens WHERE user_id NOT IN (SELECT id FROM users)";
    if (sqlite3_prepare_v2(conn, sweep, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
    return deleted;
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

#define SITE_ADMIN_SETTING "site_admin_user_id"

static int row_int(cJSON *row, const char *key) {
    cJSON *item = cJSON_GetObjectItem(row, key);
    if (cJSON_IsNumber(item)) return item->valueint;
    if (cJSON_IsString(item) && item->valuestring) return atoi(item->valuestring);
    return 0;
}

static bool username_taken(cwist_db *db, const char *username, int except_id) {
    cJSON *u = db_user_get_by_username(db, username);
    if (!u) return false;
    bool taken = row_int(u, "id") != except_id;
    cJSON_Delete(u);
    return taken;
}

/* The account's name: the admin.settings id, or "<id>-admin", "<id>-admin2"...
 * when a registered user already holds it. */
static bool pick_site_admin_name(cwist_db *db, const char *wanted, int except_id,
                                 char *out, size_t out_len) {
    for (int n = 1; n <= 100; n++) {
        if (n == 1) snprintf(out, out_len, "%s", wanted);
        else if (n == 2) snprintf(out, out_len, "%s-admin", wanted);
        else snprintf(out, out_len, "%s-admin%d", wanted, n - 1);
        if (!username_taken(db, out, except_id)) return true;
    }
    return false;
}

int db_user_ensure_site_admin(cwist_db *db, const char *username) {
    if (!username || !username[0]) return 0;
    char value[32];
    int id = db_site_setting_get(db, SITE_ADMIN_SETTING, value, sizeof(value)) ? atoi(value) : 0;
    cJSON *existing = id > 0 ? db_user_get_by_id(db, id) : NULL;
    char name[128];
    if (existing) {
        /* Follow a renamed admin.settings id and keep the row an admin. */
        cJSON *current_item = cJSON_GetObjectItem(existing, "username");
        const char *current = cJSON_IsString(current_item) && current_item->valuestring ? current_item->valuestring : "";
        if (strcmp(current, username) != 0 && !username_taken(db, username, id)) {
            snprintf(name, sizeof(name), "%s", username);
        } else {
            snprintf(name, sizeof(name), "%s", current);
        }
        cJSON_Delete(existing);
        const char *sql = "UPDATE users SET username=?, role='admin', active=1 WHERE id=?";
        sqlite3_stmt *stmt = NULL;
        if (sqlite3_prepare_v2(fly_db_conn(db), sql, -1, &stmt, NULL) != SQLITE_OK) return 0;
        sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 2, id);
        int rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        if (rc == SQLITE_DONE) journal_user_row(db, id);
        return rc == SQLITE_DONE ? id : 0;
    }

    if (!pick_site_admin_name(db, username, 0, name, sizeof(name))) return 0;
    /* "!" is no valid hash, so the row never signs in through the users
     * table; admin.settings stays the only way in. */
    const char *sql = "INSERT INTO users (username, email, password_hash, role, email_verified) "
                      "VALUES (?, ?, '!', 'admin', 1)";
    char email[160];
    snprintf(email, sizeof(email), "site-admin+%ld@localhost.invalid", (long)time(NULL));
    sqlite3 *conn = fly_db_conn(db);
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(conn, sql, -1, &stmt, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, email, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return 0;
    id = (int)sqlite3_last_insert_rowid(conn);
    snprintf(value, sizeof(value), "%d", id);
    if (!db_site_setting_set(db, SITE_ADMIN_SETTING, value)) return 0;
    journal_user_row(db, id);
    return id;
}


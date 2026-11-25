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
 * expired-unverified sweeps) are deliberately NOT journaled - a replica keeps
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

bool community_user_set_active(cwist_db *db, int id, bool active) {
    const char *sql = "UPDATE users SET active=? WHERE id=?";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(fly_db_conn(db), sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_int(stmt, 1, active ? 1 : 0);
    sqlite3_bind_int(stmt, 2, id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc == SQLITE_DONE && sqlite3_changes(fly_db_conn(db)) == 1) journal_user_row(db, id);
    return rc == SQLITE_DONE && sqlite3_changes(fly_db_conn(db)) == 1;
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

bool db_user_update_totp(cwist_db *db, int id, const char *secret_b32, bool enabled) {
    const char *sql = "UPDATE users SET totp_secret=?, totp_enabled=? WHERE id=?";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(fly_db_conn(db), sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, secret_b32 ? secret_b32 : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, enabled ? 1 : 0);
    sqlite3_bind_int(stmt, 3, id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc == SQLITE_DONE) journal_user_row(db, id);
    return rc == SQLITE_DONE;
}

bool db_user_update_totp_recovery(cwist_db *db, int id, const char *recovery_csv) {
    const char *sql = "UPDATE users SET totp_recovery=? WHERE id=?";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(fly_db_conn(db), sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, recovery_csv ? recovery_csv : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc == SQLITE_DONE) journal_user_row(db, id);
    return rc == SQLITE_DONE;
}

bool db_user_set_email_verified(cwist_db *db, int id, bool verified) {
    const char *sql = "UPDATE users SET email_verified=? WHERE id=?";
    sqlite3_stmt *stmt = NULL;    if (sqlite3_prepare_v2(fly_db_conn(db), sql, -1, &stmt, NULL) != SQLITE_OK) return false;
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
    const char *sql = "SELECT id, username, email, role, profile_pic, created_at, active, ghosted, totp_enabled FROM users ORDER BY id";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(fly_db_conn(db), sql, -1, &stmt, NULL) != SQLITE_OK) return NULL;
    return db_sqlite3_rows_to_json(stmt);
}

static bool ghost_bound_exec(sqlite3 *conn, const char *sql, int id) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(conn, sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_int(stmt, 1, id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool db_user_ghost(cwist_db *db, int id) {
    if (!db || id <= 0) return false;
    sqlite3 *conn = fly_db_conn(db);
    sqlite3 *comments = db_comment_conn();
    if (!conn || !comments) return false;
    if (!db_transaction_begin(db)) return false;
    if (sqlite3_exec(comments, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK) {
        db_transaction_rollback(db);
        return false;
    }

    static const char *const purge_sql[] = {
        "DELETE FROM files WHERE user_id=?",
        "DELETE FROM post_votes WHERE user_id=?",
        "DELETE FROM email_tokens WHERE user_id=?",
        "DELETE FROM emails WHERE owner_id=?",
        "DELETE FROM board_permissions WHERE user_id=?",
        "DELETE FROM notifications WHERE user_id=?",
        "DELETE FROM guestbook WHERE owner_uid=? OR author_uid=?",
        "DELETE FROM bookmarks WHERE user_id=?",
        "DELETE FROM user_blocks WHERE user_id=? OR blocked_id=?",
        "DELETE FROM password_resets WHERE user_id=?",
        "UPDATE post_revisions SET editor_id=0 WHERE editor_id=?",
        "UPDATE posts SET delete_pin_hash='' WHERE user_id=?",
    };
    bool ok = true;
    for (size_t i = 0; i < sizeof(purge_sql) / sizeof(purge_sql[0]); i++) {
        /* The guestbook and block statements bind the same id twice. */
        if (i == 6 || i == 8) {
            sqlite3_stmt *stmt = NULL;
            ok = sqlite3_prepare_v2(conn, purge_sql[i], -1, &stmt, NULL) == SQLITE_OK;
            if (ok) {
                sqlite3_bind_int(stmt, 1, id);
                sqlite3_bind_int(stmt, 2, id);
                ok = sqlite3_step(stmt) == SQLITE_DONE;
            }
            sqlite3_finalize(stmt);
        } else {
            ok = ghost_bound_exec(conn, purge_sql[i], id);
        }
        if (!ok) break;
    }
    if (ok) ok = ghost_bound_exec(comments,
        "UPDATE comments SET author_name='Deleted user', updated_at=CURRENT_TIMESTAMP WHERE user_id=?", id);
    if (ok) {
        char username[64], email[96];
        snprintf(username, sizeof(username), "ghost-%d", id);
        snprintf(email, sizeof(email), "ghost+%d@deleted.invalid", id);
        const char *sql = "UPDATE users SET username=?, email=?, password_hash='!', role='user', "
                          "profile_pic='', nickname='Deleted user', bio='', active=0, ghosted=1, "
                          "email_verified=0, guestbook_anon=0, totp_secret='', totp_enabled=0, "
                          "totp_recovery='' WHERE id=? AND ghosted=0";
        sqlite3_stmt *stmt = NULL;
        ok = sqlite3_prepare_v2(conn, sql, -1, &stmt, NULL) == SQLITE_OK;
        if (ok) {
            sqlite3_bind_text(stmt, 1, username, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, email, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(stmt, 3, id);
            ok = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(conn) == 1;
        }
        sqlite3_finalize(stmt);
    }
    if (ok && sqlite3_exec(comments, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) ok = false;
    if (!ok) {
        sqlite3_exec(comments, "ROLLBACK", NULL, NULL, NULL);
        db_transaction_rollback(db);
        return false;
    }
    if (!db_transaction_commit(db)) {
        db_transaction_rollback(db);
        return false;
    }
    journal_user_row(db, id);
    return true;
}

bool db_user_delete_with_cascade(cwist_db *db, int id, bool delete_replies) {
    /* All tables with a foreign key into users.id (PRAGMA foreign_keys=ON on
     * every connection). Deleting a user that still owns a row in any of them
     * fails with SQLITE_CONSTRAINT and the account silently survives, so
     * every referrer must be cleared first. */
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
    static const char *const purge_sql[] = {
        "DELETE FROM posts WHERE user_id=?",
        "DELETE FROM files WHERE user_id=?",
        "DELETE FROM post_votes WHERE user_id=?",
        "DELETE FROM email_tokens WHERE user_id=?",
        "DELETE FROM emails WHERE owner_id=?",
        "DELETE FROM board_permissions WHERE user_id=?",
        "DELETE FROM board_delegations WHERE user_id=?",
        "DELETE FROM bookmarks WHERE user_id=?",
        "DELETE FROM user_blocks WHERE user_id=?",
        "DELETE FROM user_blocks WHERE blocked_id=?",
        "DELETE FROM password_resets WHERE user_id=?",
    };
    for (size_t i = 0; i < sizeof(purge_sql) / sizeof(purge_sql[0]); i++) {
        sqlite3_stmt *stmt = NULL;
        if (sqlite3_prepare_v2(fly_db_conn(db), purge_sql[i], -1, &stmt, NULL) == SQLITE_OK) {
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
        if (rc != SQLITE_DONE)
            CWIST_LOG_ERROR("user delete failed id=%d: %s", id, sqlite3_errmsg(fly_db_conn(db)));
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

static const char *row_string(cJSON *row, const char *key) {
    const char *value = cJSON_GetStringValue(cJSON_GetObjectItem(row, key));
    return value ? value : "";
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
        /* A transferred or explicitly disabled Local Admin stays disabled
         * across restarts. The admin.settings password alone cannot revive it. */
        if (row_int(existing, "active") == 0 || row_int(existing, "ghosted") != 0) {
            cJSON_Delete(existing);
            return id;
        }
        /* Follow a renamed admin.settings id and keep an active row an admin. */
        cJSON *current_item = cJSON_GetObjectItem(existing, "username");
        const char *current = cJSON_IsString(current_item) && current_item->valuestring ? current_item->valuestring : "";
        if (strcmp(current, username) != 0 && !username_taken(db, username, id)) {
            snprintf(name, sizeof(name), "%s", username);
        } else {
            snprintf(name, sizeof(name), "%s", current);
        }
        cJSON_Delete(existing);
        const char *sql = "UPDATE users SET username=?, role='admin' WHERE id=?";
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

typedef struct {
    const char *table;
    const char *entity;
    cJSON *ids;
    bool external;
} transfer_rows_t;

static cJSON *transfer_collect_ids(sqlite3 *conn, const char *sql, int source_id, bool bind_twice) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(conn, sql, -1, &stmt, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_int(stmt, 1, source_id);
    if (bind_twice) sqlite3_bind_int(stmt, 2, source_id);
    cJSON *ids = cJSON_CreateArray();
    if (!ids) {
        sqlite3_finalize(stmt);
        return NULL;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        cJSON_AddItemToArray(ids, cJSON_CreateNumber(sqlite3_column_int(stmt, 0)));
    }
    sqlite3_finalize(stmt);
    return ids;
}

static void transfer_rows_free(transfer_rows_t *rows, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (rows[i].ids) cJSON_Delete(rows[i].ids);
        rows[i].ids = NULL;
    }
}

static bool transfer_exec(sqlite3 *conn, const char *sql, int target_id, int source_id) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(conn, sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_int(stmt, 1, target_id);
    sqlite3_bind_int(stmt, 2, source_id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

static void transfer_journal_rows(cwist_db *db, sqlite3 *comments, transfer_rows_t *rows, size_t count) {
    for (size_t i = 0; i < count; i++) {
        cJSON *id = NULL;
        cJSON_ArrayForEach(id, rows[i].ids) {
            if (!cJSON_IsNumber(id)) continue;
            if (rows[i].external) {
                db_sync_journal_external_row(comments, rows[i].table, "id", rows[i].entity, id->valueint);
            } else {
                db_sync_journal_row(db, rows[i].table, rows[i].entity, id->valueint);
            }
        }
    }
}

bool db_user_disable_site_admin(cwist_db *db, int source_id, int target_id, bool transfer_content) {
    if (!db || source_id <= 0 || target_id <= 0 || source_id == target_id) return false;
    cJSON *source = db_user_get_by_id(db, source_id);
    cJSON *target = db_user_get_by_id(db, target_id);
    char target_name[128] = {0};
    if (target) snprintf(target_name, sizeof(target_name), "%s", row_string(target, "username"));
    const char *target_totp = row_string(target, "totp_secret");
    bool valid = source && target && row_int(source, "active") == 1 &&
                 row_int(source, "ghosted") == 0 && row_int(target, "active") == 1 &&
                 row_int(target, "ghosted") == 0 && strcmp(row_string(target, "role"), "admin") == 0 &&
                 row_int(target, "totp_enabled") == 1 && target_totp[0] && target_name[0];
    cJSON_Delete(source);
    cJSON_Delete(target);
    if (!valid) return false;

    sqlite3 *conn = fly_db_conn(db);
    sqlite3 *comments = transfer_content ? db_comment_conn() : NULL;
    if (!conn || (transfer_content && !comments)) return false;
    if (!db_transaction_begin(db)) return false;
    bool comments_tx = false;
    bool ok = true;
    transfer_rows_t moved[] = {
        { "posts", "post", NULL, false },
        { "files", "file", NULL, false },
        { "emails", "email", NULL, false },
        { "guestbook", "guestbook", NULL, false },
        { "comments", "comment", NULL, true },
    };
    const size_t moved_count = sizeof(moved) / sizeof(moved[0]);

    if (transfer_content && sqlite3_exec(comments, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK) {
        ok = false;
    } else if (transfer_content) {
        comments_tx = true;
    }

    if (ok && transfer_content) {
        moved[0].ids = transfer_collect_ids(conn, "SELECT id FROM posts WHERE user_id=?", source_id, false);
        moved[1].ids = transfer_collect_ids(conn, "SELECT id FROM files WHERE user_id=?", source_id, false);
        moved[2].ids = transfer_collect_ids(conn, "SELECT id FROM emails WHERE owner_id=?", source_id, false);
        moved[3].ids = transfer_collect_ids(conn, "SELECT id FROM guestbook WHERE owner_uid=? OR author_uid=?", source_id, true);
        moved[4].ids = transfer_collect_ids(comments, "SELECT id FROM comments WHERE user_id=?", source_id, false);
        for (size_t i = 0; i < moved_count; i++) {
            if (!moved[i].ids) ok = false;
        }
    }

    if (ok && transfer_content) {
        ok = transfer_exec(conn, "UPDATE posts SET user_id=? WHERE user_id=?", target_id, source_id) &&
             transfer_exec(conn, "UPDATE files SET user_id=? WHERE user_id=?", target_id, source_id) &&
             transfer_exec(conn, "UPDATE emails SET owner_id=? WHERE owner_id=?", target_id, source_id) &&
             transfer_exec(conn, "UPDATE guestbook SET owner_uid=? WHERE owner_uid=?", target_id, source_id) &&
             transfer_exec(conn, "UPDATE guestbook SET author_uid=? WHERE author_uid=?", target_id, source_id) &&
             transfer_exec(conn, "UPDATE post_revisions SET editor_id=? WHERE editor_id=?", target_id, source_id);
    }
    if (ok && transfer_content) {
        sqlite3_stmt *stmt = NULL;
        ok = sqlite3_prepare_v2(comments,
                 "UPDATE comments SET user_id=?, author_name=? WHERE user_id=?", -1, &stmt, NULL) == SQLITE_OK;
        if (ok) {
            sqlite3_bind_int(stmt, 1, target_id);
            sqlite3_bind_text(stmt, 2, target_name, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(stmt, 3, source_id);
            ok = sqlite3_step(stmt) == SQLITE_DONE;
        }
        sqlite3_finalize(stmt);
        if (ok) {
            sqlite3_stmt *name_stmt = NULL;
            ok = sqlite3_prepare_v2(conn, "UPDATE guestbook SET author_name=? WHERE author_uid=?", -1, &name_stmt, NULL) == SQLITE_OK;
            if (ok) {
                sqlite3_bind_text(name_stmt, 1, target_name, -1, SQLITE_TRANSIENT);
                sqlite3_bind_int(name_stmt, 2, target_id);
                ok = sqlite3_step(name_stmt) == SQLITE_DONE;
            }
            sqlite3_finalize(name_stmt);
        }
    }

    if (ok) {
        long revoke_after = (long)time(NULL) + 1;
        sqlite3_stmt *stmt = NULL;
        ok = sqlite3_prepare_v2(conn,
                 "UPDATE users SET active=0, role='user', totp_secret='', totp_enabled=0, "
                 "totp_recovery='', sessions_valid_after=? WHERE id=? AND active=1", -1, &stmt, NULL) == SQLITE_OK;
        if (ok) {
            sqlite3_bind_int64(stmt, 1, revoke_after);
            sqlite3_bind_int(stmt, 2, source_id);
            ok = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(conn) == 1;
        }
        sqlite3_finalize(stmt);
        if (ok) {
            ok = transfer_exec(conn, "DELETE FROM email_tokens WHERE user_id=?", source_id, source_id) &&
                 transfer_exec(conn, "DELETE FROM password_resets WHERE user_id=?", source_id, source_id);
        }
    }

    if (ok) {
        if (transfer_content) transfer_journal_rows(db, comments, moved, moved_count);
        journal_user_row(db, source_id);
    }
    transfer_rows_free(moved, moved_count);

    if (ok && comments_tx && sqlite3_exec(comments, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) ok = false;
    if (ok && !db_transaction_commit(db)) ok = false;
    if (!ok) {
        if (comments_tx) sqlite3_exec(comments, "ROLLBACK", NULL, NULL, NULL);
        db_transaction_rollback(db);
    }
    return ok;
}

bool db_user_apply_replicated_security(sqlite3 *conn, int id, const cJSON *snapshot) {
    if (!conn || id <= 0 || !cJSON_IsObject(snapshot)) return false;
    cJSON *active = cJSON_GetObjectItem(snapshot, "active");
    cJSON *role = cJSON_GetObjectItem(snapshot, "role");
    cJSON *secret = cJSON_GetObjectItem(snapshot, "totp_secret");
    cJSON *enabled = cJSON_GetObjectItem(snapshot, "totp_enabled");
    cJSON *recovery = cJSON_GetObjectItem(snapshot, "totp_recovery");
    cJSON *valid_after = cJSON_GetObjectItem(snapshot, "sessions_valid_after");
    if (!cJSON_IsNumber(active) || !cJSON_IsString(role) || !cJSON_IsString(secret) ||
        !cJSON_IsNumber(enabled) || !cJSON_IsString(recovery) || !cJSON_IsNumber(valid_after) ||
        (strcmp(role->valuestring, "admin") != 0 && strcmp(role->valuestring, "user") != 0)) {
        return false;
    }
    const char *sql = "UPDATE users SET active=?, role=?, totp_secret=?, totp_enabled=?, "
                      "totp_recovery=?, sessions_valid_after=? WHERE id=?";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(conn, sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_int(stmt, 1, active->valueint ? 1 : 0);
    sqlite3_bind_text(stmt, 2, role->valuestring, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, secret->valuestring, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 4, enabled->valueint ? 1 : 0);
    sqlite3_bind_text(stmt, 5, recovery->valuestring, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 6, (sqlite3_int64)valid_after->valuedouble);
    sqlite3_bind_int(stmt, 7, id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE && sqlite3_changes(conn) == 1;
}


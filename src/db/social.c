#define _POSIX_C_SOURCE 200809L
#include "db.h"
#include "db_internal.h"
#include <cwist/core/log.h>
#include <openssl/sha.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Per-user social state: bookmarks, personal blocks and password-reset
 * tokens.
 *
 * Bookmarks and blocks are journaled (entities "bookmark" / "user_block")
 * so a replica renders the same lists and hides the same content. Their
 * writes arrive on the primary only: every POST on a replica is proxied.
 *
 * Reset tokens are never journaled. Only a SHA-256 of the token is stored,
 * and it is checked by the POST that the replica proxies to the primary. */

static int pair_row_id(sqlite3 *conn, const char *sql, int a, int b) {
    sqlite3_stmt *st = NULL;
    int id = 0;
    if (sqlite3_prepare_v2(conn, sql, -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int(st, 1, a);
    sqlite3_bind_int(st, 2, b);
    if (sqlite3_step(st) == SQLITE_ROW) id = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return id;
}

static bool exec_pair(sqlite3 *conn, const char *sql, int a, int b) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(conn, sql, -1, &st, NULL) != SQLITE_OK) return false;
    sqlite3_bind_int(st, 1, a);
    sqlite3_bind_int(st, 2, b);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE;
}

/* ---- Bookmarks ---- */

bool db_bookmark_set(cwist_db *db, int user_id, int post_id, bool on) {
    if (!db || user_id <= 0 || post_id <= 0) return false;
    sqlite3 *conn = fly_db_conn(db);
    const char *find = "SELECT id FROM bookmarks WHERE user_id=? AND post_id=?";
    int existing = pair_row_id(conn, find, user_id, post_id);
    if (on) {
        if (existing > 0) return true;
        if (!exec_pair(conn, "INSERT OR IGNORE INTO bookmarks (user_id, post_id) VALUES (?,?)", user_id, post_id))
            return false;
        int id = pair_row_id(conn, find, user_id, post_id);
        if (id > 0) db_sync_journal_row(db, "bookmarks", "bookmark", id);
        return id > 0;
    }
    if (existing <= 0) return true;
    if (!exec_pair(conn, "DELETE FROM bookmarks WHERE user_id=? AND post_id=?", user_id, post_id)) return false;
    db_sync_journal(db, "bookmark", existing, "delete", "");
    return true;
}

bool db_bookmark_has(cwist_db *db, int user_id, int post_id) {
    if (!db || user_id <= 0 || post_id <= 0) return false;
    return pair_row_id(fly_db_conn(db), "SELECT id FROM bookmarks WHERE user_id=? AND post_id=?", user_id, post_id) > 0;
}

/* Posts the user saved, newest bookmark first. A post that is no longer
 * published drops out of the list unless the user wrote it; the row itself
 * stays, so the post comes back if it is published again. */
cJSON *db_bookmark_list(cwist_db *db, int user_id, int limit, int offset) {
    const char *sql =
        "SELECT p.*, u.username as author_name, b.name as board_name, bm.created_at as bookmarked_at "
        "FROM bookmarks bm JOIN posts p ON p.id=bm.post_id "
        "LEFT JOIN users u ON p.user_id=u.id LEFT JOIN boards b ON p.board_id=b.id "
        "WHERE bm.user_id=? AND ((" POST_PUBLIC_SQL ") OR p.user_id=?) "
        "ORDER BY bm.created_at DESC, bm.id DESC LIMIT ? OFFSET ?";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(fly_db_conn(db), sql, -1, &st, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_int(st, 1, user_id);
    sqlite3_bind_int(st, 2, user_id);
    sqlite3_bind_int(st, 3, limit);
    sqlite3_bind_int(st, 4, offset);
    return db_post_rows_to_json(st);
}

int db_bookmark_count(cwist_db *db, int user_id) {
    const char *sql =
        "SELECT COUNT(*) FROM bookmarks bm JOIN posts p ON p.id=bm.post_id "
        "WHERE bm.user_id=? AND ((" POST_PUBLIC_SQL ") OR p.user_id=?)";
    sqlite3_stmt *st = NULL;
    int n = 0;
    if (sqlite3_prepare_v2(fly_db_conn(db), sql, -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int(st, 1, user_id);
    sqlite3_bind_int(st, 2, user_id);
    if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return n;
}

/* ---- Personal blocks ---- */

bool db_user_block_set(cwist_db *db, int user_id, int blocked_id, bool on) {
    if (!db || user_id <= 0 || blocked_id <= 0 || user_id == blocked_id) return false;
    sqlite3 *conn = fly_db_conn(db);
    const char *find = "SELECT id FROM user_blocks WHERE user_id=? AND blocked_id=?";
    int existing = pair_row_id(conn, find, user_id, blocked_id);
    if (on) {
        if (existing > 0) return true;
        if (!exec_pair(conn, "INSERT OR IGNORE INTO user_blocks (user_id, blocked_id) VALUES (?,?)", user_id, blocked_id))
            return false;
        int id = pair_row_id(conn, find, user_id, blocked_id);
        if (id > 0) db_sync_journal_row(db, "user_blocks", "user_block", id);
        return id > 0;
    }
    if (existing <= 0) return true;
    if (!exec_pair(conn, "DELETE FROM user_blocks WHERE user_id=? AND blocked_id=?", user_id, blocked_id)) return false;
    db_sync_journal(db, "user_block", existing, "delete", "");
    return true;
}

bool db_user_blocks(cwist_db *db, int user_id, int other_id) {
    if (!db || user_id <= 0 || other_id <= 0 || user_id == other_id) return false;
    return pair_row_id(fly_db_conn(db), "SELECT id FROM user_blocks WHERE user_id=? AND blocked_id=?",
                       user_id, other_id) > 0;
}

/* Ids the user blocked, ascending, for bsearch by the content filters.
 * Returns the count; *out is malloc'd (NULL when the count is 0). */
int db_user_block_ids(cwist_db *db, int user_id, int **out) {
    *out = NULL;
    if (!db || user_id <= 0) return 0;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(fly_db_conn(db),
                           "SELECT blocked_id FROM user_blocks WHERE user_id=? ORDER BY blocked_id",
                           -1, &st, NULL) != SQLITE_OK)
        return 0;
    sqlite3_bind_int(st, 1, user_id);
    int n = 0, cap = 0;
    int *ids = NULL;
    while (sqlite3_step(st) == SQLITE_ROW) {
        if (n == cap) {
            cap = cap ? cap * 2 : 16;
            int *grown = realloc(ids, (size_t)cap * sizeof(*ids));
            if (!grown) break;
            ids = grown;
        }
        ids[n++] = sqlite3_column_int(st, 0);
    }
    sqlite3_finalize(st);
    if (n == 0) {
        free(ids);
        return 0;
    }
    *out = ids;
    return n;
}

cJSON *db_user_block_list(cwist_db *db, int user_id) {
    const char *sql =
        "SELECT ub.blocked_id, ub.created_at, u.username, u.nickname, u.profile_pic "
        "FROM user_blocks ub LEFT JOIN users u ON u.id=ub.blocked_id "
        "WHERE ub.user_id=? ORDER BY ub.created_at DESC, ub.id DESC";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(fly_db_conn(db), sql, -1, &st, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_int(st, 1, user_id);
    return db_sqlite3_rows_to_json(st);
}

/* ---- Password reset tokens ---- */

static void token_digest(const char *token, char out[65]) {
    unsigned char d[SHA256_DIGEST_LENGTH];
    SHA256((const unsigned char *)token, strlen(token), d);
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) snprintf(out + i * 2, 3, "%02x", d[i]);
}

bool db_password_reset_create(cwist_db *db, int user_id, const char *token, long expires_at) {
    if (!db || user_id <= 0 || !token || !token[0]) return false;
    sqlite3 *conn = fly_db_conn(db);
    /* One live link per account: a new request voids the older one. */
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(conn, "DELETE FROM password_resets WHERE user_id=?", -1, &st, NULL) != SQLITE_OK) return false;
    sqlite3_bind_int(st, 1, user_id);
    sqlite3_step(st);
    sqlite3_finalize(st);
    char digest[65];
    token_digest(token, digest);
    if (sqlite3_prepare_v2(conn, "INSERT INTO password_resets (user_id, token_hash, expires_at) VALUES (?,?,?)",
                           -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_int(st, 1, user_id);
    sqlite3_bind_text(st, 2, digest, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 3, expires_at);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE;
}

/* Seconds since the account's newest reset link was issued, or -1. */
long db_password_reset_age(cwist_db *db, int user_id) {
    sqlite3_stmt *st = NULL;
    long age = -1;
    if (sqlite3_prepare_v2(fly_db_conn(db),
                           "SELECT CAST(strftime('%s','now') AS INTEGER) - CAST(strftime('%s',created_at) AS INTEGER) "
                           "FROM password_resets WHERE user_id=? ORDER BY id DESC LIMIT 1",
                           -1, &st, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int(st, 1, user_id);
    if (sqlite3_step(st) == SQLITE_ROW) age = (long)sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return age;
}

/* User id the token belongs to while it is unexpired, else 0. With consume
 * the token (and any other link for the account) is deleted. */
static int reset_lookup(cwist_db *db, const char *token, bool consume) {
    if (!db || !token || strlen(token) != 64) return 0;
    char digest[65];
    token_digest(token, digest);
    sqlite3 *conn = fly_db_conn(db);
    sqlite3_stmt *st = NULL;
    int uid = 0;
    long expires = 0;
    if (sqlite3_prepare_v2(conn, "SELECT user_id, expires_at FROM password_resets WHERE token_hash=? LIMIT 1",
                           -1, &st, NULL) != SQLITE_OK)
        return 0;
    sqlite3_bind_text(st, 1, digest, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) {
        uid = sqlite3_column_int(st, 0);
        expires = (long)sqlite3_column_int64(st, 1);
    }
    sqlite3_finalize(st);
    if (uid > 0 && expires < (long)time(NULL)) uid = 0;
    if (consume && uid > 0) {
        if (sqlite3_prepare_v2(conn, "DELETE FROM password_resets WHERE user_id=?", -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_int(st, 1, uid);
            sqlite3_step(st);
            sqlite3_finalize(st);
        }
    }
    return uid;
}

int db_password_reset_peek(cwist_db *db, const char *token) { return reset_lookup(db, token, false); }
int db_password_reset_consume(cwist_db *db, const char *token) { return reset_lookup(db, token, true); }

void db_password_reset_sweep(cwist_db *db) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(fly_db_conn(db), "DELETE FROM password_resets WHERE expires_at < ?", -1, &st, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_int64(st, 1, (sqlite3_int64)time(NULL));
    sqlite3_step(st);
    sqlite3_finalize(st);
}

/* Sessions issued before this instant are refused (see auth.c). Journaled
 * with the user row, so a reset signs the account out on every node. */
bool db_user_revoke_sessions(cwist_db *db, int user_id) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(fly_db_conn(db), "UPDATE users SET sessions_valid_after=? WHERE id=?", -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_int64(st, 1, (sqlite3_int64)time(NULL));
    sqlite3_bind_int(st, 2, user_id);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc == SQLITE_DONE) db_sync_journal_row(db, "users", "user", user_id);
    return rc == SQLITE_DONE;
}

cJSON *db_user_get_by_email(cwist_db *db, const char *email) {
    if (!email || !email[0]) return NULL;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(fly_db_conn(db), "SELECT * FROM users WHERE lower(email)=lower(?) ORDER BY id LIMIT 1",
                           -1, &st, NULL) != SQLITE_OK)
        return NULL;
    sqlite3_bind_text(st, 1, email, -1, SQLITE_TRANSIENT);
    return db_sqlite3_row_to_json(st);
}

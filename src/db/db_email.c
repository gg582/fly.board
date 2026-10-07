#define _POSIX_C_SOURCE 200809L
#include "db.h"
#include "db_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Webmail message storage. The public db_email_*() functions follow the
 * cwist_db * convention of the rest of this directory; each is a thin wrapper
 * around a *_conn() core so the standalone mail-import tool (which opens a
 * raw sqlite3 connection of its own) can share the same SQL.
 * ------------------------------------------------------------------------- */

int db_email_create_conn(sqlite3 *conn, int owner_id, const char *folder,
                         const char *from_addr, const char *to_addrs,
                         const char *subject, const char *body_text,
                         const char *message_id, const char *in_reply_to) {
    if (!conn || owner_id <= 0) return 0;
    const char *sql =
        "INSERT INTO emails (owner_id, folder, from_addr, to_addrs, subject, body_text, message_id, in_reply_to)"
        " VALUES (?,?,?,?,?,?,?,?)";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(conn, sql, -1, &stmt, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int(stmt, 1, owner_id);
    sqlite3_bind_text(stmt, 2, folder ? folder : "INBOX", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, from_addr ? from_addr : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, to_addrs ? to_addrs : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, subject ? subject : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, body_text ? body_text : "", -1, SQLITE_TRANSIENT);
    if (message_id && message_id[0]) sqlite3_bind_text(stmt, 7, message_id, -1, SQLITE_TRANSIENT);
    if (in_reply_to && in_reply_to[0]) sqlite3_bind_text(stmt, 8, in_reply_to, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    int id = rc == SQLITE_DONE ? (int)sqlite3_last_insert_rowid(conn) : 0;
    sqlite3_finalize(stmt);
    return id;
}

int db_email_create(cwist_db *db, int owner_id, const char *folder,
                    const char *from_addr, const char *to_addrs,
                    const char *subject, const char *body_text,
                    const char *message_id, const char *in_reply_to) {
    int id = db_email_create_conn(fly_db_conn(db), owner_id, folder, from_addr, to_addrs,
                                  subject, body_text, message_id, in_reply_to);
    /* Webmail rows sync primary->replica like every other entity; the
     * replica-side spool sweep re-creating a row is harmless (idempotent
     * upsert by id). */
    if (id > 0) db_sync_journal_row(db, "emails", "email", id);
    return id;
}

/* Append an attachment listing to a stored body (used by mail-import, which
 * saves attachment files only after the row id is known). */
bool db_email_append_body_conn(sqlite3 *conn, int owner_id, int id, const char *extra) {
    if (!conn || !extra || !extra[0]) return false;
    const char *sql = "UPDATE emails SET body_text = body_text || ? WHERE id=? AND owner_id=?";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(conn, sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, extra, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, id);
    sqlite3_bind_int(stmt, 3, owner_id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

cJSON *db_email_list(cwist_db *db, int owner_id, const char *folder, int offset, int limit) {
    const char *sql =
        "SELECT id, owner_id, folder, from_addr, to_addrs, subject, is_read, created_at"
        " FROM emails WHERE owner_id=? AND folder=?"
        " ORDER BY created_at DESC, id DESC LIMIT ? OFFSET ?";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(fly_db_conn(db), sql, -1, &stmt, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_int(stmt, 1, owner_id);
    sqlite3_bind_text(stmt, 2, folder ? folder : "INBOX", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, limit > 0 ? limit : 50);
    sqlite3_bind_int(stmt, 4, offset > 0 ? offset : 0);
    return db_sqlite3_rows_to_json(stmt);
}

int db_email_count(cwist_db *db, int owner_id, const char *folder) {
    const char *sql = "SELECT COUNT(*) FROM emails WHERE owner_id=? AND folder=?";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(fly_db_conn(db), sql, -1, &stmt, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int(stmt, 1, owner_id);
    sqlite3_bind_text(stmt, 2, folder ? folder : "INBOX", -1, SQLITE_TRANSIENT);
    int n = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) n = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return n;
}

int db_email_unread_count(cwist_db *db, int owner_id) {
    const char *sql = "SELECT COUNT(*) FROM emails WHERE owner_id=? AND folder='INBOX' AND is_read=0";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(fly_db_conn(db), sql, -1, &stmt, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int(stmt, 1, owner_id);
    int n = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) n = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return n;
}

cJSON *db_email_get(cwist_db *db, int owner_id, int id) {
    const char *sql = "SELECT * FROM emails WHERE owner_id=? AND id=? LIMIT 1";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(fly_db_conn(db), sql, -1, &stmt, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_int(stmt, 1, owner_id);
    sqlite3_bind_int(stmt, 2, id);
    return db_sqlite3_row_to_json(stmt);
}

bool db_email_set_read(cwist_db *db, int owner_id, int id, bool is_read) {
    const char *sql = "UPDATE emails SET is_read=? WHERE owner_id=? AND id=?";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(fly_db_conn(db), sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_int(stmt, 1, is_read ? 1 : 0);
    sqlite3_bind_int(stmt, 2, owner_id);
    sqlite3_bind_int(stmt, 3, id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc == SQLITE_DONE) db_sync_journal_row(db, "emails", "email", id);
    return rc == SQLITE_DONE;
}

bool db_email_set_folder(cwist_db *db, int owner_id, int id, const char *folder) {
    const char *sql = "UPDATE emails SET folder=? WHERE owner_id=? AND id=?";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(fly_db_conn(db), sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, folder ? folder : "INBOX", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, owner_id);
    sqlite3_bind_int(stmt, 3, id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc == SQLITE_DONE) db_sync_journal_row(db, "emails", "email", id);
    return rc == SQLITE_DONE;
}

bool db_email_delete(cwist_db *db, int owner_id, int id) {
    const char *sql = "DELETE FROM emails WHERE owner_id=? AND id=?";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(fly_db_conn(db), sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_int(stmt, 1, owner_id);
    sqlite3_bind_int(stmt, 2, id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc == SQLITE_DONE) db_sync_journal(db, "email", id, "delete", NULL);
    return rc == SQLITE_DONE;
}

int db_email_empty_trash(cwist_db *db, int owner_id) {
    /* Collect the ids first so each deleted row can be journaled (the
     * replica applies deletes idempotently). */
    char ids[4096] = {0};
    int nids = 0;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(fly_db_conn(db),
            "SELECT id FROM emails WHERE owner_id=? AND folder='Trash'", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int(st, 1, owner_id);
        while (sqlite3_step(st) == SQLITE_ROW && nids < 500) {
            int id = sqlite3_column_int(st, 0);
            int off = strlen(ids);
            snprintf(ids + off, sizeof(ids) - (size_t)off, "%s%d", nids ? "," : "", id);
            nids++;
        }
    }
    sqlite3_finalize(st);

    const char *sql = "DELETE FROM emails WHERE owner_id=? AND folder='Trash'";
    sqlite3_stmt *stmt = NULL;
    sqlite3 *conn = fly_db_conn(db);
    if (sqlite3_prepare_v2(conn, sql, -1, &stmt, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int(stmt, 1, owner_id);
    int rc = sqlite3_step(stmt);
    int n = rc == SQLITE_DONE ? sqlite3_changes(conn) : 0;
    sqlite3_finalize(stmt);
    if (n > 0) {
        char *save = NULL;
        for (char *tok = strtok_r(ids, ",", &save); tok; tok = strtok_r(NULL, ",", &save))
            db_sync_journal(db, "email", atoi(tok), "delete", NULL);
    }
    return n;
}

/* Username prefix search for the webmail To-field autocomplete. */
cJSON *db_user_search_prefix(cwist_db *db, const char *prefix, int limit) {
    const char *sql =
        "SELECT username FROM users"
        " WHERE username LIKE ? ESCAPE '\\' AND active=1"
        " ORDER BY username LIMIT ?";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(fly_db_conn(db), sql, -1, &stmt, NULL) != SQLITE_OK) return NULL;
    /* Escape the two LIKE wildcards in the prefix, then append '%'. */
    char pat[128];
    size_t o = 0;
    for (const char *p = prefix ? prefix : ""; *p && o + 2 < sizeof(pat) - 1; p++) {
        if (*p == '%' || *p == '_' || *p == '\\') pat[o++] = '\\';
        pat[o++] = *p;
    }
    pat[o] = '\0';
    strncat(pat, "%", sizeof(pat) - strlen(pat) - 1);
    sqlite3_bind_text(stmt, 1, pat, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, limit > 0 ? limit : 10);
    return db_sqlite3_rows_to_json(stmt);
}

/* id/username/email of every verified account, for the admin broadcast. */
cJSON *db_user_list_verified(cwist_db *db) {
    const char *sql =
        "SELECT id, username, email FROM users"
        " WHERE email_verified=1 AND active=1 AND email IS NOT NULL AND email <> ''"
        " ORDER BY id";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(fly_db_conn(db), sql, -1, &stmt, NULL) != SQLITE_OK) return NULL;
    return db_sqlite3_rows_to_json(stmt);
}

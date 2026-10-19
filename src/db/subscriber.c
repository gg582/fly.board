#define _POSIX_C_SOURCE 200809L
#include "subscriber.h"
#include "db_internal.h"
#include <string.h>

/* See subscriber.h for the lifecycle this implements. */

bool db_subscriber_add(cwist_db *db, const char *email, const char *token) {
    if (!email || !email[0] || !token || !token[0]) return false;
    sqlite3 *conn = fly_db_conn(db);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(conn,
            "INSERT INTO subscribers (email, token, status) VALUES (?, ?, 'pending')", -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_text(st, 1, email, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, token, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc == SQLITE_DONE) return true;
    /* Already subscribed (or previously unsubscribed): reset to pending with
     * the new token so the confirm mail can re-activate the row. */
    if (sqlite3_prepare_v2(conn,
            "UPDATE subscribers SET token=?, status='pending', created_at=CURRENT_TIMESTAMP, confirmed_at=NULL"
            " WHERE email=?", -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_text(st, 1, token, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, email, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(st);
    bool ok = rc == SQLITE_DONE && sqlite3_changes(conn) > 0;
    sqlite3_finalize(st);
    return ok;
}

bool db_subscriber_confirm(cwist_db *db, const char *token) {
    if (!token || !token[0]) return false;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(fly_db_conn(db),
            "UPDATE subscribers SET status='active', confirmed_at=CURRENT_TIMESTAMP"
            " WHERE token=? AND status='pending'", -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_text(st, 1, token, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(st);
    bool ok = rc == SQLITE_DONE && sqlite3_changes(fly_db_conn(db)) > 0;
    sqlite3_finalize(st);
    return ok;
}

bool db_subscriber_unsubscribe(cwist_db *db, const char *token) {
    if (!token || !token[0]) return false;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(fly_db_conn(db),
            "DELETE FROM subscribers WHERE token=?", -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_text(st, 1, token, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(st);
    bool ok = rc == SQLITE_DONE && sqlite3_changes(fly_db_conn(db)) > 0;
    sqlite3_finalize(st);
    return ok;
}

cJSON *db_subscriber_list(cwist_db *db, int limit, int offset) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(fly_db_conn(db),
            "SELECT id, email, token, status, created_at, confirmed_at FROM subscribers"
            " ORDER BY id DESC LIMIT ? OFFSET ?", -1, &st, NULL) != SQLITE_OK)
        return NULL;
    sqlite3_bind_int(st, 1, limit);
    sqlite3_bind_int(st, 2, offset);
    return db_sqlite3_rows_to_json(st);
}

cJSON *db_subscriber_list_active(cwist_db *db) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(fly_db_conn(db),
            "SELECT email, token FROM subscribers WHERE status='active' ORDER BY id", -1, &st, NULL) != SQLITE_OK)
        return NULL;
    return db_sqlite3_rows_to_json(st);
}

int db_subscriber_count(cwist_db *db) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(fly_db_conn(db),
            "SELECT COUNT(*) FROM subscribers WHERE status='active'", -1, &st, NULL) != SQLITE_OK)
        return 0;
    int count = 0;
    if (sqlite3_step(st) == SQLITE_ROW) count = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return count;
}

bool db_subscriber_delete(cwist_db *db, int id) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(fly_db_conn(db),
            "DELETE FROM subscribers WHERE id=?", -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_int(st, 1, id);
    int rc = sqlite3_step(st);
    bool ok = rc == SQLITE_DONE && sqlite3_changes(fly_db_conn(db)) > 0;
    sqlite3_finalize(st);
    return ok;
}

void db_subscriber_purge_pending(cwist_db *db) {
    db_exec_sql(db,
        "DELETE FROM subscribers WHERE status='pending'"
        " AND created_at < datetime('now', '-1 day')");
}

#define _POSIX_C_SOURCE 200809L
#include "db.h"
#include "db_internal.h"
#include <cwist/core/log.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* FlyWire change journal (see src/engine/flywire.c).
 *
 * Every content mutation on the primary appends one row to sync_journal in
 * data/blog.db. A replica polls /flywire/feed and applies the rows
 * idempotently. The journal INSERT must never fail or disturb the main
 * mutation: on error we log and let the request proceed.
 *
 * Rows older than 7 days are purged at startup and after each archive write;
 * a replica that falls behind past the purge window must be reseeded from a
 * signed archive (fly_board --restore), which also restores the checkpoint
 * marker data/.flywire_seq.
 *
 * NOTE: view_count and download_count counter increments are deliberately
 * not journaled; counters are not synced between sites. */

/* Journal insert on an existing connection (usually the one the mutation
 * itself ran on, so both land in one implicit transaction). Never fails the
 * caller: errors are logged and swallowed. */
bool db_sync_journal_conn(sqlite3 *conn, const char *entity, int entity_id, const char *op, const char *payload) {
    if (!conn || !entity || !op) return false;
    const char *sql = "INSERT INTO sync_journal (entity, entity_id, op, payload) VALUES (?,?,?,?)";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(conn, sql, -1, &stmt, NULL) != SQLITE_OK) {
        CWIST_LOG_ERROR("sync_journal prepare failed: %s", sqlite3_errmsg(conn));
        return false;
    }
    sqlite3_bind_text(stmt, 1, entity, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, entity_id);
    sqlite3_bind_text(stmt, 3, op, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, payload ? payload : "", -1, SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        CWIST_LOG_ERROR("sync_journal insert failed (%s %d %s): %s", entity, entity_id, op,
                        sqlite3_errmsg(conn));
        return false;
    }
    return true;
}

bool db_sync_journal(cwist_db *db, const char *entity, int entity_id, const char *op, const char *payload) {
    if (!db) return false;
    return db_sync_journal_conn(fly_db_conn(db), entity, entity_id, op, payload);
}

/* Convenience for upserts: re-read the row that was just written and journal
 * a full-row JSON snapshot (column names 1:1), so replica apply is a plain
 * INSERT OR REPLACE keyed by the row id. */
bool db_sync_journal_row(cwist_db *db, const char *table, const char *entity, int id) {
    if (!db || !table || !entity || id <= 0) return false;
    char sql[160];
    snprintf(sql, sizeof(sql), "SELECT * FROM %s WHERE id=? LIMIT 1", table);
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(fly_db_conn(db), sql, -1, &stmt, NULL) != SQLITE_OK) {
        CWIST_LOG_ERROR("sync_journal snapshot prepare failed on %s: %s", table,
                        sqlite3_errmsg(fly_db_conn(db)));
        return false;
    }
    sqlite3_bind_int(stmt, 1, id);
    cJSON *row = db_sqlite3_row_to_json(stmt);
    if (!row) return false;
    char *payload = cJSON_PrintUnformatted(row);
    cJSON_Delete(row);
    bool ok = false;
    if (payload) {
        ok = db_sync_journal(db, entity, id, "upsert", payload);
        free(payload);
    }
    return ok;
}

/* Journal write for mutations that run on a database without a cwist_db
 * handle at hand (board_tree.db). Opens a short-lived connection to the main
 * database, inserts, closes. Low-frequency path only. */
bool db_sync_journal_external(const char *entity, int entity_id, const char *op, const char *payload) {
    sqlite3 *conn = NULL;
    if (sqlite3_open(FLY_DB_MAIN_PATH, &conn) != SQLITE_OK) {
        sqlite3_close(conn);
        CWIST_LOG_ERROR("sync_journal: cannot open %s", FLY_DB_MAIN_PATH);
        return false;
    }
    sqlite3_busy_timeout(conn, 5000);
    bool ok = db_sync_journal_conn(conn, entity, entity_id, op, payload);
    sqlite3_close(conn);
    return ok;
}

/* Journal a full-row snapshot of a row that lives in an auxiliary database
 * (comments, board_tree): read the row on @p row_conn, write the journal
 * entry to the main database. */
bool db_sync_journal_external_row(sqlite3 *row_conn, const char *table, const char *key_col,
                                  const char *entity, int id) {
    if (!row_conn || !table || !key_col || !entity || id <= 0) return false;
    char sql[192];
    snprintf(sql, sizeof(sql), "SELECT * FROM %s WHERE %s=? LIMIT 1", table, key_col);
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(row_conn, sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_int(stmt, 1, id);
    cJSON *row = db_sqlite3_row_to_json(stmt);
    if (!row) return false;
    char *payload = cJSON_PrintUnformatted(row);
    cJSON_Delete(row);
    bool ok = false;
    if (payload) {
        ok = db_sync_journal_external(entity, id, "upsert", payload);
        free(payload);
    }
    return ok;
}

/* Time-based retention: drop rows older than 7 days. The replica polls every
 * couple of seconds, so anything older than a week means the operator must
 * reseed anyway. */
void db_sync_journal_purge(cwist_db *db) {
    if (!db) return;
    const char *sql = "DELETE FROM sync_journal WHERE created_at < datetime('now', '-7 days')";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(fly_db_conn(db), sql, -1, &stmt, NULL) != SQLITE_OK) return;
    if (sqlite3_step(stmt) == SQLITE_DONE) {
        int n = sqlite3_changes(fly_db_conn(db));
        if (n > 0) CWIST_LOG_INFO("sync_journal: purged %d rows older than 7 days", n);
    }
    sqlite3_finalize(stmt);
}

long long db_sync_journal_max_seq(cwist_db *db) {
    if (!db) return 0;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(fly_db_conn(db), "SELECT COALESCE(MAX(seq),0) FROM sync_journal", -1, &stmt, NULL) != SQLITE_OK) return 0;
    long long v = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) v = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return v;
}

long long db_sync_journal_min_seq(cwist_db *db) {
    if (!db) return 0;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(fly_db_conn(db), "SELECT COALESCE(MIN(seq),0) FROM sync_journal", -1, &stmt, NULL) != SQLITE_OK) return 0;
    long long v = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) v = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return v;
}

long long db_sync_journal_count(cwist_db *db) {
    if (!db) return 0;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(fly_db_conn(db), "SELECT COUNT(*) FROM sync_journal", -1, &stmt, NULL) != SQLITE_OK) return 0;
    long long v = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) v = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return v;
}

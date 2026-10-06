#define _POSIX_C_SOURCE 200809L
#include "db.h"
#include "db_internal.h"
#include <stdio.h>
#include <string.h>

/* Content reports (posts and comments). A report keeps the reporter's
 * account id when they were logged in, so one account reports a target
 * once; anonymous reports keep nothing that identifies the reporter. */

bool db_report_migrate(cwist_db *db) {
    db_exec_sql(db, "CREATE TABLE IF NOT EXISTS reports ("
                    "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                    "target_type TEXT NOT NULL,"   /* 'post' | 'comment' */
                    "target_id INTEGER NOT NULL,"
                    "post_id INTEGER NOT NULL,"    /* the post the target lives on */
                    "reason TEXT NOT NULL,"
                    "detail TEXT NOT NULL DEFAULT '',"
                    "reporter_user_id INTEGER,"
                    "status TEXT NOT NULL DEFAULT 'open'," /* open | resolved | dismissed */
                    "resolution TEXT NOT NULL DEFAULT '',"
                    "resolved_by INTEGER,"
                    "resolved_at DATETIME,"
                    "created_at DATETIME DEFAULT CURRENT_TIMESTAMP)");
    db_exec_sql(db, "CREATE INDEX IF NOT EXISTS idx_reports_status ON reports(status, created_at DESC)");
    db_exec_sql(db, "CREATE INDEX IF NOT EXISTS idx_reports_target ON reports(target_type, target_id)");
    db_exec_sql(db, "CREATE UNIQUE INDEX IF NOT EXISTS idx_reports_once_per_account ON reports(target_type, target_id, reporter_user_id) WHERE reporter_user_id IS NOT NULL AND status='open'");
    return true;
}

int db_report_create(cwist_db *db, const char *target_type, int target_id, int post_id,
                     const char *reason, const char *detail, int reporter_user_id) {
    const char *sql = "INSERT OR IGNORE INTO reports (target_type, target_id, post_id, reason, detail, reporter_user_id) VALUES (?,?,?,?,?,?)";
    sqlite3_stmt *stmt = NULL;
    sqlite3 *conn = fly_db_conn(db);
    if (sqlite3_prepare_v2(conn, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, target_type, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, target_id);
    sqlite3_bind_int(stmt, 3, post_id);
    sqlite3_bind_text(stmt, 4, reason, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, detail ? detail : "", -1, SQLITE_TRANSIENT);
    if (reporter_user_id > 0) sqlite3_bind_int(stmt, 6, reporter_user_id);
    else sqlite3_bind_null(stmt, 6);
    int rc = sqlite3_step(stmt);
    /* RETURNING-free: an ignored duplicate leaves changes() at 0. */
    int changed = rc == SQLITE_DONE ? sqlite3_changes(conn) : -1;
    sqlite3_finalize(stmt);
    return changed;
}

cJSON *db_report_list(cwist_db *db, const char *status) {
    const char *sql_all =
        "SELECT r.*, u.username AS reporter_name, a.username AS resolver_name FROM reports r"
        " LEFT JOIN users u ON u.id=r.reporter_user_id LEFT JOIN users a ON a.id=r.resolved_by"
        " ORDER BY r.created_at DESC, r.id DESC LIMIT 500";
    const char *sql_status =
        "SELECT r.*, u.username AS reporter_name, a.username AS resolver_name FROM reports r"
        " LEFT JOIN users u ON u.id=r.reporter_user_id LEFT JOIN users a ON a.id=r.resolved_by"
        " WHERE r.status=? ORDER BY r.created_at DESC, r.id DESC LIMIT 500";
    bool all = !status || !status[0] || strcmp(status, "all") == 0;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(fly_db_conn(db), all ? sql_all : sql_status, -1, &stmt, NULL) != SQLITE_OK) return NULL;
    if (!all) sqlite3_bind_text(stmt, 1, status, -1, SQLITE_TRANSIENT);
    return db_sqlite3_rows_to_json(stmt);
}

int db_report_count_open(cwist_db *db) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(fly_db_conn(db), "SELECT COUNT(*) FROM reports WHERE status='open'", -1, &stmt, NULL) != SQLITE_OK) return 0;
    int n = sqlite3_step(stmt) == SQLITE_ROW ? sqlite3_column_int(stmt, 0) : 0;
    sqlite3_finalize(stmt);
    return n;
}

int db_report_close_target(cwist_db *db, const char *target_type, int target_id, const char *status,
                           const char *resolution, int resolved_by) {
    const char *sql = "UPDATE reports SET status=?, resolution=?, resolved_by=?, resolved_at=CURRENT_TIMESTAMP"
                      " WHERE target_type=? AND target_id=? AND status='open'";
    sqlite3_stmt *stmt = NULL;
    sqlite3 *conn = fly_db_conn(db);
    if (sqlite3_prepare_v2(conn, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, status, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, resolution ? resolution : "", -1, SQLITE_TRANSIENT);
    if (resolved_by > 0) sqlite3_bind_int(stmt, 3, resolved_by);
    else sqlite3_bind_null(stmt, 3);
    sqlite3_bind_text(stmt, 4, target_type, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 5, target_id);
    int rc = sqlite3_step(stmt);
    int changed = rc == SQLITE_DONE ? sqlite3_changes(conn) : -1;
    sqlite3_finalize(stmt);
    return changed;
}

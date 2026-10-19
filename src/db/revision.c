#define _POSIX_C_SOURCE 200809L
#include "revision.h"
#include "db_internal.h"

/* See revision.h for the lifecycle this implements. */

bool db_revision_save(cwist_db *db, int post_id, const char *title,
                      const char *content, const char *summary, int editor_id) {
    if (post_id <= 0) return false;
    sqlite3 *conn = fly_db_conn(db);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(conn,
            "INSERT INTO post_revisions (post_id, title, content, summary, editor_id)"
            " VALUES (?, ?, ?, ?, ?)", -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_int(st, 1, post_id);
    sqlite3_bind_text(st, 2, title ? title : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, content ? content : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, summary ? summary : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 5, editor_id);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) return false;
    /* Bound the history so long-lived posts don't accumulate unbounded
     * rows; the subquery keeps the newest DB_REVISION_KEEP by id. */
    if (sqlite3_prepare_v2(conn,
            "DELETE FROM post_revisions WHERE post_id=?1 AND id NOT IN"
            " (SELECT id FROM post_revisions WHERE post_id=?1 ORDER BY id DESC LIMIT 30)",
            -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int(st, 1, post_id);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
    return true;
}

cJSON *db_revision_list(cwist_db *db, int post_id, int limit) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(fly_db_conn(db),
            "SELECT r.id, r.title, r.created_at, COALESCE(u.username, '') AS editor_name"
            " FROM post_revisions r LEFT JOIN users u ON u.id = r.editor_id"
            " WHERE r.post_id=? ORDER BY r.id DESC LIMIT ?", -1, &st, NULL) != SQLITE_OK)
        return NULL;
    sqlite3_bind_int(st, 1, post_id);
    sqlite3_bind_int(st, 2, limit);
    return db_sqlite3_rows_to_json(st);
}

cJSON *db_revision_get(cwist_db *db, int id) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(fly_db_conn(db),
            "SELECT id, post_id, title, content, summary, created_at"
            " FROM post_revisions WHERE id=?", -1, &st, NULL) != SQLITE_OK)
        return NULL;
    sqlite3_bind_int(st, 1, id);
    return db_sqlite3_row_to_json(st);
}

int db_revision_count(cwist_db *db, int post_id) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(fly_db_conn(db),
            "SELECT COUNT(*) FROM post_revisions WHERE post_id=?", -1, &st, NULL) != SQLITE_OK)
        return 0;
    sqlite3_bind_int(st, 1, post_id);
    int count = 0;
    if (sqlite3_step(st) == SQLITE_ROW) count = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return count;
}

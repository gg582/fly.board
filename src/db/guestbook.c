#define _POSIX_C_SOURCE 200809L
#include "guestbook.h"
#include "db.h"
#include "db_internal.h"
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Per-user guestbook storage. The public db_guestbook_*() functions follow
 * the cwist_db * convention of the rest of this directory. Every mutation
 * journals a "guestbook" row so FlyWire replicas replay it like posts and
 * emails (full-row upsert snapshot / delete by id, see src/engine/flywire.c).
 * ------------------------------------------------------------------------- */

int db_guestbook_create(cwist_db *db, int owner_uid, int author_uid,
                        const char *author_name, const char *content) {
    if (!db || owner_uid <= 0 || !author_name || !author_name[0] || !content || !content[0]) return 0;
    const char *sql =
        "INSERT INTO guestbook (owner_uid, author_uid, author_name, content) VALUES (?,?,?,?)";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(fly_db_conn(db), sql, -1, &stmt, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int(stmt, 1, owner_uid);
    if (author_uid > 0) sqlite3_bind_int(stmt, 2, author_uid);
    else sqlite3_bind_null(stmt, 2);
    sqlite3_bind_text(stmt, 3, author_name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, content, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    int id = rc == SQLITE_DONE ? (int)sqlite3_last_insert_rowid(fly_db_conn(db)) : 0;
    sqlite3_finalize(stmt);
    if (id > 0) db_sync_journal_row(db, "guestbook", "guestbook", id);
    return id;
}

cJSON *db_guestbook_list(cwist_db *db, int owner_uid, int offset, int limit) {
    if (!db || owner_uid <= 0) return NULL;
    const char *sql =
        "SELECT id, owner_uid, author_uid, author_name, content, created_at"
        " FROM guestbook WHERE owner_uid=?"
        " ORDER BY id DESC LIMIT ? OFFSET ?";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(fly_db_conn(db), sql, -1, &stmt, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_int(stmt, 1, owner_uid);
    sqlite3_bind_int(stmt, 2, limit > 0 ? limit : 50);
    sqlite3_bind_int(stmt, 3, offset > 0 ? offset : 0);
    return db_sqlite3_rows_to_json(stmt);
}

int db_guestbook_count(cwist_db *db, int owner_uid) {
    if (!db || owner_uid <= 0) return 0;
    const char *sql = "SELECT COUNT(*) FROM guestbook WHERE owner_uid=?";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(fly_db_conn(db), sql, -1, &stmt, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int(stmt, 1, owner_uid);
    int n = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) n = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return n;
}

cJSON *db_guestbook_get(cwist_db *db, int id) {
    if (!db || id <= 0) return NULL;
    const char *sql = "SELECT * FROM guestbook WHERE id=? LIMIT 1";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(fly_db_conn(db), sql, -1, &stmt, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_int(stmt, 1, id);
    return db_sqlite3_row_to_json(stmt);
}

bool db_guestbook_delete(cwist_db *db, int id, int actor_uid, const char *actor_role) {
    if (!db || id <= 0 || actor_uid <= 0) return false;
    cJSON *entry = db_guestbook_get(db, id);
    if (!entry) return false;
    int owner_uid = cJSON_GetObjectItem(entry, "owner_uid") ? cJSON_GetObjectItem(entry, "owner_uid")->valueint : 0;
    cJSON *au = cJSON_GetObjectItem(entry, "author_uid");
    int author_uid = (au && cJSON_IsNumber(au)) ? au->valueint : 0;
    cJSON_Delete(entry);

    bool allowed = actor_uid == owner_uid ||
                   (actor_role && strcmp(actor_role, "admin") == 0) ||
                   (author_uid > 0 && author_uid == actor_uid);
    if (!allowed) return false;

    const char *sql = "DELETE FROM guestbook WHERE id=?";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(fly_db_conn(db), sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_int(stmt, 1, id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc == SQLITE_DONE) db_sync_journal(db, "guestbook", id, "delete", NULL);
    return rc == SQLITE_DONE;
}

bool db_guestbook_set_anon(cwist_db *db, int uid, bool allow) {
    if (!db || uid <= 0) return false;
    const char *sql = "UPDATE users SET guestbook_anon=? WHERE id=?";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(fly_db_conn(db), sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_int(stmt, 1, allow ? 1 : 0);
    sqlite3_bind_int(stmt, 2, uid);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    /* The flag lives on the users row: journal a user snapshot so replicas
     * pick it up through the regular "user" entity apply. */
    if (rc == SQLITE_DONE) db_sync_journal_row(db, "users", "user", uid);
    return rc == SQLITE_DONE;
}

bool db_guestbook_anon_allowed(cwist_db *db, int owner_uid) {
    if (!db || owner_uid <= 0) return false;
    const char *sql = "SELECT guestbook_anon FROM users WHERE id=? LIMIT 1";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(fly_db_conn(db), sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_int(stmt, 1, owner_uid);
    bool allowed = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) allowed = sqlite3_column_int(stmt, 0) != 0;
    sqlite3_finalize(stmt);
    return allowed;
}

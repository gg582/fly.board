#define _POSIX_C_SOURCE 200809L
#include "db.h"
#include "db_internal.h"
#include <cwist/core/mem/alloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool db_board_create(cwist_db *db, const char *name, const char *slug, const char *description, bool admin_only, int read_perm, int write_perm, int comment_perm) {
    const char *sql = "INSERT INTO boards (name, slug, description, admin_only, read_perm, write_perm, comment_perm) VALUES (?,?,?,?,?,?,?)";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, slug, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, description ? description : "", -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 4, admin_only ? 1 : 0);
    sqlite3_bind_int(stmt, 5, read_perm);
    sqlite3_bind_int(stmt, 6, write_perm);
    sqlite3_bind_int(stmt, 7, comment_perm);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool db_board_delete(cwist_db *db, int id) {
    const char *sql = "DELETE FROM boards WHERE id=?";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_int(stmt, 1, id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool db_board_update(cwist_db *db, int id, const char *name, const char *slug, const char *description, bool admin_only, int read_perm, int write_perm, int comment_perm) {
    const char *sql = "UPDATE boards SET name=?, slug=?, description=?, admin_only=?, read_perm=?, write_perm=?, comment_perm=? WHERE id=?";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, slug, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, description ? description : "", -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 4, admin_only ? 1 : 0);
    sqlite3_bind_int(stmt, 5, read_perm);
    sqlite3_bind_int(stmt, 6, write_perm);
    sqlite3_bind_int(stmt, 7, comment_perm);
    sqlite3_bind_int(stmt, 8, id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

cJSON *db_board_list(cwist_db *db) {
    return cwist_orm_board_list_popular(db);
}

cJSON *db_board_get_by_slug(cwist_db *db, const char *slug) {
    const char *sql = "SELECT * FROM boards WHERE slug=? LIMIT 1";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_text(stmt, 1, slug, -1, SQLITE_STATIC);
    return db_sqlite3_row_to_json(stmt);
}

cJSON *db_board_get_by_id(cwist_db *db, int id) {
    const char *sql = "SELECT * FROM boards WHERE id=? LIMIT 1";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_int(stmt, 1, id);
    return db_sqlite3_row_to_json(stmt);
}

bool db_board_can_user_access(cwist_db *db, int board_id, int user_id, bool is_admin) {
    cJSON *b = db_board_get_by_id(db, board_id);
    if (!b) return false;
    cJSON *ao = cJSON_GetObjectItem(b, "admin_only");
    int admin_only = ao ? ao->valueint : 0;
    cJSON_Delete(b);
    if (!admin_only) return true;
    if (is_admin) return true;
    const char *sql = "SELECT 1 FROM board_permissions WHERE board_id=? AND user_id=? LIMIT 1";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_int(stmt, 1, board_id);
    sqlite3_bind_int(stmt, 2, user_id);
    bool ok = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) ok = true;
    sqlite3_finalize(stmt);
    return ok;
}

bool db_board_perm_grant(cwist_db *db, int board_id, int user_id) {
    const char *sql = "INSERT OR IGNORE INTO board_permissions (board_id, user_id) VALUES (?,?)";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_int(stmt, 1, board_id);
    sqlite3_bind_int(stmt, 2, user_id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool db_board_perm_revoke(cwist_db *db, int board_id, int user_id) {
    const char *sql = "DELETE FROM board_permissions WHERE board_id=? AND user_id=?";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_int(stmt, 1, board_id);
    sqlite3_bind_int(stmt, 2, user_id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

cJSON *db_board_perm_list(cwist_db *db, int board_id) {
    const char *sql = "SELECT u.id, u.username, u.email FROM board_permissions bp JOIN users u ON bp.user_id=u.id WHERE bp.board_id=?";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_int(stmt, 1, board_id);
    return db_sqlite3_rows_to_json(stmt);
}

int db_post_create_with_auto_slug(cwist_db *db, int board_id, int user_id, const char *title, const char *slug_base, const char *content, const char *summary, const char *pqc_signature, int is_notice, int is_secret, const char *category, char **out_slug) {
    if (out_slug) *out_slug = NULL;
    if (!db || !db->conn || !slug_base || !slug_base[0]) return 0;

    sqlite3_mutex *mutex = sqlite3_db_mutex(db->conn);
    sqlite3_mutex_enter(mutex);

    if (sqlite3_exec(db->conn, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK) {
        sqlite3_mutex_leave(mutex);
        return 0;
    }

    const char *next_sql =
        "WITH used(n) AS ("
        "  SELECT 0 FROM posts WHERE slug=?"
        "  UNION"
        "  SELECT CAST(substr(slug, ?) AS INTEGER) FROM posts"
        "  WHERE slug GLOB ?"
        "    AND substr(slug, ?) NOT GLOB '*[^0-9]*'"
        "    AND CAST(substr(slug, ?) AS INTEGER) > 0"
        "    AND substr(slug, ?) = printf('%d', CAST(substr(slug, ?) AS INTEGER))"
        "), candidate(n) AS ("
        "  SELECT 0"
        "  UNION"
        "  SELECT n+1 FROM used"
        ")"
        "SELECT MIN(n) FROM candidate WHERE n NOT IN (SELECT n FROM used)";
    sqlite3_stmt *next_stmt = NULL;
    int rc = sqlite3_prepare_v2(db->conn, next_sql, -1, &next_stmt, NULL);
    if (rc != SQLITE_OK) {
        sqlite3_exec(db->conn, "ROLLBACK", NULL, NULL, NULL);
        sqlite3_mutex_leave(mutex);
        return 0;
    }

    size_t base_len = strlen(slug_base);
    char *pattern = (char *)cwist_alloc(base_len + 7);
    if (!pattern) {
        sqlite3_finalize(next_stmt);
        sqlite3_exec(db->conn, "ROLLBACK", NULL, NULL, NULL);
        sqlite3_mutex_leave(mutex);
        return 0;
    }
    snprintf(pattern, base_len + 7, "%s[0-9]*", slug_base);
    sqlite3_bind_text(next_stmt, 1, slug_base, -1, SQLITE_STATIC);
    sqlite3_bind_int(next_stmt, 2, (int)base_len + 1);
    sqlite3_bind_text(next_stmt, 3, pattern, -1, SQLITE_STATIC);
    sqlite3_bind_int(next_stmt, 4, (int)base_len + 1);
    sqlite3_bind_int(next_stmt, 5, (int)base_len + 1);
    sqlite3_bind_int(next_stmt, 6, (int)base_len + 1);
    sqlite3_bind_int(next_stmt, 7, (int)base_len + 1);

    int next_suffix = -1;
    if (sqlite3_step(next_stmt) == SQLITE_ROW) next_suffix = sqlite3_column_int(next_stmt, 0);
    sqlite3_finalize(next_stmt);
    cwist_free(pattern);
    if (next_suffix < 0) {
        sqlite3_exec(db->conn, "ROLLBACK", NULL, NULL, NULL);
        sqlite3_mutex_leave(mutex);
        return 0;
    }

    char suffix[32] = {0};
    if (next_suffix > 0) snprintf(suffix, sizeof(suffix), "%d", next_suffix);
    char *final_slug = (char *)cwist_alloc(base_len + strlen(suffix) + 1);
    if (!final_slug) {
        sqlite3_exec(db->conn, "ROLLBACK", NULL, NULL, NULL);
        sqlite3_mutex_leave(mutex);
        return 0;
    }
    snprintf(final_slug, base_len + strlen(suffix) + 1, "%s%s", slug_base, suffix);

    const char *insert_sql = "INSERT INTO posts (board_id, user_id, title, slug, content, summary, pqc_signature, is_notice, is_secret, category) VALUES (?,?,?,?,?,?,?,?,?,?)";
    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(db->conn, insert_sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        cwist_free(final_slug);
        sqlite3_exec(db->conn, "ROLLBACK", NULL, NULL, NULL);
        sqlite3_mutex_leave(mutex);
        return 0;
    }
    sqlite3_bind_int(stmt, 1, board_id);
    sqlite3_bind_int(stmt, 2, user_id);
    sqlite3_bind_text(stmt, 3, title, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, final_slug, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 5, content, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 6, summary ? summary : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 7, pqc_signature ? pqc_signature : "", -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 8, is_notice);
    sqlite3_bind_int(stmt, 9, is_secret);
    sqlite3_bind_text(stmt, 10, category ? category : "", -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);
    sqlite3_int64 id = rc == SQLITE_DONE ? sqlite3_last_insert_rowid(db->conn) : 0;
    sqlite3_finalize(stmt);

    if (rc == SQLITE_DONE && id > 0) {
        if (sqlite3_exec(db->conn, "COMMIT", NULL, NULL, NULL) == SQLITE_OK) {
            rc = SQLITE_DONE;
        } else {
            sqlite3_exec(db->conn, "ROLLBACK", NULL, NULL, NULL);
            rc = SQLITE_ERROR;
        }
    } else {
        sqlite3_exec(db->conn, "ROLLBACK", NULL, NULL, NULL);
        rc = SQLITE_ERROR;
    }
    sqlite3_mutex_leave(mutex);

    if (rc != SQLITE_DONE) {
        cwist_free(final_slug);
        return 0;
    }
    if (out_slug) *out_slug = final_slug;
    else cwist_free(final_slug);
    return (int)id;
}

#define _POSIX_C_SOURCE 200809L
#include "db.h"
#include <cwist/core/mem/alloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool db_board_create(cwist_db *db, const char *name, const char *slug, const char *description, bool admin_only, int read_perm, int write_perm, int comment_perm) {
    char sql[1024];
    snprintf(sql, sizeof(sql),
        "INSERT INTO boards (name, slug, description, admin_only, read_perm, write_perm, comment_perm) VALUES ('%s','%s','%s',%d,%d,%d,%d)",
        name, slug, description ? description : "", admin_only ? 1 : 0, read_perm, write_perm, comment_perm);
    return db_exec_sql(db, sql);
}

bool db_board_delete(cwist_db *db, int id) {
    char sql[256];
    snprintf(sql, sizeof(sql), "DELETE FROM boards WHERE id=%d", id);
    return db_exec_sql(db, sql);
}

bool db_board_update(cwist_db *db, int id, const char *name, const char *slug, const char *description, bool admin_only, int read_perm, int write_perm, int comment_perm) {
    char sql[1024];
    snprintf(sql, sizeof(sql),
        "UPDATE boards SET name='%s', slug='%s', description='%s', admin_only=%d, read_perm=%d, write_perm=%d, comment_perm=%d WHERE id=%d",
        name, slug, description ? description : "", admin_only ? 1 : 0, read_perm, write_perm, comment_perm, id);
    return db_exec_sql(db, sql);
}

cJSON *db_board_list(cwist_db *db) {
    const char *sql = "SELECT * FROM boards ORDER BY created_at DESC";
    cJSON *res = NULL;
    cwist_db_query(db, sql, &res);
    return res;
}

cJSON *db_board_get_by_slug(cwist_db *db, const char *slug) {
    char sql[512];
    snprintf(sql, sizeof(sql), "SELECT * FROM boards WHERE slug='%s' LIMIT 1", slug);
    cJSON *res = NULL;
    cwist_db_query(db, sql, &res);
    if (res && cJSON_GetArraySize(res) > 0) {
        cJSON *row = cJSON_GetArrayItem(res, 0);
        cJSON *cpy = cJSON_Duplicate(row, 1);
        cJSON_Delete(res);
        return cpy;
    }
    if (res) cJSON_Delete(res);
    return NULL;
}

cJSON *db_board_get_by_id(cwist_db *db, int id) {
    char sql[256];
    snprintf(sql, sizeof(sql), "SELECT * FROM boards WHERE id=%d LIMIT 1", id);
    cJSON *res = NULL;
    cwist_db_query(db, sql, &res);
    if (res && cJSON_GetArraySize(res) > 0) {
        cJSON *row = cJSON_GetArrayItem(res, 0);
        cJSON *cpy = cJSON_Duplicate(row, 1);
        cJSON_Delete(res);
        return cpy;
    }
    if (res) cJSON_Delete(res);
    return NULL;
}

bool db_board_can_user_access(cwist_db *db, int board_id, int user_id, bool is_admin) {
    cJSON *b = db_board_get_by_id(db, board_id);
    if (!b) return false;
    cJSON *ao = cJSON_GetObjectItem(b, "admin_only");
    int admin_only = ao ? ao->valueint : 0;
    cJSON_Delete(b);
    if (!admin_only) return true;
    if (is_admin) return true;
    char sql[512];
    snprintf(sql, sizeof(sql),
        "SELECT 1 FROM board_permissions WHERE board_id=%d AND user_id=%d LIMIT 1",
        board_id, user_id);
    cJSON *res = NULL;
    cwist_db_query(db, sql, &res);
    bool ok = false;
    if (res && cJSON_GetArraySize(res) > 0) ok = true;
    if (res) cJSON_Delete(res);
    return ok;
}

/* ---- Board permissions ---- */
bool db_board_perm_grant(cwist_db *db, int board_id, int user_id) {
    char sql[512];
    snprintf(sql, sizeof(sql),
        "INSERT OR IGNORE INTO board_permissions (board_id, user_id) VALUES (%d,%d)",
        board_id, user_id);
    return db_exec_sql(db, sql);
}

bool db_board_perm_revoke(cwist_db *db, int board_id, int user_id) {
    char sql[512];
    snprintf(sql, sizeof(sql),
        "DELETE FROM board_permissions WHERE board_id=%d AND user_id=%d",
        board_id, user_id);
    return db_exec_sql(db, sql);
}

cJSON *db_board_perm_list(cwist_db *db, int board_id) {
    char sql[512];
    snprintf(sql, sizeof(sql),
        "SELECT u.id, u.username, u.email FROM board_permissions bp JOIN users u ON bp.user_id=u.id WHERE bp.board_id=%d",
        board_id);
    cJSON *res = NULL;
    cwist_db_query(db, sql, &res);
    return res;
}

/* ---- Posts ---- */
int db_post_create(cwist_db *db, int board_id, int user_id, const char *title, const char *slug, const char *content, const char *summary, const char *pqc_signature, int is_notice, int is_secret, const char *category) {
    char sql[8192];
    snprintf(sql, sizeof(sql),
        "INSERT INTO posts (board_id, user_id, title, slug, content, summary, pqc_signature, is_notice, is_secret, category) VALUES (%d,%d,'%s','%s','%s','%s','%s',%d,%d,'%s')",
        board_id, user_id, title, slug, content, summary ? summary : "", pqc_signature ? pqc_signature : "", is_notice, is_secret, category ? category : "");
    if (!db_exec_sql(db, sql)) return 0;
    return (int)sqlite3_last_insert_rowid(db->conn);
}

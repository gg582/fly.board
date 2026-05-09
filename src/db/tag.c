#define _POSIX_C_SOURCE 200809L
#include "db.h"
#include <cwist/core/mem/alloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int db_tag_get_or_create(cwist_db *db, const char *name) {
    char sql[512];
    char esc[256] = {0};
    size_t j = 0;
    for (size_t i = 0; i < strlen(name) && j < sizeof(esc)-2; i++) {
        if (name[i] == '\'') esc[j++] = '\'';
        esc[j++] = name[i];
    }
    esc[j] = '\0';
    snprintf(sql, sizeof(sql), "SELECT id FROM tags WHERE name='%s' LIMIT 1", esc);
    sqlite3_stmt *stmt = NULL;
    int tag_id = 0;
    if (sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            tag_id = sqlite3_column_int(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }
    if (tag_id > 0) return tag_id;
    snprintf(sql, sizeof(sql), "INSERT INTO tags (name) VALUES ('%s')", esc);
    if (db_exec_sql(db, sql)) {
        tag_id = (int)sqlite3_last_insert_rowid(db->conn);
    }
    return tag_id;
}

bool db_tag_link(cwist_db *db, int post_id, int tag_id) {
    char sql[512];
    snprintf(sql, sizeof(sql), "INSERT OR IGNORE INTO post_tags (post_id, tag_id) VALUES (%d,%d)", post_id, tag_id);
    return db_exec_sql(db, sql);
}

cJSON *db_tag_list_by_post(cwist_db *db, int post_id) {
    char sql[512];
    snprintf(sql, sizeof(sql), "SELECT t.name FROM tags t JOIN post_tags pt ON t.id=pt.tag_id WHERE pt.post_id=%d ORDER BY t.name", post_id);
    cJSON *res = NULL;
    cwist_db_query(db, sql, &res);
    return res;
}

bool db_tag_clear_by_post(cwist_db *db, int post_id) {
    char sql[256];
    snprintf(sql, sizeof(sql), "DELETE FROM post_tags WHERE post_id=%d", post_id);
    return db_exec_sql(db, sql);
}

/* ---- File downloads ---- */

#define _POSIX_C_SOURCE 200809L
#include "db.h"
#include <cwist/core/mem/alloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool db_post_update(cwist_db *db, int id, const char *title, const char *content, const char *summary, const char *pqc_signature, int is_notice, int is_secret, const char *category) {
    char sql[8192];
    snprintf(sql, sizeof(sql),
        "UPDATE posts SET title='%s', content='%s', summary='%s', pqc_signature='%s', is_notice=%d, is_secret=%d, category='%s', updated_at=CURRENT_TIMESTAMP WHERE id=%d",
        title, content, summary ? summary : "", pqc_signature ? pqc_signature : "", is_notice, is_secret, category ? category : "", id);
    return db_exec_sql(db, sql);
}

bool db_post_delete(cwist_db *db, int id) {
    char sql[256];
    snprintf(sql, sizeof(sql), "DELETE FROM posts WHERE id=%d", id);
    return db_exec_sql(db, sql);
}

cJSON *db_post_get_by_slug(cwist_db *db, const char *slug) {
    char sql[512];
    snprintf(sql, sizeof(sql),
        "SELECT p.*, u.username as author_name FROM posts p LEFT JOIN users u ON p.user_id=u.id WHERE p.slug='%s' LIMIT 1",
        slug);
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

cJSON *db_post_get_by_id(cwist_db *db, int id) {
    char sql[256];
    snprintf(sql, sizeof(sql),
        "SELECT p.*, u.username as author_name FROM posts p LEFT JOIN users u ON p.user_id=u.id WHERE p.id=%d LIMIT 1",
        id);
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

cJSON *db_post_list(cwist_db *db, int board_id, int limit, int offset) {
    char sql[1024];
    if (board_id > 0) {
        snprintf(sql, sizeof(sql),
            "SELECT p.*, u.username as author_name, b.name as board_name FROM posts p LEFT JOIN users u ON p.user_id=u.id LEFT JOIN boards b ON p.board_id=b.id WHERE p.board_id=%d ORDER BY p.created_at DESC LIMIT %d OFFSET %d",
            board_id, limit, offset);
    } else {
        snprintf(sql, sizeof(sql),
            "SELECT p.*, u.username as author_name, b.name as board_name FROM posts p LEFT JOIN users u ON p.user_id=u.id LEFT JOIN boards b ON p.board_id=b.id ORDER BY p.created_at DESC LIMIT %d OFFSET %d",
            limit, offset);
    }
    cJSON *res = NULL;
    cwist_db_query(db, sql, &res);
    return res;
}

cJSON *db_post_recent(cwist_db *db, int limit) {
    return db_post_list(db, 0, limit, 0);
}

cJSON *db_post_recent_by_board(cwist_db *db, int board_id, int limit) {
    char sql[1024];
    snprintf(sql, sizeof(sql),
        "SELECT p.*, u.username as author_name, b.name as board_name FROM posts p LEFT JOIN users u ON p.user_id=u.id LEFT JOIN boards b ON p.board_id=b.id WHERE p.board_id=%d ORDER BY p.created_at DESC LIMIT %d",
        board_id, limit);
    cJSON *res = NULL;
    cwist_db_query(db, sql, &res);
    return res;
}

int db_post_count(cwist_db *db, int board_id) {
    char sql[512];
    if (board_id > 0) {
        snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM posts WHERE board_id=%d", board_id);
    } else {
        snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM posts");
    }
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL) != SQLITE_OK) return 0;
    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return count;
}

bool db_post_increment_view(cwist_db *db, int id) {
    char sql[256];
    snprintf(sql, sizeof(sql), "UPDATE posts SET view_count = view_count + 1 WHERE id=%d", id);
    return db_exec_sql(db, sql);
}

/* ---- Post list with search & notice ordering ---- */
cJSON *db_post_list_search(cwist_db *db, int board_id, const char *search, int limit, int offset) {
    char sql[2048];
    char esc[512] = {0};
    if (search && search[0]) {
        size_t j = 0;
        for (size_t i = 0; i < strlen(search) && j < sizeof(esc)-2; i++) {
            if (search[i] == '\'') esc[j++] = '\'';
            esc[j++] = search[i];
        }
        esc[j] = '\0';
    }
    if (board_id > 0) {
        if (search && search[0]) {
            snprintf(sql, sizeof(sql),
                "SELECT p.*, u.username as author_name, b.name as board_name FROM posts p LEFT JOIN users u ON p.user_id=u.id LEFT JOIN boards b ON p.board_id=b.id WHERE p.board_id=%d AND (p.title LIKE '%%%s%%' OR p.content LIKE '%%%s%%') ORDER BY p.is_notice DESC, p.created_at DESC LIMIT %d OFFSET %d",
                board_id, esc, esc, limit, offset);
        } else {
            snprintf(sql, sizeof(sql),
                "SELECT p.*, u.username as author_name, b.name as board_name FROM posts p LEFT JOIN users u ON p.user_id=u.id LEFT JOIN boards b ON p.board_id=b.id WHERE p.board_id=%d ORDER BY p.is_notice DESC, p.created_at DESC LIMIT %d OFFSET %d",
                board_id, limit, offset);
        }
    } else {
        if (search && search[0]) {
            snprintf(sql, sizeof(sql),
                "SELECT p.*, u.username as author_name, b.name as board_name FROM posts p LEFT JOIN users u ON p.user_id=u.id LEFT JOIN boards b ON p.board_id=b.id WHERE p.title LIKE '%%%s%%' OR p.content LIKE '%%%s%%' ORDER BY p.is_notice DESC, p.created_at DESC LIMIT %d OFFSET %d",
                esc, esc, limit, offset);
        } else {
            snprintf(sql, sizeof(sql),
                "SELECT p.*, u.username as author_name, b.name as board_name FROM posts p LEFT JOIN users u ON p.user_id=u.id LEFT JOIN boards b ON p.board_id=b.id ORDER BY p.is_notice DESC, p.created_at DESC LIMIT %d OFFSET %d",
                limit, offset);
        }
    }
    cJSON *res = NULL;
    cwist_db_query(db, sql, &res);
    return res;
}

int db_post_count_search(cwist_db *db, int board_id, const char *search) {
    char sql[1024];
    char esc[512] = {0};
    if (search && search[0]) {
        size_t j = 0;
        for (size_t i = 0; i < strlen(search) && j < sizeof(esc)-2; i++) {
            if (search[i] == '\'') esc[j++] = '\'';
            esc[j++] = search[i];
        }
        esc[j] = '\0';
    }
    if (board_id > 0) {
        if (search && search[0]) {
            snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM posts WHERE board_id=%d AND (title LIKE '%%%s%%' OR content LIKE '%%%s%%')", board_id, esc, esc);
        } else {
            snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM posts WHERE board_id=%d", board_id);
        }
    } else {
        if (search && search[0]) {
            snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM posts WHERE title LIKE '%%%s%%' OR content LIKE '%%%s%%'", esc, esc);
        } else {
            snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM posts");
        }
    }
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL) != SQLITE_OK) return 0;
    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return count;
}


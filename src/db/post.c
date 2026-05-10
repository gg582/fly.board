#define _POSIX_C_SOURCE 200809L
#include "db.h"
#include "db_internal.h"
#include <cwist/core/mem/alloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool db_post_update(cwist_db *db, int id, int board_id, const char *title, const char *content, const char *summary, const char *pqc_signature, int is_notice, int is_secret, const char *category) {
    const char *sql = "UPDATE posts SET board_id=?, title=?, content=?, summary=?, pqc_signature=?, is_notice=?, is_secret=?, category=?, updated_at=CURRENT_TIMESTAMP WHERE id=?";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_int(stmt, 1, board_id);
    sqlite3_bind_text(stmt, 2, title, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, content, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, summary ? summary : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 5, pqc_signature ? pqc_signature : "", -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 6, is_notice);
    sqlite3_bind_int(stmt, 7, is_secret);
    sqlite3_bind_text(stmt, 8, category ? category : "", -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 9, id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool db_post_delete(cwist_db *db, int id) {
    const char *sql = "DELETE FROM posts WHERE id=?";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_int(stmt, 1, id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

cJSON *db_post_get_by_slug(cwist_db *db, const char *slug) {
    const char *sql = "SELECT p.*, u.username as author_name FROM posts p LEFT JOIN users u ON p.user_id=u.id WHERE p.slug=? LIMIT 1";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_text(stmt, 1, slug, -1, SQLITE_STATIC);
    return db_sqlite3_row_to_json(stmt);
}

cJSON *db_post_get_by_id(cwist_db *db, int id) {
    const char *sql = "SELECT p.*, u.username as author_name FROM posts p LEFT JOIN users u ON p.user_id=u.id WHERE p.id=? LIMIT 1";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_int(stmt, 1, id);
    return db_sqlite3_row_to_json(stmt);
}

cJSON *db_post_list(cwist_db *db, int board_id, int limit, int offset) {
    const char *sql = (board_id > 0)
        ? "SELECT p.*, u.username as author_name, b.name as board_name FROM posts p LEFT JOIN users u ON p.user_id=u.id LEFT JOIN boards b ON p.board_id=b.id WHERE p.board_id=? ORDER BY p.created_at DESC LIMIT ? OFFSET ?"
        : "SELECT p.*, u.username as author_name, b.name as board_name FROM posts p LEFT JOIN users u ON p.user_id=u.id LEFT JOIN boards b ON p.board_id=b.id ORDER BY p.created_at DESC LIMIT ? OFFSET ?";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL) != SQLITE_OK) return NULL;
    int idx = 1;
    if (board_id > 0) sqlite3_bind_int(stmt, idx++, board_id);
    sqlite3_bind_int(stmt, idx++, limit);
    sqlite3_bind_int(stmt, idx++, offset);
    return db_sqlite3_rows_to_json(stmt);
}

cJSON *db_post_recent(cwist_db *db, int limit) {
    return db_post_list(db, 0, limit, 0);
}

cJSON *db_post_recent_by_board(cwist_db *db, int board_id, int limit) {
    const char *sql = "SELECT p.*, u.username as author_name, b.name as board_name FROM posts p LEFT JOIN users u ON p.user_id=u.id LEFT JOIN boards b ON p.board_id=b.id WHERE p.board_id=? ORDER BY p.created_at DESC LIMIT ?";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_int(stmt, 1, board_id);
    sqlite3_bind_int(stmt, 2, limit);
    return db_sqlite3_rows_to_json(stmt);
}

int db_post_count(cwist_db *db, int board_id) {
    const char *sql = (board_id > 0) ? "SELECT COUNT(*) FROM posts WHERE board_id=?" : "SELECT COUNT(*) FROM posts";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL) != SQLITE_OK) return 0;
    if (board_id > 0) sqlite3_bind_int(stmt, 1, board_id);
    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return count;
}

bool db_post_increment_view(cwist_db *db, int id) {
    const char *sql = "UPDATE posts SET view_count = view_count + 1 WHERE id=?";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_int(stmt, 1, id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

cJSON *db_post_list_search(cwist_db *db, int board_id, const char *search, int limit, int offset) {
    const char *sql;
    int has_board = board_id > 0;
    int has_search = (search && search[0]);
    if (has_board && has_search) {
        sql = "SELECT p.*, u.username as author_name, b.name as board_name FROM posts p LEFT JOIN users u ON p.user_id=u.id LEFT JOIN boards b ON p.board_id=b.id WHERE p.board_id=? AND (p.title LIKE ? OR p.content LIKE ?) ORDER BY p.is_notice DESC, p.created_at DESC LIMIT ? OFFSET ?";
    } else if (has_board) {
        sql = "SELECT p.*, u.username as author_name, b.name as board_name FROM posts p LEFT JOIN users u ON p.user_id=u.id LEFT JOIN boards b ON p.board_id=b.id WHERE p.board_id=? ORDER BY p.is_notice DESC, p.created_at DESC LIMIT ? OFFSET ?";
    } else if (has_search) {
        sql = "SELECT p.*, u.username as author_name, b.name as board_name FROM posts p LEFT JOIN users u ON p.user_id=u.id LEFT JOIN boards b ON p.board_id=b.id WHERE p.title LIKE ? OR p.content LIKE ? ORDER BY p.is_notice DESC, p.created_at DESC LIMIT ? OFFSET ?";
    } else {
        sql = "SELECT p.*, u.username as author_name, b.name as board_name FROM posts p LEFT JOIN users u ON p.user_id=u.id LEFT JOIN boards b ON p.board_id=b.id ORDER BY p.is_notice DESC, p.created_at DESC LIMIT ? OFFSET ?";
    }
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL) != SQLITE_OK) return NULL;
    int idx = 1;
    char search_pattern[512] = {0};
    if (has_search) {
        snprintf(search_pattern, sizeof(search_pattern), "%%%s%%", search);
    }
    if (has_board) sqlite3_bind_int(stmt, idx++, board_id);
    if (has_search) {
        sqlite3_bind_text(stmt, idx++, search_pattern, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, idx++, search_pattern, -1, SQLITE_STATIC);
    }
    sqlite3_bind_int(stmt, idx++, limit);
    sqlite3_bind_int(stmt, idx++, offset);
    return db_sqlite3_rows_to_json(stmt);
}

int db_post_count_search(cwist_db *db, int board_id, const char *search) {
    const char *sql;
    int has_board = board_id > 0;
    int has_search = (search && search[0]);
    if (has_board && has_search) {
        sql = "SELECT COUNT(*) FROM posts WHERE board_id=? AND (title LIKE ? OR content LIKE ?)";
    } else if (has_board) {
        sql = "SELECT COUNT(*) FROM posts WHERE board_id=?";
    } else if (has_search) {
        sql = "SELECT COUNT(*) FROM posts WHERE title LIKE ? OR content LIKE ?";
    } else {
        sql = "SELECT COUNT(*) FROM posts";
    }
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db->conn, sql, -1, &stmt, NULL) != SQLITE_OK) return 0;
    int idx = 1;
    char search_pattern[512] = {0};
    if (has_search) {
        snprintf(search_pattern, sizeof(search_pattern), "%%%s%%", search);
    }
    if (has_board) sqlite3_bind_int(stmt, idx++, board_id);
    if (has_search) {
        sqlite3_bind_text(stmt, idx++, search_pattern, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, idx++, search_pattern, -1, SQLITE_STATIC);
    }
    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return count;
}

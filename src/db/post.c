#define _POSIX_C_SOURCE 200809L
#include "db.h"
#include "db_internal.h"
#include <cwist/core/mem/alloc.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void post_utc_now(char out[POST_TIME_LEN]) {
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    strftime(out, POST_TIME_LEN, "%Y-%m-%d %H:%M:%S", &tm);
}

static const char *post_field(cJSON *post, const char *key) {
    cJSON *v = post ? cJSON_GetObjectItem(post, key) : NULL;
    return (v && cJSON_IsString(v) && v->valuestring) ? v->valuestring : "";
}

/* Rows from before the status column read as published. */
static bool post_is_draft(cJSON *post) {
    return strcmp(post_field(post, "status"), POST_STATUS_DRAFT) == 0;
}

/* Same-format UTC strings compare chronologically with strcmp. */
bool post_is_scheduled(cJSON *post) {
    if (!post || post_is_draft(post)) return false;
    char now[POST_TIME_LEN];
    post_utc_now(now);
    return strcmp(post_field(post, "created_at"), now) > 0;
}

bool post_is_public(cJSON *post) {
    return post && !post_is_draft(post) && !post_is_scheduled(post);
}

cJSON *db_post_list_unpublished(cwist_db *db, int user_id) {
    const char *sql = (user_id > 0)
        ? "SELECT p.*, u.username as author_name, b.name as board_name FROM posts p LEFT JOIN users u ON p.user_id=u.id LEFT JOIN boards b ON p.board_id=b.id WHERE p.user_id=? AND NOT (" POST_PUBLIC_SQL ") ORDER BY p.updated_at DESC"
        : "SELECT p.*, u.username as author_name, b.name as board_name FROM posts p LEFT JOIN users u ON p.user_id=u.id LEFT JOIN boards b ON p.board_id=b.id WHERE NOT (" POST_PUBLIC_SQL ") ORDER BY p.updated_at DESC";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(fly_db_conn(db), sql, -1, &stmt, NULL) != SQLITE_OK) return NULL;
    if (user_id > 0) sqlite3_bind_int(stmt, 1, user_id);
    return db_sqlite3_rows_to_json(stmt);
}

long long db_post_next_scheduled(cwist_db *db) {
    const char *sql = "SELECT CAST(strftime('%s', MIN(created_at)) AS INTEGER) FROM posts WHERE status='published' AND created_at>CURRENT_TIMESTAMP";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(fly_db_conn(db), sql, -1, &stmt, NULL) != SQLITE_OK) return 0;
    long long due = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_type(stmt, 0) != SQLITE_NULL) {
        due = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return due;
}

bool db_post_update(cwist_db *db, int id, int board_id, const char *title, const char *content, const char *summary, const char *pqc_signature, int is_notice, int is_secret, const char *category, const char *status, const char *publish_at) {
    const char *sql = "UPDATE posts SET board_id=?, title=?, content=?, summary=?, pqc_signature=?, is_notice=?, is_secret=?, category=?, status=COALESCE(?,status), created_at=COALESCE(?,created_at), updated_at=CURRENT_TIMESTAMP WHERE id=?";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(fly_db_conn(db), sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    /* board_id 0 means "no board"; store NULL so the foreign key to
     * boards(id) (enforced via PRAGMA foreign_keys=ON) is not violated. */
    if (board_id > 0) sqlite3_bind_int(stmt, 1, board_id);
    else sqlite3_bind_null(stmt, 1);
    sqlite3_bind_text(stmt, 2, title, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, content, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, summary ? summary : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 5, pqc_signature ? pqc_signature : "", -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 6, is_notice);
    sqlite3_bind_int(stmt, 7, is_secret);
    sqlite3_bind_text(stmt, 8, category ? category : "", -1, SQLITE_STATIC);
    if (status) sqlite3_bind_text(stmt, 9, status, -1, SQLITE_STATIC);
    else sqlite3_bind_null(stmt, 9);
    if (publish_at) sqlite3_bind_text(stmt, 10, publish_at, -1, SQLITE_STATIC);
    else sqlite3_bind_null(stmt, 10);
    sqlite3_bind_int(stmt, 11, id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return false;
    /* Hidden again (draft or rescheduled): announce once more when it goes
     * public. A post that stays public keeps its flag. */
    const char *reset_sql = "UPDATE posts SET announced=0 WHERE id=? AND NOT (status='published' AND created_at<=CURRENT_TIMESTAMP)";
    if (sqlite3_prepare_v2(fly_db_conn(db), reset_sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_int(stmt, 1, id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

cJSON *db_post_claim_unannounced(cwist_db *db) {
    const char *sql = "SELECT p.id, p.title, p.slug, p.summary FROM posts p WHERE p.announced=0 AND " POST_PUBLIC_SQL " ORDER BY p.created_at";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(fly_db_conn(db), sql, -1, &stmt, NULL) != SQLITE_OK) return NULL;
    cJSON *due = db_sqlite3_rows_to_json(stmt);
    cJSON *claimed = cJSON_CreateArray();
    if (!due || !claimed) {
        if (due) cJSON_Delete(due);
        return claimed;
    }
    /* Workers and the scheduler thread race for the same rows; the
     * conditional UPDATE lets exactly one of them win each post. */
    const char *claim_sql = "UPDATE posts SET announced=1 WHERE id=? AND announced=0 RETURNING id";
    cJSON *row = NULL;
    cJSON_ArrayForEach(row, due) {
        cJSON *idj = cJSON_GetObjectItem(row, "id");
        if (!idj || !cJSON_IsNumber(idj)) continue;
        if (sqlite3_prepare_v2(fly_db_conn(db), claim_sql, -1, &stmt, NULL) != SQLITE_OK) break;
        sqlite3_bind_int(stmt, 1, idj->valueint);
        /* RETURNING, not sqlite3_changes(): the connection is shared. */
        bool won = sqlite3_step(stmt) == SQLITE_ROW;
        sqlite3_finalize(stmt);
        if (won) cJSON_AddItemToArray(claimed, cJSON_Duplicate(row, true));
    }
    cJSON_Delete(due);
    return claimed;
}

int db_post_count_drafts(cwist_db *db, int user_id) {
    const char *sql = "SELECT COUNT(*) FROM posts WHERE user_id=? AND status='draft'";
    sqlite3_stmt *stmt = NULL;
    if (user_id <= 0 || sqlite3_prepare_v2(fly_db_conn(db), sql, -1, &stmt, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int(stmt, 1, user_id);
    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return count;
}

bool db_post_delete(cwist_db *db, int id) {
    const char *sql = "DELETE FROM posts WHERE id=?";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(fly_db_conn(db), sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_int(stmt, 1, id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool db_post_set_delete_pin_hash(cwist_db *db, int id, const char *delete_pin_hash) {
    const char *sql = "UPDATE posts SET delete_pin_hash=? WHERE id=?";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(fly_db_conn(db), sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, delete_pin_hash ? delete_pin_hash : "", -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

static int extract_post_id_from_slug(const char *slug) {
    if (!slug || !slug[0]) return 0;
    if (strncmp(slug, "post-", 5) == 0 && isdigit((unsigned char)slug[5])) return atoi(slug + 5);
    if (strncmp(slug, "post", 4) == 0 && isdigit((unsigned char)slug[4])) return atoi(slug + 4);
    if (isdigit((unsigned char)slug[0])) return atoi(slug);
    return 0;
}

cJSON *db_post_get_by_slug(cwist_db *db, const char *slug) {
    if (!slug || !slug[0]) return NULL;
    const char *sql = "SELECT p.*, u.username as author_name FROM posts p LEFT JOIN users u ON p.user_id=u.id WHERE p.slug=? OR p.id=? LIMIT 1";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(fly_db_conn(db), sql, -1, &stmt, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_text(stmt, 1, slug, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, extract_post_id_from_slug(slug));
    return db_sqlite3_row_to_json(stmt);
}

cJSON *db_post_get_by_id(cwist_db *db, int id) {
    const char *sql = "SELECT p.*, u.username as author_name FROM posts p LEFT JOIN users u ON p.user_id=u.id WHERE p.id=? LIMIT 1";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(fly_db_conn(db), sql, -1, &stmt, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_int(stmt, 1, id);
    return db_sqlite3_row_to_json(stmt);
}

cJSON *db_post_list(cwist_db *db, int board_id, int limit, int offset) {
    const char *sql = (board_id > 0)
        ? "SELECT p.*, u.username as author_name, b.name as board_name FROM posts p LEFT JOIN users u ON p.user_id=u.id LEFT JOIN boards b ON p.board_id=b.id WHERE p.board_id=? AND " POST_PUBLIC_SQL " ORDER BY p.created_at DESC LIMIT ? OFFSET ?"
        : "SELECT p.*, u.username as author_name, b.name as board_name FROM posts p LEFT JOIN users u ON p.user_id=u.id LEFT JOIN boards b ON p.board_id=b.id WHERE " POST_PUBLIC_SQL " ORDER BY p.created_at DESC LIMIT ? OFFSET ?";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(fly_db_conn(db), sql, -1, &stmt, NULL) != SQLITE_OK) return NULL;
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
    const char *sql = "SELECT p.*, u.username as author_name, b.name as board_name FROM posts p LEFT JOIN users u ON p.user_id=u.id LEFT JOIN boards b ON p.board_id=b.id WHERE p.board_id=? AND " POST_PUBLIC_SQL " ORDER BY p.created_at DESC LIMIT ?";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(fly_db_conn(db), sql, -1, &stmt, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_int(stmt, 1, board_id);
    sqlite3_bind_int(stmt, 2, limit);
    return db_sqlite3_rows_to_json(stmt);
}

cJSON *db_post_recent_by_boards_batch(cwist_db *db, int limit_per_board) {
    const char *sql =
        "WITH ranked_posts AS ("
        "  SELECT p.*, u.username as author_name, b.name as board_name,"
        "         ROW_NUMBER() OVER (PARTITION BY p.board_id ORDER BY p.created_at DESC) as rn"
        "  FROM posts p"
        "  LEFT JOIN users u ON p.user_id = u.id"
        "  LEFT JOIN boards b ON p.board_id = b.id"
        "  WHERE " POST_PUBLIC_SQL
        ") "
        "SELECT * FROM ranked_posts WHERE rn <= ? ORDER BY board_id, rn";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(fly_db_conn(db), sql, -1, &stmt, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_int(stmt, 1, limit_per_board);
    return db_sqlite3_rows_to_json(stmt);
}

int db_post_count(cwist_db *db, int board_id) {
    const char *sql = (board_id > 0) ? "SELECT COUNT(*) FROM posts p WHERE p.board_id=? AND " POST_PUBLIC_SQL : "SELECT COUNT(*) FROM posts p WHERE " POST_PUBLIC_SQL;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(fly_db_conn(db), sql, -1, &stmt, NULL) != SQLITE_OK) return 0;
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
    if (sqlite3_prepare_v2(fly_db_conn(db), sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_int(stmt, 1, id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

cJSON *db_post_list_search(cwist_db *db, int board_id, const char *search, const char *search_type, int limit, int offset) {
    int has_board = board_id > 0;
    int has_search = (search && search[0]);
    char search_pattern[512] = {0};
    if (has_search) {
        snprintf(search_pattern, sizeof(search_pattern), "%%%s%%", search);
    }

    char sql[1024] = {0};
    strcpy(sql, "SELECT p.*, u.username as author_name, b.name as board_name FROM posts p LEFT JOIN users u ON p.user_id=u.id LEFT JOIN boards b ON p.board_id=b.id WHERE " POST_PUBLIC_SQL);

    if (has_board) strcat(sql, " AND p.board_id=?");

    if (has_search) {
        strcat(sql, " AND ");
        if (!search_type || !search_type[0]) {
            strcat(sql, "(p.title LIKE ? OR p.content LIKE ?)");
        } else if (strcmp(search_type, "title") == 0) {
            strcat(sql, "p.title LIKE ?");
        } else if (strcmp(search_type, "body") == 0) {
            strcat(sql, "p.content LIKE ?");
        } else if (strcmp(search_type, "board") == 0) {
            strcat(sql, "b.name LIKE ?");
        } else {
            strcat(sql, "(p.title LIKE ? OR p.content LIKE ?)");
        }
    }

    strcat(sql, " ORDER BY p.is_notice DESC, p.created_at DESC LIMIT ? OFFSET ?");

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(fly_db_conn(db), sql, -1, &stmt, NULL) != SQLITE_OK) {
        return NULL;
    }
    int idx = 1;
    if (has_board) sqlite3_bind_int(stmt, idx++, board_id);
    if (has_search) {
        int is_default = (!search_type || !search_type[0] ||
            (strcmp(search_type, "title") != 0 && strcmp(search_type, "body") != 0 && strcmp(search_type, "board") != 0));
        sqlite3_bind_text(stmt, idx++, search_pattern, -1, SQLITE_STATIC);
        if (is_default) {
            sqlite3_bind_text(stmt, idx++, search_pattern, -1, SQLITE_STATIC);
        }
    }
    sqlite3_bind_int(stmt, idx++, limit);
    sqlite3_bind_int(stmt, idx++, offset);
    return db_sqlite3_rows_to_json(stmt);
}

int db_post_count_search(cwist_db *db, int board_id, const char *search, const char *search_type) {
    int has_board = board_id > 0;
    int has_search = (search && search[0]);
    char search_pattern[512] = {0};
    if (has_search) {
        snprintf(search_pattern, sizeof(search_pattern), "%%%s%%", search);
    }

    char sql[1024] = {0};
    int needs_join = has_search && search_type && strcmp(search_type, "board") == 0;
    if (needs_join) {
        strcpy(sql, "SELECT COUNT(*) FROM posts p LEFT JOIN boards b ON p.board_id=b.id");
    } else {
        strcpy(sql, "SELECT COUNT(*) FROM posts p");
    }
    strcat(sql, " WHERE " POST_PUBLIC_SQL);

    if (has_board) strcat(sql, " AND p.board_id=?");

    if (has_search) {
        strcat(sql, " AND ");
        if (!search_type || !search_type[0]) {
            strcat(sql, "(p.title LIKE ? OR p.content LIKE ?)");
        } else if (strcmp(search_type, "title") == 0) {
            strcat(sql, "p.title LIKE ?");
        } else if (strcmp(search_type, "body") == 0) {
            strcat(sql, "p.content LIKE ?");
        } else if (strcmp(search_type, "board") == 0) {
            strcat(sql, "b.name LIKE ?");
        } else {
            strcat(sql, "(p.title LIKE ? OR p.content LIKE ?)");
        }
    }

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(fly_db_conn(db), sql, -1, &stmt, NULL) != SQLITE_OK) {
        return 0;
    }
    int idx = 1;
    if (has_board) sqlite3_bind_int(stmt, idx++, board_id);
    if (has_search) {
        int is_default = (!search_type || !search_type[0] ||
            (strcmp(search_type, "title") != 0 && strcmp(search_type, "body") != 0 && strcmp(search_type, "board") != 0));
        sqlite3_bind_text(stmt, idx++, search_pattern, -1, SQLITE_STATIC);
        if (is_default) {
            sqlite3_bind_text(stmt, idx++, search_pattern, -1, SQLITE_STATIC);
        }
    }
    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return count;
}

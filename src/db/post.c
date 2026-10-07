#define _POSIX_C_SOURCE 200809L
#include "db.h"
#include "db_internal.h"
#include "search.h"
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
    db_search_index_post(db, id);
    /* Hidden again (draft or rescheduled): announce once more when it goes
     * public. A post that stays public keeps its flag. */
    const char *reset_sql = "UPDATE posts SET announced=0 WHERE id=? AND NOT (status='published' AND created_at<=CURRENT_TIMESTAMP)";
    if (sqlite3_prepare_v2(fly_db_conn(db), reset_sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_int(stmt, 1, id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return false;
    db_sync_journal_row(db, "posts", "post", id);
    return true;
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
    if (rc == SQLITE_DONE && sqlite3_changes(fly_db_conn(db)) > 0) {
        db_sync_journal(db, "post", id, "delete", "");
    }
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
    if (rc == SQLITE_DONE && sqlite3_changes(fly_db_conn(db)) > 0) {
        db_sync_journal_row(db, "posts", "post", id);
    }
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
    search_query q;
    search_query_build(&q, search, search_type);
    bool ranked = q.title_rank->size > 0;

    cwist_sstring *sql = cwist_sstring_create();
    cwist_sstring_assign(sql, "SELECT p.*, u.username as author_name, b.name as board_name FROM posts p LEFT JOIN users u ON p.user_id=u.id LEFT JOIN boards b ON p.board_id=b.id WHERE " POST_PUBLIC_SQL);
    if (board_id > 0) cwist_sstring_append(sql, " AND p.board_id=?");
    cwist_sstring_append_sstring(sql, q.where);
    if (ranked) {
        /* A search lists the best title matches first; browsing keeps
         * notices pinned on top. */
        cwist_sstring_append(sql, " ORDER BY (");
        cwist_sstring_append_sstring(sql, q.title_rank);
        cwist_sstring_append(sql, ") DESC, p.created_at DESC LIMIT ? OFFSET ?");
    } else {
        cwist_sstring_append(sql, " ORDER BY p.is_notice DESC, p.created_at DESC LIMIT ? OFFSET ?");
    }

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(fly_db_conn(db), sql->data, -1, &stmt, NULL);
    cwist_sstring_destroy(sql);
    if (rc != SQLITE_OK) {
        search_query_free(&q);
        return NULL;
    }
    int idx = 1;
    if (board_id > 0) sqlite3_bind_int(stmt, idx++, board_id);
    search_query_bind(&q, stmt, &idx, ranked);
    sqlite3_bind_int(stmt, idx++, limit);
    sqlite3_bind_int(stmt, idx++, offset);
    search_query_free(&q);
    return db_sqlite3_rows_to_json(stmt);
}

int db_post_count_search(cwist_db *db, int board_id, const char *search, const char *search_type) {
    search_query q;
    search_query_build(&q, search, search_type);

    cwist_sstring *sql = cwist_sstring_create();
    if (search_type && strcmp(search_type, "board") == 0 && q.terms > 0) {
        cwist_sstring_assign(sql, "SELECT COUNT(*) FROM posts p LEFT JOIN boards b ON p.board_id=b.id");
    } else {
        cwist_sstring_assign(sql, "SELECT COUNT(*) FROM posts p");
    }
    cwist_sstring_append(sql, " WHERE " POST_PUBLIC_SQL);
    if (board_id > 0) cwist_sstring_append(sql, " AND p.board_id=?");
    cwist_sstring_append_sstring(sql, q.where);

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(fly_db_conn(db), sql->data, -1, &stmt, NULL);
    cwist_sstring_destroy(sql);
    if (rc != SQLITE_OK) {
        search_query_free(&q);
        return 0;
    }
    int idx = 1;
    if (board_id > 0) sqlite3_bind_int(stmt, idx++, board_id);
    search_query_bind(&q, stmt, &idx, false);
    search_query_free(&q);
    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return count;
}

/* "YYYY-MM" -> [start, end) as UTC timestamps, so the month filter can use
 * the created_at index instead of strftime() on every row. */
static bool month_bounds(const char *ym, char start[POST_TIME_LEN], char end[POST_TIME_LEN]) {
    if (!ym || strlen(ym) != 7 || ym[4] != '-') return false;
    for (int i = 0; i < 7; i++) {
        if (i != 4 && !isdigit((unsigned char)ym[i])) return false;
    }
    int y = atoi(ym), m = atoi(ym + 5);
    if (y < 1970 || y > 9998 || m < 1 || m > 12) return false;
    int ny = m == 12 ? y + 1 : y, nm = m == 12 ? 1 : m + 1;
    snprintf(start, POST_TIME_LEN, "%04u-%02u-01 00:00:00", (unsigned)y % 10000u, (unsigned)m % 13u);
    snprintf(end, POST_TIME_LEN, "%04u-%02u-01 00:00:00", (unsigned)ny % 10000u, (unsigned)nm % 13u);
    return true;
}

cJSON *db_post_archive_months(cwist_db *db) {
    const char *sql = "SELECT substr(p.created_at, 1, 7) AS ym, COUNT(*) AS n FROM posts p WHERE " POST_PUBLIC_SQL
                      " GROUP BY ym ORDER BY ym DESC";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(fly_db_conn(db), sql, -1, &stmt, NULL) != SQLITE_OK) return NULL;
    return db_sqlite3_rows_to_json(stmt);
}

cJSON *db_post_list_by_month(cwist_db *db, const char *ym, int limit, int offset) {
    char start[POST_TIME_LEN], end[POST_TIME_LEN];
    if (!month_bounds(ym, start, end)) return NULL;
    const char *sql = "SELECT p.*, u.username as author_name, b.name as board_name FROM posts p LEFT JOIN users u ON p.user_id=u.id LEFT JOIN boards b ON p.board_id=b.id"
                      " WHERE p.created_at>=? AND p.created_at<? AND " POST_PUBLIC_SQL
                      " ORDER BY p.created_at DESC LIMIT ? OFFSET ?";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(fly_db_conn(db), sql, -1, &stmt, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_text(stmt, 1, start, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, end, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, limit);
    sqlite3_bind_int(stmt, 4, offset);
    return db_sqlite3_rows_to_json(stmt);
}

int db_post_count_by_month(cwist_db *db, const char *ym) {
    char start[POST_TIME_LEN], end[POST_TIME_LEN];
    if (!month_bounds(ym, start, end)) return 0;
    const char *sql = "SELECT COUNT(*) FROM posts p WHERE p.created_at>=? AND p.created_at<? AND " POST_PUBLIC_SQL;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(fly_db_conn(db), sql, -1, &stmt, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_text(stmt, 1, start, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, end, -1, SQLITE_TRANSIENT);
    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return count;
}

static cJSON *adjacent_one(cwist_db *db, const char *sql, int post_id, const char *created_at) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(fly_db_conn(db), sql, -1, &stmt, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_text(stmt, 1, created_at, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, post_id);
    return db_sqlite3_row_to_json(stmt);
}

cJSON *db_post_adjacent(cwist_db *db, int post_id, const char *created_at) {
    cJSON *out = cJSON_CreateObject();
    if (!out || post_id <= 0 || !created_at || !created_at[0]) return out;
    /* Ties on created_at fall back to id so every post has one neighbour on
     * each side. */
    cJSON *prev = adjacent_one(db,
        "SELECT p.slug AS slug, p.title AS title FROM posts p WHERE (p.created_at<?1 OR (p.created_at=?1 AND p.id<?2)) AND " POST_PUBLIC_SQL
        " ORDER BY p.created_at DESC, p.id DESC LIMIT 1", post_id, created_at);
    cJSON *next = adjacent_one(db,
        "SELECT p.slug AS slug, p.title AS title FROM posts p WHERE (p.created_at>?1 OR (p.created_at=?1 AND p.id>?2)) AND " POST_PUBLIC_SQL
        " ORDER BY p.created_at ASC, p.id ASC LIMIT 1", post_id, created_at);
    if (prev) cJSON_AddItemToObject(out, "prev", prev);
    if (next) cJSON_AddItemToObject(out, "next", next);
    return out;
}

cJSON *db_post_feed(cwist_db *db, int board_id, int limit) {
    const char *sql = (board_id > 0)
        ? "SELECT p.*, u.username as author_name, b.name as board_name FROM posts p LEFT JOIN users u ON p.user_id=u.id LEFT JOIN boards b ON p.board_id=b.id WHERE p.board_id=? AND COALESCE(p.is_secret,0)=0 AND " POST_PUBLIC_SQL " ORDER BY p.created_at DESC LIMIT ?"
        : "SELECT p.*, u.username as author_name, b.name as board_name FROM posts p LEFT JOIN users u ON p.user_id=u.id LEFT JOIN boards b ON p.board_id=b.id WHERE COALESCE(p.is_secret,0)=0 AND " POST_PUBLIC_SQL " ORDER BY p.created_at DESC LIMIT ?";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(fly_db_conn(db), sql, -1, &stmt, NULL) != SQLITE_OK) return NULL;
    int idx = 1;
    if (board_id > 0) sqlite3_bind_int(stmt, idx++, board_id);
    sqlite3_bind_int(stmt, idx++, limit);
    return db_sqlite3_rows_to_json(stmt);
}

cJSON *db_post_list_for_signing(cwist_db *db) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(fly_db_conn(db), "SELECT id, title, content, pqc_signature FROM posts ORDER BY id", -1, &stmt, NULL) != SQLITE_OK) return NULL;
    return db_sqlite3_rows_to_json(stmt);
}

bool db_post_set_signature(cwist_db *db, int id, const char *pqc_signature) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(fly_db_conn(db), "UPDATE posts SET pqc_signature=? WHERE id=?", -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, pqc_signature ? pqc_signature : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc == SQLITE_DONE && sqlite3_changes(fly_db_conn(db)) > 0) {
        db_sync_journal_row(db, "posts", "post", id);
    }
    return rc == SQLITE_DONE;
}

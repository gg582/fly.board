#define _POSIX_C_SOURCE 200809L
#include "db.h"
#include "db_internal.h"
#include "search.h"
#include "engine/forkgate.h"
#include <cwist/core/log.h>
#include <cwist/core/mem/alloc.h>
#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
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

/* ---- Batched view counters ----
 *
 * Every post view used to issue "UPDATE posts SET view_count=view_count+1"
 * on the request path, turning pure read traffic into serialized writer
 * traffic across all worker processes (each holding its own connection with a
 * 5 s busy timeout).  Deltas are now accumulated per post id in an
 * anonymous mmap shared by every worker (MAP_SHARED before the fork, same
 * pattern as src/utils/spam_guard.c, with a robust pshared mutex) and flushed
 * to SQLite periodically - every 30 s - by the supervisor's schedule worker
 * (src/utils/post_schedule.c).  Semantics: eventual consistency; a view
 * becomes visible in view_count (and in hot-score queries) within ~30 s
 * instead of instantly.  Total counts are exact: deltas are only cleared
 * after their UPDATE commits. */

#define VIEW_COUNTER_SLOTS 4096

typedef struct {
    int64_t post_id;  /* 0 = empty */
    uint64_t delta;
} view_counter_slot_t;

typedef struct {
    pthread_mutex_t lock;
    view_counter_slot_t slots[VIEW_COUNTER_SLOTS];
} view_counter_table_t;

static view_counter_table_t *g_view_counters = NULL;

bool db_view_counters_init(void) {
    if (g_view_counters) return true;
    void *mem = mmap(NULL, sizeof(view_counter_table_t), PROT_READ | PROT_WRITE,
                     MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) {
        CWIST_LOG_ERROR("View counters: shared memory unavailable; per-view writes stay enabled");
        return false;
    }
    view_counter_table_t *t = (view_counter_table_t *)mem;
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    /* A worker killed while holding the lock must not wedge the others. */
    pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);
    bool ok = pthread_mutex_init(&t->lock, &attr) == 0;
    pthread_mutexattr_destroy(&attr);
    if (!ok) {
        munmap(mem, sizeof(view_counter_table_t));
        CWIST_LOG_ERROR("View counters: init failed; per-view writes stay enabled");
        return false;
    }
    g_view_counters = t;
    return true;
}

/* Accumulate one view.  Never blocks on SQLite. */
static void view_counter_bump(int id) {
    view_counter_table_t *t = g_view_counters;
    if (!t || id <= 0) return;
    uint64_t key = (uint64_t)id;
    size_t start = (size_t)(key % VIEW_COUNTER_SLOTS);
    if (pthread_mutex_lock(&t->lock) == EOWNERDEAD) pthread_mutex_consistent(&t->lock);
    for (size_t i = 0; i < 32; i++) {
        view_counter_slot_t *s = &t->slots[(start + i) % VIEW_COUNTER_SLOTS];
        if (s->post_id == (int64_t)id) {
            s->delta++;
            goto done;
        }
        if (s->post_id == 0) {
            s->post_id = id;
            s->delta = 1;
            goto done;
        }
    }
    /* Probe exhausted: the table is saturated with other posts.  Rare;
     * sacrifice this count rather than grow shared memory at runtime. */
done:
    pthread_mutex_unlock(&t->lock);
}

/* Snapshot-and-clear under the lock, then apply outside it so request-path
 * bumps are never blocked by SQLite I/O.  Uses its own short-lived
 * connection; must be called inside a fly_forkgate bracket when forks can
 * still happen. */
void db_view_counters_flush(void) {
    view_counter_table_t *t = g_view_counters;
    if (!t) return;
    view_counter_slot_t pending[VIEW_COUNTER_SLOTS];
    size_t pending_idx[VIEW_COUNTER_SLOTS];
    size_t n = 0;
    if (pthread_mutex_lock(&t->lock) == EOWNERDEAD) pthread_mutex_consistent(&t->lock);
    for (size_t i = 0; i < VIEW_COUNTER_SLOTS; i++) {
        if (t->slots[i].post_id != 0 && t->slots[i].delta > 0) {
            pending[n].post_id = t->slots[i].post_id;
            pending[n].delta = t->slots[i].delta;
            pending_idx[n] = i;
            n++;
            t->slots[i].delta = 0;
        }
    }
    pthread_mutex_unlock(&t->lock);
    if (n == 0) return;

    sqlite3 *conn = NULL;
    if (sqlite3_open(FLY_DB_MAIN_PATH, &conn) != SQLITE_OK) {
        sqlite3_close(conn);
        /* Put the deltas back so no count is lost on the next flush. */
        if (pthread_mutex_lock(&t->lock) == EOWNERDEAD) pthread_mutex_consistent(&t->lock);
        for (size_t i = 0; i < n; i++) t->slots[pending_idx[i]].delta += pending[i].delta;
        pthread_mutex_unlock(&t->lock);
        CWIST_LOG_WARN("View counters: flush could not open %s", FLY_DB_MAIN_PATH);
        return;
    }
    db_configure_connection(conn);
    const char *sql = "UPDATE posts SET view_count = view_count + ? WHERE id=?";
    sqlite3_stmt *stmt = NULL;
    bool ok = sqlite3_prepare_v2(conn, sql, -1, &stmt, NULL) == SQLITE_OK;
    size_t applied = 0;
    for (size_t i = 0; ok && i < n; i++) {
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        sqlite3_bind_int64(stmt, 1, (sqlite3_int64)pending[i].delta);
        sqlite3_bind_int64(stmt, 2, (sqlite3_int64)pending[i].post_id);
        if (sqlite3_step(stmt) != SQLITE_DONE) ok = false;
        else applied++;
    }
    if (stmt) sqlite3_finalize(stmt);
    if (!ok) {
        CWIST_LOG_WARN("View counters: flush failed: %s", sqlite3_errmsg(conn));
        /* Return unapplied deltas so no count is lost on the next flush. */
        if (pthread_mutex_lock(&t->lock) == EOWNERDEAD) pthread_mutex_consistent(&t->lock);
        for (size_t i = applied; i < n; i++) t->slots[pending_idx[i]].delta += pending[i].delta;
        pthread_mutex_unlock(&t->lock);
    }
    sqlite3_close(conn);
}

bool db_post_increment_view(cwist_db *db, int id) {
    /* Fast path: accumulate the delta in shared memory; the supervisor's
     * schedule worker flushes it every 30 s (see db_view_counters_flush). */
    if (g_view_counters) {
        view_counter_bump(id);
        return true;
    }
    /* Shared table unavailable: keep the exact old behavior. */
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

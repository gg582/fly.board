#define _POSIX_C_SOURCE 200809L
#include "db.h"
#include "db_internal.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Series (ordered post collections) and translation pairs.
 *
 * A post belongs to at most one series (posts.series_id, ordered by
 * posts.series_pos). Translations are kept in one table for every kind of
 * item ("post", "board", "series"): each item has a language and a group;
 * items sharing a non-zero group are translations of each other. */

bool db_series_migrate(cwist_db *db) {
    db_exec_sql(db, "CREATE TABLE IF NOT EXISTS series ("
                    "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                    "title TEXT NOT NULL,"
                    "description TEXT NOT NULL DEFAULT '',"
                    "user_id INTEGER,"
                    "created_at DATETIME DEFAULT CURRENT_TIMESTAMP)");
    db_exec_sql(db, "ALTER TABLE posts ADD COLUMN series_id INTEGER");
    db_exec_sql(db, "ALTER TABLE posts ADD COLUMN series_pos INTEGER NOT NULL DEFAULT 0");
    db_exec_sql(db, "CREATE INDEX IF NOT EXISTS idx_posts_series ON posts(series_id, series_pos)");
    db_exec_sql(db, "CREATE TABLE IF NOT EXISTS i18n ("
                    "kind TEXT NOT NULL, item_id INTEGER NOT NULL,"
                    "lang TEXT NOT NULL DEFAULT '', grp INTEGER NOT NULL DEFAULT 0,"
                    "PRIMARY KEY (kind, item_id)) WITHOUT ROWID");
    db_exec_sql(db, "CREATE INDEX IF NOT EXISTS idx_i18n_grp ON i18n(kind, grp)");
    return true;
}

/* ---- Series ---- */

int db_series_create(cwist_db *db, const char *title, int user_id) {
    sqlite3 *conn = fly_db_conn(db);
    sqlite3_stmt *st = NULL;
    if (!title || !title[0] || sqlite3_prepare_v2(conn, "INSERT INTO series (title, user_id) VALUES (?, ?)", -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_text(st, 1, title, -1, SQLITE_TRANSIENT);
    if (user_id > 0) sqlite3_bind_int(st, 2, user_id);
    else sqlite3_bind_null(st, 2);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc == SQLITE_DONE) {
        int id = (int)sqlite3_last_insert_rowid(conn);
        db_sync_journal_row(db, "series", "series", id);
        return id;
    }
    return 0;
}

cJSON *db_series_get(cwist_db *db, int id) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(fly_db_conn(db), "SELECT s.*, u.username AS owner_name FROM series s LEFT JOIN users u ON u.id=s.user_id WHERE s.id=?", -1, &st, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_int(st, 1, id);
    return db_sqlite3_row_to_json(st);
}

int db_series_find(cwist_db *db, const char *title, int user_id) {
    /* Admins (user_id 0 here) may use anyone's series. */
    const char *sql = user_id > 0
        ? "SELECT id FROM series WHERE title=? AND user_id=? ORDER BY id LIMIT 1"
        : "SELECT id FROM series WHERE title=? ORDER BY id LIMIT 1";
    sqlite3_stmt *st = NULL;
    if (!title || sqlite3_prepare_v2(fly_db_conn(db), sql, -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_text(st, 1, title, -1, SQLITE_TRANSIENT);
    if (user_id > 0) sqlite3_bind_int(st, 2, user_id);
    int id = sqlite3_step(st) == SQLITE_ROW ? sqlite3_column_int(st, 0) : 0;
    sqlite3_finalize(st);
    return id;
}

cJSON *db_series_list(cwist_db *db, int user_id, bool public_only) {
    /* user_id > 0 limits to that owner (editor suggestions). */
    char sql[512];
    snprintf(sql, sizeof(sql),
             "SELECT s.id, s.title, s.description, s.user_id,"
             " (SELECT COUNT(*) FROM posts p WHERE p.series_id=s.id%s) AS n,"
             " (SELECT MAX(p.created_at) FROM posts p WHERE p.series_id=s.id%s) AS latest"
             " FROM series s%s ORDER BY latest DESC, s.id DESC",
             public_only ? " AND " POST_PUBLIC_SQL : "", public_only ? " AND " POST_PUBLIC_SQL : "",
             user_id > 0 ? " WHERE s.user_id=?" : "");
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(fly_db_conn(db), sql, -1, &st, NULL) != SQLITE_OK) return NULL;
    if (user_id > 0) sqlite3_bind_int(st, 1, user_id);
    cJSON *rows = db_sqlite3_rows_to_json(st);
    if (public_only && rows) {
        /* A series with no public post is not listed publicly. */
        for (int i = cJSON_GetArraySize(rows) - 1; i >= 0; i--) {
            cJSON *n = cJSON_GetObjectItem(cJSON_GetArrayItem(rows, i), "n");
            if (!cJSON_IsNumber(n) || n->valueint == 0) cJSON_DeleteItemFromArray(rows, i);
        }
    }
    return rows;
}

cJSON *db_series_posts(cwist_db *db, int series_id, bool public_only) {
    const char *sql = public_only
        ? "SELECT p.id, p.slug, p.title, p.created_at, p.series_pos, p.status FROM posts p WHERE p.series_id=? AND " POST_PUBLIC_SQL " ORDER BY p.series_pos, p.created_at, p.id"
        : "SELECT p.id, p.slug, p.title, p.created_at, p.series_pos, p.status FROM posts p WHERE p.series_id=? ORDER BY p.series_pos, p.created_at, p.id";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(fly_db_conn(db), sql, -1, &st, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_int(st, 1, series_id);
    return db_sqlite3_rows_to_json(st);
}

static bool exec_bind(sqlite3 *conn, const char *sql, int a, int b, int c) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(conn, sql, -1, &st, NULL) != SQLITE_OK) return false;
    if (sqlite3_bind_parameter_count(st) >= 1) sqlite3_bind_int(st, 1, a);
    if (sqlite3_bind_parameter_count(st) >= 2) sqlite3_bind_int(st, 2, b);
    if (sqlite3_bind_parameter_count(st) >= 3) sqlite3_bind_int(st, 3, c);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE;
}

/* Renumber a series 1..n in its current order, leaving out one post. */
static bool renumber(sqlite3 *conn, int series_id, int except_id) {
    return exec_bind(conn,
        "UPDATE posts SET series_pos=(SELECT rn FROM (SELECT id, ROW_NUMBER() OVER (ORDER BY series_pos, created_at, id) AS rn"
        " FROM posts WHERE series_id=?1 AND id<>?2) r WHERE r.id=posts.id) WHERE series_id=?1 AND id<>?2",
        series_id, except_id, 0);
}

bool db_post_set_series(cwist_db *db, int post_id, int series_id, int pos) {
    sqlite3 *conn = fly_db_conn(db);
    int old_series = 0, old_pos = 0;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(conn, "SELECT COALESCE(series_id, 0), series_pos FROM posts WHERE id=?", -1, &st, NULL) != SQLITE_OK) return false;
    sqlite3_bind_int(st, 1, post_id);
    bool found = sqlite3_step(st) == SQLITE_ROW;
    if (found) { old_series = sqlite3_column_int(st, 0); old_pos = sqlite3_column_int(st, 1); }
    sqlite3_finalize(st);
    if (!found) return false;

    if (sqlite3_exec(conn, "SAVEPOINT series_move", NULL, NULL, NULL) != SQLITE_OK) return false;
    bool ok = true;
    if (series_id <= 0) {
        ok = exec_bind(conn, "UPDATE posts SET series_id=NULL, series_pos=0 WHERE id=?1", post_id, 0, 0);
        if (ok && old_series > 0) ok = renumber(conn, old_series, 0);
    } else {
        /* Take the post out, close the gap, then insert it at the asked
         * place (the end when none, or its old place when it stays). */
        ok = exec_bind(conn, "UPDATE posts SET series_id=?1, series_pos=0 WHERE id=?2", series_id, post_id, 0);
        if (ok && old_series > 0 && old_series != series_id) ok = renumber(conn, old_series, 0);
        if (ok) ok = renumber(conn, series_id, post_id);
        int count = 0;
        if (ok && sqlite3_prepare_v2(conn, "SELECT COUNT(*) FROM posts WHERE series_id=? AND id<>?", -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_int(st, 1, series_id);
            sqlite3_bind_int(st, 2, post_id);
            if (sqlite3_step(st) == SQLITE_ROW) count = sqlite3_column_int(st, 0);
            sqlite3_finalize(st);
        }
        if (pos <= 0) pos = (old_series == series_id && old_pos > 0) ? old_pos : count + 1;
        if (pos > count + 1) pos = count + 1;
        if (ok) ok = exec_bind(conn, "UPDATE posts SET series_pos=series_pos+1 WHERE series_id=?1 AND id<>?2 AND series_pos>=?3",
                               series_id, post_id, pos);
        if (ok) ok = exec_bind(conn, "UPDATE posts SET series_pos=?1 WHERE id=?2", pos, post_id, 0);
    }
    if (ok) {
        sqlite3_exec(conn, "RELEASE series_move", NULL, NULL, NULL);
        db_sync_journal_row(db, "posts", "post", post_id);
    } else {
        sqlite3_exec(conn, "ROLLBACK TO series_move", NULL, NULL, NULL);
        sqlite3_exec(conn, "RELEASE series_move", NULL, NULL, NULL);
    }
    return ok;
}

bool db_series_set_order(cwist_db *db, int series_id, const int *post_ids, int n) {
    sqlite3 *conn = fly_db_conn(db);
    if (sqlite3_exec(conn, "SAVEPOINT series_order", NULL, NULL, NULL) != SQLITE_OK) return false;
    bool ok = true;
    for (int i = 0; ok && i < n; i++) {
        ok = exec_bind(conn, "UPDATE posts SET series_pos=?1 WHERE id=?2 AND series_id=?3", i + 1, post_ids[i], series_id);
    }
    sqlite3_exec(conn, ok ? "RELEASE series_order" : "ROLLBACK TO series_order", NULL, NULL, NULL);
    if (!ok) sqlite3_exec(conn, "RELEASE series_order", NULL, NULL, NULL);
    if (ok) {
        for (int i = 0; i < n; i++) db_sync_journal_row(db, "posts", "post", post_ids[i]);
    }
    return ok;
}

bool db_series_update(cwist_db *db, int id, const char *title, const char *description) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(fly_db_conn(db), "UPDATE series SET title=?, description=? WHERE id=?", -1, &st, NULL) != SQLITE_OK) return false;
    sqlite3_bind_text(st, 1, title, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, description ? description : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 3, id);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc == SQLITE_DONE && sqlite3_changes(fly_db_conn(db)) > 0) {
        db_sync_journal_row(db, "series", "series", id);
    }
    return rc == SQLITE_DONE;
}

bool db_series_delete(cwist_db *db, int id) {
    sqlite3 *conn = fly_db_conn(db);
    if (!db_transaction_begin(db)) return false;
    sqlite3_stmt *st = NULL;
    bool ok = sqlite3_prepare_v2(conn, "UPDATE posts SET series_id=NULL, series_pos=0 WHERE series_id=?", -1, &st, NULL) == SQLITE_OK;
    if (ok) { sqlite3_bind_int(st, 1, id); ok = sqlite3_step(st) == SQLITE_DONE; sqlite3_finalize(st); }
    if (ok && sqlite3_prepare_v2(conn, "DELETE FROM series WHERE id=?", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int(st, 1, id);
        ok = sqlite3_step(st) == SQLITE_DONE;
        sqlite3_finalize(st);
    }
    if (ok && sqlite3_prepare_v2(conn, "DELETE FROM i18n WHERE kind='series' AND item_id=?", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int(st, 1, id);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
    if (ok) {
        bool committed = db_transaction_commit(db);
        if (committed) db_sync_journal(db, "series", id, "delete", "");
        return committed;
    }
    db_transaction_rollback(db);
    return false;
}

/* ---- Languages and translation groups ---- */

bool i18n_lang_valid(const char *lang) {
    /* BCP 47 subset: "ko", "en", "zh-tw", "pt-br"; empty means unset. */
    if (!lang) return false;
    size_t n = strlen(lang);
    if (n == 0) return true;
    if (n < 2 || n > 12) return false;
    size_t i = 0;
    while (i < n && islower((unsigned char)lang[i])) i++;
    if (i < 2 || i > 3) return false;
    if (i == n) return true;
    if (lang[i] != '-') return false;
    size_t rest = n - i - 1;
    if (rest < 2 || rest > 8) return false;
    for (size_t j = i + 1; j < n; j++) if (!islower((unsigned char)lang[j]) && !isdigit((unsigned char)lang[j])) return false;
    return true;
}

static bool i18n_kind_valid(const char *kind) {
    return kind && (!strcmp(kind, "post") || !strcmp(kind, "board") || !strcmp(kind, "series"));
}

cJSON *db_i18n_get(cwist_db *db, const char *kind, int id) {
    sqlite3_stmt *st = NULL;
    if (!i18n_kind_valid(kind) || sqlite3_prepare_v2(fly_db_conn(db), "SELECT lang, grp FROM i18n WHERE kind=? AND item_id=?", -1, &st, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_text(st, 1, kind, -1, SQLITE_STATIC);
    sqlite3_bind_int(st, 2, id);
    return db_sqlite3_row_to_json(st);
}

static int i18n_group_of(sqlite3 *conn, const char *kind, int id) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(conn, "SELECT grp FROM i18n WHERE kind=? AND item_id=?", -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_text(st, 1, kind, -1, SQLITE_STATIC);
    sqlite3_bind_int(st, 2, id);
    int g = sqlite3_step(st) == SQLITE_ROW ? sqlite3_column_int(st, 0) : 0;
    sqlite3_finalize(st);
    return g;
}

static bool i18n_upsert(sqlite3 *conn, const char *kind, int id, const char *lang, int grp, bool set_lang, bool set_grp) {
    sqlite3_stmt *st = NULL;
    const char *sql =
        "INSERT INTO i18n (kind, item_id, lang, grp) VALUES (?1, ?2, COALESCE(?3, ''), COALESCE(?4, 0))"
        " ON CONFLICT(kind, item_id) DO UPDATE SET lang=COALESCE(?3, lang), grp=COALESCE(?4, grp)";
    if (sqlite3_prepare_v2(conn, sql, -1, &st, NULL) != SQLITE_OK) return false;
    sqlite3_bind_text(st, 1, kind, -1, SQLITE_STATIC);
    sqlite3_bind_int(st, 2, id);
    if (set_lang) sqlite3_bind_text(st, 3, lang ? lang : "", -1, SQLITE_TRANSIENT);
    else sqlite3_bind_null(st, 3);
    if (set_grp) sqlite3_bind_int(st, 4, grp);
    else sqlite3_bind_null(st, 4);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE;
}

bool db_i18n_set(cwist_db *db, const char *kind, int id, const char *lang, int pair_with) {
    if (!i18n_kind_valid(kind) || id <= 0 || (lang && !i18n_lang_valid(lang))) return false;
    sqlite3 *conn = fly_db_conn(db);
    if (!i18n_upsert(conn, kind, id, lang, 0, lang != NULL, false)) return false;
    bool ok = true;
    if (pair_with < 0) ok = i18n_upsert(conn, kind, id, NULL, 0, false, true);
    else if (pair_with == 0 || pair_with == id) ok = true;
    else {
        /* Join the other item's group, starting one keyed by its id. */
        int grp = i18n_group_of(conn, kind, pair_with);
        if (grp == 0) {
            grp = pair_with;
            ok = i18n_upsert(conn, kind, pair_with, NULL, grp, false, true);
        }
        if (ok) ok = i18n_upsert(conn, kind, id, NULL, grp, false, true);
    }
    if (ok) {
        /* Journal the full i18n row ({kind,item_id,lang,grp}) for the item. */
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(conn, "SELECT kind, item_id, lang, grp FROM i18n WHERE kind=? AND item_id=?", -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, kind, -1, SQLITE_STATIC);
            sqlite3_bind_int(st, 2, id);
            cJSON *row = db_sqlite3_row_to_json(st);
            if (row) {
                char *payload = cJSON_PrintUnformatted(row);
                if (payload) {
                    db_sync_journal(db, "i18n", id, "upsert", payload);
                    free(payload);
                }
                cJSON_Delete(row);
            }
        }
    }
    return ok;
}

cJSON *db_i18n_siblings(cwist_db *db, const char *kind, int id, bool public_only) {
    if (!i18n_kind_valid(kind)) return NULL;
    char sql[640];
    if (!strcmp(kind, "post")) {
        snprintf(sql, sizeof(sql),
                 "SELECT o.item_id AS id, o.lang AS lang, p.slug AS slug, p.title AS title FROM i18n me"
                 " JOIN i18n o ON o.kind=me.kind AND o.grp=me.grp AND o.item_id<>me.item_id"
                 " JOIN posts p ON p.id=o.item_id"
                 " WHERE me.kind='post' AND me.item_id=? AND me.grp<>0%s ORDER BY o.lang",
                 public_only ? " AND " POST_PUBLIC_SQL : "");
    } else if (!strcmp(kind, "board")) {
        snprintf(sql, sizeof(sql),
                 "SELECT o.item_id AS id, o.lang AS lang, b.slug AS slug, b.name AS title FROM i18n me"
                 " JOIN i18n o ON o.kind=me.kind AND o.grp=me.grp AND o.item_id<>me.item_id"
                 " JOIN boards b ON b.id=o.item_id"
                 " WHERE me.kind='board' AND me.item_id=? AND me.grp<>0 ORDER BY o.lang");
    } else {
        snprintf(sql, sizeof(sql),
                 "SELECT o.item_id AS id, o.lang AS lang, s.title AS title FROM i18n me"
                 " JOIN i18n o ON o.kind=me.kind AND o.grp=me.grp AND o.item_id<>me.item_id"
                 " JOIN series s ON s.id=o.item_id"
                 " WHERE me.kind='series' AND me.item_id=? AND me.grp<>0 ORDER BY o.lang");
    }
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(fly_db_conn(db), sql, -1, &st, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_int(st, 1, id);
    return db_sqlite3_rows_to_json(st);
}

cJSON *db_post_pick_list(cwist_db *db, int user_id) {
    /* Posts an author may pair a translation with: their own (all for
     * admins, user_id 0), newest first. */
    const char *sql = user_id > 0
        ? "SELECT id, title FROM posts WHERE user_id=? ORDER BY created_at DESC LIMIT 500"
        : "SELECT id, title FROM posts ORDER BY created_at DESC LIMIT 500";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(fly_db_conn(db), sql, -1, &st, NULL) != SQLITE_OK) return NULL;
    if (user_id > 0) sqlite3_bind_int(st, 1, user_id);
    return db_sqlite3_rows_to_json(st);
}

cJSON *db_series_feed(cwist_db *db, int series_id, int limit) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(fly_db_conn(db),
            "SELECT p.*, u.username as author_name, b.name as board_name FROM posts p LEFT JOIN users u ON p.user_id=u.id"
            " LEFT JOIN boards b ON p.board_id=b.id WHERE p.series_id=? AND COALESCE(p.is_secret,0)=0 AND " POST_PUBLIC_SQL
            " ORDER BY p.created_at DESC LIMIT ?", -1, &st, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_int(st, 1, series_id);
    sqlite3_bind_int(st, 2, limit);
    return db_sqlite3_rows_to_json(st);
}

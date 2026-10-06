#define _POSIX_C_SOURCE 200809L
#include "db.h"
#include "db_internal.h"
#include "search.h"
#include <cwist/core/mem/alloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Trigram search index for posts.
 *
 * The SQLite bundled with cwist is built without FTS5, so fly.board keeps
 * its own inverted index: every distinct run of three code points inside a
 * whitespace-separated word of a post's title or body, ASCII-folded to lower
 * case, maps to the post. A query term of three or more code points can only
 * occur in a post that holds all of the term's trigrams, so the index narrows
 * the candidates and LIKE then confirms the exact substring. Trigrams work the
 * same for Hangul, CJK and Latin text, which a word tokenizer would not.
 *
 * Bump SEARCH_INDEX_VERSION whenever gram extraction changes; the next start
 * rebuilds the whole index. */
#define SEARCH_INDEX_VERSION "1"
#define SEARCH_GRAM_MAX 16 /* 3 code points of at most 4 bytes, plus NUL */

static size_t utf8_cp_len(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1; /* stray continuation or invalid lead byte: one unit */
}

static bool is_space(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

typedef void (*gram_fn)(const char *gram, void *ud);

/* Calls @p fn for every trigram of @p text, word by word. Duplicates are
 * reported as often as they occur; callers dedupe. */
static void for_each_gram(const char *text, gram_fn fn, void *ud) {
    if (!text) return;
    const unsigned char *s = (const unsigned char *)text;
    size_t i = 0;
    while (s[i]) {
        while (s[i] && is_space(s[i])) i++;
        size_t start[3];
        size_t len[3];
        int have = 0;
        while (s[i] && !is_space(s[i])) {
            size_t n = utf8_cp_len(s[i]);
            size_t k = 1;
            while (k < n && s[i + k] && (s[i + k] & 0xC0) == 0x80) k++;
            if (have == 3) {
                start[0] = start[1]; len[0] = len[1];
                start[1] = start[2]; len[1] = len[2];
                have = 2;
            }
            start[have] = i;
            len[have] = k;
            have++;
            if (have == 3) {
                char gram[SEARCH_GRAM_MAX];
                size_t g = 0;
                for (int j = 0; j < 3; j++) {
                    for (size_t b = 0; b < len[j]; b++) {
                        unsigned char c = s[start[j] + b];
                        gram[g++] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : (char)c;
                    }
                }
                gram[g] = '\0';
                fn(gram, ud);
            }
            i += k;
        }
    }
}

static size_t utf8_cp_count(const char *s) {
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if ((*p & 0xC0) != 0x80) n++;
    }
    return n;
}

/* ---- Index maintenance ---- */

static void insert_gram(const char *gram, void *ud) {
    sqlite3_stmt *stmt = (sqlite3_stmt *)ud;
    sqlite3_bind_text(stmt, 1, gram, -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_reset(stmt);
}

static bool index_post_text(sqlite3 *conn, int post_id, const char *title, const char *content) {
    sqlite3_stmt *del = NULL;
    if (sqlite3_prepare_v2(conn, "DELETE FROM post_search_grams WHERE post_id=?", -1, &del, NULL) != SQLITE_OK) return false;
    sqlite3_bind_int(del, 1, post_id);
    int rc = sqlite3_step(del);
    sqlite3_finalize(del);
    if (rc != SQLITE_DONE) return false;

    sqlite3_stmt *ins = NULL;
    if (sqlite3_prepare_v2(conn, "INSERT OR IGNORE INTO post_search_grams (gram, post_id) VALUES (?, ?)", -1, &ins, NULL) != SQLITE_OK) return false;
    sqlite3_bind_int(ins, 2, post_id);
    for_each_gram(title, insert_gram, ins);
    for_each_gram(content, insert_gram, ins);
    sqlite3_finalize(ins);
    return true;
}

bool db_search_index_post(cwist_db *db, int post_id) {
    if (post_id <= 0) return false;
    sqlite3 *conn = fly_db_conn(db);
    if (!conn) return false;
    if (sqlite3_exec(conn, "SAVEPOINT search_index", NULL, NULL, NULL) != SQLITE_OK) return false;

    bool ok = false;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(conn, "SELECT title, content FROM posts WHERE id=?", -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, post_id);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            ok = index_post_text(conn, post_id,
                                 (const char *)sqlite3_column_text(stmt, 0),
                                 (const char *)sqlite3_column_text(stmt, 1));
        }
        sqlite3_finalize(stmt);
    }
    if (ok) {
        sqlite3_exec(conn, "RELEASE search_index", NULL, NULL, NULL);
    } else {
        sqlite3_exec(conn, "ROLLBACK TO search_index", NULL, NULL, NULL);
        sqlite3_exec(conn, "RELEASE search_index", NULL, NULL, NULL);
    }
    return ok;
}

bool db_search_migrate(cwist_db *db) {
    /* Deleting a post (directly or through its author's cascade) drops its
     * grams through the foreign key; post_id is indexed so that cascade does
     * not scan the whole table. */
    db_exec_sql(db, "CREATE TABLE IF NOT EXISTS post_search_grams ("
                    "gram TEXT NOT NULL, post_id INTEGER NOT NULL,"
                    "PRIMARY KEY (gram, post_id),"
                    "FOREIGN KEY(post_id) REFERENCES posts(id) ON DELETE CASCADE"
                    ") WITHOUT ROWID");
    db_exec_sql(db, "CREATE INDEX IF NOT EXISTS idx_post_search_grams_post ON post_search_grams(post_id)");

    char version[16] = {0};
    if (db_site_setting_get(db, "search_index_version", version, sizeof(version)) &&
        strcmp(version, SEARCH_INDEX_VERSION) == 0) {
        return true;
    }

    sqlite3 *conn = fly_db_conn(db);
    if (!conn) return false;
    if (!db_transaction_begin(db)) return false;
    /* Everything inside the transaction runs on this thread's connection;
     * db_exec_sql() goes through cwist's own handle and would block on it. */
    bool ok = sqlite3_exec(conn, "DELETE FROM post_search_grams", NULL, NULL, NULL) == SQLITE_OK;
    sqlite3_stmt *stmt = NULL;
    int indexed = 0;
    if (ok && sqlite3_prepare_v2(conn, "SELECT id, title, content FROM posts", -1, &stmt, NULL) == SQLITE_OK) {
        while (ok && sqlite3_step(stmt) == SQLITE_ROW) {
            ok = index_post_text(conn, sqlite3_column_int(stmt, 0),
                                 (const char *)sqlite3_column_text(stmt, 1),
                                 (const char *)sqlite3_column_text(stmt, 2));
            indexed++;
        }
        sqlite3_finalize(stmt);
    } else {
        ok = false;
    }
    if (ok) ok = db_site_setting_set(db, "search_index_version", SEARCH_INDEX_VERSION);
    if (ok && db_transaction_commit(db)) {
        fprintf(stderr, "[search] rebuilt trigram index for %d posts\n", indexed);
        return true;
    }
    fprintf(stderr, "[search] index rebuild failed: %s\n", sqlite3_errmsg(conn));
    db_transaction_rollback(db);
    return false;
}

/* ---- Query building ---- */

static void escape_like(cwist_sstring *out, const char *term) {
    cwist_sstring_append(out, "%");
    for (const char *p = term; *p; p++) {
        if (*p == '%' || *p == '_' || *p == '\\') cwist_sstring_append(out, "\\");
        cwist_sstring_append_len(out, p, 1);
    }
    cwist_sstring_append(out, "%");
}

void search_query_add_bind(search_query *q, const char *value) {
    if (q->nbinds == q->cap) {
        int cap = q->cap ? q->cap * 2 : 16;
        char **grown = (char **)realloc(q->binds, (size_t)cap * sizeof(char *));
        if (!grown) return;
        q->binds = grown;
        q->cap = cap;
    }
    q->binds[q->nbinds++] = strdup(value ? value : "");
}

typedef struct {
    char grams[SEARCH_TERM_MAX_BYTES][SEARCH_GRAM_MAX];
    int n;
} gram_set;

static void collect_gram(const char *gram, void *ud) {
    gram_set *set = (gram_set *)ud;
    for (int i = 0; i < set->n; i++) {
        if (strcmp(set->grams[i], gram) == 0) return;
    }
    if (set->n < SEARCH_TERM_MAX_BYTES) {
        snprintf(set->grams[set->n++], SEARCH_GRAM_MAX, "%s", gram);
    }
}

int search_split_terms(const char *query, char terms[SEARCH_MAX_TERMS][SEARCH_TERM_MAX_BYTES]) {
    int n = 0;
    const unsigned char *s = (const unsigned char *)(query ? query : "");
    while (*s && n < SEARCH_MAX_TERMS) {
        while (*s && is_space(*s)) s++;
        if (!*s) break;
        size_t len = 0;
        while (s[len] && !is_space(s[len])) len++;
        /* Cut over-long terms at a code point boundary. */
        size_t keep = len;
        if (keep >= SEARCH_TERM_MAX_BYTES) {
            keep = SEARCH_TERM_MAX_BYTES - 1;
            while (keep > 0 && (s[keep] & 0xC0) == 0x80) keep--;
        }
        memcpy(terms[n], s, keep);
        terms[n][keep] = '\0';
        if (keep > 0) n++;
        s += len;
    }
    return n;
}

void search_query_build(search_query *q, const char *query, const char *search_type) {
    memset(q, 0, sizeof(*q));
    q->where = cwist_sstring_create();
    q->title_rank = cwist_sstring_create();
    q->terms = search_split_terms(query, q->term);

    bool by_board = search_type && strcmp(search_type, "board") == 0;
    bool title_only = search_type && strcmp(search_type, "title") == 0;
    bool body_only = search_type && strcmp(search_type, "body") == 0;

    cwist_sstring *pattern = cwist_sstring_create();
    for (int t = 0; t < q->terms; t++) {
        cwist_sstring_assign(pattern, "");
        escape_like(pattern, q->term[t]);

        if (by_board) {
            cwist_sstring_append(q->where, " AND b.name LIKE ? ESCAPE '\\'");
            search_query_add_bind(q, pattern->data);
            continue;
        }

        if (utf8_cp_count(q->term[t]) >= 3) {
            gram_set set;
            set.n = 0;
            for_each_gram(q->term[t], collect_gram, &set);
            if (set.n > 0) {
                cwist_sstring_append(q->where, " AND p.id IN (SELECT post_id FROM post_search_grams WHERE gram IN (");
                for (int g = 0; g < set.n; g++) {
                    cwist_sstring_append(q->where, g ? ",?" : "?");
                    search_query_add_bind(q, set.grams[g]);
                }
                char having[64];
                snprintf(having, sizeof(having), ") GROUP BY post_id HAVING COUNT(*)=%d)", set.n);
                cwist_sstring_append(q->where, having);
            }
        }

        /* The grams only prove the pieces are present somewhere in the
         * post; LIKE confirms the term itself in the requested field. */
        if (title_only) {
            cwist_sstring_append(q->where, " AND p.title LIKE ? ESCAPE '\\'");
            search_query_add_bind(q, pattern->data);
        } else if (body_only) {
            cwist_sstring_append(q->where, " AND p.content LIKE ? ESCAPE '\\'");
            search_query_add_bind(q, pattern->data);
        } else {
            cwist_sstring_append(q->where, " AND (p.title LIKE ? ESCAPE '\\' OR p.content LIKE ? ESCAPE '\\')");
            search_query_add_bind(q, pattern->data);
            search_query_add_bind(q, pattern->data);
        }
    }

    /* Rank: posts whose title holds more of the terms come first. Its binds
     * follow the WHERE binds, in the order the SQL text lists them. */
    q->rank_first_bind = q->nbinds;
    if (!by_board && q->terms > 0) {
        for (int t = 0; t < q->terms; t++) {
            cwist_sstring_assign(pattern, "");
            escape_like(pattern, q->term[t]);
            cwist_sstring_append(q->title_rank, t ? " + (p.title LIKE ? ESCAPE '\\')" : "(p.title LIKE ? ESCAPE '\\')");
            search_query_add_bind(q, pattern->data);
        }
    }
    cwist_sstring_destroy(pattern);
}

void search_query_bind(const search_query *q, sqlite3_stmt *stmt, int *idx, bool with_rank) {
    int end = with_rank ? q->nbinds : q->rank_first_bind;
    for (int i = 0; i < end; i++) {
        sqlite3_bind_text(stmt, (*idx)++, q->binds[i], -1, SQLITE_TRANSIENT);
    }
}

void search_query_free(search_query *q) {
    for (int i = 0; i < q->nbinds; i++) free(q->binds[i]);
    free(q->binds);
    if (q->where) cwist_sstring_destroy(q->where);
    if (q->title_rank) cwist_sstring_destroy(q->title_rank);
    memset(q, 0, sizeof(*q));
}

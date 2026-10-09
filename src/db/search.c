#define _POSIX_C_SOURCE 200809L
#include "db.h"
#include "db_internal.h"
#include "fts5_mecab.h"
#include "search.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* FTS5 full-text search over posts, backed by the SQLite FTS5 module enabled
 * in the cwist amalgamation (-DSQLITE_ENABLE_FTS5).
 *
 * Index: a regular FTS5 table posts_fts(rowid, title, body) whose rowid is
 * the post id. Two tokenizers are supported (see fts5_mecab.h):
 *
 *   - "mecab":   Korean morphological tokens plus eojeol surfaces, so both
 *                "형태소" (morpheme inside a word) and exact surface phrases
 *                hit.
 *   - "trigram": substring matching for any text, terms need 3+ characters.
 *
 * db_search_migrate picks the tokenizer once (MeCab available -> mecab, else
 * trigram), stores it in the site_settings row "search_tokenizer" and stamps
 * "search_index_version"; a version bump or tokenizer change rebuilds the
 * whole index on the next start. The old hand-rolled trigram tables
 * (post_search_grams) are dropped.
 *
 * Queries keep the historical semantics: every whitespace-separated term
 * must match (AND), and a LIKE confirmation on the requested field
 * (title/body/both) always applies, so the FTS table only narrows candidates
 * and short terms (< 3 chars under the trigram tokenizer) still work through
 * LIKE alone. With the mecab tokenizer a Hangul term is branched:
 * the surface (quoted) OR the morpheme conjunction, e.g. 개발일지 ->
 * ("개발일지" OR (개발 AND 일지)), so it also matches text written as
 * "개발 일지". */
#define SEARCH_INDEX_VERSION "2"

/* Tokenizer chosen at migrate time; read by search_query_build. Workers
 * fork after migration, so the value is stable per process. Empty means
 * "no FTS index": queries degrade to pure LIKE filtering. */
static char g_search_tokenizer[16] = "";

static size_t utf8_cp_count(const char *s) {
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if ((*p & 0xC0) != 0x80) n++;
    }
    return n;
}

static bool term_is_match_safe(const char *term) {
    for (const unsigned char *p = (const unsigned char *)term; *p; p++) {
        if (*p < 0x20 || *p == '"' || *p == '(' || *p == ')' ||
            *p == ':' || *p == '*' || *p == '^') return false;
    }
    return true;
}

static bool term_has_hangul(const char *term) {
    /* Hangul syllables (U+AC00..U+D7A3) and jamo encode to lead bytes
     * 0xEA..0xED; Hangul compatibility jamo sit in the 0xE1..0xE3 range.
     * Checking the lead byte is enough for branching purposes. */
    for (const unsigned char *p = (const unsigned char *)term; *p; p++) {
        if (*p >= 0xEA && *p <= 0xED) return true;
    }
    return false;
}

/* ---- Index maintenance ---- */

static bool index_post_text(sqlite3 *conn, int post_id, const char *title, const char *content) {
    sqlite3_stmt *stmt = NULL;
    bool ok;
    if (title || content) {
        ok = sqlite3_prepare_v2(conn,
            "INSERT OR REPLACE INTO posts_fts (rowid, title, body) VALUES (?, ?, ?)",
            -1, &stmt, NULL) == SQLITE_OK;
        if (ok) {
            sqlite3_bind_int(stmt, 1, post_id);
            sqlite3_bind_text(stmt, 2, title ? title : "", -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 3, content ? content : "", -1, SQLITE_TRANSIENT);
            ok = sqlite3_step(stmt) == SQLITE_DONE;
        }
    } else {
        /* Post gone (or never existed): drop any stale FTS row so deletes
         * made without a rebuild do not leave dangling entries. */
        ok = sqlite3_prepare_v2(conn, "DELETE FROM posts_fts WHERE rowid=?", -1, &stmt, NULL) == SQLITE_OK;
        if (ok) {
            sqlite3_bind_int(stmt, 1, post_id);
            ok = sqlite3_step(stmt) == SQLITE_DONE;
        }
    }
    sqlite3_finalize(stmt);
    return ok;
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
        int rc = sqlite3_step(stmt);
        if (rc == SQLITE_ROW) {
            ok = index_post_text(conn, post_id,
                                 (const char *)sqlite3_column_text(stmt, 0),
                                 (const char *)sqlite3_column_text(stmt, 1));
        } else if (rc == SQLITE_DONE) {
            ok = index_post_text(conn, post_id, NULL, NULL);
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

static bool setting_get_conn(sqlite3 *conn, const char *key, char *out, size_t out_len) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(conn, "SELECT value FROM site_settings WHERE key=?", -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *v = (const char *)sqlite3_column_text(stmt, 0);
        if (v) {
            snprintf(out, out_len, "%s", v);
            found = true;
        }
    }
    sqlite3_finalize(stmt);
    return found;
}

static bool setting_set_conn(sqlite3 *conn, const char *key, const char *value) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(conn,
            "INSERT INTO site_settings (key, value) VALUES (?, ?) "
            "ON CONFLICT(key) DO UPDATE SET value=excluded.value",
            -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, value, -1, SQLITE_STATIC);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

static bool fts_table_exists(sqlite3 *conn) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(conn, "SELECT 1 FROM sqlite_master WHERE type='table' AND name='posts_fts'", -1, &stmt, NULL) != SQLITE_OK) return false;
    bool found = sqlite3_step(stmt) == SQLITE_ROW;
    sqlite3_finalize(stmt);
    return found;
}

/* (Re)create posts_fts with @p tokenizer; on failure drop the partial table
 * so the caller can retry with the trigram fallback. */
static bool fts_create_table(sqlite3 *conn, const char *tokenizer) {
    char sql[128];
    snprintf(sql, sizeof(sql), "CREATE VIRTUAL TABLE posts_fts USING fts5(title, body, tokenize='%s')", tokenizer);
    if (sqlite3_exec(conn, sql, NULL, NULL, NULL) == SQLITE_OK) return true;
    sqlite3_exec(conn, "DROP TABLE IF EXISTS posts_fts", NULL, NULL, NULL);
    return false;
}

bool db_search_migrate(cwist_db *db) {
    sqlite3 *conn = fly_db_conn(db);
    if (!conn) return false;

    /* The old hand-rolled trigram inverted index is gone; dropping it is
     * idempotent and cheap. */
    sqlite3_exec(conn, "DROP TABLE IF EXISTS post_search_grams", NULL, NULL, NULL);

    char version[16] = {0};
    char stored_tok[16] = {0};
    setting_get_conn(conn, "search_index_version", version, sizeof(version));
    setting_get_conn(conn, "search_tokenizer", stored_tok, sizeof(stored_tok));

    const char *preferred = fts5_search_preferred_tokenizer();
    bool fresh = strcmp(version, SEARCH_INDEX_VERSION) != 0 ||
                 strcmp(stored_tok, preferred) != 0 ||
                 !fts_table_exists(conn);
    snprintf(g_search_tokenizer, sizeof(g_search_tokenizer), "%s",
             fts_table_exists(conn) && stored_tok[0] ? stored_tok : preferred);
    if (!fresh) return true;

    if (!db_transaction_begin(db)) return false;
    bool ok = sqlite3_exec(conn, "DROP TABLE IF EXISTS posts_fts", NULL, NULL, NULL) == SQLITE_OK;
    const char *chosen = NULL;
    if (ok) {
        if (strcmp(preferred, "mecab") == 0 && fts_create_table(conn, "mecab")) {
            chosen = "mecab";
        } else if (fts_create_table(conn, "trigram")) {
            chosen = "trigram";
        } else {
            ok = false;
        }
    }
    int indexed = 0;
    sqlite3_stmt *stmt = NULL;
    if (ok && sqlite3_prepare_v2(conn, "SELECT id, title, content FROM posts", -1, &stmt, NULL) == SQLITE_OK) {
        while (ok && sqlite3_step(stmt) == SQLITE_ROW) {
            ok = index_post_text(conn, sqlite3_column_int(stmt, 0),
                                 (const char *)sqlite3_column_text(stmt, 1),
                                 (const char *)sqlite3_column_text(stmt, 2));
            indexed++;
        }
        sqlite3_finalize(stmt);
    } else if (ok) {
        ok = false;
    }
    if (ok) ok = setting_set_conn(conn, "search_tokenizer", chosen);
    if (ok) ok = setting_set_conn(conn, "search_index_version", SEARCH_INDEX_VERSION);
    if (ok && db_transaction_commit(db)) {
        snprintf(g_search_tokenizer, sizeof(g_search_tokenizer), "%s", chosen);
        fprintf(stderr, "[search] rebuilt FTS5 index (%s tokenizer) for %d posts\n", chosen, indexed);
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

int search_split_terms(const char *query, char terms[SEARCH_MAX_TERMS][SEARCH_TERM_MAX_BYTES]) {
    int n = 0;
    const unsigned char *s = (const unsigned char *)(query ? query : "");
    while (*s && n < SEARCH_MAX_TERMS) {
        while (*s && (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r' || *s == '\f' || *s == '\v')) s++;
        if (!*s) break;
        size_t len = 0;
        while (s[len] && !(s[len] == ' ' || s[len] == '\t' || s[len] == '\n' || s[len] == '\r' || s[len] == '\f' || s[len] == '\v')) len++;
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

/* Build the FTS5 MATCH expression for one term, honoring the tokenizer the
 * index was actually built with. Returns an empty string when the term must
 * be left to LIKE alone (unsafe characters, or too short for trigram).
 * Sets @p branched when the mecab morpheme branch fired: the exact-substring
 * LIKE confirmation would reject legitimate morphological matches (eojeol
 * boundaries differ), so the caller skips it for such terms. */
static void build_fts_expr(cwist_sstring *out, const char *term, bool *branched) {
    bool is_mecab = strcmp(g_search_tokenizer, "mecab") == 0;
    bool is_trigram = strcmp(g_search_tokenizer, "trigram") == 0;
    if ((!is_mecab && !is_trigram) || !term_is_match_safe(term)) return;

    if (is_trigram && utf8_cp_count(term) < 3) return; /* LIKE-only */

    if (is_mecab && term_has_hangul(term)) {
        char morph[8][64];
        int n = fts5_mecab_split_term(term, morph, 8);
        if (n > 1) {
            /* Branch: exact surface OR morpheme conjunction. */
            cwist_sstring_append(out, "(\"");
            cwist_sstring_append(out, term);
            cwist_sstring_append(out, "\" OR (");
            for (int i = 0; i < n; i++) {
                cwist_sstring_append(out, i ? " AND " : "");
                cwist_sstring_append(out, morph[i]);
            }
            cwist_sstring_append(out, "))");
            *branched = true;
            return;
        }
    }
    /* Plain term / trigram substring: a quoted string matches the eojeol
     * surface under mecab and a substring under trigram. */
    cwist_sstring_append(out, "\"");
    cwist_sstring_append(out, term);
    cwist_sstring_append(out, "\"");
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
    cwist_sstring *expr = cwist_sstring_create();
    for (int t = 0; t < q->terms; t++) {
        cwist_sstring_assign(pattern, "");
        escape_like(pattern, q->term[t]);

        if (by_board) {
            cwist_sstring_append(q->where, " AND b.name LIKE ? ESCAPE '\\'");
            search_query_add_bind(q, pattern->data);
            continue;
        }

        /* FTS5 narrows candidates; LIKE below confirms the exact substring
         * in the requested field, which also covers terms the FTS expression
         * skipped (short trigram terms, unsafe characters). Branched mecab
         * terms skip LIKE: the whole point of the branch is matching across
         * eojeol boundaries the raw substring would reject. */
        cwist_sstring_assign(expr, "");
        bool branched = false;
        build_fts_expr(expr, q->term[t], &branched);
        if (expr->size > 0) {
            cwist_sstring_append(q->where, " AND p.id IN (SELECT rowid FROM posts_fts WHERE posts_fts MATCH ?)");
            search_query_add_bind(q, expr->data);
        }

        if (branched) continue;

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
    cwist_sstring_destroy(expr);
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

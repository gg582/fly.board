#define _POSIX_C_SOURCE 200809L
#include "fts5_mecab.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HAVE_MECAB
#include <mecab.h>

/* FTS5 does not expose its extension API through the C header; sqlite3.h only
 * defines fts5_api when the amalgamation was built with SQLITE_ENABLE_FTS5,
 * which the cwist Makefile now sets for lib/sqlite3/sqlite3.o and the app
 * defines mirror. */

/* ---- MeCab instance management -------------------------------------------
 *
 * Tokenizer objects are created per FTS5 table use; sharing one MeCab tagger
 * process-wide behind a mutex avoids hundreds of tagger opens. MeCab analysis
 * is read-only, so a single tagger is safe to time-slice across threads. */
static mecab_t *g_tagger;
static pthread_mutex_t g_tagger_lock = PTHREAD_MUTEX_INITIALIZER;

/* Candidate dictionary locations, tried in order when MECAB_DIC_DIR is not
 * set: mecab-ko-dic installs into the distro prefix on Debian/Ubuntu and
 * into /usr/local elsewhere. */
static const char *const g_dic_candidates[] = {
    "/usr/lib/x86_64-linux-gnu/mecab/dic/mecab-ko-dic",
    "/usr/local/lib/mecab/dic/mecab-ko-dic",
    NULL,
};

static mecab_t *tagger_open(void) {
    const char *env = getenv("MECAB_DIC_DIR");
    if (env && *env) {
        char args[1100];
        snprintf(args, sizeof(args), "-d %s", env);
        return mecab_new2(args);
    }
    for (int i = 0; g_dic_candidates[i]; i++) {
        char args[1100];
        snprintf(args, sizeof(args), "-d %s", g_dic_candidates[i]);
        mecab_t *m = mecab_new2(args);
        if (m) return m;
    }
    return mecab_new(0, NULL);
}

static mecab_t *tagger_get(void) {
    pthread_mutex_lock(&g_tagger_lock);
    if (!g_tagger) g_tagger = tagger_open();
    mecab_t *m = g_tagger;
    pthread_mutex_unlock(&g_tagger_lock);
    return m;
}

/* ---- FTS5 tokenizer vtable ----------------------------------------------- */

static int mecab_xCreate(void *pCtx, const char **azArg, int nArg, Fts5Tokenizer **ppOut) {
    (void)pCtx;
    (void)azArg;
    (void)nArg;
    if (!tagger_get()) return SQLITE_ERROR;
    /* The tokenizer handle is opaque to us; any private allocation works. */
    *ppOut = (Fts5Tokenizer *)calloc(1, sizeof(int));
    return *ppOut ? SQLITE_OK : SQLITE_NOMEM;
}

static void mecab_xDelete(Fts5Tokenizer *pTok) {
    free(pTok);
}

static bool mecab_is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

/* Tokenize @p pText as eojeol (whitespace-separated words). For every eojeol
 * we emit two kinds of tokens:
 *
 *   1. the eojeol surface itself (its own position), so a phrase/exact
 *      surface query such as "개발 일지" still matches;
 *   2. each MeCab morpheme, the first with FTS5_TOKEN_COLOCATED (same
 *      position as the eojeol) and the rest at following positions, so
 *      morpheme-level AND queries match e.g. 개발+일지 inside 개발일지.
 *
 * Positions advance by the morpheme count, keeping eojeol positions
 * consecutive across a sentence. */
static int mecab_xTokenize(Fts5Tokenizer *pTok, void *pCtx, int flags,
                           const char *pText, int nText,                           int (*xToken)(void *, int, const char *, int, int, int)) {
    (void)pTok;
    (void)flags;
    mecab_t *tagger = tagger_get();
    if (!tagger) return SQLITE_ERROR;

    int pos = 0;
    int i = 0;
    while (i < nText) {
        while (i < nText && mecab_is_space(pText[i])) i++;
        int start = i;
        while (i < nText && !mecab_is_space(pText[i])) i++;
        int len = i - start;
        if (len <= 0) break;

        int rc = xToken(pCtx, 0, pText + start, len, start, start + len);
        if (rc != SQLITE_OK) return rc;

        /* mecab_sparse_tonode() needs a NUL-terminated string. */
        char stack_buf[1024];
        char *heap = NULL;
        const char *span = pText + start;
        if (len < (int)sizeof(stack_buf)) {
            memcpy(stack_buf, span, (size_t)len);
            stack_buf[len] = '\0';
            span = stack_buf;
        } else {
            heap = strndup(span, (size_t)len);
            if (!heap) return SQLITE_NOMEM;
            span = heap;
        }

        pthread_mutex_lock(&g_tagger_lock);
        const mecab_node_t *node = mecab_sparse_tonode(tagger, span);
        int first = 1;
        for (; node; node = node->next) {
            if (node->length == 0) continue;
            /* surface points into MeCab's lattice; copy-safe during the call */
            rc = xToken(pCtx, first ? FTS5_TOKEN_COLOCATED : 0,
                        node->surface, (int)node->length, start, start + len);
            first = 0;
            pos++;
            if (rc != SQLITE_OK) break;
        }
        pthread_mutex_unlock(&g_tagger_lock);
        free(heap);
        if (rc != SQLITE_OK) return rc;
        if (first) pos++; /* no morphemes emitted: eojeol still occupies a slot */
    }
    return SQLITE_OK;
}

static fts5_tokenizer g_mecab_tokenizer = {    .xCreate = mecab_xCreate,
    .xDelete = mecab_xDelete,
    .xTokenize = mecab_xTokenize,
};

#endif /* HAVE_MECAB */

bool fts5_search_register_conn(sqlite3 *conn) {
    if (!conn) return false;
#ifdef HAVE_MECAB
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(conn, "SELECT fts5(?1)", -1, &stmt, NULL) != SQLITE_OK) return false;
    fts5_api *api = NULL;
    sqlite3_bind_pointer(stmt, 1, &api, "fts5_api_ptr", NULL);
    bool ok = sqlite3_step(stmt) == SQLITE_ROW && api != NULL;
    sqlite3_finalize(stmt);
    if (!ok || !api) return false;
    if (api->iVersion < 2) return false;
    /* xCreateTokenizer fails (SQLITE_BUSY/ERROR) if the name is taken; this
     * connection may already have it. Treat "already registered" as success
     * by probing the tagger instead. */
    if (api->xCreateTokenizer(api, "mecab", NULL, &g_mecab_tokenizer, NULL) != SQLITE_OK) {
        /* fall through: maybe registered earlier on this connection */
    }
    return tagger_get() != NULL;
#else
    (void)conn;
    return false;
#endif
}

const char *fts5_search_preferred_tokenizer(void) {
#ifdef HAVE_MECAB
    return tagger_get() ? "mecab" : "trigram";
#else
    return "trigram";
#endif
}

int fts5_mecab_split_term(const char *term, char out[][64], int max) {
    if (!term || !out || max <= 0) return 0;
#ifdef HAVE_MECAB
    mecab_t *tagger = tagger_get();
    if (!tagger) return 0;
    int n = 0;
    pthread_mutex_lock(&g_tagger_lock);
    const mecab_node_t *node = mecab_sparse_tonode(tagger, term);
    for (; node && n < max; node = node->next) {
        if (node->length == 0) continue;
        size_t len = node->length < 63 ? node->length : 63;
        memcpy(out[n], node->surface, len);
        out[n][len] = '\0';
        n++;
    }
    pthread_mutex_unlock(&g_tagger_lock);
    return n;
#else
    (void)term;
    (void)out;
    return 0;
#endif
}

#ifndef FLYBOARD_DB_FTS5_MECAB_H
#define FLYBOARD_DB_FTS5_MECAB_H

#include <sqlite3.h>
#include <stdbool.h>

/* FTS5-based post search with a MeCab (mecab-ko-dic) morphological tokenizer.
 *
 * Two tokenizer personalities exist at runtime:
 *   - "mecab":   compiled with HAVE_MECAB and the dictionary loads. Korean
 *                text is indexed by morpheme; eojeol (whitespace-separated
 *                word) surfaces are also emitted so exact surface matches
 *                still hit.
 *   - "trigram": built into SQLite >= 3.34; substring matching of any text
 *                with terms of 3+ characters. Used when MeCab is unavailable.
 *
 * The choice is made once per database in db_search_migrate and stored in the
 * site_settings row "search_tokenizer"; search_query_build reads it back so
 * query branching matches the index actually built. */

/* Register the "mecab" FTS5 tokenizer on @p conn. FTS5 tokenizers are
 * registered per database connection, so this runs from
 * db_configure_connection() for every thread-local connection. Returns true
 * only when the tokenizer was registered AND a MeCab dictionary opened, so it
 * doubles as the runtime availability probe. Always defined; false when built
 * without HAVE_MECAB. */
bool fts5_search_register_conn(sqlite3 *conn);

/* Name of the tokenizer to use for new indexes: "mecab" when available on
 * this build/runtime, otherwise "trigram". */
const char *fts5_search_preferred_tokenizer(void);

/* Split a single query term into morpheme surfaces with MeCab (query-side
 * branching). Returns the morpheme count (0 when unavailable or on error,
 * in which case @p out is untouched). */
int fts5_mecab_split_term(const char *term, char out[][64], int max);

#endif

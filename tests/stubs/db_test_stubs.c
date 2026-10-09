/* Stubs for symbols the src/db objects under test reference but that belong
 * to subsystems outside the scope of these unit tests (search index, file
 * dedup, report/search migrations, view counters, engine bump).
 *
 * Used by: test_db_users_boards.c, test_db_posts_tags_series.c,
 *          test_db_db_email.c
 */
#include "db/db.h"
#include "db/search.h"
#include "engine/bdr.h"
#include <sqlite3.h>
#include <string.h>

void db_file_cleanup_duplicates(cwist_db *db) { (void)db; }
bool db_report_migrate(cwist_db *db) { (void)db; return true; }
bool db_search_migrate(cwist_db *db) { (void)db; return true; }
bool db_search_index_post(cwist_db *db, int post_id) { (void)db; (void)post_id; return true; }

/* search_query is opaque-ish here; no-op is fine because the tests never
 * exercise full-text search.  Match the real signatures from db/search.h. */
void search_query_build(search_query *q, const char *query, const char *search_type) {
    (void)q; (void)query; (void)search_type;
}
void search_query_add_bind(search_query *q, const char *value) { (void)q; (void)value; }
void search_query_bind(const search_query *q, sqlite3_stmt *stmt, int *idx, bool with_rank) {
    (void)q; (void)stmt; (void)idx; (void)with_rank;
}
void search_query_free(search_query *q) { (void)q; }

void db_comment_close_thread(void) {}
void db_board_tree_close_thread(void) {}
void engine_bdr_bump(void) {}

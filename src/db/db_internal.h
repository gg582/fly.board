#ifndef DB_INTERNAL_H
#define DB_INTERNAL_H

#include <sqlite3.h>
#include <cjson/cJSON.h>

/* Apply concurrency-safe defaults to a freshly opened SQLite connection:
 * busy timeout for graceful contention handling and WAL mode so readers and
 * writers do not block each other. */
void db_configure_connection(sqlite3 *conn);

/* Re-open the auxiliary databases after a fork() so each process owns its own
 * SQLite file descriptor and page cache. */
void db_comment_reopen(void);
void db_board_tree_reopen(void);

cJSON *db_sqlite3_rows_to_json(sqlite3_stmt *stmt);
cJSON *db_sqlite3_row_to_json(sqlite3_stmt *stmt);

#endif

#ifndef DB_INTERNAL_H
#define DB_INTERNAL_H

#include <sqlite3.h>
#include <cjson/cJSON.h>
#include "db.h"

#define FLY_DB_MAIN_PATH "data/blog.db"

/* Apply concurrency-safe defaults to a freshly opened SQLite connection:
 * busy timeout for graceful contention handling and WAL mode so readers and
 * writers do not block each other.  Returns false if a required pragma fails,
 * so callers can abort instead of running with unsafe defaults. */
bool db_configure_connection(sqlite3 *conn);

/* Return this thread's own SQLite connection for the main database.
 *
 * The whole server used to share a single sqlite3* across every worker
 * thread, which serialized ALL database work (reads included) behind the
 * connection mutex and made BEGIN IMMEDIATE/COMMIT from one request govern
 * statements issued by other requests.  With WAL mode, giving each thread a
 * private connection lets readers run concurrently and confines writer
 * contention to actual writers.  Connections are opened lazily on first use
 * and live for the lifetime of the thread. */
sqlite3 *fly_db_conn(cwist_db *db);

/* Drop this thread's cached connection pointer without closing it.  Must be
 * called in the child after fork(): the pointer refers to the parent's
 * connection copy and must never be used or closed there. */
void fly_db_conn_forget(void);

/* Request a passive WAL checkpoint on the main database.  Safe to call after
 * large writes or before shutdown; failures are logged but not fatal. */
bool db_checkpoint(cwist_db *db);

/* Re-open the auxiliary databases after a fork() so each process owns its own
 * SQLite file descriptor and page cache. */
void db_comment_reopen(void);
void db_board_tree_reopen(void);

cJSON *db_sqlite3_rows_to_json(sqlite3_stmt *stmt);
cJSON *db_sqlite3_row_to_json(sqlite3_stmt *stmt);

#endif

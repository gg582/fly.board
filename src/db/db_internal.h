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

/* Close the calling thread's per-thread connections (main/comments/
 * board_tree) if it holds any.  cwist forks worker children from the main
 * thread inside cwist_app_listen(); pthread TLS values survive fork for the
 * forking thread, so an open per-thread connection here would be inherited
 * by every child as a dead copy of the parent's sqlite state that the child
 * must never close (it shares the parent's WAL file descriptors) and
 * LeakSanitizer then reports in every child.  Shutting them down just
 * before the fork keeps the inherited TLS state empty. */
void fly_db_close_thread_conns(void);

/* Request a passive WAL checkpoint on the main database.  Safe to call after
 * large writes or before shutdown; failures are logged but not fatal. */
bool db_checkpoint(cwist_db *db);

/* Run "PRAGMA wal_checkpoint(TRUNCATE)" on a short-lived connection; silently
 * ignores SQLITE_BUSY/SQLITE_LOCKED.  Bracket with the fork gate. */
void db_wal_checkpoint_truncate(void);

/* Shared (process-wide) batched view counters (src/db/post.c).
 * db_view_counters_init must run before cwist forks workers; the supervisor's
 * periodic worker calls db_view_counters_flush every 30 s. */
bool db_view_counters_init(void);
void db_view_counters_flush(void);

/* Re-open the auxiliary databases after a fork() so each process owns its own
 * SQLite file descriptor and page cache. */
void db_comment_reopen(void);
void db_board_tree_reopen(void);
void db_comment_close_thread(void);
void db_board_tree_close_thread(void);

/* The calling thread's connection to the auxiliary databases, opened lazily.
 * Used by the FlyWire replica applier (src/engine/flywire.c) to apply journal
 * rows directly with SQL. */
sqlite3 *db_comment_conn(void);
sqlite3 *db_board_tree_conn(void);

/* Same as db_pqc_keys_sync() on a raw connection (backup tooling); with
 * register_current false only the stored keys are loaded. */
bool pqc_keys_sync_conn(sqlite3 *conn, bool register_current);

cJSON *db_sqlite3_rows_to_json(sqlite3_stmt *stmt);
cJSON *db_sqlite3_row_to_json(sqlite3_stmt *stmt);

/* Raw-connection cores of the webmail store, shared with the standalone
 * mail-import tool (src/tools/mail_import.c). */
int db_email_create_conn(sqlite3 *conn, int owner_id, const char *folder,
                         const char *from_addr, const char *to_addrs,
                         const char *subject, const char *body_text,
                         const char *message_id, const char *in_reply_to);
bool db_email_append_body_conn(sqlite3 *conn, int owner_id, int id, const char *extra);

#endif

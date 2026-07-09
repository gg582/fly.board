#include "engine/db.h"
#include "db/db.h"
#include "db/db_internal.h"
#include "cwist/board_tree.h"
#include <cwist/core/log.h>
#include <sqlite3.h>
#include <pthread.h>

#define DB_PATH   "data/blog.db"

/* CWIST may fork worker processes before entering the event loop.  SQLite
 * connections must not be shared across processes: file descriptors, page
 * caches and prepared statement state become inconsistent after fork().  We
 * keep a reference to the app so that a child-handler installed with
 * pthread_atfork() can replace every connection with a fresh one. */
static cwist_app *g_app = NULL;

static void reopen_databases_after_fork(void) {
    cwist_db *db = g_app ? cwist_app_get_db(g_app) : NULL;
    if (db && db->conn) {
        const char *path = sqlite3_db_filename(db->conn, "main");
        sqlite3 *new_conn = NULL;
        if (sqlite3_open(path ? path : DB_PATH, &new_conn) == SQLITE_OK) {
            sqlite3_close(db->conn);
            db->conn = new_conn;
            db_configure_connection(db->conn);
        } else {
            sqlite3_close(new_conn);
        }
    }
    db_comment_reopen();
    db_board_tree_reopen();
}

bool engine_db_init(cwist_app *app, cwist_db **db_out) {
    cwist_error_t dberr = cwist_app_use_db(app, DB_PATH);
    if (dberr.errtype != CWIST_ERR_INT16 || dberr.error.err_i16 != 0) {
        FLY_LOG_ERROR("Failed to open database");
        return false;
    }
    CWIST_LOG_INFO("Database opened: %s", DB_PATH);

    cwist_db *db = cwist_app_get_db(app);
    db_configure_connection(db->conn);
    if (!db_init(db)) {
        FLY_LOG_ERROR("Failed to initialize database schema");
        return false;
    }
    CWIST_LOG_INFO("Database schema initialized");

    if (!db_comment_init("data/comments.db")) {
        FLY_LOG_ERROR("Failed to initialize comments database");
        return false;
    }
    CWIST_LOG_INFO("Comments database initialized");

    if (!db_board_tree_init("data/board_tree.db")) {
        FLY_LOG_ERROR("Failed to initialize board tree database");
        return false;
    }
    CWIST_LOG_INFO("Board tree database initialized");

    g_app = app;
    pthread_atfork(NULL, NULL, reopen_databases_after_fork);

    *db_out = db;
    return true;
}

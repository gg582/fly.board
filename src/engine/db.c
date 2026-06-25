#include "engine/db.h"
#include "db/db.h"
#include "cwist/board_tree.h"
#include <cwist/core/log.h>

#define DB_PATH   "data/blog.db"

bool engine_db_init(cwist_app *app, cwist_db **db_out) {
    cwist_error_t dberr = cwist_app_use_db(app, DB_PATH);
    if (dberr.errtype != CWIST_ERR_INT16 || dberr.error.err_i16 != 0) {
        FLY_LOG_ERROR("Failed to open database");
        return false;
    }
    CWIST_LOG_INFO("Database opened: %s", DB_PATH);

    cwist_db *db = cwist_app_get_db(app);
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

    *db_out = db;
    return true;
}

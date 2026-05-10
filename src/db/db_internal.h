#ifndef DB_INTERNAL_H
#define DB_INTERNAL_H

#include <sqlite3.h>
#include <cjson/cJSON.h>

cJSON *db_sqlite3_rows_to_json(sqlite3_stmt *stmt);
cJSON *db_sqlite3_row_to_json(sqlite3_stmt *stmt);

#endif

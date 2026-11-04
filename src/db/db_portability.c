#include "db_portability.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *fly_db_main_location(void) {
    const char *location = getenv("FLY_DB_LOCATION");
    return location && location[0] ? location : FLY_DB_DEFAULT_PATH;
}

bool fly_db_backend_supported(char *error, size_t error_size) {
    const char *backend = getenv("FLY_DB_BACKEND");
    if (!backend || !backend[0] || strcmp(backend, "sqlite") == 0) return true;
    if (error && error_size > 0) {
        snprintf(error, error_size, "unsupported FLY_DB_BACKEND '%s' (only 'sqlite' is implemented)", backend);
    }
    return false;
}

int fly_db_open_main(sqlite3 **conn) {
    if (!conn) return SQLITE_MISUSE;
    *conn = NULL;
    int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI;
    return sqlite3_open_v2(fly_db_main_location(), conn, flags, NULL);
}

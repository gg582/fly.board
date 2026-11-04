#ifndef DB_PORTABILITY_H
#define DB_PORTABILITY_H

#include <stdbool.h>
#include <stddef.h>
#include <sqlite3.h>

#define FLY_DB_DEFAULT_PATH "data/blog.db"

const char *fly_db_main_location(void);
bool fly_db_backend_supported(char *error, size_t error_size);
int fly_db_open_main(sqlite3 **conn);

#endif

#ifndef ENGINE_DB_H
#define ENGINE_DB_H

#include <cwist/sys/app/app.h>
#include <stdbool.h>

bool engine_db_init(cwist_app *app, cwist_db **db_out);

#endif

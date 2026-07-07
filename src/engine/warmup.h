#ifndef FLYBOARD_WARMUP_H
#define FLYBOARD_WARMUP_H

#include "db/db.h"

/* Pre-render hot pages into the in-memory page cache at startup.
 * This is a lightweight form of SSG: the server starts with cache already
 * warm, so the first request for the home page does not hit the DB. */
void page_cache_warmup(cwist_db *db);

#endif

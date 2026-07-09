#ifndef FLYBOARD_REQSHARE_H
#define FLYBOARD_REQSHARE_H

#include <stdbool.h>
#include <cwist/core/sstring/sstring.h>

/* Per-process request coalescing.
 *
 * When many concurrent requests hit the same expensive page (home, board,
 * post detail), the first request becomes the "leader" and does the DB work
 * and render.  Subsequent requests for the same key wait and then receive a
 * copy of the leader's rendered response.  This collapses N identical DB
 * transactions into one under burst load (C100K/C1M), cuts peak CPU, and
 * reduces memory pressure from concurrent renders.
 *
 * The module is layered on top of page_cache_*; the leader is still expected
 * to insert the final response into the page cache so later requests can be
 * served without entering the coalescer at all. */

bool reqshare_init(void);
void reqshare_cleanup(void);

/* If no request is in flight for `key`, returns NULL and sets *leader=true.
 * The caller must produce the response and then call reqshare_finish().
 *
 * If a request is already in flight or a fresh result is still available,
 * returns a newly allocated cwist_sstring copy of the response and sets
 * *leader=false.  The caller owns the returned sstring and must destroy it. */
cwist_sstring *reqshare_wait_or_start(const char *key, bool *leader);

/* Store the result for `key` and wake any waiters.  The supplied sstring is
 * copied; the caller keeps ownership of `html`.  Passing NULL finishes with an
 * empty result (waiters will get NULL). */
void reqshare_finish(const char *key, cwist_sstring *html);

#endif

#ifndef UTILS_POST_SCHEDULE_H
#define UTILS_POST_SCHEDULE_H

#include <cwist/core/db/sql.h>

/* Scheduled posts go public by the clock, not by a write, so nothing would
 * invalidate the cached listings or send the NATS "published" event when
 * one comes due. The earliest pending publish time lives in memory shared
 * by every worker; a thread in the master process wakes once it passes,
 * bumps a generation that is part of every page cache key (and the route
 * BDR version), announces the post, and loads the next due time. */

/* Call once before the workers fork. */
void post_schedule_init(cwist_db *db);

/* Start/stop the master-process thread. Start after post_schedule_init(). */
void post_schedule_start(cwist_db *db);
void post_schedule_stop(void);

/* A post was saved with this UTC "YYYY-MM-DD HH:MM:SS" publish time. */
void post_schedule_note(const char *publish_at);

/* Send the NATS event for every public post not announced yet. Safe to
 * call from any process: each post is claimed in the database first. */
void post_schedule_announce(cwist_db *db);

/* A post was published or unpublished by hand: stale every worker's pages. */
void post_schedule_bump(void);

unsigned post_schedule_generation(void);

#endif

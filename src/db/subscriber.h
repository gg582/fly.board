#ifndef FLYBOARD_DB_SUBSCRIBER_H
#define FLYBOARD_DB_SUBSCRIBER_H

#include "db.h"

/* Email newsletter subscribers (table created in db_migrate).
 *
 * A subscription starts as 'pending': db_subscriber_add stores the address
 * with a fresh token and the caller mails a confirm link. Confirming flips
 * it to 'active'. Re-adding an existing address resets it to pending with a
 * new token so a mistyped or stale entry can be re-confirmed; the UNIQUE
 * email constraint keeps one row per address.
 *
 * Not journaled to FlyWire: subscription state is primary-node local. */

/* Insert a new pending subscriber, or reset an existing address to pending
 * with the given token. Returns false on failure. */
bool db_subscriber_add(cwist_db *db, const char *email, const char *token);

/* Activate the subscription holding this token. Returns false for an
 * unknown token or one that is not pending. */
bool db_subscriber_confirm(cwist_db *db, const char *token);

/* Delete the subscription holding this token (one-click unsubscribe).
 * Returns false for an unknown token. */
bool db_subscriber_unsubscribe(cwist_db *db, const char *token);

/* All subscribers, newest first: [{id, email, token, status, created_at,
 * confirmed_at}]. */
cJSON *db_subscriber_list(cwist_db *db, int limit, int offset);

/* Active subscribers for the announcement worker: [{email, token}]. */
cJSON *db_subscriber_list_active(cwist_db *db);

/* Number of active subscribers. */
int db_subscriber_count(cwist_db *db);

/* Remove one subscriber by id (admin). */
bool db_subscriber_delete(cwist_db *db, int id);

/* Drop pending subscriptions older than a day (stale confirm links). */
void db_subscriber_purge_pending(cwist_db *db);

#endif

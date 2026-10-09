#ifndef FLYBOARD_DB_GUESTBOOK_H
#define FLYBOARD_DB_GUESTBOOK_H

#include <cwist/core/db/sql.h>
#include <cjson/cJSON.h>
#include <stdbool.h>

/* Per-user guestbook (see src/db/guestbook.c).
 *
 * Every entry belongs to a profile owner; the writer is either a logged-in
 * member (author_uid set, author_name = username) or an anonymous visitor
 * (author_uid NULL, author_name = the display name they typed). Whether
 * anonymous posts are accepted is the owner-controlled users.guestbook_anon
 * flag. Mutations are journaled for FlyWire under the "guestbook" entity. */

int db_guestbook_create(cwist_db *db, int owner_uid, int author_uid,
                        const char *author_name, const char *content);
/* Entries of one owner, newest first, as a cJSON array (house list style). */
cJSON *db_guestbook_list(cwist_db *db, int owner_uid, int offset, int limit);
int db_guestbook_count(cwist_db *db, int owner_uid);
/* One entry by id: {id, owner_uid, author_uid, author_name, content,
 * created_at}; NULL when absent. Caller frees with cJSON_Delete. */
cJSON *db_guestbook_get(cwist_db *db, int id);
/* Delete allowed for the profile owner, a site admin, or the entry's author
 * (author_uid must match actor_uid). Journals the FlyWire delete. */
bool db_guestbook_delete(cwist_db *db, int id, int actor_uid, const char *actor_role);
/* Owner toggle for anonymous posts (users.guestbook_anon). Journals the user
 * row so the flag replicates like any other profile change. */
bool db_guestbook_set_anon(cwist_db *db, int uid, bool allow);
bool db_guestbook_anon_allowed(cwist_db *db, int owner_uid);

#endif

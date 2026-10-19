#ifndef FLYBOARD_DB_REVISION_H
#define FLYBOARD_DB_REVISION_H

#include "db.h"

/* Post revision history (table created in db_migrate).
 *
 * handler_post_edit_post stores the pre-edit title/content/summary inside
 * the edit savepoint, so each accepted edit leaves one snapshot of the
 * version it replaced. The list feeds the editor's "Revision history"
 * section; loading a revision fills the form and a normal Update applies
 * it (which itself stores another snapshot — reverting is non-destructive).
 * Kept local per node, like the rest of the editor experience. */

#define DB_REVISION_KEEP 30

/* Store one snapshot and prune the post's history to the newest
 * DB_REVISION_KEEP rows. Must be called inside the caller's transaction so
 * the snapshot commits or rolls back with the edit. */
bool db_revision_save(cwist_db *db, int post_id, const char *title,
                      const char *content, const char *summary, int editor_id);

/* Newest first: [{id, title, editor_name, created_at}] (no content). */
cJSON *db_revision_list(cwist_db *db, int post_id, int limit);

/* One full revision row [{id, post_id, title, content, summary, created_at}]
 * or NULL. */
cJSON *db_revision_get(cwist_db *db, int id);

int db_revision_count(cwist_db *db, int post_id);

#endif

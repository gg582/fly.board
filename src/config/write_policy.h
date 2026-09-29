#ifndef FLY_WRITE_POLICY_H
#define FLY_WRITE_POLICY_H

#include <cwist/core/db/sql.h>
#include <stdbool.h>

/* Who may create posts or comments. Set from the admin dashboard, stored in
 * the site_settings table, and mirrored into shared memory so every forked
 * worker process enforces a change immediately. */
/* Three tiers, each including the ones above it. */
typedef enum {
    WRITE_SCOPE_GUEST = 0,  /* Guests, members and admins (the default, as before). */
    WRITE_SCOPE_MEMBER = 1, /* Registered members and admins. */
    WRITE_SCOPE_ADMIN = 2,  /* Admins only. */
} write_scope_t;

typedef struct {
    write_scope_t post;
    write_scope_t comment;
    /* New and edited posts must name an existing board. */
    bool require_board;
} write_policy_t;

/* Load the stored policy. Call once after the database is migrated and
 * before cwist_app_listen() forks the workers. */
bool write_policy_init(cwist_db *db);

write_policy_t write_policy_get(void);

/* Bumped by every write_policy_set(). Page cache keys carry it, since the
 * page cache is per process and cached pages embed the New Post button and
 * the comment forms. */
unsigned write_policy_generation(void);

/* Persist and publish a new policy. */
bool write_policy_set(cwist_db *db, const write_policy_t *policy);

/* role is "" for guests, as the handlers and renderers pass it. */
bool write_scope_allows(write_scope_t scope, const char *role);
bool write_policy_can_post(const char *role);
bool write_policy_can_comment(const char *role);

const char *write_scope_name(write_scope_t scope);
bool write_scope_parse(const char *name, write_scope_t *out);

#endif

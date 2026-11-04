#ifndef FLYBOARD_COMMUNITY_H
#define FLYBOARD_COMMUNITY_H

#include <cwist/core/db/sql.h>
#include <stdbool.h>

typedef struct {
    bool roles;
    bool new_user_limits;
    bool trust;
    bool spam_detection;
    bool approval_queue;
} community_config_t;

extern community_config_t community;

/* Load community.settings before workers fork. A missing file disables every
 * optional protection; malformed values fail closed to all features off. */
bool community_config_load(const char *path);
bool community_roles_enabled(void);

/* Moderators exist only while roles=true. Admins always retain their existing
 * privileges, including report processing when the optional system is off. */
bool community_role_can_moderate(const char *role);

/* Reuses users.active (1 active, 0 suspended) and journals the changed row.
 * The declaration lives here to keep this optional feature's integration
 * surface together; the implementation is in src/db/user.c. */
bool community_user_set_active(cwist_db *db, int id, bool active);

#endif

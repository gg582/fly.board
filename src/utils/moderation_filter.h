#ifndef FLYBOARD_MODERATION_FILTER_H
#define FLYBOARD_MODERATION_FILTER_H

#include "aho_moderation.h"
#include <cwist/core/db/sql.h>
#include <stdbool.h>

#define MODERATION_FILTER_MATCH_LIMIT_DEFAULT 64
#define MODERATION_FILTER_MATCH_LIMIT_MAX 1024

typedef enum {
    MODERATION_FILTER_PENDING = 0,
    MODERATION_FILTER_REJECT = 1
} moderation_filter_action_t;

typedef struct {
    bool enabled;
    moderation_filter_action_t action;
    bool case_fold;
    size_t match_limit;
    char rules[AHO_MODERATION_RULES_MAX_BYTES + 1];
} moderation_filter_policy_t;

bool moderation_filter_init(cwist_db *db);
moderation_filter_policy_t moderation_filter_get(void);
bool moderation_filter_set(cwist_db *db, const moderation_filter_policy_t *policy);
bool moderation_filter_check(const char *content, moderation_filter_action_t *action);

#endif

#ifndef FLYBOARD_MODERATION_H
#define FLYBOARD_MODERATION_H

#include <stdbool.h>
#include <cjson/cJSON.h>
#include <cwist/core/db/sql.h>

#define MODERATION_POST "post"
#define MODERATION_COMMENT "comment"

typedef enum {
    MODERATION_ALLOW = 0,
    MODERATION_PENDING = 1,
    MODERATION_REJECT = 2
} moderation_action_t;

typedef struct {
    moderation_action_t action;
    const char *reason;
} moderation_decision_t;

bool db_moderation_migrate(cwist_db *db);
bool moderation_can_review(const char *role);
moderation_decision_t moderation_check_submission(cwist_db *db, int uid, const char *role,
                                                  const char *target_type, const char *content);
bool db_moderation_queue_post(cwist_db *db, int post_id, int uid, const char *reason);
int db_moderation_create_pending_comment(cwist_db *db, const char *target_type, int target_id,
                                         int uid, const char *author_name, int parent_id,
                                         const char *content, const char *reason);
cJSON *db_moderation_list_pending(cwist_db *db);
int db_moderation_target_board_id(cwist_db *db, const char *target_type, int target_id);
bool db_moderation_approve(cwist_db *db, const char *target_type, int target_id, int reviewer_uid);
bool db_moderation_reject(cwist_db *db, const char *target_type, int target_id, int reviewer_uid);
void db_moderation_filter_comments(cwist_db *db, cJSON *comments);

typedef enum {
    MODERATION_EXTERNAL_ERROR = 0,
    MODERATION_EXTERNAL_APPLIED,
    MODERATION_EXTERNAL_DUPLICATE,
    MODERATION_EXTERNAL_REJECTED
} moderation_external_result_t;

cJSON *db_moderation_external_create(cwist_db *db, const char *target_type, int target_id,
                                     const char *correlation_id, long long issued_at, long long expires_at);
moderation_external_result_t db_moderation_external_apply(cwist_db *db, const char *correlation_id,
                                                         const char *decision_id, moderation_action_t action,
                                                         long long issued_at, const char *reason, long long now);

#endif

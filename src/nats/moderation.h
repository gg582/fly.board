#ifndef FLY_NATS_MODERATION_H
#define FLY_NATS_MODERATION_H

#include "db/moderation.h"
#include <stdbool.h>
#include <stddef.h>

#define FLY_MODERATION_REVIEW_SUBJECT "flyboard.moderation.review"
#define FLY_MODERATION_DECISION_SUBJECT "flyboard.moderation.decision"
#define FLY_MODERATION_CORRELATION_LEN 32

char *fly_moderation_build_request(const char *correlation_id, const char *target_type,
                                   const char *content, const char *reason,
                                   long long issued_at, long long expires_at);
moderation_external_result_t fly_moderation_process_decision(cwist_db *db, const char *data, size_t len,
                                                            const char *reviewer_pubkey_b64, long long now);
bool fly_moderation_correlation_id(char out[FLY_MODERATION_CORRELATION_LEN + 1]);

#endif

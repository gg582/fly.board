#include "config/config.h"
#include "crypto/fly_crypto.h"
#include "db/db.h"
#include "nats/moderation.h"
#include <stdbool.h>

blog_config_t g_config = {0};

bool fly_crypto_sign(const uint8_t *msg, size_t msg_len, char **sig_out) {
    (void)msg; (void)msg_len; (void)sig_out;
    return false;
}

bool fly_crypto_pubkey_export(char **pk_b64) {
    (void)pk_b64;
    return false;
}

bool db_notification_deliver_federated(long uid, const char *actor, const char *kind,
                                       const char *slug, const char *excerpt, const char *event_id) {
    (void)uid; (void)actor; (void)kind; (void)slug; (void)excerpt; (void)event_id;
    return true;
}

void page_cache_invalidate_post(const char *slug) { (void)slug; }
void page_cache_invalidate_all(void) {}
void post_schedule_bump(void) {}

bool fly_crypto_key_id_of(const char *pk_b64, char out[FLY_PQC_KEY_ID_LEN + 1]) {
    (void)pk_b64; (void)out;
    return false;
}

char *fly_moderation_build_request(const char *correlation_id, const char *target_type,
                                   const char *content, const char *reason,
                                   long long issued_at, long long expires_at) {
    (void)correlation_id; (void)target_type; (void)content; (void)reason; (void)issued_at; (void)expires_at;
    return NULL;
}

moderation_external_result_t fly_moderation_process_decision(cwist_db *db, const char *data, size_t len,
                                                            const char *reviewer_pubkey_b64, long long now) {
    (void)db; (void)data; (void)len; (void)reviewer_pubkey_b64; (void)now;
    return MODERATION_EXTERNAL_ERROR;
}

bool fly_moderation_correlation_id(char out[FLY_MODERATION_CORRELATION_LEN + 1]) {
    (void)out;
    return false;
}

cJSON *db_moderation_external_create(cwist_db *db, const char *target_type, int target_id,
                                     const char *correlation_id, long long issued_at, long long expires_at) {
    (void)db; (void)target_type; (void)target_id; (void)correlation_id; (void)issued_at; (void)expires_at;
    return NULL;
}

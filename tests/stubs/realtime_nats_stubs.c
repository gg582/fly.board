#include "config/config.h"
#include "crypto/fly_crypto.h"
#include "db/db.h"
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
                                       const char *slug, const char *excerpt) {
    (void)uid; (void)actor; (void)kind; (void)slug; (void)excerpt;
    return true;
}

void page_cache_invalidate_post(const char *slug) { (void)slug; }
void page_cache_invalidate_all(void) {}

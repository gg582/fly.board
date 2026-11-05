#include "engine/nats.h"
#include "nats/fly_nats.h"
#include <cwist/core/log.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

bool engine_nats_init(void) {
    const char *nats_url = getenv("NATS_URL");
    const char *enabled = getenv("FLY_MODERATION_WEBHOOKS");
    bool moderation = enabled && (strcmp(enabled, "1") == 0 || strcmp(enabled, "true") == 0);
    if (moderation && !nats_url) {
        FLY_LOG_ERROR("FLY_MODERATION_WEBHOOKS requires NATS_URL");
        return false;
    }
    if (!nats_url) return true;

    /* Only records the URL: connecting here, before cwist forks the
     * workers, would hand every worker a connection without its threads. */
    fly_nats_init(nats_url);
    if (moderation) {
        const char *pubkey = getenv("FLY_MODERATION_REVIEWER_PUBKEY_B64");
        const char *timeout = getenv("FLY_MODERATION_TIMEOUT_SECONDS");
        char *end = NULL;
        long seconds = timeout ? strtol(timeout, &end, 10) : 300;
        if (!pubkey || !pubkey[0] || (timeout && (!end || *end != '\0')) ||
            !fly_nats_moderation_init(pubkey, (int)seconds)) {
            FLY_LOG_ERROR("External moderation requires a valid reviewer ML-DSA-65 public key and 30..86400 second timeout");
            return false;
        }
    }
    return true;
}

void engine_nats_stop(void) {
    fly_nats_close();
}

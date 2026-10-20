#include "engine/nats.h"
#include "nats/fly_nats.h"
#include <cwist/core/log.h>
#include <stdlib.h>
#include <stdbool.h>

bool engine_nats_init(void) {
    const char *nats_url = getenv("NATS_URL");
    if (!nats_url) return true;

    /* Only records the URL: connecting here, before cwist forks the
     * workers, would hand every worker a connection without its threads. */
    fly_nats_init(nats_url);
    return true;
}

void engine_nats_stop(void) {
    fly_nats_close();
}

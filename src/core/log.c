#include "cwist/core/log.h"
#include <stdlib.h>
#include <string.h>

int fly_log_level = FLY_LOG_LEVEL_ERROR;

void fly_log_init(void) {
#ifdef DEBUG
    fly_log_level = FLY_LOG_LEVEL_DEBUG;
#else
    const char *env = getenv("DEBUG");
    if (env && (env[0] == '1' || strcmp(env, "true") == 0 || strcmp(env, "yes") == 0)) {
        fly_log_level = FLY_LOG_LEVEL_DEBUG;
    } else {
        fly_log_level = FLY_LOG_LEVEL_ERROR;
    }
#endif
}

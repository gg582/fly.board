#ifndef CWIST_CORE_LOG_H
#define CWIST_CORE_LOG_H

#include <stdio.h>

typedef enum {
    CWIST_LOG_LEVEL_NONE = 0,
    CWIST_LOG_LEVEL_DEBUG = 1,
    CWIST_LOG_LEVEL_INFO = 2,
    CWIST_LOG_LEVEL_WARN = 3,
    CWIST_LOG_LEVEL_ERROR = 4,
} cwist_macro_log_level_t;

/** @brief Global active log level. Set this to control output verbosity. */
extern cwist_macro_log_level_t g_cwist_log_level;

void cwist_log_write(cwist_macro_log_level_t level, const char *fmt, ...);

#define CWIST_LOG(level, fmt, ...) \
    do { \
        if ((level) >= g_cwist_log_level && g_cwist_log_level > CWIST_LOG_LEVEL_NONE) { \
            cwist_log_write((level), fmt, ##__VA_ARGS__); \
        } \
    } while (0)

#define CWIST_LOG_DEBUG(fmt, ...) CWIST_LOG(CWIST_LOG_LEVEL_DEBUG, fmt, ##__VA_ARGS__)
#define CWIST_LOG_INFO(fmt, ...)  CWIST_LOG(CWIST_LOG_LEVEL_INFO,  fmt, ##__VA_ARGS__)
#define CWIST_LOG_WARN(fmt, ...)  CWIST_LOG(CWIST_LOG_LEVEL_WARN,  fmt, ##__VA_ARGS__)
#define CWIST_LOG_ERROR(fmt, ...) CWIST_LOG(CWIST_LOG_LEVEL_ERROR, fmt, ##__VA_ARGS__)

/* Legacy fly.board aliases for backward compatibility */
#define FLY_LOG_LEVEL_ERROR CWIST_LOG_LEVEL_ERROR
#define FLY_LOG_LEVEL_DEBUG CWIST_LOG_LEVEL_DEBUG
#define fly_log_level       g_cwist_log_level
#define fly_log_init        cwist_log_init
#define FLY_LOG_DEBUG       CWIST_LOG_DEBUG
#define FLY_LOG_ERROR       CWIST_LOG_ERROR

void cwist_log_init(void);

#endif

#ifndef CWIST_CORE_LOG_H
#define CWIST_CORE_LOG_H

#include <stdio.h>

#define FLY_LOG_LEVEL_ERROR 1
#define FLY_LOG_LEVEL_DEBUG 2

extern int fly_log_level;

void fly_log_init(void);

#define FLY_LOG_DEBUG(fmt, ...) do { if (fly_log_level >= FLY_LOG_LEVEL_DEBUG) fprintf(stderr, "[DEBUG] " fmt "\n", ##__VA_ARGS__); } while(0)
#define FLY_LOG_ERROR(fmt, ...) do { if (fly_log_level >= FLY_LOG_LEVEL_ERROR) fprintf(stderr, "[ERROR] " fmt "\n", ##__VA_ARGS__); } while(0)

#endif

#ifndef FLYBOARD_REALTIME_H
#define FLYBOARD_REALTIME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define REALTIME_SESSION_HEX_LEN 32
#define REALTIME_PRESENCE_LEASE_MS 45000
#define REALTIME_TYPING_LEASE_MS 5000
#define REALTIME_TYPING_MIN_INTERVAL_MS 2500

typedef enum {
    REALTIME_OK = 0,
    REALTIME_INVALID,
    REALTIME_RATE_LIMITED,
    REALTIME_FULL
} realtime_result_t;

int64_t realtime_now_ms(void);
void realtime_init(void);
bool realtime_session_valid(const char *session);
void realtime_reset(void);

realtime_result_t realtime_presence_at(int post_id, const char *session, bool active, int64_t now_ms);
realtime_result_t realtime_typing_at(int post_id, const char *session, bool active, int64_t now_ms);
void realtime_typing_event_at(int post_id, const char *session, bool active, int64_t now_ms);

int realtime_viewer_count_at(int post_id, int64_t now_ms);
int realtime_typing_count_at(int post_id, const char *exclude_session, int64_t now_ms);
void realtime_viewer_label(int count, char *out, size_t out_size);

#endif

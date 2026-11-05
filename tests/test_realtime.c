#include "realtime/realtime.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static const char *A = "00000000000000000000000000000001";
static const char *B = "00000000000000000000000000000002";

int main(void) {
    realtime_reset();
    assert(!realtime_session_valid("short"));
    assert(!realtime_session_valid("zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz"));
    assert(realtime_session_valid(A));
    assert(realtime_presence_at(0, A, true, 1000) == REALTIME_INVALID);
    assert(realtime_presence_at(7, A, true, 1000) == REALTIME_OK);
    assert(realtime_presence_at(7, B, true, 1000) == REALTIME_OK);
    assert(realtime_viewer_count_at(7, 1000) == 2);
    assert(realtime_viewer_count_at(8, 1000) == 0);
    char label[32];
    realtime_viewer_label(2, label, sizeof(label));
    assert(strcmp(label, "2-5 viewers") == 0);
    realtime_viewer_label(22, label, sizeof(label));
    assert(strcmp(label, "21+ viewers") == 0);

    assert(realtime_typing_at(7, A, true, 1100) == REALTIME_OK);
    assert(realtime_typing_at(7, A, true, 1200) == REALTIME_RATE_LIMITED);
    assert(realtime_typing_count_at(7, B, 1200) == 1);
    assert(realtime_typing_count_at(7, A, 1200) == 0);
    realtime_typing_event_at(7, B, true, 1300);
    assert(realtime_typing_count_at(7, NULL, 1300) == 2);
    assert(realtime_typing_count_at(7, NULL, 1300 + REALTIME_TYPING_LEASE_MS) == 0);
    assert(realtime_viewer_count_at(7, 1000 + REALTIME_PRESENCE_LEASE_MS) == 0);

    assert(realtime_presence_at(7, A, true, 2000) == REALTIME_OK);
    assert(realtime_typing_at(7, A, true, 4000) == REALTIME_OK);
    assert(realtime_presence_at(7, A, false, 4100) == REALTIME_OK);
    assert(realtime_typing_count_at(7, NULL, 4100) == 1);
    assert(realtime_typing_at(7, B, false, 4200) == REALTIME_OK);
    assert(realtime_typing_count_at(7, NULL, 4200) == 0);
    assert(realtime_viewer_count_at(7, 4200) == 1);
    puts("realtime state tests passed");
    return 0;
}

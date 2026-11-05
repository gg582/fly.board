#include "nats/fly_nats.h"
#include "realtime/realtime.h"
#include <cwist/core/mem/gc.h>
#include <cwist/net/nats/cwist_nats.h>
#include <cjson/cJSON.h>
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static void session_for(char out[33], int value) {
    snprintf(out, 33, "%032x", value);
}

static bool wait_connected(int seconds) {
    for (int i = 0; i < seconds * 20; i++) {
        if (fly_nats_is_connected()) return true;
        usleep(50000);
    }
    return false;
}

static bool wait_counts(int viewers, int typing, int seconds) {
    for (int i = 0; i < seconds * 20; i++) {
        int64_t now = realtime_now_ms();
        if (realtime_viewer_count_at(42, now) >= viewers &&
            realtime_typing_count_at(42, NULL, now) >= typing) return true;
        usleep(50000);
    }
    return false;
}

static _Atomic int g_comment_id = 0;

static void observe_comment(const char *subject, const char *data, size_t len, void *ctx) {
    (void)subject; (void)ctx;
    cJSON *obj = cJSON_ParseWithLength(data, len);
    cJSON *id = cJSON_GetObjectItemCaseSensitive(obj, "comment_id");
    if (cJSON_IsNumber(id)) atomic_store(&g_comment_id, id->valueint);
    cJSON_Delete(obj);
}

static void multi_process(const char *url) {
    realtime_reset();
    fly_nats_init(url);
    fly_nats_ensure();
    assert(wait_connected(10));
    char a[33], b[33];
    session_for(a, getpid());
    assert(fly_nats_publish_realtime_presence(42, a, true));

    cwist_nats_t *observer = NULL;
    cwist_error_t err = cwist_nats_connect(&observer, url);
    assert(cwist_error_is_ok(&err));
    err = cwist_nats_subscribe(observer, "flyboard.realtime.comments", observe_comment, NULL);
    assert(cwist_error_is_ok(&err));
    cwist_nats_dispatch(observer);
    usleep(100000);
    assert(fly_nats_publish_realtime_comment(42, 1044));
    for (int i = 0; i < 100 && atomic_load(&g_comment_id) != 1044; i++) usleep(50000);
    assert(atomic_load(&g_comment_id) == 1044);
    cwist_nats_destroy(observer);

    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        realtime_reset();
        fly_nats_ensure();
        if (!wait_connected(10)) _exit(2);
        session_for(b, getpid());
        if (!fly_nats_publish_realtime_presence(42, b, true) ||
            !fly_nats_publish_realtime_typing(42, b, true)) _exit(3);
        if (!wait_counts(2, 1, 10)) _exit(4);
        fly_nats_publish_realtime_typing(42, b, false);
        fly_nats_publish_realtime_presence(42, b, false);
        usleep(500000);
        fly_nats_close();
        _exit(0);
    }
    assert(wait_counts(2, 1, 10));
    /* Replay a heartbeat so the late-joining child also observes both peers. */
    assert(fly_nats_publish_realtime_presence(42, a, true));
    int status = 0;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    for (int i = 0; i < 100 && realtime_viewer_count_at(42, realtime_now_ms()) != 1; i++) usleep(50000);
    assert(realtime_viewer_count_at(42, realtime_now_ms()) == 1);
    fly_nats_close();
    puts("NATS multi-process delivery tests passed");
}

static void reconnect(const char *url, const char *ready_path) {
    realtime_reset();
    fly_nats_init(url);
    fly_nats_ensure();
    assert(wait_connected(15));
    FILE *ready = fopen(ready_path, "w");
    assert(ready);
    fclose(ready);
    char session[33];
    session_for(session, getpid());
    bool down = false;
    for (int i = 0; i < 400; i++) {
        fly_nats_publish_realtime_presence(42, session, true);
        bool connected = fly_nats_is_connected();
        if (!connected) down = true;
        if (down && connected && realtime_viewer_count_at(42, realtime_now_ms()) > 0) {
            fly_nats_close();
            puts("NATS outage reconnect test passed");
            return;
        }
        usleep(100000);
    }
    fprintf(stderr, "NATS did not recover after broker outage\n");
    abort();
}

int main(int argc, char **argv) {
    assert(argc >= 3);
    cwist_full_gc(true);
    if (strcmp(argv[1], "multi") == 0) multi_process(argv[2]);
    else if (strcmp(argv[1], "reconnect") == 0 && argc == 4) reconnect(argv[2], argv[3]);
    else return 2;
    return 0;
}

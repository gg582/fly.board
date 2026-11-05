#include "utils/traffic_guard.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;
#define CHECK(cond, name) do { \
    if (cond) printf("PASS: %s\n", name); \
    else { printf("FAIL: %s (line %d)\n", name, __LINE__); failures++; } \
} while (0)

bool auth_get_cookie(cwist_http_request *req, const char *name, char *out, size_t out_len) {
    (void)req;
    (void)name;
    (void)out;
    (void)out_len;
    return false;
}

typedef struct {
    const char *key;
    uint64_t now_ns;
    int iterations;
    int allowed;
} worker_args_t;

static void *thread_worker(void *ctx) {
    worker_args_t *args = ctx;
    for (int i = 0; i < args->iterations; i++) {
        if (traffic_guard_check_key(args->key, strlen(args->key), TRAFFIC_READ,
                                    args->now_ns, NULL)) args->allowed++;
    }
    return NULL;
}

int main(void) {
    CHECK(traffic_guard_check_key("off", 3, TRAFFIC_READ, 1, NULL),
          "zero-value configuration fails open before init");
    setenv("FLY_TRAFFIC_READ_RATE", "4", 1);
    setenv("FLY_TRAFFIC_READ_BURST", "4", 1);
    setenv("FLY_TRAFFIC_WRITE_RATE", "8", 1);
    setenv("FLY_TRAFFIC_WRITE_BURST", "8", 1);
    setenv("FLY_TRAFFIC_ACTION", "cached", 1);
    setenv("FLY_TRAFFIC_TARPIT_MS", "10000", 1);
    CHECK(traffic_guard_init(), "shared table initializes");
    CHECK(traffic_guard_action() == TRAFFIC_ACTION_CACHED, "cached response policy parsed");
    CHECK(traffic_guard_tarpit_ms() == 100, "invalid tarpit uses bounded default");

    const char *key = "policy-key";
    for (int i = 0; i < 4; i++) {
        CHECK(traffic_guard_check_key(key, strlen(key), TRAFFIC_READ, 1000, NULL),
              "initial burst token accepted");
    }
    unsigned retry = 0;
    CHECK(!traffic_guard_check_key(key, strlen(key), TRAFFIC_READ, 1000, &retry) && retry == 1,
          "exhausted bucket returns 429 retry policy");
    CHECK(traffic_guard_check_key(key, strlen(key), TRAFFIC_READ, 250001000, NULL),
          "token bucket refills by elapsed time");
    CHECK(traffic_guard_check_key(key, strlen(key), TRAFFIC_WRITE, 1000, NULL),
          "read and write buckets are independent");

    enum { THREADS = 8, PER_THREAD = 100 };
    pthread_t threads[THREADS];
    worker_args_t args[THREADS];
    for (int i = 0; i < THREADS; i++) {
        args[i] = (worker_args_t){ .key = "threaded", .now_ns = 2000, .iterations = PER_THREAD };
        pthread_create(&threads[i], NULL, thread_worker, &args[i]);
    }
    int allowed = 0;
    for (int i = 0; i < THREADS; i++) {
        pthread_join(threads[i], NULL);
        allowed += args[i].allowed;
    }
    CHECK(allowed == 4, "concurrent threads cannot overspend a shared bucket");

    pid_t children[4];
    int status_fds[4][2];
    for (int i = 0; i < 4; i++) {
        CHECK(pipe(status_fds[i]) == 0, "child result pipe");
        children[i] = fork();
        if (children[i] == 0) {
            close(status_fds[i][0]);
            int child_allowed = 0;
            for (int n = 0; n < 100; n++) {
                if (traffic_guard_check_key("processes", 9, TRAFFIC_WRITE, 3000, NULL)) child_allowed++;
            }
            (void)write(status_fds[i][1], &child_allowed, sizeof(child_allowed));
            close(status_fds[i][1]);
            _exit(0);
        }
        close(status_fds[i][1]);
    }
    allowed = 0;
    for (int i = 0; i < 4; i++) {
        int child_allowed = 0;
        ssize_t got = read(status_fds[i][0], &child_allowed, sizeof(child_allowed));
        int status = 0;
        waitpid(children[i], &status, 0);
        close(status_fds[i][0]);
        CHECK(got == (ssize_t)sizeof(child_allowed) && WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "forked worker completed");
        allowed += child_allowed;
    }
    CHECK(allowed == 8, "forked processes share one robust bucket");

    for (unsigned i = 0; i < 20000; i++) {
        char eviction_key[32];
        snprintf(eviction_key, sizeof(eviction_key), "unique-%u", i);
        (void)traffic_guard_check_key(eviction_key, strlen(eviction_key), TRAFFIC_WRITE,
                                      4000 + i, NULL);
    }
    size_t used = 0;
    uint64_t allowed_total = 0, limited_total = 0;
    traffic_guard_stats(&allowed_total, &limited_total, &used);
    CHECK(used > 0 && used <= 4096, "unique attackers cannot grow table beyond fixed slots");
    CHECK(allowed_total + limited_total >= 20000, "shared counters remain consistent after eviction");

    cwist_http_response res;
    memset(&res, 0, sizeof(res));
    res.body = cwist_sstring_create();
    traffic_guard_reject(&res, 7);
    CHECK((int)res.status_code == 429 && strcmp(res.body->data, "Traffic limit exceeded. Please retry later.") == 0,
          "limited request receives bounded plain-text 429");
    const char *retry_header = cwist_http_header_get(res.headers, "Retry-After");
    const char *cache_header = cwist_http_header_get(res.headers, "Cache-Control");
    CHECK(retry_header && strcmp(retry_header, "7") == 0 && cache_header && strcmp(cache_header, "no-store") == 0,
          "429 policy sets retry and no-store headers");
    cwist_sstring_destroy(res.body);

    printf("%s\n", failures == 0 ? "ALL PASS" : "SOME FAILED");
    return failures == 0 ? 0 : 1;
}

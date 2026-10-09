/* Unit test: src/utils/reqshare.c request coalescing.
 *
 * Covers: leader election, waiter receiving the leader's result, result TTL
 * (fresh copy vs. expired re-lead), stuck-leader TTL supersede, and the
 * write-lock duplicate path.
 *
 * Build:
 *   gcc -O1 -Iinclude -Isrc -I/home/yjlee/cwist/include -I/home/yjlee/cwist/lib \
 *       -I/home/yjlee/cwist/lib/cjson \
 *       tests/test_utils_reqshare.c tests/stubs/utils_test_stubs.c \
 *       src/utils/reqshare.o src/utils/cache.o \
 *       /home/yjlee/cwist/libcwist.a /home/yjlee/cwist/lib/libttak/lib/libttak.a \
 *       /home/yjlee/cwist/lib/cjson/libcjson.a \
 *       -lpthread -lm -ldl -o /tmp/test_utils_reqshare
 * Run: /tmp/test_utils_reqshare
 */
#include "utils/reqshare.h"
#include <cwist/core/sstring/sstring.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures = 0;
#define CHECK(cond, name) do { \
    if (cond) printf("PASS: %s\n", name); \
    else { printf("FAIL: %s (line %d)\n", name, __LINE__); failures++; } \
} while (0)

static void *leader_thread(void *arg) {
    (void)arg;
    bool leader = false;
    cwist_sstring *s = reqshare_wait_or_start("page:home", &leader);
    CHECK(s == NULL && leader, "first caller becomes leader");
    usleep(200 * 1000); /* simulate render work */
    cwist_sstring *html = cwist_sstring_create();
    cwist_sstring_append(html, "RENDERED_HOME");
    reqshare_finish("page:home", html);
    cwist_sstring_destroy(html);
    return NULL;
}

int main(void) {
    setenv("FLYBOARD_CACHE_MAX_MB", "8", 1);
    CHECK(reqshare_init(), "reqshare_init");

    /* leader + waiter */
    pthread_t t;
    pthread_create(&t, NULL, leader_thread, NULL);
    usleep(50 * 1000); /* let the leader register before we join in */
    bool leader = true;
    cwist_sstring *copy = reqshare_wait_or_start("page:home", &leader);
    CHECK(copy != NULL && !leader, "waiter gets copy, not leader");
    CHECK(copy && copy->data && strcmp(copy->data, "RENDERED_HOME") == 0,
          "waiter receives leader result");
    if (copy) cwist_sstring_destroy(copy);
    pthread_join(t, NULL);

    /* fresh result still served without a leader (within RS_RESULT_TTL_SEC) */
    copy = reqshare_wait_or_start("page:home", &leader);
    CHECK(copy != NULL && !leader, "fresh result served directly");
    if (copy) cwist_sstring_destroy(copy);

    /* after the result TTL the key is stale: caller becomes leader again */
    sleep(3);
    copy = reqshare_wait_or_start("page:home", &leader);
    CHECK(copy == NULL && leader, "expired result re-elects leader");
    reqshare_finish("page:home", NULL); /* finish empty so state is clean */

    /* stuck leader superseded after RS_LEADER_TTL_SEC (5s) */
    copy = reqshare_wait_or_start("page:stuck", &leader);
    CHECK(copy == NULL && leader, "stuck-key first caller is leader");
    sleep(6);
    copy = reqshare_wait_or_start("page:stuck", &leader);
    CHECK(copy == NULL && leader, "stuck leader superseded after TTL");
    cwist_sstring *html = cwist_sstring_create();
    cwist_sstring_append(html, "RECOVERY");
    reqshare_finish("page:stuck", html);
    cwist_sstring_destroy(html);
    copy = reqshare_wait_or_start("page:stuck", &leader);
    CHECK(copy != NULL && !leader && strcmp(copy->data, "RECOVERY") == 0,
          "superseding leader's result served");
    if (copy) cwist_sstring_destroy(copy);

    /* write lock: second try fails, release allows re-acquire */
    CHECK(reqshare_write_lock_try("write:post:1"), "write lock acquired");
    CHECK(!reqshare_write_lock_try("write:post:1"), "duplicate write lock rejected");
    CHECK(reqshare_write_lock_try("write:post:2"), "different key not blocked");
    reqshare_write_lock_release("write:post:2");
    reqshare_write_lock_release("write:post:1");
    CHECK(reqshare_write_lock_try("write:post:1"), "write lock re-acquirable after release");
    reqshare_write_lock_release("write:post:1");

    reqshare_cleanup();
    printf("%s\n", failures == 0 ? "ALL PASS" : "SOME FAILED");
    return failures == 0 ? 0 : 1;
}

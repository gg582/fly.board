/* Unit test: src/utils/spam_guard.c honeypot tokens + rate limit counters,
 * with src/config/write_policy.c for policy state.
 *
 * The site-settings backing store is replaced by an in-memory map (stub
 * below) so write_policy_init/write_policy_set can run without a database.
 * A socketpair stands in for the client connection used to key anonymous
 * writers.
 *
 * Build:
 *   gcc -O1 -Iinclude -Isrc -I/home/yjlee/cwist/include -I/home/yjlee/cwist/lib \
 *       -I/home/yjlee/cwist/lib/cjson -I/home/yjlee/cwist/lib/boringssl/include \
 *       tests/test_utils_spam_guard.c src/utils/spam_guard.o src/config/write_policy.o \
 *       src/auth/auth.o \
 *       /home/yjlee/cwist/libcwist.a /home/yjlee/cwist/lib/libttak/lib/libttak.a \
 *       /home/yjlee/cwist/lib/cjson/libcjson.a \
 *       /home/yjlee/cwist/lib/boringssl/build/libcrypto.a \
 *       -lpthread -lm -ldl -lstdc++ -o /tmp/test_utils_spam_guard
 * Run: /tmp/test_utils_spam_guard
 */
#include "utils/spam_guard.h"
#include "config/write_policy.h"
#include "auth/auth.h"
#include "db/db.h"
#include <cwist/net/http/http.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int failures = 0;
#define CHECK(cond, name) do { \
    if (cond) printf("PASS: %s\n", name); \
    else { printf("FAIL: %s (line %d)\n", name, __LINE__); failures++; } \
} while (0)

/* ---- in-memory site settings store (stands in for src/db/db.o) ---- */
static char kv[16][2][64];
static int kv_n = 0;
static int kv_find(const char *k) {
    for (int i = 0; i < kv_n; i++) if (strcmp(kv[i][0], k) == 0) return i;
    return -1;
}
bool db_site_setting_get(cwist_db *db, const char *key, char *out, size_t out_len) {
    (void)db;
    int i = kv_find(key);
    if (i < 0) return false;
    snprintf(out, out_len, "%s", kv[i][1]);
    return true;
}
bool db_site_setting_set(cwist_db *db, const char *key, const char *value) {
    (void)db;
    int i = kv_find(key);
    if (i < 0) {
        if (kv_n >= 16) return false;
        i = kv_n++;
    }
    snprintf(kv[i][0], sizeof(kv[i][0]), "%s", key);
    snprintf(kv[i][1], sizeof(kv[i][1]), "%s", value);
    return true;
}

static bool extract_token(const char *html, char *out, size_t out_len) {
    const char *p = strstr(html, "name='" SPAM_FIELD_TOKEN "' value='");
    if (!p) return false;
    p += strlen("name='" SPAM_FIELD_TOKEN "' value='");
    size_t n = strcspn(p, "'");
    if (n >= out_len) return false;
    memcpy(out, p, n);
    out[n] = '\0';
    return true;
}

int main(void) {
    remove("/tmp/flytest_spam_secret");
    CHECK(auth_jwt_init("/tmp/flytest_spam_secret"), "jwt init for token MAC");
    CHECK(spam_guard_init(), "spam_guard_init");
    CHECK(write_policy_init(NULL), "write_policy_init with empty store");

    /* default policy: everything off -> always OK, no counting */
    cwist_http_request req;
    memset(&req, 0, sizeof(req));
    req.client_fd = -1;
    CHECK(spam_guard_check(&req, 0, NULL, "bot!", NULL) == SPAM_OK, "all-off policy passes anything");

    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair for peer identity");
    req.client_fd = sv[0];

    /* ---- honeypot ---- */
    write_policy_t pol = write_policy_get();
    pol.spam_honeypot = true;
    pol.spam_rate_limit = 0;
    CHECK(write_policy_set(NULL, &pol), "enable honeypot");

    char *html = NULL;
    cwist_sstring *b = cwist_sstring_create();
    spam_guard_append_fields(b);
    html = strdup(b->data);
    cwist_sstring_destroy(b);
    CHECK(html && strstr(html, SPAM_FIELD_TRAP) && strstr(html, SPAM_FIELD_TOKEN),
          "append_fields emits trap + token inputs");
    char token[80];
    CHECK(html && extract_token(html, token, sizeof(token)), "token parseable from fields");

    CHECK(spam_guard_check(&req, 0, NULL, "http://spam", token) == SPAM_REJECTED,
          "filled trap rejected");
    CHECK(spam_guard_check(&req, 0, NULL, "", NULL) == SPAM_REJECTED,
          "missing token rejected");
    CHECK(spam_guard_check(&req, 0, NULL, "", "123.forgedforgedforgedforgedforged01") == SPAM_REJECTED,
          "forged token rejected");
    CHECK(spam_guard_check(&req, 0, NULL, "", token) == SPAM_REJECTED,
          "fresh token (age < 3s) rejected");

    sleep(4); /* SPAM_MIN_FILL_SECONDS */
    CHECK(spam_guard_check(&req, 0, NULL, "", token) == SPAM_OK,
          "aged valid token accepted");

    /* ---- rate limit (uid-keyed writers, honeypot off) ---- */
    pol.spam_honeypot = false;
    pol.spam_rate_limit = 2;
    CHECK(write_policy_set(NULL, &pol), "enable rate limit 2");
    CHECK(spam_guard_check(&req, 100, NULL, NULL, NULL) == SPAM_OK, "writer 100: 1st ok");
    CHECK(spam_guard_check(&req, 100, NULL, NULL, NULL) == SPAM_OK, "writer 100: 2nd ok");
    CHECK(spam_guard_check(&req, 100, NULL, NULL, NULL) == SPAM_RATE_LIMITED,
          "writer 100: 3rd rate limited");
    CHECK(spam_guard_check(&req, 101, NULL, NULL, NULL) == SPAM_OK,
          "different writer unaffected");
    CHECK(spam_guard_check(&req, 0, "admin", NULL, NULL) == SPAM_OK,
          "admin never limited");

    free(html);
    close(sv[0]);
    close(sv[1]);
    remove("/tmp/flytest_spam_secret");
    printf("%s\n", failures == 0 ? "ALL PASS" : "SOME FAILED");
    return failures == 0 ? 0 : 1;
}

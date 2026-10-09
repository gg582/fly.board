/* Unit test: src/auth JWT lifecycle + password hashing.
 *
 * Covers: auth_jwt_init (secret file create/reload), auth_jwt_issue ->
 * cwist_jwt_verify claim roundtrip, signature tamper rejection, expired
 * token rejection, wrong-secret rejection, password hash/verify.
 *
 * Build:
 *   gcc -O1 -Iinclude -Isrc -I/home/yjlee/cwist/include -I/home/yjlee/cwist/lib \
 *       -I/home/yjlee/cwist/lib/cjson -I/home/yjlee/cwist/lib/boringssl/include \
 *       tests/test_auth_jwt.c src/auth/auth.o \
 *       /home/yjlee/cwist/libcwist.a /home/yjlee/cwist/lib/libttak/lib/libttak.a \
 *       /home/yjlee/cwist/lib/cjson/libcjson.a \
 *       /home/yjlee/cwist/lib/boringssl/build/libcrypto.a \
 *       -lpthread -lm -ldl -lstdc++ -o /tmp/test_auth_jwt
 * Run: /tmp/test_auth_jwt
 */
#include "auth/auth.h"
#include <cwist/security/jwt/jwt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures = 0;
#define CHECK(cond, name) do { \
    if (cond) printf("PASS: %s\n", name); \
    else { printf("FAIL: %s (line %d)\n", name, __LINE__); failures++; } \
} while (0)

#define SECRET_PATH "/tmp/flytest_jwt_secret"

int main(void) {
    remove(SECRET_PATH);
    CHECK(auth_jwt_init(SECRET_PATH), "jwt init creates secret file");
    const char *secret = auth_jwt_secret();
    CHECK(secret && strlen(secret) == 64, "secret is 64 hex chars");

    /* a second init from the same file must load, not overwrite */
    char first_secret[65];
    snprintf(first_secret, sizeof(first_secret), "%s", secret);
    CHECK(auth_jwt_init(SECRET_PATH), "jwt init reload");
    CHECK(strcmp(auth_jwt_secret(), first_secret) == 0, "secret stable across reload");

    /* issue + verify roundtrip */
    char *token = auth_jwt_issue(42, "alice", "admin");
    CHECK(token && token[0], "jwt issue");
    if (token) {
        cwist_jwt_claims *claims = cwist_jwt_verify(token, auth_jwt_secret());
        CHECK(claims != NULL, "jwt verify roundtrip");
        if (claims) {
            const char *sub = cwist_jwt_claims_get(claims, "sub");
            const char *name = cwist_jwt_claims_get(claims, "username");
            const char *role = cwist_jwt_claims_get(claims, "role");
            CHECK(sub && strcmp(sub, "42") == 0, "claim sub roundtrip");
            CHECK(name && strcmp(name, "alice") == 0, "claim username roundtrip");
            CHECK(role && strcmp(role, "admin") == 0, "claim role roundtrip");
            cwist_jwt_claims_destroy(claims);
        }

        /* tamper with the payload segment */
        char *tampered = strdup(token);
        CHECK(tampered != NULL, "strdup for tamper");
        if (tampered) {
            tampered[strlen(tampered) - 3] =
                tampered[strlen(tampered) - 3] == 'A' ? 'B' : 'A';
            CHECK(cwist_jwt_verify(tampered, auth_jwt_secret()) == NULL, "tampered token rejected");
            free(tampered);
        }

        /* wrong secret */
        CHECK(cwist_jwt_verify(token, "deadbeefdeadbeefdeadbeefdeadbeef") == NULL,
              "wrong secret rejected");
        free(token);
    }

    /* expired token: exp in the past */
    char *old = cwist_jwt_sign("{\"sub\":\"7\",\"exp\":1000000000}",
                               auth_jwt_secret(), 0);
    CHECK(old != NULL, "sign expired-payload token");
    if (old) {
        CHECK(cwist_jwt_verify(old, auth_jwt_secret()) == NULL, "expired token rejected");
        free(old);
    }

    /* not-yet-valid token: nbf far in the future */
    char *early = cwist_jwt_sign("{\"sub\":\"7\",\"nbf\":9999999999}",
                                 auth_jwt_secret(), 0);
    CHECK(early != NULL, "sign future-nbf token");
    if (early) {
        CHECK(cwist_jwt_verify(early, auth_jwt_secret()) == NULL, "future nbf rejected");
        free(early);
    }

    /* password hashing */
    char hash[256];
    CHECK(auth_hash_password("hunter2", hash, sizeof(hash)), "password hash");
    CHECK(auth_verify_password("hunter2", hash), "password verify ok");
    CHECK(!auth_verify_password("hunter3", hash), "wrong password rejected");
    char hash2[256];
    CHECK(auth_hash_password("hunter2", hash2, sizeof(hash2)) &&
          strcmp(hash, hash2) != 0, "same password hashes differently (salt)");

    remove(SECRET_PATH);
    printf("%s\n", failures == 0 ? "ALL PASS" : "SOME FAILED");
    return failures == 0 ? 0 : 1;
}

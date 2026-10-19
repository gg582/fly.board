/* Unit test: src/auth/totp.c — RFC 6238 SHA-1 test vectors, base32 round
 * trips, verification window.
 *
 * Build:
 *   gcc -O1 -Iinclude -Isrc -I/home/yjlee/cwist/include \
 *       tests/test_auth_totp.c src/auth/totp.o \
 *       -lssl -lcrypto -o /tmp/test_auth_totp
 * Run: /tmp/test_auth_totp
 */
#include "auth/totp.h"
#include <stdio.h>
#include <string.h>

static int failures = 0;
#define CHECK(cond, name) do { \
    if (cond) printf("PASS: %s\n", name); \
    else { printf("FAIL: %s (line %d)\n", name, __LINE__); failures++; } \
} while (0)

int main(void) {
    /* RFC 6238 appendix B: secret "12345678901234567890" (ASCII) =
     * GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ in base32. */
    const char *secret = "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ";
    struct { time_t t; const char *code; } vectors[] = {
        { 59,         "287082" },
        { 1111111109, "081804" },
        { 1111111111, "050471" },
        { 1234567890, "005924" },
        { 2000000000, "279037" },
        { 20000000000,"353130" },
    };
    for (size_t i = 0; i < sizeof(vectors) / sizeof(vectors[0]); i++) {
        char code[TOTP_CODE_DIGITS + 1];
        bool ok = totp_code_at(secret, vectors[i].t, code);
        char name[64];
        snprintf(name, sizeof(name), "RFC6238 vector t=%lld", (long long)vectors[i].t);
        CHECK(ok && strcmp(code, vectors[i].code) == 0, name);
        CHECK(totp_verify_at(secret, vectors[i].code, vectors[i].t), "vector verifies");
    }

    /* Wrong code, wrong shape. */
    CHECK(!totp_verify_at(secret, "000000", 59), "wrong code rejected");
    CHECK(!totp_verify_at(secret, "28708", 59), "short code rejected");
    CHECK(!totp_verify_at(secret, "28708a", 59), "non-digit rejected");
    CHECK(!totp_verify_at("not-valid-base32!!", "287082", 59), "bad secret rejected");

    /* Window: one step before/after still verifies, two steps does not. */
    CHECK(totp_verify_at(secret, "081804", 1111111109 + 30), "adjacent step verifies (+1)");
    CHECK(totp_verify_at(secret, "081804", 1111111109 - 30), "adjacent step verifies (-1)");
    CHECK(!totp_verify_at(secret, "081804", 1111111109 + 60), "two steps away rejected");

    /* base32 round trip. */
    unsigned char bytes[] = { 0x66, 0x6F, 0x6F, 0x62, 0x61, 0x72 };
    char enc[64];
    unsigned char dec[64];
    CHECK(totp_base32_encode(bytes, sizeof(bytes), enc, sizeof(enc)) == 10 &&
          strcmp(enc, "MZXW6YTBOI") == 0, "base32 encode RFC4648 vector");
    CHECK(totp_base32_decode(enc, dec, sizeof(dec)) == sizeof(bytes) &&
          memcmp(dec, bytes, sizeof(bytes)) == 0, "base32 round trip");
    CHECK(totp_base32_decode("mzxw6ytboi======", dec, sizeof(dec)) == sizeof(bytes),
          "base32 decode lowercase+padding");

    /* Random secrets are well formed. */
    char s1[TOTP_B32_LEN + 1], s2[TOTP_B32_LEN + 1];
    CHECK(totp_secret_generate(s1) && totp_secret_generate(s2), "secret generation");
    CHECK(strlen(s1) == TOTP_B32_LEN && strcmp(s1, s2) != 0, "secrets distinct/wellformed");
    char code[TOTP_CODE_DIGITS + 1];
    CHECK(totp_code_now(s1, code) && strlen(code) == TOTP_CODE_DIGITS, "code from fresh secret");

    printf("%s\n", failures ? "SOME TESTS FAILED" : "ALL TESTS PASSED");
    return failures ? 1 : 0;
}

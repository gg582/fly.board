#define _POSIX_C_SOURCE 200809L
#include "post_unlock.h"
#include "auth.h"
#include <openssl/hmac.h>
#include <openssl/mem.h>
#include <openssl/sha.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAC_HEX_LEN 64

static bool token_mac(const char *secret, int post_id, int access_ver, long expires,
                      char out[MAC_HEX_LEN + 1]) {
    if (!secret || !secret[0]) return false;
    char msg[96];
    int n = snprintf(msg, sizeof(msg), "fly.board/post-unlock/v1|%d|%d|%ld", post_id, access_ver, expires);
    if (n <= 0 || (size_t)n >= sizeof(msg)) return false;
    uint8_t mac[SHA256_DIGEST_LENGTH];
    unsigned int mac_len = 0;
    if (!HMAC(EVP_sha256(), secret, strlen(secret), (const uint8_t *)msg, (size_t)n, mac, &mac_len) ||
        mac_len != SHA256_DIGEST_LENGTH) {
        return false;
    }
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) snprintf(out + 2 * i, 3, "%02x", mac[i]);
    OPENSSL_cleanse(mac, sizeof(mac));
    return true;
}

bool post_unlock_token_make(const char *secret, int post_id, int access_ver, long expires,
                            char *out, size_t out_len) {
    if (!out || out_len == 0 || post_id <= 0) return false;
    out[0] = '\0';
    char mac[MAC_HEX_LEN + 1];
    if (!token_mac(secret, post_id, access_ver, expires, mac)) return false;
    int n = snprintf(out, out_len, "%ld.%s", expires, mac);
    return n > 0 && (size_t)n < out_len;
}

bool post_unlock_token_check(const char *secret, int post_id, int access_ver, long now,
                             const char *token) {
    if (!token || !token[0] || post_id <= 0) return false;
    char *dot = NULL;
    long expires = strtol(token, &dot, 10);
    if (!dot || dot == token || *dot != '.' || strlen(dot + 1) != MAC_HEX_LEN) return false;
    if (expires < now || expires > now + POST_UNLOCK_LIFETIME + 300) return false;
    char mac[MAC_HEX_LEN + 1];
    if (!token_mac(secret, post_id, access_ver, expires, mac)) return false;
    return CRYPTO_memcmp(mac, dot + 1, MAC_HEX_LEN) == 0;
}

bool post_unlock_request_ok(cwist_http_request *req, int post_id, int access_ver) {
    if (!req || post_id <= 0) return false;
    char name[32];
    snprintf(name, sizeof(name), POST_UNLOCK_COOKIE_PREFIX "%d", post_id);
    char token[POST_UNLOCK_TOKEN_MAX];
    if (!auth_get_cookie(req, name, token, sizeof(token))) return false;
    return post_unlock_token_check(auth_jwt_secret(), post_id, access_ver, (long)time(NULL), token);
}

bool post_unlock_cookie_header(int post_id, int access_ver, bool secure, char *out, size_t out_len) {
    long expires = (long)time(NULL) + POST_UNLOCK_LIFETIME;
    char token[POST_UNLOCK_TOKEN_MAX];
    if (!post_unlock_token_make(auth_jwt_secret(), post_id, access_ver, expires, token, sizeof(token)))
        return false;
    /* Path=/ so attachment downloads of the post see the grant too. */
    int n = snprintf(out, out_len, POST_UNLOCK_COOKIE_PREFIX "%d=%s; Path=/; Max-Age=%d; HttpOnly; SameSite=Lax%s",
                     post_id, token, POST_UNLOCK_LIFETIME, secure ? "; Secure" : "");
    OPENSSL_cleanse(token, sizeof(token));
    return n > 0 && (size_t)n < out_len;
}

bool post_unlock_has_any_cookie(cwist_http_request *req) {
    return auth_has_cookie_prefix(req, POST_UNLOCK_COOKIE_PREFIX);
}

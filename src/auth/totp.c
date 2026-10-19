#define _POSIX_C_SOURCE 200809L
#include "totp.h"
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* RFC 6238 / RFC 4648 implementations - see totp.h. */

static const char k_b32_alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";

size_t totp_base32_encode(const unsigned char *in, size_t in_len, char *out, size_t out_cap) {
    if (!in || !out || out_cap == 0) return 0;
    size_t o = 0;
    unsigned int buffer = 0;
    int bits = 0;
    for (size_t i = 0; i < in_len; i++) {
        buffer = (buffer << 8) | in[i];
        bits += 8;
        while (bits >= 5) {
            if (o + 1 >= out_cap) return 0;
            out[o++] = k_b32_alphabet[(buffer >> (bits - 5)) & 0x1F];
            bits -= 5;
        }
    }
    if (bits > 0) {
        if (o + 1 >= out_cap) return 0;
        out[o++] = k_b32_alphabet[(buffer << (5 - bits)) & 0x1F];
    }
    if (o >= out_cap) return 0;
    out[o] = '\0';
    return o;
}

static int b32_value(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a';
    if (c >= '2' && c <= '7') return c - '2' + 26;
    return -1;
}

size_t totp_base32_decode(const char *b32, unsigned char *out, size_t out_cap) {
    if (!b32 || !out || out_cap == 0) return 0;
    size_t o = 0;
    unsigned int buffer = 0;
    int bits = 0;
    for (const unsigned char *p = (const unsigned char *)b32; *p; p++) {
        if (*p == '=' || *p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') continue;
        int v = b32_value(*p);
        if (v < 0) return 0;
        buffer = (buffer << 5) | (unsigned int)v;
        bits += 5;
        if (bits >= 8) {
            if (o >= out_cap) return 0;
            out[o++] = (unsigned char)((buffer >> (bits - 8)) & 0xFF);
            bits -= 8;
        }
    }
    return o;
}

bool totp_secret_generate(char *out_b32) {
    if (!out_b32) return false;
    unsigned char bytes[TOTP_SECRET_BYTES];
    if (RAND_bytes(bytes, sizeof(bytes)) != 1) return false;
    return totp_base32_encode(bytes, sizeof(bytes), out_b32, TOTP_B32_LEN + 1) == TOTP_B32_LEN;
}

bool totp_code_at(const char *secret_b32, time_t t, char out_code[TOTP_CODE_DIGITS + 1]) {
    if (!secret_b32 || !out_code) return false;
    unsigned char key[64];
    size_t key_len = totp_base32_decode(secret_b32, key, sizeof(key));
    if (key_len < 10) return false; /* refuse implausibly short secrets */

    unsigned long long counter = (unsigned long long)(t / TOTP_STEP_SEC);
    unsigned char msg[8];
    for (int i = 7; i >= 0; i--) {
        msg[i] = (unsigned char)(counter & 0xFF);
        counter >>= 8;
    }
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;
    if (!HMAC(EVP_sha1(), key, (int)key_len, msg, sizeof(msg), digest, &digest_len) || digest_len < 20)
        return false;

    unsigned int offset = digest[19] & 0x0F;
    unsigned int bin = ((digest[offset] & 0x7F) << 24) |
                       (digest[offset + 1] << 16) |
                       (digest[offset + 2] << 8) |
                       digest[offset + 3];
    snprintf(out_code, TOTP_CODE_DIGITS + 1, "%0*u", TOTP_CODE_DIGITS, bin % 1000000);
    return true;
}

bool totp_code_now(const char *secret_b32, char out_code[TOTP_CODE_DIGITS + 1]) {
    return totp_code_at(secret_b32, time(NULL), out_code);
}

static bool code_is_welformed(const char *code) {
    if (!code) return false;
    size_t len = strlen(code);
    if (len != TOTP_CODE_DIGITS) return false;
    for (size_t i = 0; i < len; i++) {
        if (code[i] < '0' || code[i] > '9') return false;
    }
    return true;
}

bool totp_verify_at(const char *secret_b32, const char *code, time_t t) {
    if (!code_is_welformed(code)) return false;
    char expected[TOTP_CODE_DIGITS + 1];
    for (int drift = -TOTP_WINDOW_STEPS; drift <= TOTP_WINDOW_STEPS; drift++) {
        if (totp_code_at(secret_b32, t + (time_t)drift * TOTP_STEP_SEC, expected) &&
            CRYPTO_memcmp(expected, code, TOTP_CODE_DIGITS) == 0)
            return true;
    }
    return false;
}

bool totp_verify(const char *secret_b32, const char *code) {
    return totp_verify_at(secret_b32, code, time(NULL));
}

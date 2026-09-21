/**
 * @file aesgcm.h
 * @brief Self-contained AES-256-GCM for the TASFA crypto WASM module.
 *
 * WASI preview1 has no OpenSSL, so the sandboxed module carries its own
 * implementation: AES-256 encryption (FIPS-197) plus GHASH/GCM per SP
 * 800-38D.  Only the operations TASFA needs are provided: one-shot
 * encrypt-with-tag and decrypt-with-tag using a 96-bit IV.
 *
 * Correctness is established externally: tests/wasm_crypto_parity cross-checks
 * every operation against OpenSSL EVP on the host in both directions, plus
 * NIST GCMVS known-answer vectors.
 *
 * Internal-linkage only; include once per translation unit.
 */

#ifndef FLYBOARD_TASFA_AESGCM_H
#define FLYBOARD_TASFA_AESGCM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    uint32_t rk[60]; /**< Round keys, 14 rounds + initial. */
    uint8_t h[16];   /**< GHASH subkey E_K(0^128). */
    uint8_t s0[16];  /**< E_K(J0) for the final tag mask. */
} fb_aes256gcm_ctx;

static const uint8_t fb_aes_sbox[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab,
    0x76, 0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4,
    0x72, 0xc0, 0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71,
    0xd8, 0x31, 0x15, 0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2,
    0xeb, 0x27, 0xb2, 0x75, 0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6,
    0xb3, 0x29, 0xe3, 0x2f, 0x84, 0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb,
    0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf, 0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45,
    0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8, 0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5,
    0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2, 0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44,
    0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73, 0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a,
    0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb, 0xe0, 0x32, 0x3a, 0x0a, 0x49,
    0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79, 0xe7, 0xc8, 0x37, 0x6d,
    0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08, 0xba, 0x78, 0x25,
    0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a, 0x70, 0x3e,
    0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e, 0xe1,
    0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb,
    0x16,
};

static uint8_t fb_aes_xt(uint8_t x) { return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1b : 0)); }

static uint32_t fb_aes_subword(uint32_t w) {
    return (uint32_t)fb_aes_sbox[(w >> 24) & 0xff] << 24 |
           (uint32_t)fb_aes_sbox[(w >> 16) & 0xff] << 16 |
           (uint32_t)fb_aes_sbox[(w >> 8) & 0xff] << 8 |
           (uint32_t)fb_aes_sbox[w & 0xff];
}

static uint32_t fb_aes_rotword(uint32_t w) { return (w << 8) | (w >> 24); }

static void fb_aes256_keyexpand(const uint8_t key[32], uint32_t rk[60]) {
    static const uint32_t rcon[7] = {0x01000000, 0x02000000, 0x04000000, 0x08000000,
                                     0x10000000, 0x20000000, 0x40000000};
    for (int i = 0; i < 8; i++)
        rk[i] = (uint32_t)key[4 * i] << 24 | (uint32_t)key[4 * i + 1] << 16 |
                (uint32_t)key[4 * i + 2] << 8 | (uint32_t)key[4 * i + 3];
    for (int i = 8; i < 60; i++) {
        uint32_t t = rk[i - 1];
        if (i % 8 == 0)
            t = fb_aes_subword(fb_aes_rotword(t)) ^ rcon[i / 8 - 1];
        else if (i % 8 == 4)
            t = fb_aes_subword(t);
        rk[i] = rk[i - 8] ^ t;
    }
}

static void fb_aes256_encrypt_block(const fb_aes256gcm_ctx *ctx, const uint8_t in[16], uint8_t out[16]) {
    /* state[r][c]: AES input is column-major (in[c*4 + r]). */
    uint8_t s[4][4];
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            s[r][c] = in[c * 4 + r] ^ (uint8_t)(ctx->rk[c] >> (24 - 8 * r));
    for (int round = 1; round < 14; round++) {
        const uint32_t *rk = &ctx->rk[4 * round];
        uint8_t t[4][4];
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++)
                t[r][c] = fb_aes_sbox[s[r][(c + r) % 4]]; /* ShiftRows + SubBytes */
        for (int c = 0; c < 4; c++) {
            uint8_t a0 = t[0][c], a1 = t[1][c], a2 = t[2][c], a3 = t[3][c];
            s[0][c] = fb_aes_xt(a0) ^ (fb_aes_xt(a1) ^ a1) ^ a2 ^ a3;
            s[1][c] = a0 ^ fb_aes_xt(a1) ^ (fb_aes_xt(a2) ^ a2) ^ a3;
            s[2][c] = a0 ^ a1 ^ fb_aes_xt(a2) ^ (fb_aes_xt(a3) ^ a3);
            s[3][c] = (fb_aes_xt(a0) ^ a0) ^ a1 ^ a2 ^ fb_aes_xt(a3);
        }
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++)
                s[r][c] ^= (uint8_t)(rk[c] >> (24 - 8 * r));
    }
    const uint32_t *rk = &ctx->rk[56];
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            out[c * 4 + r] = fb_aes_sbox[s[r][(c + r) % 4]] ^ (uint8_t)(rk[c] >> (24 - 8 * r));
}

/* GHASH multiplication in GF(2^128), reflected-bit convention per 800-38D. */
static void fb_ghash_mul(uint8_t y[16], const uint8_t x[16]) {
    uint8_t z[16] = {0};
    uint8_t v[16];
    memcpy(v, y, 16);
    for (int i = 0; i < 16; i++) {
        for (int bit = 0; bit < 8; bit++) {
            if (x[i] & (1 << (7 - bit))) {
                for (int j = 0; j < 16; j++)
                    z[j] ^= v[j];
            }
            uint8_t lsb = v[15] & 1;
            for (int j = 15; j > 0; j--)
                v[j] = (uint8_t)((v[j] >> 1) | (v[j - 1] << 7));
            v[0] >>= 1;
            if (lsb)
                v[0] ^= 0xe1;
        }
    }
    memcpy(y, z, 16);
}

/* y must be zero-initialized by the caller on the first block. */
static void fb_ghash_update(const fb_aes256gcm_ctx *ctx, const uint8_t *data, size_t len,
                            uint8_t y[16]) {
    while (len > 0) {
        uint8_t blk[16] = {0};
        size_t n = len < 16 ? len : 16;
        memcpy(blk, data, n);
        for (int i = 0; i < 16; i++)
            y[i] ^= blk[i];
        fb_ghash_mul(y, ctx->h);
        data += n;
        len -= n;
    }
}

static void fb_gcm_inc32(uint8_t ctr[16]) {
    uint32_t c = (uint32_t)ctr[12] << 24 | (uint32_t)ctr[13] << 16 | (uint32_t)ctr[14] << 8 |
                 (uint32_t)ctr[15];
    c = c + 1;
    ctr[12] = (uint8_t)(c >> 24);
    ctr[13] = (uint8_t)(c >> 16);
    ctr[14] = (uint8_t)(c >> 8);
    ctr[15] = (uint8_t)c;
}

static void fb_gcm_ctr_crypt(const fb_aes256gcm_ctx *ctx, uint8_t ctr[16], const uint8_t *in,
                             uint8_t *out, size_t len) {
    while (len > 0) {
        uint8_t ks[16];
        size_t n = len < 16 ? len : 16;
        fb_gcm_inc32(ctr);
        fb_aes256_encrypt_block(ctx, ctr, ks);
        for (size_t i = 0; i < n; i++)
            out[i] = in[i] ^ ks[i];
        in += n;
        out += n;
        len -= n;
    }
}

static void fb_aes256gcm_init(fb_aes256gcm_ctx *ctx, const uint8_t key[32], const uint8_t iv[12]) {
    fb_aes256_keyexpand(key, ctx->rk);
    uint8_t zero[16] = {0};
    fb_aes256_encrypt_block(ctx, zero, ctx->h);
    uint8_t j0[16];
    memcpy(j0, iv, 12);
    j0[12] = j0[13] = j0[14] = 0;
    j0[15] = 1;
    fb_aes256_encrypt_block(ctx, j0, ctx->s0);
}

static void fb_gcm_tag(const fb_aes256gcm_ctx *ctx, const uint8_t *aad, size_t aad_len,
                       const uint8_t *ct, size_t ct_len, uint8_t tag[16]) {
    uint8_t y[16] = {0};
    fb_ghash_update(ctx, aad, aad_len, y);
    fb_ghash_update(ctx, ct, ct_len, y);
    uint8_t lens[16] = {0};
    uint64_t abits = (uint64_t)aad_len * 8, cbits = (uint64_t)ct_len * 8;
    for (int i = 0; i < 8; i++)
        lens[i] = (uint8_t)(abits >> (56 - 8 * i));
    for (int i = 0; i < 8; i++)
        lens[8 + i] = (uint8_t)(cbits >> (56 - 8 * i));
    fb_ghash_update(ctx, lens, 16, y);
    for (int i = 0; i < 16; i++)
        tag[i] = y[i] ^ ctx->s0[i];
}

/**
 * One-shot AES-256-GCM encrypt.  `ct_out` must hold pt_len + 16 bytes; the
 * 16-byte authentication tag is appended.
 */
static bool fb_aes256gcm_encrypt(const uint8_t key[32], const uint8_t iv[12], const uint8_t *aad,
                                 size_t aad_len, const uint8_t *pt, size_t pt_len,
                                 uint8_t *ct_out, uint8_t tag[16]) {
    fb_aes256gcm_ctx ctx;
    fb_aes256gcm_init(&ctx, key, iv);
    uint8_t ctr[16];
    memcpy(ctr, iv, 12);
    ctr[12] = ctr[13] = ctr[14] = 0;
    ctr[15] = 1;
    fb_gcm_ctr_crypt(&ctx, ctr, pt, ct_out, pt_len);
    fb_gcm_tag(&ctx, aad, aad_len, ct_out, pt_len, tag);
    return true;
}

/**
 * One-shot AES-256-GCM decrypt.  `tag` is the trailing 16 bytes separated by
 * the caller.  Returns false on authentication failure.
 */
static bool fb_aes256gcm_decrypt(const uint8_t key[32], const uint8_t iv[12], const uint8_t *aad,
                                 size_t aad_len, const uint8_t *ct, size_t ct_len,
                                 const uint8_t tag[16], uint8_t *pt_out) {
    fb_aes256gcm_ctx ctx;
    fb_aes256gcm_init(&ctx, key, iv);
    uint8_t expect[16];
    fb_gcm_tag(&ctx, aad, aad_len, ct, ct_len, expect);
    uint8_t diff = 0;
    for (int i = 0; i < 16; i++)
        diff |= expect[i] ^ tag[i];
    if (diff != 0)
        return false;
    uint8_t ctr[16];
    memcpy(ctr, iv, 12);
    ctr[12] = ctr[13] = ctr[14] = 0;
    ctr[15] = 1;
    fb_gcm_ctr_crypt(&ctx, ctr, ct, pt_out, ct_len);
    return true;
}

#endif /* FLYBOARD_TASFA_AESGCM_H */

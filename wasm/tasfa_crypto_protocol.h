/**
 * @file tasfa_crypto_protocol.h
 * @brief Shared request/response framing for the TASFA crypto WASM module.
 *
 * Both the WASI module (wasm/tasfa_crypto_module.c) and the native host
 * (src/wasm_host/tasfa_crypto_wasm.c) speak this protocol over stdin/stdout.
 * Multi-byte fields are big-endian.  One request per process invocation;
 * the module exits after answering.
 *
 * Request:
 *   u8     op            1 = encrypt, 2 = decrypt
 *   u8     key[32]       AES-256 key
 *   u8     iv_seed[12]   TASFA IV seed; chunk_index is XORed into bytes 8..11
 *   be32   chunk_index
 *   be16   sid_len
 *   u8     sid[sid_len]  session/upload id, AAD prefix
 *   be32   data_len
 *   u8     data[data_len]
 *
 * Response:
 *   u8     status       0 = ok, 1 = operation/auth failure
 *   be32   out_len
 *   u8     out[out_len]  encrypt: ciphertext + 16-byte tag
 *                        decrypt: plaintext (only when status == 0)
 */

#ifndef FLYBOARD_TASFA_CRYPTO_PROTOCOL_H
#define FLYBOARD_TASFA_CRYPTO_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#define TASFA_CRYPTO_OP_ENCRYPT 1u
#define TASFA_CRYPTO_OP_DECRYPT 2u

#define TASFA_CRYPTO_KEY_LEN 32u
#define TASFA_CRYPTO_IV_SEED_LEN 12u
#define TASFA_CRYPTO_TAG_LEN 16u
#define TASFA_CRYPTO_MAX_SID_LEN 512u
/* Mirrors the 512-byte AAD buffer in src/handlers/tasfa/crypto.c. */

static inline uint32_t fb_be32_read(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | (uint32_t)p[3];
}

static inline uint16_t fb_be16_read(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

static inline void fb_be32_write(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static inline void fb_be16_write(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

#endif /* FLYBOARD_TASFA_CRYPTO_PROTOCOL_H */

/**
 * @file tasfa_crypto_module.c
 * @brief WASI preview1 module exposing TASFA block crypto to the host.
 *
 * Reads one framed request from stdin (see tasfa_crypto_protocol.h), runs the
 * AES-256-GCM block operation with the same IV derivation and AAD layout as
 * src/handlers/tasfa/crypto.c, and writes one framed response to stdout.
 * Process-per-request: stateless by construction, no filesystem or network
 * capability is used, which is the point of the sandbox.
 *
 * Build: see the wasm-tasfa-crypto target in the top-level Makefile.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aesgcm.h"
#include "tasfa_crypto_protocol.h"

static void derive_stream_iv(uint8_t out[12], const uint8_t seed[12], int32_t chunk_index) {
    memcpy(out, seed, 12);
    out[8] ^= (uint8_t)((chunk_index >> 24) & 0xff);
    out[9] ^= (uint8_t)((chunk_index >> 16) & 0xff);
    out[10] ^= (uint8_t)((chunk_index >> 8) & 0xff);
    out[11] ^= (uint8_t)(chunk_index & 0xff);
}

static int build_stream_aad(uint8_t *out, size_t out_len, const char *sid, int32_t chunk_index) {
    int n = snprintf((char *)out, out_len, "%s:%d", sid ? sid : "", chunk_index);
    if (n < 0 || (size_t)n >= out_len)
        return -1;
    return n;
}

static uint8_t *read_all_stdin(size_t *len_out) {
    size_t cap = 4096, len = 0;
    uint8_t *buf = (uint8_t *)malloc(cap);
    if (!buf)
        return NULL;
    for (;;) {
        if (len == cap) {
            cap *= 2;
            uint8_t *nb = (uint8_t *)realloc(buf, cap);
            if (!nb) {
                free(buf);
                return NULL;
            }
            buf = nb;
        }
        size_t n = fread(buf + len, 1, cap - len, stdin);
        len += n;
        if (n == 0)
            break;
    }
    *len_out = len;
    return buf;
}

static int write_all_stdout(const uint8_t *buf, size_t len) {
    while (len > 0) {
        size_t n = fwrite(buf, 1, len, stdout);
        if (n == 0)
            return -1;
        buf += n;
        len -= n;
    }
    return fflush(stdout) == 0 ? 0 : -1;
}

int main(void) {
    size_t req_len = 0;
    uint8_t *req = read_all_stdin(&req_len);
    uint8_t fail_resp[5] = {1, 0, 0, 0, 0};
    if (!req) {
        write_all_stdout(fail_resp, sizeof(fail_resp));
        return 0;
    }

    /* Fixed header: op(1) + key(32) + iv_seed(12) + chunk_index(4) + sid_len(2) */
    const size_t fixed = 1 + TASFA_CRYPTO_KEY_LEN + TASFA_CRYPTO_IV_SEED_LEN + 4 + 2;
    if (req_len < fixed + 4) {
        free(req);
        write_all_stdout(fail_resp, sizeof(fail_resp));
        return 0;
    }

    const uint8_t op = req[0];
    const uint8_t *key = req + 1;
    const uint8_t *iv_seed = req + 1 + TASFA_CRYPTO_KEY_LEN;
    const int32_t chunk_index = (int32_t)fb_be32_read(req + 45);
    const uint16_t sid_len = fb_be16_read(req + 49);
    if (sid_len > TASFA_CRYPTO_MAX_SID_LEN || req_len < fixed + sid_len + 4) {
        free(req);
        write_all_stdout(fail_resp, sizeof(fail_resp));
        return 0;
    }
    char sid[TASFA_CRYPTO_MAX_SID_LEN + 1];
    memcpy(sid, req + fixed, sid_len);
    sid[sid_len] = '\0';
    const uint8_t *data = req + fixed + sid_len + 4;
    const size_t data_len = fb_be32_read(req + fixed + sid_len);

    /* Defensive: the declared data_len must match the actual payload. */
    if (req_len != fixed + sid_len + 4 + data_len) {
        free(req);
        write_all_stdout(fail_resp, sizeof(fail_resp));
        return 0;
    }

    uint8_t iv[12];
    derive_stream_iv(iv, iv_seed, chunk_index);
    uint8_t aad[TASFA_CRYPTO_MAX_SID_LEN + 16];
    int aad_len = build_stream_aad(aad, sizeof(aad), sid, chunk_index);
    if (aad_len <= 0) {
        free(req);
        write_all_stdout(fail_resp, sizeof(fail_resp));
        return 0;
    }

    size_t out_cap = 5 + data_len + TASFA_CRYPTO_TAG_LEN;
    uint8_t *resp = (uint8_t *)malloc(out_cap);
    if (!resp) {
        free(req);
        write_all_stdout(fail_resp, sizeof(fail_resp));
        return 0;
    }

    uint8_t status = 1;
    uint32_t out_len = 0;
    if (op == TASFA_CRYPTO_OP_ENCRYPT) {
        uint8_t tag[TASFA_CRYPTO_TAG_LEN];
        fb_aes256gcm_encrypt(key, iv, aad, (size_t)aad_len, data, data_len, resp + 5, tag);
        memcpy(resp + 5 + data_len, tag, TASFA_CRYPTO_TAG_LEN);
        out_len = (uint32_t)(data_len + TASFA_CRYPTO_TAG_LEN);
        status = 0;
    } else if (op == TASFA_CRYPTO_OP_DECRYPT) {
        if (data_len >= TASFA_CRYPTO_TAG_LEN &&
            fb_aes256gcm_decrypt(key, iv, aad, (size_t)aad_len, data,
                                 data_len - TASFA_CRYPTO_TAG_LEN,
                                 data + data_len - TASFA_CRYPTO_TAG_LEN, resp + 5)) {
            out_len = (uint32_t)(data_len - TASFA_CRYPTO_TAG_LEN);
            status = 0;
        }
    }

    resp[0] = status;
    fb_be32_write(resp + 1, out_len);
    write_all_stdout(resp, 5 + out_len);
    free(resp);
    free(req);
    return 0;
}

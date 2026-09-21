/**
 * @file tasfa_crypto_ref.c
 * @brief Native OpenSSL EVP reference implementation of the TASFA crypto
 *        WASM protocol, used to prove interop with the sandboxed module.
 *
 * Speaks the exact stdin/stdout framing from tasfa_crypto_protocol.h using
 * the production code path (EVP_aes_256_gcm, same IV derivation and AAD).
 * The parity driver feeds identical requests to this binary and to the WASM
 * module and cross-checks the outputs.
 *
 * Build (see Makefile target wasm-tasfa-crypto-test):
 *   cc -O2 -o build-wasm/tasfa_crypto_ref wasm/tasfa_crypto_ref.c -lcrypto
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <openssl/evp.h>

#include "../wasm/tasfa_crypto_protocol.h"

static void derive_stream_iv(unsigned char out[12], const unsigned char seed[12], int chunk_index) {
    memcpy(out, seed, 12);
    out[8] ^= (unsigned char)((chunk_index >> 24) & 0xff);
    out[9] ^= (unsigned char)((chunk_index >> 16) & 0xff);
    out[10] ^= (unsigned char)((chunk_index >> 8) & 0xff);
    out[11] ^= (unsigned char)(chunk_index & 0xff);
}

static int build_stream_aad(unsigned char *out, size_t out_len, const char *sid, int chunk_index) {
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

    const size_t fixed = 1 + TASFA_CRYPTO_KEY_LEN + TASFA_CRYPTO_IV_SEED_LEN + 4 + 2;
    if (req_len < fixed + 4) {
        free(req);
        write_all_stdout(fail_resp, sizeof(fail_resp));
        return 0;
    }

    const uint8_t op = req[0];
    const unsigned char *key = req + 1;
    const unsigned char *iv_seed = req + 1 + TASFA_CRYPTO_KEY_LEN;
    const int chunk_index = (int)fb_be32_read(req + 45);
    const uint16_t sid_len = fb_be16_read(req + 49);
    if (sid_len > TASFA_CRYPTO_MAX_SID_LEN || req_len < fixed + sid_len + 4) {
        free(req);
        write_all_stdout(fail_resp, sizeof(fail_resp));
        return 0;
    }
    char sid[TASFA_CRYPTO_MAX_SID_LEN + 1];
    memcpy(sid, req + fixed, sid_len);
    sid[sid_len] = '\0';
    const unsigned char *data = req + fixed + sid_len + 4;
    const size_t data_len = fb_be32_read(req + fixed + sid_len);
    if (req_len != fixed + sid_len + 4 + data_len) {
        free(req);
        write_all_stdout(fail_resp, sizeof(fail_resp));
        return 0;
    }

    unsigned char iv[12];
    derive_stream_iv(iv, iv_seed, chunk_index);
    unsigned char aad[TASFA_CRYPTO_MAX_SID_LEN + 16];
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
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (ctx) {
        int aadl = 0, outl = 0, finl = 0;
        if (op == TASFA_CRYPTO_OP_ENCRYPT &&
            EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) == 1 &&
            EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, NULL) == 1 &&
            EVP_EncryptInit_ex(ctx, NULL, NULL, key, iv) == 1 &&
            EVP_EncryptUpdate(ctx, NULL, &aadl, aad, aad_len) == 1 &&
            (data_len == 0 || EVP_EncryptUpdate(ctx, resp + 5, &outl, data, (int)data_len) == 1) &&
            EVP_EncryptFinal_ex(ctx, resp + 5 + outl, &finl) == 1) {
            unsigned char tag[TASFA_CRYPTO_TAG_LEN];
            if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag) == 1) {
                memcpy(resp + 5 + outl + finl, tag, TASFA_CRYPTO_TAG_LEN);
                out_len = (uint32_t)(outl + finl + TASFA_CRYPTO_TAG_LEN);
                status = 0;
            }
        } else if (op == TASFA_CRYPTO_OP_DECRYPT && data_len >= TASFA_CRYPTO_TAG_LEN) {
            const unsigned char *tag = data + data_len - TASFA_CRYPTO_TAG_LEN;
            size_t ct_len = data_len - TASFA_CRYPTO_TAG_LEN;
            if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) == 1 &&
                EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, NULL) == 1 &&
                EVP_DecryptInit_ex(ctx, NULL, NULL, key, iv) == 1 &&
                EVP_DecryptUpdate(ctx, NULL, &aadl, aad, aad_len) == 1 &&
                (ct_len == 0 || EVP_DecryptUpdate(ctx, resp + 5, &outl, data, (int)ct_len) == 1) &&
                EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, (void *)tag) == 1 &&
                EVP_DecryptFinal_ex(ctx, resp + 5 + outl, &finl) == 1) {
                out_len = (uint32_t)(outl + finl);
                status = 0;
            }
        }
        EVP_CIPHER_CTX_free(ctx);
    }

    resp[0] = status;
    fb_be32_write(resp + 1, out_len);
    write_all_stdout(resp, 5 + out_len);
    free(resp);
    free(req);
    return 0;
}

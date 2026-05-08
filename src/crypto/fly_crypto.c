#define _GNU_SOURCE
#include "fly_crypto.h"
#include <cwist/security/pqc/pqc_sig.h>
#include <cwist/core/mem/alloc.h>
#include <openssl/evp.h>
#include <string.h>

static cwist_pqc_sig_keypair_t *g_sign_key = NULL;

static char *base64_encode(const uint8_t *data, size_t len) {
    int out_len = 4 * ((int)len + 2) / 3;
    char *out = (char *)cwist_alloc((size_t)out_len + 1);
    if (!out) return NULL;
    EVP_EncodeBlock((unsigned char *)out, data, (int)len);
    out[out_len] = '\0';
    return out;
}

static uint8_t *base64_decode(const char *str, size_t *out_len) {
    size_t len = strlen(str);
    int buf_len = 3 * (int)len / 4;
    uint8_t *out = (uint8_t *)cwist_alloc((size_t)buf_len + 1);
    if (!out) return NULL;
    int rc = EVP_DecodeBlock(out, (const unsigned char *)str, (int)len);
    if (rc < 0) {
        cwist_free(out);
        return NULL;
    }
    if (len > 0 && str[len - 1] == '=') rc--;
    if (len > 1 && str[len - 2] == '=') rc--;
    *out_len = (size_t)rc;
    return out;
}

bool fly_crypto_init(void) {
    if (g_sign_key) return true;
    cwist_error_t err = cwist_pqc_sig_keygen(&g_sign_key);
    return err.errtype == CWIST_ERR_INT16 && err.error.err_i16 == 0;
}

bool fly_crypto_sign(const uint8_t *msg, size_t msg_len, char **sig_b64) {
    if (!g_sign_key || !msg || !sig_b64) return false;
    uint8_t *sig = NULL;
    size_t sig_len = 0;
    cwist_error_t err = cwist_pqc_sig_sign(g_sign_key, msg, msg_len, &sig, &sig_len);
    if (err.errtype != CWIST_ERR_INT16 || err.error.err_i16 != 0) return false;
    char *b64 = base64_encode(sig, sig_len);
    cwist_free(sig);
    if (!b64) return false;
    *sig_b64 = b64;
    return true;
}

bool fly_crypto_verify(const uint8_t *msg, size_t msg_len, const char *sig_b64) {
    if (!g_sign_key || !msg || !sig_b64) return false;
    size_t sig_len = 0;
    uint8_t *sig = base64_decode(sig_b64, &sig_len);
    if (!sig) return false;
    bool ok = cwist_pqc_sig_verify(g_sign_key, msg, msg_len, sig, sig_len);
    cwist_free(sig);
    return ok;
}

bool fly_crypto_pubkey_export(char **pk_b64) {
    if (!g_sign_key || !pk_b64) return false;
    uint8_t *pk = NULL;
    size_t pk_len = 0;
    cwist_error_t err = cwist_pqc_sig_pubkey_export(g_sign_key, &pk, &pk_len);
    if (err.errtype != CWIST_ERR_INT16 || err.error.err_i16 != 0) return false;
    char *b64 = base64_encode(pk, pk_len);
    cwist_free(pk);
    if (!b64) return false;
    *pk_b64 = b64;
    return true;
}

void fly_crypto_cleanup(void) {
    cwist_pqc_sig_free(g_sign_key);
    g_sign_key = NULL;
}

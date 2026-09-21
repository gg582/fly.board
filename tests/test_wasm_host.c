/**
 * @file test_wasm_host.c
 * @brief End-to-end check of the wasm_host TASFA crypto wrapper.
 *
 * Links only src/wasm_host/tasfa_crypto_wasm.c and cross-checks it against
 * the OpenSSL EVP reference binary (build-wasm/tasfa_crypto_ref) in both
 * directions.  Requires TASFA_CRYPTO_WASM to point at the built module and
 * the ref binary to exist; used by the wasm-tasfa-crypto-host-test target.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/wasm_host/tasfa_crypto_wasm.h"

static int run_ref(int op, const unsigned char *key, const unsigned char *iv_seed, int chunk,
                   const char *sid, const unsigned char *data, size_t data_len,
                   unsigned char *out, size_t *out_len) {
    /* Framed protocol identical to wasm/tasfa_crypto_protocol.h, built here
     * to keep this test self-contained. */
    size_t sid_len = strlen(sid);
    size_t fixed = 1 + 32 + 12 + 4 + 2;
    size_t total = fixed + sid_len + 4 + data_len;
    unsigned char *req = (unsigned char *)malloc(total);
    if (!req)
        return -1;
    req[0] = (unsigned char)op;
    memcpy(req + 1, key, 32);
    memcpy(req + 33, iv_seed, 12);
    req[45] = (unsigned char)((unsigned)chunk >> 24);
    req[46] = (unsigned char)((unsigned)chunk >> 16);
    req[47] = (unsigned char)((unsigned)chunk >> 8);
    req[48] = (unsigned char)chunk;
    req[49] = (unsigned char)(sid_len >> 8);
    req[50] = (unsigned char)sid_len;
    memcpy(req + fixed, sid, sid_len);
    req[fixed + sid_len] = (unsigned char)(data_len >> 24);
    req[fixed + sid_len + 1] = (unsigned char)(data_len >> 16);
    req[fixed + sid_len + 2] = (unsigned char)(data_len >> 8);
    req[fixed + sid_len + 3] = (unsigned char)data_len;
    memcpy(req + fixed + sid_len + 4, data, data_len);

    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "build-wasm/tasfa_crypto_ref > /tmp/ref_resp.bin");
    FILE *fp = popen(cmd, "w");
    if (!fp) {
        free(req);
        return -1;
    }
    fwrite(req, 1, total, fp);
    int rc = pclose(fp);
    free(req);
    if (rc != 0)
        return -1;
    fp = fopen("/tmp/ref_resp.bin", "rb");
    if (!fp)
        return -1;
    unsigned char hdr[5];
    if (fread(hdr, 1, 5, fp) != 5) {
        fclose(fp);
        return -1;
    }
    size_t olen = (size_t)hdr[1] << 24 | (size_t)hdr[2] << 16 | (size_t)hdr[3] << 8 | hdr[4];
    if (fread(out, 1, olen, fp) != olen) {
        fclose(fp);
        return -1;
    }
    fclose(fp);
    *out_len = olen;
    return hdr[0];
}

int main(void) {
    if (!tasfa_wasm_crypto_available()) {
        fprintf(stderr, "SKIP: TASFA_CRYPTO_WASM not set or module missing\n");
        return 0;
    }

    unsigned char key[32], iv_seed[12], pt[1024];
    for (int i = 0; i < 32; i++)
        key[i] = (unsigned char)(i * 7 + 1);
    for (int i = 0; i < 12; i++)
        iv_seed[i] = (unsigned char)(i * 3 + 5);
    for (int i = 0; i < (int)sizeof(pt); i++)
        pt[i] = (unsigned char)(i * 31 + 17);

    const char *sid = "host-test-session";
    unsigned char ct_host[1040], ct_ref[1040];
    unsigned char pt_back[1024];
    size_t ct_host_len = 0, ct_ref_len = 0;

    if (!tasfa_wasm_encrypt_block(key, iv_seed, 3, sid, pt, sizeof(pt), ct_host, &ct_host_len)) {
        fprintf(stderr, "FAIL: host encrypt\n");
        return 1;
    }
    if (run_ref(1, key, iv_seed, 3, sid, pt, sizeof(pt), ct_ref, &ct_ref_len) != 0 ||
        ct_ref_len != ct_host_len || memcmp(ct_host, ct_ref, ct_host_len) != 0) {
        fprintf(stderr, "FAIL: host encrypt != EVP reference\n");
        return 1;
    }

    if (!tasfa_wasm_decrypt_block(key, iv_seed, 3, sid, ct_ref, ct_ref_len, pt_back, sizeof(pt)) ||
        memcmp(pt_back, pt, sizeof(pt)) != 0) {
        fprintf(stderr, "FAIL: host decrypt of reference output\n");
        return 1;
    }

    /* Tampered tag must fail on the host path too. */
    ct_ref[ct_ref_len - 1] ^= 1;
    if (tasfa_wasm_decrypt_block(key, iv_seed, 3, sid, ct_ref, ct_ref_len, pt_back, sizeof(pt))) {
        fprintf(stderr, "FAIL: host accepted tampered tag\n");
        return 1;
    }

    printf("PASS: wasm_host wrapper interops with OpenSSL EVP both directions\n");
    return 0;
}

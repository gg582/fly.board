#include "fly_crypto.h"
#include <cwist/core/mem/alloc.h>
#include <openssl/bytestring.h>
#include <openssl/evp.h>
#include <openssl/mem.h>
#include <openssl/mldsa.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Post signatures: ML-DSA-65 (FIPS 204) from the BoringSSL that cwist
 * embeds. The 32-byte key seed is kept in a 0600 file so signatures stay
 * verifiable across restarts; the key is loaded before cwist forks, so all
 * workers share it. Signing and verifying only read the key, so concurrent
 * calls from worker threads are safe. */

/* Domain separation: a signature over a post can never be replayed as a
 * signature over anything else this key might sign later. */
static const uint8_t k_context[] = "fly.board/post/v1";
#define CONTEXT_LEN (sizeof(k_context) - 1)

static struct MLDSA65_private_key g_priv;
static struct MLDSA65_public_key g_pub;
static uint8_t g_pub_encoded[MLDSA65_PUBLIC_KEY_BYTES];
static bool g_ready = false;

static char *base64_encode(const uint8_t *data, size_t len) {
    size_t out_len = 4 * ((len + 2) / 3);
    char *out = (char *)cwist_alloc(out_len + 1);
    if (!out) return NULL;
    EVP_EncodeBlock((unsigned char *)out, data, len);
    out[out_len] = '\0';
    return out;
}

static uint8_t *base64_decode(const char *str, size_t *out_len) {
    size_t len = strlen(str);
    if (len == 0 || len % 4 != 0) return NULL;
    uint8_t *out = (uint8_t *)cwist_alloc(3 * len / 4 + 1);
    if (!out) return NULL;
    int rc = EVP_DecodeBlock(out, (const unsigned char *)str, len);
    if (rc < 0) {
        cwist_free(out);
        return NULL;
    }
    if (str[len - 1] == '=') rc--;
    if (str[len - 2] == '=') rc--;
    *out_len = (size_t)rc;
    return out;
}

static bool hex_decode(const char *hex, uint8_t *out, size_t out_len) {
    for (size_t i = 0; i < out_len; i++) {
        unsigned int byte;
        if (sscanf(hex + 2 * i, "%2x", &byte) != 1) return false;
        out[i] = (uint8_t)byte;
    }
    return true;
}

/* Read the seed, or create the file with a fresh one when it does not
 * exist. An existing file that cannot be read is an error: generating a
 * new key there would silently invalidate every stored signature. */
static bool load_or_create_seed(const char *path, uint8_t seed[MLDSA_SEED_BYTES], bool *created) {
    *created = false;
    FILE *f = fopen(path, "r");
    if (f) {
        char hex[2 * MLDSA_SEED_BYTES + 2] = {0};
        bool ok = fgets(hex, sizeof(hex), f) != NULL && strlen(hex) >= 2 * MLDSA_SEED_BYTES &&
                  hex_decode(hex, seed, MLDSA_SEED_BYTES);
        fclose(f);
        if (!ok) fprintf(stderr, "[pqc] %s is not a valid ML-DSA-65 seed\n", path);
        return ok;
    }
    if (errno != ENOENT) {
        fprintf(stderr, "[pqc] cannot read %s: %s\n", path, strerror(errno));
        return false;
    }

    if (!MLDSA65_generate_key(g_pub_encoded, seed, &g_priv)) return false;
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        fprintf(stderr, "[pqc] cannot create %s: %s\n", path, strerror(errno));
        return false;
    }
    char hex[2 * MLDSA_SEED_BYTES + 2];
    for (size_t i = 0; i < MLDSA_SEED_BYTES; i++) snprintf(hex + 2 * i, 3, "%02x", seed[i]);
    hex[2 * MLDSA_SEED_BYTES] = '\n';
    hex[2 * MLDSA_SEED_BYTES + 1] = '\0';
    bool ok = write(fd, hex, 2 * MLDSA_SEED_BYTES + 1) == (ssize_t)(2 * MLDSA_SEED_BYTES + 1) && fsync(fd) == 0;
    close(fd);
    if (!ok) {
        unlink(path);
        return false;
    }
    *created = true;
    return true;
}

bool fly_crypto_init(const char *seed_path) {
    if (g_ready) return true;
    uint8_t seed[MLDSA_SEED_BYTES];
    bool created = false;
    if (!load_or_create_seed(seed_path, seed, &created)) return false;
    bool ok = MLDSA65_private_key_from_seed(&g_priv, seed, sizeof(seed)) &&
              MLDSA65_public_from_private(&g_pub, &g_priv);
    OPENSSL_cleanse(seed, sizeof(seed));
    if (!ok) return false;

    CBB cbb;
    if (!CBB_init_fixed(&cbb, g_pub_encoded, sizeof(g_pub_encoded)) ||
        !MLDSA65_marshal_public_key(&cbb, &g_pub) || CBB_len(&cbb) != sizeof(g_pub_encoded)) {
        return false;
    }
    if (created) fprintf(stderr, "[pqc] generated a new ML-DSA-65 signing key in %s\n", seed_path);
    g_ready = true;
    return true;
}

bool fly_crypto_sign(const uint8_t *msg, size_t msg_len, char **sig_b64) {
    if (!g_ready || !msg || !sig_b64) return false;
    uint8_t sig[MLDSA65_SIGNATURE_BYTES];
    if (!MLDSA65_sign(sig, &g_priv, msg, msg_len, k_context, CONTEXT_LEN)) {
        fprintf(stderr, "[pqc] signing failed\n");
        return false;
    }
    char *b64 = base64_encode(sig, sizeof(sig));
    if (!b64) return false;
    *sig_b64 = b64;
    return true;
}

bool fly_crypto_verify(const uint8_t *msg, size_t msg_len, const char *sig_b64) {
    if (!g_ready || !msg || !sig_b64 || !sig_b64[0]) return false;
    size_t sig_len = 0;
    uint8_t *sig = base64_decode(sig_b64, &sig_len);
    if (!sig) return false;
    bool ok = sig_len == MLDSA65_SIGNATURE_BYTES &&
              MLDSA65_verify(&g_pub, sig, sig_len, msg, msg_len, k_context, CONTEXT_LEN) == 1;
    cwist_free(sig);
    return ok;
}

bool fly_crypto_pubkey_export(char **pk_b64) {
    if (!g_ready || !pk_b64) return false;
    char *b64 = base64_encode(g_pub_encoded, sizeof(g_pub_encoded));
    if (!b64) return false;
    *pk_b64 = b64;
    return true;
}

void fly_crypto_cleanup(void) {
    OPENSSL_cleanse(&g_priv, sizeof(g_priv));
    g_ready = false;
}

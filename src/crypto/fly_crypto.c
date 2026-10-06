#include "fly_crypto.h"
#include <cwist/core/mem/alloc.h>
#include <openssl/bytestring.h>
#include <openssl/evp.h>
#include <openssl/mem.h>
#include <openssl/mldsa.h>
#include <openssl/sha.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Post signatures: ML-DSA-65 (FIPS 204) from the BoringSSL that cwist
 * embeds. The 32-byte key seed is kept in a 0600 file so signatures stay
 * verifiable across restarts; the key is loaded before cwist forks, so all
 * workers share it. Signing and verifying only read the keys, so concurrent
 * calls from worker threads are safe.
 *
 * A stored signature is "<key id>:<base64>", the key id being the first 16
 * hex digits of SHA-256 over the encoded public key. Old public keys stay
 * registered for verification (see db/pqc_keys.c), so a site that moves to
 * a new host, or rotates its key, keeps verifying earlier posts without the
 * old private key. A bare "<base64>" from before key ids is checked against
 * the current key. */

/* Domain separation: a post signature can never be replayed as a backup
 * manifest signature, or the other way round. */
#define CTX_POST "fly.board/post/v1"
#define CTX_BACKUP "fly.board/backup-manifest/v1"

#define MAX_VERIFY_KEYS 32

typedef struct {
    char id[FLY_PQC_KEY_ID_LEN + 1];
    struct MLDSA65_public_key pub;
} verify_key_t;

static struct MLDSA65_private_key g_priv;
static uint8_t g_pub_encoded[MLDSA65_PUBLIC_KEY_BYTES];
static verify_key_t g_keys[MAX_VERIFY_KEYS]; /* g_keys[0] is the signing key */
static int g_nkeys = 0;
static bool g_ready = false;

static char *base64_encode(const uint8_t *data, size_t len) {
    size_t out_len = 4 * ((len + 2) / 3);
    char *out = (char *)cwist_alloc(out_len + 1);
    if (!out) return NULL;
    EVP_EncodeBlock((unsigned char *)out, data, len);
    out[out_len] = '\0';
    return out;
}

static uint8_t *base64_decode(const char *str, size_t len, size_t *out_len) {
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

static void key_id_of(const uint8_t *encoded, size_t len, char out[FLY_PQC_KEY_ID_LEN + 1]) {
    uint8_t digest[SHA256_DIGEST_LENGTH];
    SHA256(encoded, len, digest);
    for (int i = 0; i < FLY_PQC_KEY_ID_LEN / 2; i++) snprintf(out + 2 * i, 3, "%02x", digest[i]);
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
              MLDSA65_public_from_private(&g_keys[0].pub, &g_priv);
    OPENSSL_cleanse(seed, sizeof(seed));
    if (!ok) return false;

    CBB cbb;
    if (!CBB_init_fixed(&cbb, g_pub_encoded, sizeof(g_pub_encoded)) ||
        !MLDSA65_marshal_public_key(&cbb, &g_keys[0].pub) || CBB_len(&cbb) != sizeof(g_pub_encoded)) {
        return false;
    }
    key_id_of(g_pub_encoded, sizeof(g_pub_encoded), g_keys[0].id);
    if (g_nkeys == 0) g_nkeys = 1; /* keep verify-only keys added earlier */
    if (created) fprintf(stderr, "[pqc] generated a new ML-DSA-65 signing key %s in %s\n", g_keys[0].id, seed_path);
    g_ready = true;
    return true;
}

const char *fly_crypto_key_id(void) {
    return g_ready ? g_keys[0].id : NULL;
}

static bool parse_public_key(const char *pk_b64, struct MLDSA65_public_key *out, char id[FLY_PQC_KEY_ID_LEN + 1]) {
    size_t len = 0;
    uint8_t *raw = pk_b64 ? base64_decode(pk_b64, strlen(pk_b64), &len) : NULL;
    if (!raw) return false;
    CBS cbs;
    CBS_init(&cbs, raw, len);
    bool ok = len == MLDSA65_PUBLIC_KEY_BYTES && MLDSA65_parse_public_key(out, &cbs) && CBS_len(&cbs) == 0;
    if (ok && id) key_id_of(raw, len, id);
    cwist_free(raw);
    return ok;
}

bool fly_crypto_add_public_key(const char *pk_b64) {
    verify_key_t k;
    if (!parse_public_key(pk_b64, &k.pub, k.id)) return false;
    for (int i = 0; i < g_nkeys; i++) {
        if (strcmp(g_keys[i].id, k.id) == 0) return true;
    }
    if (g_nkeys >= MAX_VERIFY_KEYS) return false;
    /* Verify-only setups (backup tools) have no signing key in slot 0. */
    if (g_nkeys == 0 && !g_ready) {
        static verify_key_t empty;
        g_keys[g_nkeys++] = empty;
    }
    g_keys[g_nkeys++] = k;
    return true;
}

static bool sign_ctx(const char *ctx, const uint8_t *msg, size_t msg_len, char **sig_out) {
    if (!g_ready || !msg || !sig_out) return false;
    uint8_t sig[MLDSA65_SIGNATURE_BYTES];
    if (!MLDSA65_sign(sig, &g_priv, msg, msg_len, (const uint8_t *)ctx, strlen(ctx))) {
        fprintf(stderr, "[pqc] signing failed\n");
        return false;
    }
    char *b64 = base64_encode(sig, sizeof(sig));
    if (!b64) return false;
    size_t n = FLY_PQC_KEY_ID_LEN + 1 + strlen(b64) + 1;
    char *out = (char *)cwist_alloc(n);
    if (out) snprintf(out, n, "%s:%s", g_keys[0].id, b64);
    cwist_free(b64);
    if (!out) return false;
    *sig_out = out;
    return true;
}

static bool verify_ctx(const char *ctx, const struct MLDSA65_public_key *only, const uint8_t *msg, size_t msg_len,
                       const char *sig_str) {
    if (!msg || !sig_str || !sig_str[0]) return false;
    const struct MLDSA65_public_key *pub = only;
    const char *b64 = sig_str;
    const char *colon = strchr(sig_str, ':');
    if (colon) {
        if (colon - sig_str != FLY_PQC_KEY_ID_LEN) return false;
        b64 = colon + 1;
        if (!pub) {
            for (int i = 0; i < g_nkeys; i++) {
                if (g_keys[i].id[0] && strncmp(g_keys[i].id, sig_str, FLY_PQC_KEY_ID_LEN) == 0) {
                    pub = &g_keys[i].pub;
                    break;
                }
            }
        }
    } else if (!pub) {
        if (!g_ready) return false;
        pub = &g_keys[0].pub; /* signature from before key ids */
    }
    if (!pub) return false;
    size_t sig_len = 0;
    uint8_t *sig = base64_decode(b64, strlen(b64), &sig_len);
    if (!sig) return false;
    bool ok = sig_len == MLDSA65_SIGNATURE_BYTES &&
              MLDSA65_verify(pub, sig, sig_len, msg, msg_len, (const uint8_t *)ctx, strlen(ctx)) == 1;
    cwist_free(sig);
    return ok;
}

bool fly_crypto_sign(const uint8_t *msg, size_t msg_len, char **sig_out) {
    return sign_ctx(CTX_POST, msg, msg_len, sig_out);
}

bool fly_crypto_verify(const uint8_t *msg, size_t msg_len, const char *sig) {
    return verify_ctx(CTX_POST, NULL, msg, msg_len, sig);
}

bool fly_crypto_sign_manifest(const uint8_t *msg, size_t msg_len, char **sig_out) {
    return sign_ctx(CTX_BACKUP, msg, msg_len, sig_out);
}

bool fly_crypto_verify_manifest(const char *pk_b64, const uint8_t *msg, size_t msg_len, const char *sig) {
    struct MLDSA65_public_key pub;
    char id[FLY_PQC_KEY_ID_LEN + 1];
    if (!parse_public_key(pk_b64, &pub, id)) return false;
    /* The signature must name the key it is checked against. */
    if (!sig || strncmp(sig, id, FLY_PQC_KEY_ID_LEN) != 0 || sig[FLY_PQC_KEY_ID_LEN] != ':') return false;
    return verify_ctx(CTX_BACKUP, &pub, msg, msg_len, sig);
}

bool fly_crypto_key_id_of(const char *pk_b64, char out[FLY_PQC_KEY_ID_LEN + 1]) {
    struct MLDSA65_public_key pub;
    return parse_public_key(pk_b64, &pub, out);
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
    g_nkeys = 0;
}

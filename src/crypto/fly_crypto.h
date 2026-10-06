#ifndef FLY_CRYPTO_H
#define FLY_CRYPTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Hex digits in a key id: SHA-256 of the encoded public key, truncated. */
#define FLY_PQC_KEY_ID_LEN 16

/**
 * @brief Initialize flyboard PQC signing subsystem.
 * Loads the ML-DSA-65 key seed from @p seed_path, creating the file (0600)
 * with a fresh key when it does not exist yet. Call before forking workers.
 * @return true on success; false when an existing seed file is unreadable.
 */
bool fly_crypto_init(const char *seed_path);

/** @brief Id of the signing key; NULL before fly_crypto_init(). */
const char *fly_crypto_key_id(void);

/**
 * @brief Register an extra public key (base64) for verification only, e.g.
 * the key of a previous host. Call before forking workers.
 */
bool fly_crypto_add_public_key(const char *pk_b64);

/** @brief Key id of a base64 public key. */
bool fly_crypto_key_id_of(const char *pk_b64, char out[FLY_PQC_KEY_ID_LEN + 1]);

/**
 * @brief Sign a post message with ML-DSA-65.
 * @param[out] sig_out "<key id>:<base64 signature>". Ownership transferred via cwist_alloc.
 */
bool fly_crypto_sign(const uint8_t *msg, size_t msg_len, char **sig_out);

/**
 * @brief Verify a post signature against the registered key it names; a
 * signature without a key id is checked against the signing key.
 */
bool fly_crypto_verify(const uint8_t *msg, size_t msg_len, const char *sig);

/** @brief Sign / verify a backup manifest (separate signing context). */
bool fly_crypto_sign_manifest(const uint8_t *msg, size_t msg_len, char **sig_out);
bool fly_crypto_verify_manifest(const char *pk_b64, const uint8_t *msg, size_t msg_len, const char *sig);

/**
 * @brief Export the signing public key in base64.
 * @param[out] pk_b64 Base64-encoded public key. Ownership transferred.
 */
bool fly_crypto_pubkey_export(char **pk_b64);

/** @brief Wipe the private key. */
void fly_crypto_cleanup(void);

#ifdef __cplusplus
}
#endif

#endif

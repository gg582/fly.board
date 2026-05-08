#ifndef FLY_CRYPTO_H
#define FLY_CRYPTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize flyboard PQC signing subsystem.
 * Generates a global ML-DSA-65 signing key on first call.
 * @return true on success.
 */
bool fly_crypto_init(void);

/**
 * @brief Sign a message with ML-DSA-65 and return a base64-encoded signature.
 * @param[in]  msg      Message bytes (zero-copy read).
 * @param[in]  msg_len  Message length.
 * @param[out] sig_b64  Base64-encoded signature. Ownership transferred via cwist_alloc.
 * @return true on success.
 */
bool fly_crypto_sign(const uint8_t *msg, size_t msg_len, char **sig_b64);

/**
 * @brief Verify a base64-encoded ML-DSA-65 signature.
 * @param[in] msg      Message bytes (zero-copy read).
 * @param[in] msg_len  Message length.
 * @param[in] sig_b64  Base64-encoded signature.
 * @return true if valid.
 */
bool fly_crypto_verify(const uint8_t *msg, size_t msg_len, const char *sig_b64);

/**
 * @brief Export the global public key in base64.
 * @param[out] pk_b64 Base64-encoded public key. Ownership transferred.
 * @return true on success.
 */
bool fly_crypto_pubkey_export(char **pk_b64);

/**
 * @brief Clean up flyboard PQC subsystem.
 */
void fly_crypto_cleanup(void);

#ifdef __cplusplus
}
#endif

#endif

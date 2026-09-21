/**
 * @file tasfa_crypto_wasm.h
 * @brief Optional WASM-sandboxed backend for TASFA block crypto.
 *
 * Enabled at runtime by pointing TASFA_CRYPTO_WASM at the built module
 * (build-wasm/tasfa_crypto.wasm).  When unset or the module is missing,
 * tasfa_wasm_crypto_available() returns false and callers keep using the
 * native OpenSSL EVP path, so this is strictly opt-in.
 *
 * Each operation spawns `wasmtime run <module>` as a subprocess and speaks
 * the framed protocol from wasm/tasfa_crypto_protocol.h over stdin/stdout.
 * Process-per-request keeps the host trivially stateless; chunk crypto is not
 * hot enough for the spawn cost to matter at blog scale.
 */

#ifndef FLYBOARD_TASFA_CRYPTO_WASM_H
#define FLYBOARD_TASFA_CRYPTO_WASM_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

bool tasfa_wasm_crypto_available(void);

bool tasfa_wasm_encrypt_block(const unsigned char *key, const unsigned char *iv_seed,
                              int chunk_index, const char *session_id,
                              const unsigned char *plaintext, size_t plaintext_len,
                              unsigned char *ciphertext, size_t *ciphertext_len_out);

bool tasfa_wasm_decrypt_block(const unsigned char *key, const unsigned char *iv_seed,
                              int chunk_index, const char *upload_id,
                              const unsigned char *ciphertext, size_t ciphertext_len,
                              unsigned char *plaintext, size_t plaintext_len);

#ifdef __cplusplus
}
#endif

#endif /* FLYBOARD_TASFA_CRYPTO_WASM_H */

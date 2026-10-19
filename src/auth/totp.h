#ifndef FLYBOARD_AUTH_TOTP_H
#define FLYBOARD_AUTH_TOTP_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

/* RFC 6238 TOTP (HMAC-SHA1, 30-second steps, 6 digits) plus the base32
 * encoding authenticator apps expect. The site JWT secret is NOT involved -
 * each user gets their own random secret at enable time. */

#define TOTP_SECRET_BYTES 20          /* 160 bits, the RFC 4226 recommendation */
#define TOTP_B32_LEN 32               /* base32 length of 20 bytes, no padding */
#define TOTP_CODE_DIGITS 6
#define TOTP_STEP_SEC 30
/* Accepted clock drift in steps either side of the current one. */
#define TOTP_WINDOW_STEPS 1

/* Generate a fresh random secret, base32-encoded (A-Z2-7, unpadded) into
 * out_b32 (needs TOTP_B32_LEN + 1 bytes). Returns false on RNG failure. */
bool totp_secret_generate(char *out_b32);

/* 6-digit code (NUL-terminated) for the given secret at time t. Returns
 * false when the secret is not valid base32. */
bool totp_code_at(const char *secret_b32, time_t t, char out_code[TOTP_CODE_DIGITS + 1]);

/* Current-time wrapper around totp_code_at(). */
bool totp_code_now(const char *secret_b32, char out_code[TOTP_CODE_DIGITS + 1]);

/* Constant-time check of a user-typed code, accepting the current and
 * adjacent steps (TOTP_WINDOW_STEPS). Digits only. */
bool totp_verify(const char *secret_b32, const char *code);

/* Same as totp_verify() pinned to time t (unit tests, RFC 6238 vectors). */
bool totp_verify_at(const char *secret_b32, const char *code, time_t t);

/* Decode base32 (A-Z2-7, case-insensitive, padding optional). Returns the
 * number of bytes written or 0 on invalid input. */
size_t totp_base32_decode(const char *b32, unsigned char *out, size_t out_cap);

/* Encode bytes as unpadded uppercase base32. Returns bytes written. */
size_t totp_base32_encode(const unsigned char *in, size_t in_len, char *out, size_t out_cap);

#endif

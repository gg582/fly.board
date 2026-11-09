#ifndef FLY_POST_UNLOCK_H
#define FLY_POST_UNLOCK_H

#include <cwist/net/http/http.h>
#include <stdbool.h>
#include <stddef.h>

/* Unlock grants for password-locked posts.
 *
 * After a correct password the visitor gets the cookie fb_pu_<post id> with
 * "<expiry>.<hex HMAC-SHA256>" over (post id, access_ver, expiry), keyed by
 * a label-separated derivation of the JWT secret. Every node of a cluster
 * shares that secret (sessions already depend on it), so a grant issued by
 * one node is honoured by the others behind DNS round-robin. Changing or
 * removing the password bumps access_ver, which voids every older grant. */

#define POST_UNLOCK_COOKIE_PREFIX "fb_pu_"
#define POST_UNLOCK_LIFETIME 86400
#define POST_UNLOCK_TOKEN_MAX 96

/* Pure token helpers (secret may be NULL; then nothing verifies). */
bool post_unlock_token_make(const char *secret, int post_id, int access_ver, long expires,
                            char *out, size_t out_len);
bool post_unlock_token_check(const char *secret, int post_id, int access_ver, long now,
                             const char *token);

/* Does the request carry a valid grant for this post and version? */
bool post_unlock_request_ok(cwist_http_request *req, int post_id, int access_ver);
/* "fb_pu_<id>=<token>; Path=/; Max-Age=...; HttpOnly; SameSite=Lax[; Secure]" */
bool post_unlock_cookie_header(int post_id, int access_ver, bool secure, char *out, size_t out_len);
/* Any fb_pu_ cookie at all: such requests must bypass shared caches. */
bool post_unlock_has_any_cookie(cwist_http_request *req);

#endif

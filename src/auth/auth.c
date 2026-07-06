#define _POSIX_C_SOURCE 200809L
#include "auth.h"
#include <cwist/core/mem/alloc.h>
#include <cwist/core/log.h>
#include <cwist/core/sstring/sstring.h>
#include <cwist/security/jwt/jwt.h>
#include <openssl/rand.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/opensslv.h>
#if OPENSSL_VERSION_MAJOR >= 3
#include <openssl/core_names.h>
#include <openssl/params.h>
#endif
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

static const char *CLIENT_NONCE = "fly.board";

static char g_jwt_secret[256] = {0};

/* Precise 401 reason codes for auth failure instrumentation.
 * These strings are intentionally stable; do not localize or change. */
#define AUTH_FAIL_NO_COOKIE_HEADER      "AUTH_FAIL_NO_COOKIE_HEADER"
#define AUTH_FAIL_SESSION_COOKIE_MISSING "AUTH_FAIL_SESSION_COOKIE_MISSING"
#define AUTH_FAIL_EMPTY_SESSION_COOKIE  "AUTH_FAIL_EMPTY_SESSION_COOKIE"
#define AUTH_FAIL_TOKEN_TOO_LONG        "AUTH_FAIL_TOKEN_TOO_LONG"
#define AUTH_FAIL_JWT_VERIFY_NULL       "AUTH_FAIL_JWT_VERIFY_NULL"
#define AUTH_FAIL_MISSING_SUB           "AUTH_FAIL_MISSING_SUB"
#define AUTH_FAIL_BAD_SUB               "AUTH_FAIL_BAD_SUB"
#define AUTH_FAIL_MISSING_USERNAME      "AUTH_FAIL_MISSING_USERNAME"
#define AUTH_FAIL_MISSING_ROLE          "AUTH_FAIL_MISSING_ROLE"
#define AUTH_FAIL_ROLE_NOT_ADMIN        "AUTH_FAIL_ROLE_NOT_ADMIN"
#define AUTH_FAIL_USER_LOOKUP           "AUTH_FAIL_USER_LOOKUP"
#define AUTH_FAIL_UNKNOWN               "AUTH_FAIL_UNKNOWN"

bool auth_jwt_init(const char *secret_path) {
    const char *path = secret_path ? secret_path : "data/.jwt_secret";
    FILE *f = fopen(path, "r");
    if (f) {
        if (fgets(g_jwt_secret, sizeof(g_jwt_secret), f)) {
            size_t len = strlen(g_jwt_secret);
            while (len > 0 && (g_jwt_secret[len - 1] == '\n' || g_jwt_secret[len - 1] == '\r')) {
                g_jwt_secret[len - 1] = '\0';
                len--;
            }
        }
        fclose(f);
        if (g_jwt_secret[0]) return true;
    }

    unsigned char rand_bytes[32];
    if (RAND_bytes(rand_bytes, sizeof(rand_bytes)) != 1) {
        CWIST_LOG_ERROR("Failed to generate JWT secret");
        return false;
    }
    for (int i = 0; i < 32; i++) snprintf(g_jwt_secret + i * 2, 3, "%02x", rand_bytes[i]);
    g_jwt_secret[64] = '\0';

    f = fopen(path, "w");
    if (!f) {
        CWIST_LOG_ERROR("Failed to write JWT secret file: %s", path);
        return false;
    }
    fprintf(f, "%s\n", g_jwt_secret);
    fclose(f);
    chmod(path, 0600);
    CWIST_LOG_INFO("Generated new JWT secret: %s", path);
    return true;
}

const char *auth_jwt_secret(void) {
    return g_jwt_secret[0] ? g_jwt_secret : NULL;
}

static bool sha512_string_hex(const char *input, char *out, size_t out_len) {
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) return false;
    unsigned char hash[64];
    unsigned int hash_len = 0;
    if (!EVP_DigestInit_ex(ctx, EVP_sha512(), NULL) ||
        !EVP_DigestUpdate(ctx, input, strlen(input)) ||
        !EVP_DigestFinal_ex(ctx, hash, &hash_len)) {
        EVP_MD_CTX_free(ctx);
        return false;
    }
    EVP_MD_CTX_free(ctx);
    if (out_len < 129) return false;
    for (int i = 0; i < 64; i++) snprintf(out + i * 2, 3, "%02x", hash[i]);
    out[128] = '\0';
    return true;
}

/* Legacy PBKDF2-HMAC-SHA256 verifier for backward compatibility */
static bool legacy_verify_password(const char *prehash, const char *hash) {
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", hash);
    char *salt_hex = strtok(buf, ":");
    char *iter_str = strtok(NULL, ":");
    char *key_hex  = strtok(NULL, ":");
    if (!salt_hex || !iter_str || !key_hex) return false;
    int iterations = atoi(iter_str);
    if (iterations <= 0) return false;
    unsigned char salt[16];
    for (int i = 0; i < 16; i++) {
        unsigned int v;
        sscanf(salt_hex + i*2, "%2x", &v);
        salt[i] = (unsigned char)v;
    }
    unsigned char key[32];
    if (!PKCS5_PBKDF2_HMAC(prehash, (int)strlen(prehash), salt, sizeof(salt), iterations, EVP_sha256(), sizeof(key), key))
        return false;
    char derived[65];
    for (int i = 0; i < 32; i++) snprintf(derived + i*2, 3, "%02x", key[i]);
    derived[64] = '\0';
    return strcmp(derived, key_hex) == 0;
}

/* PBKDF2-HMAC-SHA256 hash for OpenSSL < 3 or fallback */
static bool pbkdf2_hash(const char *password, char *out_hash, size_t out_len) {
    unsigned char salt[16];
    if (RAND_bytes(salt, sizeof(salt)) != 1) return false;
    int iterations = 100000;
    unsigned char key[32];
    if (!PKCS5_PBKDF2_HMAC(password, (int)strlen(password), salt, sizeof(salt), iterations, EVP_sha256(), sizeof(key), key))
        return false;
    char salt_hex[33];
    for (int i = 0; i < 16; i++) snprintf(salt_hex + i*2, 3, "%02x", salt[i]);
    salt_hex[32] = '\0';
    char key_hex[65];
    for (int i = 0; i < 32; i++) snprintf(key_hex + i*2, 3, "%02x", key[i]);
    key_hex[64] = '\0';
    snprintf(out_hash, out_len, "%s:%d:%s", salt_hex, iterations, key_hex);
    return true;
}

#if OPENSSL_VERSION_MAJOR >= 3
static bool argon2id_hash(const char *password, char *out_hash, size_t out_len) {
    unsigned char salt[32];
    if (RAND_bytes(salt, sizeof(salt)) != 1) return false;

    EVP_KDF *kdf = EVP_KDF_fetch(NULL, "ARGON2ID", NULL);
    if (!kdf) return pbkdf2_hash(password, out_hash, out_len);
    EVP_KDF_CTX *kctx = EVP_KDF_CTX_new(kdf);
    EVP_KDF_free(kdf);
    if (!kctx) return pbkdf2_hash(password, out_hash, out_len);

    int lanes = 4;
    size_t memcost = 65536;
    int iter = 3;
    int version = 19;

    OSSL_PARAM params[] = {
        OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_PASSWORD, (void *)password, strlen(password)),
        OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SALT, salt, sizeof(salt)),
        OSSL_PARAM_construct_int(OSSL_KDF_PARAM_ITER, &iter),
        OSSL_PARAM_construct_int(OSSL_KDF_PARAM_ARGON2_LANES, &lanes),
        OSSL_PARAM_construct_size_t(OSSL_KDF_PARAM_ARGON2_MEMCOST, &memcost),
        OSSL_PARAM_construct_int(OSSL_KDF_PARAM_ARGON2_VERSION, &version),
        OSSL_PARAM_construct_end()
    };

    unsigned char key[64];
    if (EVP_KDF_derive(kctx, key, sizeof(key), params) != 1) {
        EVP_KDF_CTX_free(kctx);
        return pbkdf2_hash(password, out_hash, out_len);
    }
    EVP_KDF_CTX_free(kctx);

    char salt_hex[65];
    for (int i = 0; i < 32; i++) snprintf(salt_hex + i*2, 3, "%02x", salt[i]);
    salt_hex[64] = '\0';
    char key_hex[129];
    for (int i = 0; i < 64; i++) snprintf(key_hex + i*2, 3, "%02x", key[i]);
    key_hex[128] = '\0';

    snprintf(out_hash, out_len, "$argon2id$v=%d$m=%zu,t=%d,p=%d$%s$%s",
             version, memcost, iter, lanes, salt_hex, key_hex);
    return true;
}

static bool argon2id_verify(const char *password, const char *hash) {
    int version, iter, lanes;
    unsigned int memcost_u;
    size_t memcost;
    char salt_hex[65] = {0};
    char key_hex[129] = {0};
    if (sscanf(hash, "$argon2id$v=%u$m=%u,t=%u,p=%u$%64[^$]$%128s",
               &version, &memcost_u, &iter, &lanes, salt_hex, key_hex) != 6)
        return false;
    memcost = memcost_u;

    unsigned char salt[32];
    for (int i = 0; i < 32; i++) sscanf(salt_hex + i*2, "%2hhx", &salt[i]);

    EVP_KDF *kdf = EVP_KDF_fetch(NULL, "ARGON2ID", NULL);
    if (!kdf) return false;
    EVP_KDF_CTX *kctx = EVP_KDF_CTX_new(kdf);
    EVP_KDF_free(kdf);
    if (!kctx) return false;

    OSSL_PARAM params[] = {
        OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_PASSWORD, (void *)password, strlen(password)),
        OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SALT, salt, sizeof(salt)),
        OSSL_PARAM_construct_int(OSSL_KDF_PARAM_ITER, &iter),
        OSSL_PARAM_construct_int(OSSL_KDF_PARAM_ARGON2_LANES, &lanes),
        OSSL_PARAM_construct_size_t(OSSL_KDF_PARAM_ARGON2_MEMCOST, &memcost),
        OSSL_PARAM_construct_int(OSSL_KDF_PARAM_ARGON2_VERSION, &version),
        OSSL_PARAM_construct_end()
    };

    unsigned char key[64];
    if (EVP_KDF_derive(kctx, key, sizeof(key), params) != 1) {
        EVP_KDF_CTX_free(kctx);
        return false;
    }
    EVP_KDF_CTX_free(kctx);

    char derived_hex[129];
    for (int i = 0; i < 64; i++) snprintf(derived_hex + i*2, 3, "%02x", key[i]);
    derived_hex[128] = '\0';
    return strcmp(derived_hex, key_hex) == 0;
}
#else
static bool argon2id_hash(const char *password, char *out_hash, size_t out_len) {
    return pbkdf2_hash(password, out_hash, out_len);
}
static bool argon2id_verify(const char *password, const char *hash) {
    (void)password; (void)hash;
    return false;
}
#endif

bool auth_hash_password(const char *password, char *out_hash, size_t out_len) {
    char combined[512];
    snprintf(combined, sizeof(combined), "%s%s", CLIENT_NONCE, password);
    char prehash[129];
    if (!sha512_string_hex(combined, prehash, sizeof(prehash))) return false;
    return argon2id_hash(prehash, out_hash, out_len);
}

bool auth_verify_password(const char *password, const char *hash) {
    char combined[512];
    snprintf(combined, sizeof(combined), "%s%s", CLIENT_NONCE, password);
    char prehash[129];
    if (!sha512_string_hex(combined, prehash, sizeof(prehash))) return false;

    if (strncmp(hash, "$argon2id$", 10) == 0) {
        return argon2id_verify(prehash, hash);
    }
    /* Legacy PBKDF2-HMAC-SHA256 hash (colon-separated) */
    return legacy_verify_password(prehash, hash);
}

/* Append a string to an sstring with minimal JSON string escaping.
 * Escapes backslash, double quote, and ASCII control characters so that
 * usernames/roles containing JSON metacharacters do not corrupt the JWT
 * payload and cause cwist_jwt_sign() to fail. */
static void append_json_escaped(cwist_sstring *ss, const char *s) {
    if (!s) return;
    for (const char *p = s; *p; ++p) {
        if (*p == '"') {
            cwist_sstring_append(ss, "\\\"");
        } else if (*p == '\\') {
            cwist_sstring_append(ss, "\\\\");
        } else if ((unsigned char)*p < 0x20) {
            char buf[7];
            snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)*p);
            cwist_sstring_append(ss, buf);
        } else {
            char buf[2] = { *p, '\0' };
            cwist_sstring_append(ss, buf);
        }
    }
}

char *auth_jwt_issue(int user_id, const char *username, const char *role) {
    const char *secret = auth_jwt_secret();
    if (!secret) return NULL;

    cwist_sstring *payload = cwist_sstring_create();
    if (!payload) return NULL;

    char sub[32];
    snprintf(sub, sizeof(sub), "%d", user_id);

    /* Use a single timestamp for all time claims so iat, nbf, and exp are
     * perfectly aligned and not subject to clock drift between claim adds. */
    time_t now = time(NULL);
    time_t exp = now + AUTH_SESSION_LIFETIME;
    char iat_str[32], nbf_str[32], exp_str[32];
    snprintf(iat_str, sizeof(iat_str), "%ld", (long)now);
    snprintf(nbf_str, sizeof(nbf_str), "%ld", (long)now);
    snprintf(exp_str, sizeof(exp_str), "%ld", (long)exp);

    cwist_sstring_append(payload, "{\"sub\":\"");
    cwist_sstring_append(payload, sub);
    cwist_sstring_append(payload, "\",\"username\":\"");
    append_json_escaped(payload, username);
    cwist_sstring_append(payload, "\",\"role\":\"");
    append_json_escaped(payload, role);
    cwist_sstring_append(payload, "\",\"iat\":");
    cwist_sstring_append(payload, iat_str);
    cwist_sstring_append(payload, ",\"nbf\":");
    cwist_sstring_append(payload, nbf_str);
    cwist_sstring_append(payload, ",\"exp\":");
    cwist_sstring_append(payload, exp_str);
    cwist_sstring_append(payload, "}");

    char *token = cwist_jwt_sign(payload->data, secret, AUTH_SESSION_LIFETIME);
    cwist_sstring_destroy(payload);
    return token;
}

bool auth_jwt_verify_from_request(cwist_http_request *req, int *out_user_id, char *out_role, size_t role_len) {
    const char *secret = auth_jwt_secret();
    if (!secret) {
        CWIST_LOG_ERROR("auth verify failed: reason=%s method=%s path=%s detail=%s",
                        AUTH_FAIL_UNKNOWN,
                        cwist_http_method_to_string(req->method),
                        (req->path && req->path->data) ? req->path->data : "?",
                        "no_jwt_secret");
        return false;
    }

    const char *name_eq = SESSION_COOKIE_NAME "=";
    size_t name_eq_len = strlen(name_eq);
    int cookie_headers = 0;
    int token_attempts = 0;
    int valid_claims = 0;
    bool session_cookie_found = false;
    size_t session_token_len = 0;
    const char *reason = NULL;
    int parsed_uid = 0;
    const char *last_role = NULL;
    const char *last_iat = NULL;
    const char *last_nbf = NULL;
    const char *last_exp = NULL;

    for (cwist_http_header_node *h = req->headers; h; h = h->next) {
        if (!h->key || !h->key->data || !h->value || !h->value->data) continue;
        if (strcasecmp(h->key->data, "Cookie") != 0) continue;
        cookie_headers++;

        /* Scan every occurrence of the session cookie in this header value.
         * Browsers may send duplicate session cookies (e.g. a stale domain-scoped
         * cookie left over from a previous deployment plus the current one). */
        const char *scan = h->value->data;
        while ((scan = strstr(scan, name_eq)) != NULL) {
            session_cookie_found = true;
            const char *start = scan + name_eq_len;
            const char *end = strchr(start, ';');
            size_t len = end ? (size_t)(end - start) : strlen(start);
            session_token_len = len;
            token_attempts++;

            if (len == 0) {
                reason = AUTH_FAIL_EMPTY_SESSION_COOKIE;
                if (!end) break;
                scan = end + 1;
                continue;
            }
            if (len >= 1024) {
                reason = AUTH_FAIL_TOKEN_TOO_LONG;
                if (!end) break;
                scan = end + 1;
                continue;
            }

            cwist_sstring *token = cwist_sstring_create();
            if (!token) {
                if (!end) break;
                scan = end + 1;
                continue;
            }
            cwist_sstring_assign_len(token, start, len);

            cwist_jwt_claims *claims = cwist_jwt_verify(token->data, secret);
            if (!claims) {
                reason = AUTH_FAIL_JWT_VERIFY_NULL;
                cwist_sstring_destroy(token);
                if (!end) break;
                scan = end + 1;
                continue;
            }

            valid_claims++;
            const char *sub = cwist_jwt_claims_get(claims, "sub");
            const char *username = cwist_jwt_claims_get(claims, "username");
            const char *role = cwist_jwt_claims_get(claims, "role");
            last_iat = cwist_jwt_claims_get(claims, "iat");
            last_nbf = cwist_jwt_claims_get(claims, "nbf");
            last_exp = cwist_jwt_claims_get(claims, "exp");

            if (!sub) {
                reason = AUTH_FAIL_MISSING_SUB;
            } else {
                parsed_uid = atoi(sub);
                if (parsed_uid <= 0) {
                    reason = AUTH_FAIL_BAD_SUB;
                } else if (!username) {
                    reason = AUTH_FAIL_MISSING_USERNAME;
                } else if (!role) {
                    reason = AUTH_FAIL_MISSING_ROLE;
                } else {
                    *out_user_id = parsed_uid;
                    snprintf(out_role, role_len, "%s", role);
                    cwist_jwt_claims_destroy(claims);
                    cwist_sstring_destroy(token);
                    return true;
                }
            }

            last_role = role;
            cwist_jwt_claims_destroy(claims);
            cwist_sstring_destroy(token);
            if (!end) break;
            scan = end + 1;
        }
    }

    if (!reason) {
        if (cookie_headers == 0) {
            reason = AUTH_FAIL_NO_COOKIE_HEADER;
        } else if (!session_cookie_found) {
            reason = AUTH_FAIL_SESSION_COOKIE_MISSING;
        } else {
            reason = AUTH_FAIL_UNKNOWN;
        }
    }

    CWIST_LOG_ERROR("auth verify failed: reason=%s method=%s path=%s "
                    "cookie_headers=%d session_found=%d token_len=%zu "
                    "token_attempts=%d valid_claims=%d parsed_uid=%d "
                    "parsed_role=%s iat=%s nbf=%s exp=%s now=%ld",
                    reason,
                    cwist_http_method_to_string(req->method),
                    (req->path && req->path->data) ? req->path->data : "?",
                    cookie_headers,
                    session_cookie_found ? 1 : 0,
                    session_token_len,
                    token_attempts,
                    valid_claims,
                    parsed_uid,
                    last_role ? last_role : "(none)",
                    last_iat ? last_iat : "(none)",
                    last_nbf ? last_nbf : "(none)",
                    last_exp ? last_exp : "(none)",
                    (long)time(NULL));
    return false;
}

bool auth_is_logged_in(cwist_http_request *req, int *out_user_id, char *out_role, size_t role_len) {
    return auth_jwt_verify_from_request(req, out_user_id, out_role, role_len);
}

bool auth_require_login(cwist_http_request *req, cwist_http_response *res, int *out_user_id, char *out_role, size_t role_len) {
    if (!auth_is_logged_in(req, out_user_id, out_role, role_len)) {
        res->status_code = CWIST_HTTP_UNAUTHORIZED;
        cwist_sstring_assign(res->body, "Unauthorized. Please <a href='/login'>login</a>.");
        return false;
    }
    return true;
}

static char g_admin_id[64] = {0};
static char g_admin_pw[129] = {0};

bool auth_admin_load(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        f = fopen(path, "w");
        if (f) {
            fprintf(f, "admin\nfly.board\n");
            fclose(f);
        }
        strcpy(g_admin_id, "admin");
        char combined[512];
        snprintf(combined, sizeof(combined), "%s%s", CLIENT_NONCE, "fly.board");
        sha512_string_hex(combined, g_admin_pw, sizeof(g_admin_pw));
        return true;
    }
    if (fgets(g_admin_id, sizeof(g_admin_id), f)) {
        size_t len = strlen(g_admin_id);
        if (len > 0 && g_admin_id[len - 1] == '\n') g_admin_id[len - 1] = '\0';
    }
    char plain_pw[128] = {0};
    if (fgets(plain_pw, sizeof(plain_pw), f)) {
        size_t len = strlen(plain_pw);
        if (len > 0 && plain_pw[len - 1] == '\n') plain_pw[len - 1] = '\0';
    }
    fclose(f);
    char combined[512];
    snprintf(combined, sizeof(combined), "%s%s", CLIENT_NONCE, plain_pw);
    sha512_string_hex(combined, g_admin_pw, sizeof(g_admin_pw));
    return g_admin_id[0] && g_admin_pw[0];
}

bool auth_admin_check(const char *username, const char *password) {
    return strcmp(username, g_admin_id) == 0 && strcmp(password, g_admin_pw) == 0;
}

bool auth_require_admin(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0;
    char role[32] = {0};
    if (!auth_require_login(req, res, &uid, role, sizeof(role))) return false;
    if (strcmp(role, "admin") != 0) {
        CWIST_LOG_ERROR("auth admin failed: reason=%s method=%s path=%s parsed_uid=%d parsed_role=%s now=%ld",
                        AUTH_FAIL_ROLE_NOT_ADMIN,
                        cwist_http_method_to_string(req->method),
                        (req->path && req->path->data) ? req->path->data : "?",
                        uid,
                        role[0] ? role : "(none)",
                        (long)time(NULL));
        res->status_code = CWIST_HTTP_FORBIDDEN;
        cwist_sstring_assign(res->body, "Forbidden. Admin only.");
        return false;
    }
    return true;
}

/* Returns true if the request carries at least one Cookie header that
 * contains the session cookie name. This is used by handlers that allow
 * anonymous fallback (e.g. post creation) to distinguish "anonymous by
 * choice" from "logged-in session unexpectedly missing/invalid". */
bool auth_has_session_cookie(cwist_http_request *req) {
    const char *name_eq = SESSION_COOKIE_NAME "=";
    for (cwist_http_header_node *h = req->headers; h; h = h->next) {
        if (!h->key || !h->key->data || !h->value || !h->value->data) continue;
        if (strcasecmp(h->key->data, "Cookie") != 0) continue;
        if (strstr(h->value->data, name_eq) != NULL) return true;
    }
    return false;
}

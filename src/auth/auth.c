#define _POSIX_C_SOURCE 200809L
#include "auth.h"
#include <cwist/core/mem/alloc.h>
#include <cwist/core/log.h>
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

static const char *CLIENT_NONCE = "fly.board";

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

char *auth_jwt_issue(int user_id, const char *username, const char *role) {
    char payload[512];
    snprintf(payload, sizeof(payload), "{\"sub\":\"%d\",\"username\":\"%s\",\"role\":\"%s\"}", user_id, username, role);
    return cwist_jwt_sign(payload, JWT_SECRET, 86400 * 7); /* 7 days */
}

bool auth_jwt_verify_from_request(cwist_http_request *req, int *out_user_id, char *out_role, size_t role_len) {
    const char *target_cookie_val = NULL;
    cwist_http_header_node *curr = req->headers;
    while (curr) {
        if (curr->key && curr->key->data && strcasecmp(curr->key->data, "Cookie") == 0) {
            const char *cookie_val = curr->value ? curr->value->data : NULL;
            if (cookie_val && strstr(cookie_val, SESSION_COOKIE_NAME "=")) {
                target_cookie_val = cookie_val;
                break;
            }
        }
        curr = curr->next;
    }
    if (!target_cookie_val) return false;

    const char *start = strstr(target_cookie_val, SESSION_COOKIE_NAME "=");
    start += strlen(SESSION_COOKIE_NAME "=");
    const char *end = strchr(start, ';');
    size_t len = end ? (size_t)(end - start) : strlen(start);
    if (len == 0 || len > 1024) return false;

    char token[1024];
    memcpy(token, start, len);
    token[len] = '\0';

    cwist_jwt_claims *claims = cwist_jwt_verify(token, JWT_SECRET);
    if (!claims) return false;

    const char *sub = cwist_jwt_claims_get(claims, "sub");
    if (!sub || sub[0] == '\0') {
        cwist_jwt_claims_destroy(claims);
        return false;
    }
    const char *role = cwist_jwt_claims_get(claims, "role");
    if (!role || role[0] == '\0') {
        cwist_jwt_claims_destroy(claims);
        return false;
    }
    if (sub) *out_user_id = atoi(sub);
    if (role && out_role) snprintf(out_role, role_len, "%s", role);
    cwist_jwt_claims_destroy(claims);
    return true;
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
        res->status_code = CWIST_HTTP_FORBIDDEN;
        cwist_sstring_assign(res->body, "Forbidden. Admin only.");
        return false;
    }
    return true;
}

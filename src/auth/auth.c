#define _POSIX_C_SOURCE 200809L
#include "auth.h"
#include <cwist/core/sstring/sstring.h>
#include <cwist/security/jwt/jwt.h>
#include <cwist/core/log.h>
#include <cwist/core/mem/alloc.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/hmac.h>
#if !defined(OPENSSL_IS_BORINGSSL) && defined(OPENSSL_VERSION_MAJOR) && (OPENSSL_VERSION_MAJOR >= 3)
#include <openssl/kdf.h>
#include <openssl/core_names.h>
#include <openssl/params.h>
#endif
#include <openssl/crypto.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/stat.h>

#define CLIENT_NONCE "fly.board"
#define PBKDF2_ITERATIONS 100000

static char g_jwt_secret[65] = {0};
static char g_admin_id[64] = {0};
static char g_admin_pw[129] = {0};
static char g_admin_plain_pw[128] = {0};

/* --------------------------------------------------------------------------
 * Constant-Time String Comparison
 * -------------------------------------------------------------------------- */
static bool auth_constant_time_streq(const char *a, const char *b) {
    if (!a || !b) return false;
    size_t len_a = strlen(a);
    size_t len_b = strlen(b);
    if (len_a != len_b) return false;
    return CRYPTO_memcmp(a, b, len_a) == 0;
}

/* --------------------------------------------------------------------------
 * SHA-512 Helper
 * -------------------------------------------------------------------------- */
static bool sha512_hex(const char *input, char *out, size_t out_len) {
    if (!input || !out || out_len < 129) return false;
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
    for (int i = 0; i < 64; i++) snprintf(out + i * 2, 3, "%02x", hash[i]);
    out[128] = '\0';
    return true;
}

/* --------------------------------------------------------------------------
 * Password Hashing (Argon2id / PBKDF2 Compatibility)
 * -------------------------------------------------------------------------- */
static bool pbkdf2_raw(const char *password, const unsigned char *salt, size_t salt_len,
                       int iterations, unsigned char *out_key, size_t key_len) {
    return PKCS5_PBKDF2_HMAC(password, (int)strlen(password),
                             salt, (int)salt_len,
                             iterations, EVP_sha256(),
                             (int)key_len, out_key) == 1;
}

static bool pbkdf2_hash_create(const char *password, char *out_hash, size_t out_len) {
    unsigned char salt[16];
    if (RAND_bytes(salt, sizeof(salt)) != 1) return false;
    unsigned char key[32];
    if (!pbkdf2_raw(password, salt, sizeof(salt), PBKDF2_ITERATIONS, key, sizeof(key))) return false;

    char salt_hex[33], key_hex[65];
    for (int i = 0; i < 16; i++) snprintf(salt_hex + i * 2, 3, "%02x", salt[i]);
    for (int i = 0; i < 32; i++) snprintf(key_hex + i * 2, 3, "%02x", key[i]);
    snprintf(out_hash, out_len, "%s:%d:%s", salt_hex, PBKDF2_ITERATIONS, key_hex);
    return true;
}

static bool pbkdf2_hash_verify(const char *password, const char *hash) {
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", hash);
    char *colon1 = strchr(buf, ':');
    if (!colon1) return false;
    *colon1 = '\0';
    char *colon2 = strchr(colon1 + 1, ':');
    if (!colon2) return false;
    *colon2 = '\0';

    const char *salt_hex = buf;
    int iterations = atoi(colon1 + 1);
    const char *key_hex = colon2 + 1;
    if (strlen(salt_hex) != 32 || iterations <= 0 || strlen(key_hex) != 64) return false;

    unsigned char salt[16];
    for (int i = 0; i < 16; i++) {
        unsigned int byte;
        if (sscanf(salt_hex + i * 2, "%02x", &byte) != 1) return false;
        salt[i] = (unsigned char)byte;
    }
    unsigned char key[32];
    if (!pbkdf2_raw(password, salt, sizeof(salt), iterations, key, sizeof(key))) return false;

    char derived[65];
    for (int i = 0; i < 32; i++) snprintf(derived + i * 2, 3, "%02x", key[i]);
    derived[64] = '\0';
    return auth_constant_time_streq(derived, key_hex);
}

#if !defined(OPENSSL_IS_BORINGSSL) && defined(OPENSSL_VERSION_MAJOR) && (OPENSSL_VERSION_MAJOR >= 3)
static bool argon2id_hash_create(const char *password, char *out_hash, size_t out_len) {
    unsigned char salt[32];
    if (RAND_bytes(salt, sizeof(salt)) != 1) return false;
    EVP_KDF *kdf = EVP_KDF_fetch(NULL, "ARGON2ID", NULL);
    if (!kdf) return pbkdf2_hash_create(password, out_hash, out_len);
    EVP_KDF_CTX *kctx = EVP_KDF_CTX_new(kdf);
    EVP_KDF_free(kdf);
    if (!kctx) return pbkdf2_hash_create(password, out_hash, out_len);

    int lanes = 4, iter = 3, version = 19;
    size_t memcost = 65536;
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
        return pbkdf2_hash_create(password, out_hash, out_len);
    }
    EVP_KDF_CTX_free(kctx);

    char salt_hex[65], key_hex[129];
    for (int i = 0; i < 32; i++) snprintf(salt_hex + i * 2, 3, "%02x", salt[i]);
    for (int i = 0; i < 64; i++) snprintf(key_hex + i * 2, 3, "%02x", key[i]);
    snprintf(out_hash, out_len, "$argon2id$v=%d$m=%zu,t=%d,p=%d$%s$%s",
             version, memcost, iter, lanes, salt_hex, key_hex);
    return true;
}

static bool argon2id_hash_verify(const char *password, const char *hash) {
    int version, iter, lanes;
    unsigned int memcost_u;
    char salt_hex[65] = {0}, key_hex[129] = {0};
    if (sscanf(hash, "$argon2id$v=%u$m=%u,t=%u,p=%u$%64[^$]$%128s",
               &version, &memcost_u, &iter, &lanes, salt_hex, key_hex) != 6)
        return false;
    size_t memcost = memcost_u;
    unsigned char salt[32];
    for (int i = 0; i < 32; i++) sscanf(salt_hex + i * 2, "%2hhx", &salt[i]);

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
    for (int i = 0; i < 64; i++) snprintf(derived_hex + i * 2, 3, "%02x", key[i]);
    derived_hex[128] = '\0';
    return auth_constant_time_streq(derived_hex, key_hex);
}
#else
static bool argon2id_hash_create(const char *password, char *out_hash, size_t out_len) {
    return pbkdf2_hash_create(password, out_hash, out_len);
}
static bool argon2id_hash_verify(const char *password, const char *hash) {
    (void)password; (void)hash;
    return false;
}
#endif

bool auth_hash_password(const char *password, char *out_hash, size_t out_len) {
    char combined[512];
    snprintf(combined, sizeof(combined), "%s%s", CLIENT_NONCE, password);
    char prehash[129];
    if (!sha512_hex(combined, prehash, sizeof(prehash))) return false;
    return argon2id_hash_create(prehash, out_hash, out_len);
}

static bool verify_raw_hash_candidate(const char *candidate, const char *hash) {
    if (strncmp(hash, "$argon2id$", 10) == 0) {
        return argon2id_hash_verify(candidate, hash);
    }
    return pbkdf2_hash_verify(candidate, hash);
}

bool auth_verify_password(const char *password, const char *hash) {
    if (!password || !hash) return false;

    /* Candidate 1: 1st tier client prehash (SHA-512("fly.board" + password)) */
    char combined[512];
    snprintf(combined, sizeof(combined), "%s%s", CLIENT_NONCE, password);
    char prehash[129];
    if (sha512_hex(combined, prehash, sizeof(prehash))) {
        if (verify_raw_hash_candidate(prehash, hash)) return true;

        /* Candidate 2: 2nd tier hash (SHA-512("fly.board" + prehash)) */
        char combined2[512];
        snprintf(combined2, sizeof(combined2), "%s%s", CLIENT_NONCE, prehash);
        char pre_prehash[129];
        if (sha512_hex(combined2, pre_prehash, sizeof(pre_prehash))) {
            if (verify_raw_hash_candidate(pre_prehash, hash)) return true;
        }
    }

    /* Candidate 3: Direct raw password string (when client sent prehash or plain) */
    if (verify_raw_hash_candidate(password, hash)) return true;

    return false;
}

/* --------------------------------------------------------------------------
 * Admin Authentication
 * -------------------------------------------------------------------------- */
bool auth_admin_load(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        f = fopen(path, "w");
        if (f) {
            fprintf(f, "admin\nfly.board\n");
            fclose(f);
        }
        strcpy(g_admin_id, "admin");
        strcpy(g_admin_plain_pw, "fly.board");
        char combined[512];
        snprintf(combined, sizeof(combined), "%s%s", CLIENT_NONCE, "fly.board");
        sha512_hex(combined, g_admin_pw, sizeof(g_admin_pw));
        return true;
    }
    if (fgets(g_admin_id, sizeof(g_admin_id), f)) {
        size_t len = strlen(g_admin_id);
        while (len > 0 && (g_admin_id[len - 1] == '\r' || g_admin_id[len - 1] == '\n')) {
            g_admin_id[--len] = '\0';
        }
    }
    char plain_pw[128] = {0};
    if (fgets(plain_pw, sizeof(plain_pw), f)) {
        size_t len = strlen(plain_pw);
        while (len > 0 && (plain_pw[len - 1] == '\r' || plain_pw[len - 1] == '\n')) {
            plain_pw[--len] = '\0';
        }
    }
    fclose(f);
    snprintf(g_admin_plain_pw, sizeof(g_admin_plain_pw), "%s", plain_pw);
    char combined[512];
    snprintf(combined, sizeof(combined), "%s%s", CLIENT_NONCE, plain_pw);
    sha512_hex(combined, g_admin_pw, sizeof(g_admin_pw));
    return g_admin_id[0] && (g_admin_pw[0] || g_admin_plain_pw[0]);
}

bool auth_admin_check(const char *username, const char *password) {
    if (!username || !password || !g_admin_id[0]) return false;
    if (!auth_constant_time_streq(username, g_admin_id)) return false;

    /* 1. Client SHA-512 prehash matches stored admin hash */
    if (g_admin_pw[0] && auth_constant_time_streq(password, g_admin_pw)) return true;

    /* 2. Plain password matches stored plain admin password */
    if (g_admin_plain_pw[0] && auth_constant_time_streq(password, g_admin_plain_pw)) return true;

    /* 3. Plain password hashed matches stored admin hash */
    char combined[512];
    snprintf(combined, sizeof(combined), "%s%s", CLIENT_NONCE, password);
    char hashed[129];
    if (sha512_hex(combined, hashed, sizeof(hashed))) {
        if (g_admin_pw[0] && auth_constant_time_streq(hashed, g_admin_pw)) return true;
    }
    return false;
}

/* --------------------------------------------------------------------------
 * JWT Lifecycle & Header/Cookie Verification
 * -------------------------------------------------------------------------- */
bool auth_jwt_init(const char *secret_path) {
    const char *path = secret_path ? secret_path : "data/.jwt_secret";
    FILE *f = fopen(path, "r");
    if (f) {
        if (fgets(g_jwt_secret, sizeof(g_jwt_secret), f)) {
            size_t len = strlen(g_jwt_secret);
            while (len > 0 && (g_jwt_secret[len - 1] == '\n' || g_jwt_secret[len - 1] == '\r')) {
                g_jwt_secret[--len] = '\0';
            }
        }
        fclose(f);
        if (g_jwt_secret[0]) return true;
    }

    unsigned char rand_bytes[32];
    if (RAND_bytes(rand_bytes, sizeof(rand_bytes)) != 1) return false;
    for (int i = 0; i < 32; i++) snprintf(g_jwt_secret + i * 2, 3, "%02x", rand_bytes[i]);
    g_jwt_secret[64] = '\0';

    f = fopen(path, "w");
    if (!f) return false;
    fprintf(f, "%s\n", g_jwt_secret);
    fclose(f);
    chmod(path, 0600);
    return true;
}

const char *auth_jwt_secret(void) {
    return g_jwt_secret[0] ? g_jwt_secret : NULL;
}

static void append_json_escaped(cwist_sstring *ss, const char *s) {
    if (!s) return;
    for (const char *p = s; *p; ++p) {
        if (*p == '"') cwist_sstring_append(ss, "\\\"");
        else if (*p == '\\') cwist_sstring_append(ss, "\\\\");
        else if ((unsigned char)*p < 0x20) {
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

    time_t now = time(NULL);
    time_t issued_at = now - AUTH_TIME_LEEWAY_SECONDS;
    time_t exp = now + AUTH_SESSION_LIFETIME + AUTH_TIME_LEEWAY_SECONDS;

    char sub[32], iat_str[32], nbf_str[32], exp_str[32];
    snprintf(sub, sizeof(sub), "%d", user_id);
    snprintf(iat_str, sizeof(iat_str), "%ld", (long)issued_at);
    snprintf(nbf_str, sizeof(nbf_str), "%ld", (long)issued_at);
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

    char *token = cwist_jwt_sign(payload->data, secret, 0);
    cwist_sstring_destroy(payload);
    return token;
}

static bool auth_verify_token(const char *token, const char *secret,
                              int *out_user_id, char *out_role, size_t role_len) {
    if (!token || !token[0] || strlen(token) >= 1024) return false;

    cwist_jwt_claims *claims = cwist_jwt_verify(token, secret);
    if (!claims) return false;

    const char *sub = cwist_jwt_claims_get(claims, "sub");
    const char *role = cwist_jwt_claims_get(claims, "role");
    bool ok = false;
    if (sub && role) {
        int uid = atoi(sub);
        if (uid > 0) {
            *out_user_id = uid;
            snprintf(out_role, role_len, "%s", role);
            ok = true;
        }
    }
    cwist_jwt_claims_destroy(claims);
    return ok;
}

typedef struct {
    const char *name;
    size_t name_len;
    const char *value;
    size_t value_len;
} auth_cookie_iter_t;

static bool auth_cookie_iter_next(const char **cursor, auth_cookie_iter_t *out) {
    if (!cursor || !out) return false;
    const char *p = *cursor;
    while (*p == ' ' || *p == '\t') p++;
    if (!*p) return false;

    out->name = p;
    while (*p && *p != '=' && *p != ';') p++;
    out->name_len = (size_t)(p - out->name);
    out->value = NULL;
    out->value_len = 0;

    if (*p == '=') {
        p++;
        out->value = p;
        while (*p && *p != ';') p++;
        out->value_len = (size_t)(p - out->value);
        while (out->value_len > 0 &&
               (out->value[out->value_len - 1] == ' ' ||
                out->value[out->value_len - 1] == '\t')) {
            out->value_len--;
        }
        if (out->value_len >= 2 && out->value[0] == '"' && out->value[out->value_len - 1] == '"') {
            out->value++;
            out->value_len -= 2;
        }
    }
    if (*p == ';') p++;
    *cursor = p;
    return true;
}

bool auth_jwt_verify_from_request(cwist_http_request *req, int *out_user_id, char *out_role, size_t role_len) {
    if (!req || !out_user_id || !out_role || role_len == 0) return false;

    const char *secret = auth_jwt_secret();
    if (!secret) return false;

    /* 1. Cookie-based verification (HttpOnly session cookie or JS access cookie) */
    const char *session_name = SESSION_COOKIE_NAME;
    size_t session_name_len = strlen(session_name);

    for (cwist_http_header_node *h = req->headers; h; h = h->next) {
        if (!h->key || !h->key->data || !h->value || !h->value->data) continue;
        if (strcasecmp(h->key->data, "Cookie") != 0) continue;

        auth_cookie_iter_t cookie;
        const char *cursor = h->value->data;
        while (auth_cookie_iter_next(&cursor, &cookie)) {
            bool is_session = (cookie.name_len == session_name_len && strncmp(cookie.name, session_name, session_name_len) == 0) ||
                              (cookie.name_len == 10 && strncmp(cookie.name, "jwt_access", 10) == 0);
            if (!is_session || cookie.value_len == 0) continue;

            cwist_sstring *token = cwist_sstring_create();
            if (!token) continue;
            cwist_sstring_assign_len(token, cookie.value, cookie.value_len);
            bool ok = auth_verify_token(token->data, secret, out_user_id, out_role, role_len);
            cwist_sstring_destroy(token);
            if (ok) return true;
        }
    }

    /* 2. Authorization: Bearer Header (for API requests / explicit clients) */
    const char *auth_header = cwist_http_header_get(req->headers, "Authorization");
    if (auth_header && strncasecmp(auth_header, "Bearer ", 7) == 0) {
        const char *bearer_token = auth_header + 7;
        while (*bearer_token == ' ' || *bearer_token == '\t') bearer_token++;
        size_t b_len = strlen(bearer_token);
        while (b_len > 0 && (bearer_token[b_len - 1] == ' ' || bearer_token[b_len - 1] == '\t' ||
                             bearer_token[b_len - 1] == '\r' || bearer_token[b_len - 1] == '\n')) {
            b_len--;
        }
        if (b_len >= 2 && bearer_token[0] == '"' && bearer_token[b_len - 1] == '"') {
            bearer_token++;
            b_len -= 2;
        }
        cwist_sstring *clean_bearer = cwist_sstring_create();
        if (clean_bearer) {
            cwist_sstring_assign_len(clean_bearer, bearer_token, b_len);
            bool ok = auth_verify_token(clean_bearer->data, secret, out_user_id, out_role, role_len);
            cwist_sstring_destroy(clean_bearer);
            if (ok) return true;
        }
    }

    return false;
}

bool auth_is_logged_in(cwist_http_request *req, int *out_user_id, char *out_role, size_t role_len) {
    return auth_jwt_verify_from_request(req, out_user_id, out_role, role_len);
}

bool auth_has_session_cookie(cwist_http_request *req) {
    if (!req) return false;
    const char *session_name = SESSION_COOKIE_NAME;
    size_t session_name_len = strlen(session_name);
    for (cwist_http_header_node *h = req->headers; h; h = h->next) {
        if (!h->key || !h->key->data || !h->value || !h->value->data) continue;
        if (strcasecmp(h->key->data, "Cookie") != 0) continue;
        auth_cookie_iter_t cookie;
        const char *cursor = h->value->data;
        while (auth_cookie_iter_next(&cursor, &cookie)) {
            bool is_session = (cookie.name_len == session_name_len && strncmp(cookie.name, session_name, session_name_len) == 0) ||
                              (cookie.name_len == 10 && strncmp(cookie.name, "jwt_access", 10) == 0);
            if (is_session && cookie.value_len > 0) return true;
        }
    }
    return false;
}

bool auth_require_login(cwist_http_request *req, cwist_http_response *res, int *out_user_id, char *out_role, size_t role_len) {
    if (!res) return false;
    if (!auth_is_logged_in(req, out_user_id, out_role, role_len)) {
        res->status_code = (cwist_http_status_t)302;
        cwist_http_header_add(&res->headers, "Location", "/login");
        cwist_sstring_assign(res->body, "Redirecting to /login...");
        return false;
    }
    return true;
}

bool auth_require_admin(cwist_http_request *req, cwist_http_response *res) {
    if (!res) return false;
    int uid = 0;
    char role[32] = {0};
    if (!auth_require_login(req, res, &uid, role, sizeof(role))) return false;
    if (strcmp(role, "admin") != 0) {
        res->status_code = (cwist_http_status_t)403;
        cwist_sstring_assign(res->body, "Forbidden: Admin access required");
        return false;
    }
    return true;
}

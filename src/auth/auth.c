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
#include <dlfcn.h>

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

/* --------------------------------------------------------------------------
 * Dynamic OpenSSL 3 EVP_KDF Argon2id Support (BoringSSL & OpenSSL compatible)
 * -------------------------------------------------------------------------- */
typedef struct ossl_param_dyn_st {
    const char *key;
    unsigned int data_type;
    void *data;
    size_t data_size;
    size_t return_size;
} ossl_param_dyn_t;

#define OSSL_PARAM_DYN_INTEGER 1
#define OSSL_PARAM_DYN_UNSIGNED_INTEGER 2
#define OSSL_PARAM_DYN_OCTET_STRING 5

static void *g_crypto_dlhandle = NULL;
static void *(*g_fn_EVP_KDF_fetch)(void *, const char *, const char *) = NULL;
static void *(*g_fn_EVP_KDF_CTX_new)(void *) = NULL;
static void (*g_fn_EVP_KDF_free)(void *) = NULL;
static void (*g_fn_EVP_KDF_CTX_free)(void *) = NULL;
static int (*g_fn_EVP_KDF_derive)(void *, unsigned char *, size_t, const ossl_param_dyn_t *) = NULL;

static bool init_argon2_dl(void) {
    if (g_fn_EVP_KDF_derive) return true;
    if (!g_crypto_dlhandle) {
        g_crypto_dlhandle = dlopen("libcrypto.so.3", RTLD_LAZY);
        if (!g_crypto_dlhandle) g_crypto_dlhandle = dlopen("libcrypto.so", RTLD_LAZY);
    }
    if (!g_crypto_dlhandle) return false;

    g_fn_EVP_KDF_fetch = (void *(*)(void *, const char *, const char *))dlsym(g_crypto_dlhandle, "EVP_KDF_fetch");
    g_fn_EVP_KDF_CTX_new = (void *(*)(void *))dlsym(g_crypto_dlhandle, "EVP_KDF_CTX_new");
    g_fn_EVP_KDF_free = (void (*)(void *))dlsym(g_crypto_dlhandle, "EVP_KDF_free");
    g_fn_EVP_KDF_CTX_free = (void (*)(void *))dlsym(g_crypto_dlhandle, "EVP_KDF_CTX_free");
    g_fn_EVP_KDF_derive = (int (*)(void *, unsigned char *, size_t, const ossl_param_dyn_t *))dlsym(g_crypto_dlhandle, "EVP_KDF_derive");

    return g_fn_EVP_KDF_fetch && g_fn_EVP_KDF_CTX_new && g_fn_EVP_KDF_derive;
}

static bool argon2id_derive_raw(const char *password, const unsigned char *salt, size_t salt_len,
                                int iter, int lanes, size_t memcost, int version,
                                unsigned char *out_key, size_t key_len) {
    if (!init_argon2_dl()) return false;

    void *kdf = g_fn_EVP_KDF_fetch(NULL, "ARGON2ID", NULL);
    if (!kdf) return false;
    void *kctx = g_fn_EVP_KDF_CTX_new(kdf);
    g_fn_EVP_KDF_free(kdf);
    if (!kctx) return false;

    ossl_param_dyn_t params[] = {
        { "pass", OSSL_PARAM_DYN_OCTET_STRING, (void *)password, strlen(password), 0 },
        { "salt", OSSL_PARAM_DYN_OCTET_STRING, (void *)salt, salt_len, 0 },
        { "iter", OSSL_PARAM_DYN_INTEGER, &iter, sizeof(iter), 0 },
        { "lanes", OSSL_PARAM_DYN_INTEGER, &lanes, sizeof(lanes), 0 },
        { "memcost", OSSL_PARAM_DYN_UNSIGNED_INTEGER, &memcost, sizeof(memcost), 0 },
        { "version", OSSL_PARAM_DYN_INTEGER, &version, sizeof(version), 0 },
        { NULL, 0, NULL, 0, 0 }
    };

    int rc = g_fn_EVP_KDF_derive(kctx, out_key, key_len, params);
    g_fn_EVP_KDF_CTX_free(kctx);
    return rc == 1;
}

static bool argon2id_hash_create(const char *password, char *out_hash, size_t out_len) {
    unsigned char salt[32];
    if (RAND_bytes(salt, sizeof(salt)) != 1) return pbkdf2_hash_create(password, out_hash, out_len);

    int lanes = 4, iter = 3, version = 19;
    size_t memcost = 65536;
    unsigned char key[64];

    if (!argon2id_derive_raw(password, salt, sizeof(salt), iter, lanes, memcost, version, key, sizeof(key))) {
        return pbkdf2_hash_create(password, out_hash, out_len);
    }

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
    for (int i = 0; i < 32; i++) {
        unsigned int byte;
        if (sscanf(salt_hex + i * 2, "%02x", &byte) != 1) return false;
        salt[i] = (unsigned char)byte;
    }

    unsigned char derived[64];
    if (!argon2id_derive_raw(password, salt, sizeof(salt), iter, lanes, memcost, version, derived, sizeof(derived))) {
        return false;
    }

    char derived_hex[129];
    for (int i = 0; i < 64; i++) snprintf(derived_hex + i * 2, 3, "%02x", derived[i]);
    derived_hex[128] = '\0';
    return auth_constant_time_streq(derived_hex, key_hex);
}

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
    while (out->name_len > 0 &&
           (out->name[out->name_len - 1] == ' ' ||
            out->name[out->name_len - 1] == '\t')) {
        out->name_len--;
    }
    out->value = NULL;
    out->value_len = 0;

    if (*p == '=') {
        p++;
        while (*p == ' ' || *p == '\t') p++;
        out->value = p;
        while (*p && *p != ';') p++;
        out->value_len = (size_t)(p - out->value);
        while (out->value_len > 0 &&
               (out->value[out->value_len - 1] == ' ' ||
                out->value[out->value_len - 1] == '\t' ||
                out->value[out->value_len - 1] == '\r' ||
                out->value[out->value_len - 1] == '\n')) {
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

static bool is_auth_cookie_name(const char *name, size_t name_len) {
    const char *session_name = SESSION_COOKIE_NAME;
    size_t session_name_len = strlen(session_name);
    return (name_len == session_name_len && strncmp(name, session_name, session_name_len) == 0) ||
           (name_len == 10 && strncmp(name, "jwt_access", 10) == 0);
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static size_t url_decode_inplace(char *str, size_t len) {
    size_t r = 0, w = 0;
    while (r < len) {
        if (str[r] == '%' && r + 2 < len) {
            int h1 = hex_val(str[r + 1]);
            int h2 = hex_val(str[r + 2]);
            if (h1 >= 0 && h2 >= 0) {
                str[w++] = (char)((h1 << 4) | h2);
                r += 3;
                continue;
            }
        } else if (str[r] == '+') {
            str[w++] = ' ';
            r++;
            continue;
        }
        str[w++] = str[r++];
    }
    str[w] = '\0';
    return w;
}

static bool auth_verify_token_len(const char *token, size_t token_len, const char *secret,
                                  int *out_user_id, char *out_role, size_t role_len) {
    if (!token || token_len == 0 || token_len >= 4096 || !secret) return false;
    char token_buf[4096];
    memcpy(token_buf, token, token_len);
    token_buf[token_len] = '\0';

    /* Strip potential surrounding quotes */
    char *p = token_buf;
    size_t len = token_len;
    while (len > 0 && (*p == ' ' || *p == '\t' || *p == '"')) {
        p++;
        len--;
    }
    while (len > 0 && (p[len - 1] == ' ' || p[len - 1] == '\t' ||
                       p[len - 1] == '\r' || p[len - 1] == '\n' || p[len - 1] == '"')) {
        p[--len] = '\0';
    }

    if (len == 0) return false;

    /* If cookie was percent-encoded (e.g. %2E instead of .), decode in-place */
    if (strchr(p, '%')) {
        len = url_decode_inplace(p, len);
    }

    return auth_verify_token(p, secret, out_user_id, out_role, role_len);
}

bool auth_jwt_verify_from_request(cwist_http_request *req, int *out_user_id, char *out_role, size_t role_len) {
    if (!req) return false;

    int dummy_uid = 0;
    char dummy_role[32] = {0};
    if (!out_user_id) out_user_id = &dummy_uid;
    if (!out_role || role_len == 0) {
        out_role = dummy_role;
        role_len = sizeof(dummy_role);
    }

    const char *secret = auth_jwt_secret();
    if (!secret) return false;

    /* 1. Cookie-based verification (HttpOnly session cookie or JS access cookie) */
    for (cwist_http_header_node *h = req->headers; h; h = h->next) {
        if (!h->key || !h->key->data || !h->value || !h->value->data) continue;
        if (strcasecmp(h->key->data, "Cookie") != 0) continue;

        auth_cookie_iter_t cookie;
        const char *cursor = h->value->data;
        while (auth_cookie_iter_next(&cursor, &cookie)) {
            if (!is_auth_cookie_name(cookie.name, cookie.name_len) || cookie.value_len == 0) continue;
            if (auth_verify_token_len(cookie.value, cookie.value_len, secret, out_user_id, out_role, role_len)) {
                return true;
            }
        }
    }

    /* 2. Authorization: Bearer Header (for API requests / explicit clients) */
    const char *auth_header = cwist_http_header_get(req->headers, "Authorization");
    if (auth_header && strncasecmp(auth_header, "Bearer ", 7) == 0) {
        const char *bearer = auth_header + 7;
        while (*bearer == ' ' || *bearer == '\t') bearer++;
        size_t b_len = strlen(bearer);
        while (b_len > 0 && (bearer[b_len - 1] == ' ' || bearer[b_len - 1] == '\t' ||
                             bearer[b_len - 1] == '\r' || bearer[b_len - 1] == '\n')) {
            b_len--;
        }
        if (b_len >= 2 && bearer[0] == '"' && bearer[b_len - 1] == '"') {
            bearer++;
            b_len -= 2;
        }
        if (auth_verify_token_len(bearer, b_len, secret, out_user_id, out_role, role_len)) {
            return true;
        }
    }

    return false;
}

bool auth_is_logged_in(cwist_http_request *req, int *out_user_id, char *out_role, size_t role_len) {
    return auth_jwt_verify_from_request(req, out_user_id, out_role, role_len);
}

bool auth_has_session_cookie(cwist_http_request *req) {
    if (!req) return false;
    for (cwist_http_header_node *h = req->headers; h; h = h->next) {
        if (!h->key || !h->key->data || !h->value || !h->value->data) continue;
        if (strcasecmp(h->key->data, "Cookie") != 0) continue;
        auth_cookie_iter_t cookie;
        const char *cursor = h->value->data;
        while (auth_cookie_iter_next(&cursor, &cookie)) {
            if (is_auth_cookie_name(cookie.name, cookie.name_len) && cookie.value_len > 0) {
                return true;
            }
        }
    }
    return false;
}

bool auth_get_cookie(cwist_http_request *req, const char *name, char *out, size_t out_len) {
    if (!req || !name || !out || out_len == 0) return false;
    size_t target_len = strlen(name);
    for (cwist_http_header_node *h = req->headers; h; h = h->next) {
        if (!h->key || !h->key->data || !h->value || !h->value->data) continue;
        if (strcasecmp(h->key->data, "Cookie") != 0) continue;

        auth_cookie_iter_t cookie;
        const char *cursor = h->value->data;
        while (auth_cookie_iter_next(&cursor, &cookie)) {
            if (cookie.name_len == target_len && strncmp(cookie.name, name, target_len) == 0) {
                size_t copy_len = cookie.value_len < out_len - 1 ? cookie.value_len : out_len - 1;
                memcpy(out, cookie.value, copy_len);
                out[copy_len] = '\0';
                return true;
            }
        }
    }
    return false;
}

bool auth_require_login(cwist_http_request *req, cwist_http_response *res, int *out_user_id, char *out_role, size_t role_len) {
    if (!res) return false;
    if (!auth_is_logged_in(req, out_user_id, out_role, role_len)) {
        res->status_code = (cwist_http_status_t)302;
        char redirect_loc[512] = "/login";
        if (req && req->path && req->path->data && req->path->size > 1 &&
            strcmp(req->path->data, "/login") != 0 && strcmp(req->path->data, "/logout") != 0) {
            snprintf(redirect_loc, sizeof(redirect_loc), "/login?redirect=%s", req->path->data);
        }
        cwist_http_header_add(&res->headers, "Location", redirect_loc);
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

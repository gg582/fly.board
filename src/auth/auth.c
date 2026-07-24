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
#include <arpa/inet.h>

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

/* In-memory session hint cache.
 * When a valid JWT is verified we remember (IP + User-Agent) -> identity.
 * If a later request from the same IP+UA arrives without a Cookie header
 * (e.g. keep-alive/header reuse bug), we can fall back to the remembered
 * identity instead of treating the user as logged out. This is intentionally
 * a heuristic: it only covers missing-cookie cases, never invalid tokens,
 * and entries expire quickly.
 *
 * Implementation uses chained hash buckets instead of a flat array so that
 * lookups/updates are O(1) average and do not scan thousands of entries under
 * a single global lock. This prevents the cache from becoming a CPU and
 * contention hot-spot when the server has been running for a long time and
 * many distinct clients have been seen. */
#include <pthread.h>

#define AUTH_HINT_CACHE_BUCKETS 1024
#define AUTH_HINT_MAX_ENTRIES   8192
#define AUTH_HINT_TTL_SECONDS   3600

typedef struct auth_hint_entry {
    uint64_t key_hash;
    int user_id;
    char role[32];
    time_t last_seen;
    struct auth_hint_entry *next;
} auth_hint_entry_t;

static auth_hint_entry_t *g_auth_hint_buckets[AUTH_HINT_CACHE_BUCKETS];
static size_t g_auth_hint_count = 0;
static pthread_mutex_t g_auth_hint_mutex = PTHREAD_MUTEX_INITIALIZER;

static uint64_t auth_hint_hash(const char *ip, const char *ua) {
    if (!ip) ip = "";
    if (!ua) ua = "";
    /* FNV-1a 64-bit */
    uint64_t h = 0xcbf29ce484222325ULL;
    for (const char *p = ip; *p; ++p) {
        h ^= (uint64_t)(unsigned char)*p;
        h *= 0x100000001b3ULL;
    }
    h ^= 0x3a; /* separator */
    h *= 0x100000001b3ULL;
    for (const char *p = ua; *p; ++p) {
        h ^= (uint64_t)(unsigned char)*p;
        h *= 0x100000001b3ULL;
    }
    return h;
}

static inline size_t auth_hint_bucket_index(uint64_t h) {
    return (size_t)(h & (AUTH_HINT_CACHE_BUCKETS - 1));
}

/* Copy the client IP and User-Agent into caller-owned buffers so the hash
 * key remains valid regardless of how cwist_get_client_ip_from_fd() manages
 * its return value. This eliminates any chance of hashing a dangling pointer
 * if the sstring is destroyed before the key is used. */
static void auth_hint_get_client_info(cwist_http_request *req,
                                      char *ip_buf, size_t ip_len,
                                      const char **out_ua) {
    ip_buf[0] = '\0';
    *out_ua = "";
    if (req->client_fd >= 0) {
        cwist_sstring *ip_ss = cwist_get_client_ip_from_fd(req->client_fd);
        if (ip_ss && ip_ss->data) {
            snprintf(ip_buf, ip_len, "%s", ip_ss->data);
        }
        if (ip_ss) cwist_sstring_destroy(ip_ss);
    }
    *out_ua = cwist_http_header_get(req->headers, "User-Agent");
    if (!*out_ua) *out_ua = "";
}

/* Evict one arbitrary entry to keep the cache bounded. Called while the lock
 * is held. This is intentionally simple (drop the head of a non-empty bucket)
 * because the hint cache is a best-effort heuristic; exact LRU is not required. */
static void auth_hint_evict_one_locked(void) {
    for (size_t i = 0; i < AUTH_HINT_CACHE_BUCKETS; ++i) {
        auth_hint_entry_t *e = g_auth_hint_buckets[i];
        if (e) {
            g_auth_hint_buckets[i] = e->next;
            free(e);
            g_auth_hint_count--;
            return;
        }
    }
}

static void auth_hint_update(cwist_http_request *req, int user_id, const char *role) {
    char ip[INET6_ADDRSTRLEN];
    const char *ua;
    auth_hint_get_client_info(req, ip, sizeof(ip), &ua);
    uint64_t h = auth_hint_hash(ip, ua);

    pthread_mutex_lock(&g_auth_hint_mutex);
    time_t now = time(NULL);
    size_t idx = auth_hint_bucket_index(h);
    auth_hint_entry_t *e = g_auth_hint_buckets[idx];
    while (e) {
        if (e->key_hash == h) {
            e->user_id = user_id;
            snprintf(e->role, sizeof(e->role), "%s", role ? role : "");
            e->last_seen = now;
            pthread_mutex_unlock(&g_auth_hint_mutex);
            return;
        }
        e = e->next;
    }

    if (g_auth_hint_count >= AUTH_HINT_MAX_ENTRIES) {
        auth_hint_evict_one_locked();
    }

    e = (auth_hint_entry_t *)calloc(1, sizeof(*e));
    if (!e) {
        pthread_mutex_unlock(&g_auth_hint_mutex);
        return;
    }
    e->key_hash = h;
    e->user_id = user_id;
    snprintf(e->role, sizeof(e->role), "%s", role ? role : "");
    e->last_seen = now;
    e->next = g_auth_hint_buckets[idx];
    g_auth_hint_buckets[idx] = e;
    g_auth_hint_count++;
    pthread_mutex_unlock(&g_auth_hint_mutex);
}

static void auth_hint_remove(cwist_http_request *req) {
    char ip[INET6_ADDRSTRLEN];
    const char *ua;
    auth_hint_get_client_info(req, ip, sizeof(ip), &ua);
    uint64_t h = auth_hint_hash(ip, ua);

    pthread_mutex_lock(&g_auth_hint_mutex);
    size_t idx = auth_hint_bucket_index(h);
    auth_hint_entry_t **pp = &g_auth_hint_buckets[idx];
    while (*pp) {
        auth_hint_entry_t *e = *pp;
        if (e->key_hash == h) {
            *pp = e->next;
            free(e);
            g_auth_hint_count--;
            break;
        }
        pp = &e->next;
    }
    pthread_mutex_unlock(&g_auth_hint_mutex);
}

void auth_session_hint_update(cwist_http_request *req, int user_id, const char *role) {
    auth_hint_update(req, user_id, role);
}

void auth_session_hint_remove(cwist_http_request *req) {
    auth_hint_remove(req);
}

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

    /* Backdate the issue/not-before boundary and extend exp by a small grace
     * window so high-RTT clients and queued workers do not fail at the exact
     * session boundary. Cookie Max-Age remains AUTH_SESSION_LIFETIME. */
    time_t now = time(NULL);
    time_t issued_at = now - AUTH_TIME_LEEWAY_SECONDS;
    time_t exp = now + AUTH_SESSION_LIFETIME + AUTH_TIME_LEEWAY_SECONDS;
    char iat_str[32], nbf_str[32], exp_str[32];
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

/* Cookie name/value pair produced by auth_cookie_iter_next(). */
typedef struct {
    const char *name;
    size_t name_len;
    const char *value;
    size_t value_len;
} auth_cookie_iter_t;

/* Iterate over cookies in a Cookie header value.
 * Sets *cursor to the position after the consumed cookie and fills *out.
 * Returns false when no more cookies remain. */
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
    }

    if (*p == ';') p++;
    *cursor = p;
    return true;
}

bool auth_jwt_verify_from_request(cwist_http_request *req, int *out_user_id, char *out_role, size_t role_len) {
    if (!req || !out_user_id || !out_role || role_len == 0) return false;

    const char *secret = auth_jwt_secret();
    if (!secret) {
        CWIST_LOG_ERROR("auth verify failed: reason=%s method=%s path=%s detail=%s",
                        AUTH_FAIL_UNKNOWN,
                        cwist_http_method_to_string(req->method),
                        (req->path && req->path->data) ? req->path->data : "?",
                        "no_jwt_secret");
        return false;
    }

    /* Authorization: Bearer <token> fallback. Some clients (e.g. the post
     * editor after transient cookie loss) send the JWT explicitly so the
     * request can still be authenticated even when the Cookie header is
     * dropped by the framework/transport layer. */
    const char *auth_header = cwist_http_header_get(req->headers, "Authorization");
    if (auth_header && strncasecmp(auth_header, "Bearer ", 7) == 0) {
        const char *bearer_token = auth_header + 7;
        while (*bearer_token == ' ' || *bearer_token == '\t') bearer_token++;
        if (bearer_token[0]) {
            cwist_jwt_claims *claims = cwist_jwt_verify(bearer_token, secret);
            if (claims) {
                const char *sub = cwist_jwt_claims_get(claims, "sub");
                const char *username = cwist_jwt_claims_get(claims, "username");
                const char *role = cwist_jwt_claims_get(claims, "role");
                if (sub && username && role) {
                    int uid = atoi(sub);
                    if (uid > 0) {
                        *out_user_id = uid;
                        snprintf(out_role, role_len, "%s", role);
                        auth_hint_update(req, uid, role);
                        cwist_jwt_claims_destroy(claims);
                        return true;
                    }
                }
                cwist_jwt_claims_destroy(claims);
            }
        }
    }

    const char *cookie_name = SESSION_COOKIE_NAME;
    size_t cookie_name_len = strlen(cookie_name);
    int cookie_headers = 0;
    int token_attempts = 0;
    int valid_claims = 0;
    bool session_cookie_found = false;
    size_t session_token_len = 0;
    const char *reason = NULL;
    int parsed_uid = 0;

    for (cwist_http_header_node *h = req->headers; h; h = h->next) {
        if (!h->key || !h->key->data || !h->value || !h->value->data) continue;
        if (strcasecmp(h->key->data, "Cookie") != 0) continue;
        cookie_headers++;

        /* Scan every occurrence of the session cookie in this header value.
         * Browsers may send duplicate session cookies (e.g. a stale domain-scoped
         * cookie left over from a previous deployment plus the current one).
         * We parse the header explicitly instead of using strstr() so another
         * cookie whose name or value contains SESSION_COOKIE_NAME cannot be
         * mistaken for the session cookie. */
        auth_cookie_iter_t cookie;
        const char *cursor = h->value->data;
        while (auth_cookie_iter_next(&cursor, &cookie)) {
            if (cookie.name_len != cookie_name_len ||
                strncmp(cookie.name, cookie_name, cookie_name_len) != 0) {
                continue;
            }
            session_cookie_found = true;
            size_t len = cookie.value_len;
            if (!reason) session_token_len = len;
            token_attempts++;

            if (len == 0) {
                if (!reason) reason = AUTH_FAIL_EMPTY_SESSION_COOKIE;
                continue;
            }
            if (len >= 1024) {
                if (!reason) reason = AUTH_FAIL_TOKEN_TOO_LONG;
                continue;
            }

            cwist_sstring *token = cwist_sstring_create();
            if (!token) continue;
            cwist_sstring_assign_len(token, cookie.value, len);

            cwist_jwt_claims *claims = cwist_jwt_verify(token->data, secret);
            if (!claims) {
                if (!reason) reason = AUTH_FAIL_JWT_VERIFY_NULL;
                cwist_sstring_destroy(token);
                continue;
            }

            valid_claims++;
            const char *sub = cwist_jwt_claims_get(claims, "sub");
            const char *username = cwist_jwt_claims_get(claims, "username");
            const char *role = cwist_jwt_claims_get(claims, "role");

            if (!sub) {
                if (!reason) reason = AUTH_FAIL_MISSING_SUB;
            } else {
                int uid = atoi(sub);
                if (!reason) parsed_uid = uid;
                if (uid <= 0) {
                    if (!reason) reason = AUTH_FAIL_BAD_SUB;
                } else if (!username) {
                    if (!reason) reason = AUTH_FAIL_MISSING_USERNAME;
                } else if (!role) {
                    if (!reason) reason = AUTH_FAIL_MISSING_ROLE;
                } else {
                    *out_user_id = uid;
                    snprintf(out_role, role_len, "%s", role);
                    auth_hint_update(req, *out_user_id, out_role);
                    cwist_jwt_claims_destroy(claims);
                    cwist_sstring_destroy(token);
                    return true;
                }
            }

            cwist_jwt_claims_destroy(claims);
            cwist_sstring_destroy(token);
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
                    "token_attempts=%d valid_claims=%d parsed_uid=%d now=%ld",
                    reason,
                    cwist_http_method_to_string(req->method),
                    (req->path && req->path->data) ? req->path->data : "?",
                    cookie_headers,
                    session_cookie_found ? 1 : 0,
                    session_token_len,
                    token_attempts,
                    valid_claims,
                    parsed_uid,
                    (long)time(NULL));
    return false;
}

bool auth_is_logged_in(cwist_http_request *req, int *out_user_id, char *out_role, size_t role_len) {
    return auth_jwt_verify_from_request(req, out_user_id, out_role, role_len);
}

bool auth_require_login(cwist_http_request *req, cwist_http_response *res, int *out_user_id, char *out_role, size_t role_len) {
    if (!res) return false;
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
    if (!res) return false;
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
    if (!req) return false;
    const char *cookie_name = SESSION_COOKIE_NAME;
    size_t cookie_name_len = strlen(cookie_name);
    for (cwist_http_header_node *h = req->headers; h; h = h->next) {
        if (!h->key || !h->key->data || !h->value || !h->value->data) continue;
        if (strcasecmp(h->key->data, "Cookie") != 0) continue;

        auth_cookie_iter_t cookie;
        const char *cursor = h->value->data;
        while (auth_cookie_iter_next(&cursor, &cookie)) {
            if (cookie.name_len == cookie_name_len &&
                strncmp(cookie.name, cookie_name, cookie_name_len) == 0) {
                return true;
            }
        }
    }
    return false;
}

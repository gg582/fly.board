#define _POSIX_C_SOURCE 200809L
#include "spam_guard.h"
#include "auth/auth.h"
#include "config/write_policy.h"
#include <arpa/inet.h>
#include <errno.h>
#include <cwist/core/log.h>
#include <netinet/in.h>
#include <openssl/hmac.h>
#include <openssl/mem.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <time.h>

#define RATE_WINDOW_SECONDS 600
#define RATE_SLOTS 8192
#define RATE_PROBE 16

typedef struct {
    uint64_t key;      /* 0 = empty */
    uint32_t window;   /* window number (time / RATE_WINDOW_SECONDS) */
    uint32_t count;
} rate_slot_t;

/* Shared by every worker process (MAP_SHARED before fork). */
typedef struct {
    pthread_mutex_t lock;
    uint8_t key[32];
    rate_slot_t slots[RATE_SLOTS];
} rate_table_t;

static rate_table_t *g_rate = NULL;

bool spam_guard_init(void) {
    void *mem = mmap(NULL, sizeof(rate_table_t), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) {
        CWIST_LOG_ERROR("Spam guard: shared memory unavailable; rate limiting disabled");
        return false;
    }
    rate_table_t *t = (rate_table_t *)mem;
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    /* A worker killed while holding the lock must not wedge the others. */
    pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);
    bool ok = pthread_mutex_init(&t->lock, &attr) == 0 && RAND_bytes(t->key, sizeof(t->key)) == 1;
    pthread_mutexattr_destroy(&attr);
    if (!ok) {
        munmap(mem, sizeof(rate_table_t));
        CWIST_LOG_ERROR("Spam guard: init failed; rate limiting disabled");
        return false;
    }
    g_rate = t;
    return true;
}

/* ---- Honeypot token: "<unix time>.<hmac>" under the persistent JWT secret,
 * so forms rendered before a restart stay valid. ---- */

static void token_mac(long ts, char out[33]) {
    const char *secret = auth_jwt_secret();
    char msg[64];
    int n = snprintf(msg, sizeof(msg), "fly.board/form/%ld", ts);
    uint8_t mac[SHA256_DIGEST_LENGTH];
    unsigned int mac_len = 0;
    HMAC(EVP_sha256(), secret ? secret : "", secret ? strlen(secret) : 0,
         (const uint8_t *)msg, (size_t)n, mac, &mac_len);
    for (int i = 0; i < 16; i++) snprintf(out + 2 * i, 3, "%02x", mac[i]);
}

void spam_guard_append_fields(cwist_sstring *b) {
    if (!write_policy_get().spam_honeypot) return;
    long now = (long)time(NULL);
    char mac[33];
    token_mac(now, mac);
    char token[64];
    snprintf(token, sizeof(token), "%ld.%s", now, mac);
    /* Off-screen rather than display:none: some bots skip hidden inputs. */
    cwist_sstring_append(b, "<div aria-hidden='true' style='position:absolute;left:-10000px;width:1px;height:1px;overflow:hidden'>"
                            "<label>Leave this empty<input type='text' name='" SPAM_FIELD_TRAP "' value='' tabindex='-1' autocomplete='off'></label></div>"
                            "<input type='hidden' name='" SPAM_FIELD_TOKEN "' value='");
    cwist_sstring_append(b, token);
    cwist_sstring_append(b, "'>");
}

static bool token_ok(const char *token) {
    if (!token) return false;
    char *dot = NULL;
    long ts = strtol(token, &dot, 10);
    if (!dot || *dot != '.' || strlen(dot + 1) != 32) return false;
    char mac[33];
    token_mac(ts, mac);
    if (CRYPTO_memcmp(mac, dot + 1, 32) != 0) return false;
    long age = (long)time(NULL) - ts;
    return age >= SPAM_MIN_FILL_SECONDS;
}

/* ---- Rate limit ---- */

/* Writer identity as bytes: "u:<uid>" when logged in, else the peer address
 * (IPv6 cut to its /64, the usual per-subscriber allocation). */
static bool writer_id(cwist_http_request *req, int uid, char *out, size_t out_size, size_t *out_len) {
    if (uid > 0) {
        *out_len = (size_t)snprintf(out, out_size, "u:%d", uid);
        return true;
    }
    if (req->client_fd < 0) return false;
    struct sockaddr_storage addr;
    socklen_t len = sizeof(addr);
    if (getpeername(req->client_fd, (struct sockaddr *)&addr, &len) != 0) return false;
    if (addr.ss_family == AF_INET) {
        struct sockaddr_in *a = (struct sockaddr_in *)&addr;
        out[0] = '4';
        memcpy(out + 1, &a->sin_addr, 4);
        *out_len = 5;
        return true;
    }
    if (addr.ss_family == AF_INET6) {
        struct sockaddr_in6 *a = (struct sockaddr_in6 *)&addr;
        if (IN6_IS_ADDR_V4MAPPED(&a->sin6_addr)) {
            out[0] = '4';
            memcpy(out + 1, a->sin6_addr.s6_addr + 12, 4);
            *out_len = 5;
        } else {
            out[0] = '6';
            memcpy(out + 1, a->sin6_addr.s6_addr, 8);
            *out_len = 9;
        }
        return true;
    }
    return false;
}

static bool rate_allow(const char *id, size_t id_len, int limit) {
    uint8_t mac[SHA256_DIGEST_LENGTH];
    unsigned int mac_len = 0;
    HMAC(EVP_sha256(), g_rate->key, sizeof(g_rate->key), (const uint8_t *)id, id_len, mac, &mac_len);
    uint64_t key;
    memcpy(&key, mac, sizeof(key));
    if (key == 0) key = 1;
    uint32_t window = (uint32_t)(time(NULL) / RATE_WINDOW_SECONDS);

    if (pthread_mutex_lock(&g_rate->lock) == EOWNERDEAD) pthread_mutex_consistent(&g_rate->lock);
    bool allow = true;
    rate_slot_t *free_slot = NULL;
    size_t start = (size_t)(key % RATE_SLOTS);
    for (size_t i = 0; i < RATE_PROBE; i++) {
        rate_slot_t *s = &g_rate->slots[(start + i) % RATE_SLOTS];
        if (s->key == key) {
            if (s->window != window) {
                s->window = window;
                s->count = 0;
            }
            if ((int)s->count >= limit) allow = false;
            else s->count++;
            free_slot = NULL;
            goto done;
        }
        if (!free_slot && (s->key == 0 || s->window != window)) free_slot = s;
    }
    /* New writer. With no free slot nearby the table is saturated with
     * active writers in this window; fail open rather than block people. */
    if (free_slot) {
        free_slot->key = key;
        free_slot->window = window;
        free_slot->count = 1;
    }
done:
    pthread_mutex_unlock(&g_rate->lock);
    OPENSSL_cleanse(mac, sizeof(mac));
    return allow;
}

spam_verdict_t spam_guard_check(cwist_http_request *req, int uid, const char *role,
                                const char *trap, const char *token) {
    if (role && strcmp(role, "admin") == 0) return SPAM_OK;
    write_policy_t policy = write_policy_get();
    if (policy.spam_honeypot) {
        if ((trap && trap[0]) || !token_ok(token)) {
            CWIST_LOG_WARN("Spam guard: honeypot rejected a submission to %s",
                           req->path && req->path->data ? req->path->data : "?");
            return SPAM_REJECTED;
        }
    }
    if (policy.spam_rate_limit > 0 && g_rate) {
        char id[32];
        size_t id_len = 0;
        if (writer_id(req, uid, id, sizeof(id), &id_len) && !rate_allow(id, id_len, policy.spam_rate_limit)) {
            OPENSSL_cleanse(id, sizeof(id));
            CWIST_LOG_WARN("Spam guard: rate limit reached on %s",
                           req->path && req->path->data ? req->path->data : "?");
            return SPAM_RATE_LIMITED;
        }
        OPENSSL_cleanse(id, sizeof(id));
    }
    return SPAM_OK;
}

void spam_guard_reject(cwist_http_response *res, spam_verdict_t verdict) {
    if (verdict == SPAM_RATE_LIMITED) {
        res->status_code = (cwist_http_status_t)429;
        cwist_http_header_add(&res->headers, "Retry-After", "600");
        cwist_sstring_assign(res->body, "Too many submissions from you in a short time. Please wait a few minutes and try again.");
    } else {
        res->status_code = CWIST_HTTP_BAD_REQUEST;
        cwist_sstring_assign(res->body, "The form was submitted too quickly or was incomplete. Go back, wait a moment, and submit again.");
    }
    cwist_http_header_add(&res->headers, "Content-Type", "text/plain; charset=utf-8");
    cwist_http_header_add(&res->headers, "Cache-Control", "no-store");
}

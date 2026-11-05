#define _POSIX_C_SOURCE 200809L
#include "traffic_guard.h"
#include "auth/auth.h"
#include <arpa/inet.h>
#include <cwist/core/log.h>
#include <errno.h>
#include <netinet/in.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <time.h>

#define TRAFFIC_SLOTS 4096
#define TRAFFIC_PROBE 8
#define TOKEN_SCALE 1000ULL
#define MAX_REFILL_NS (3600ULL * 1000000000ULL)

typedef struct {
    uint64_t key;
    int64_t tokens;
    uint64_t updated_ns;
    uint64_t used_ns;
} traffic_slot_t;

typedef struct {
    pthread_mutex_t lock;
    uint8_t secret[32];
    uint64_t allowed;
    uint64_t limited;
    traffic_slot_t slots[TRAFFIC_SLOTS];
} traffic_table_t;

static traffic_table_t *g_table;
static unsigned g_rates[2];
static unsigned g_bursts[2];
static traffic_action_t g_action;
static unsigned g_tarpit_ms;

static unsigned env_uint(const char *name, unsigned fallback, unsigned maximum) {
    const char *value = getenv(name);
    if (!value || !value[0]) return fallback;
    char *end = NULL;
    errno = 0;
    unsigned long parsed = strtoul(value, &end, 10);
    if (errno || !end || *end || parsed > maximum) {
        CWIST_LOG_WARN("Traffic guard: ignoring invalid %s=%s", name, value);
        return fallback;
    }
    return (unsigned)parsed;
}

bool traffic_guard_init(void) {
    g_rates[TRAFFIC_READ] = env_uint("FLY_TRAFFIC_READ_RATE", 0, 1000000);
    g_rates[TRAFFIC_WRITE] = env_uint("FLY_TRAFFIC_WRITE_RATE", 0, 1000000);
    g_bursts[TRAFFIC_READ] = env_uint("FLY_TRAFFIC_READ_BURST", 60, 1000000);
    g_bursts[TRAFFIC_WRITE] = env_uint("FLY_TRAFFIC_WRITE_BURST", 20, 1000000);
    g_tarpit_ms = env_uint("FLY_TRAFFIC_TARPIT_MS", 100, 250);
    const char *action = getenv("FLY_TRAFFIC_ACTION");
    g_action = TRAFFIC_ACTION_429;
    if (action && strcmp(action, "cached") == 0) g_action = TRAFFIC_ACTION_CACHED;
    else if (action && strcmp(action, "tarpit") == 0) g_action = TRAFFIC_ACTION_TARPIT;
    else if (action && action[0] && strcmp(action, "429") != 0) {
        CWIST_LOG_WARN("Traffic guard: unknown action '%s'; using 429", action);
    }
    if (!g_rates[TRAFFIC_READ] && !g_rates[TRAFFIC_WRITE]) {
        CWIST_LOG_INFO("Traffic guard: disabled");
        return true;
    }

    void *mem = mmap(NULL, sizeof(*g_table), PROT_READ | PROT_WRITE,
                     MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) {
        CWIST_LOG_ERROR("Traffic guard: shared memory unavailable; disabled");
        return false;
    }
    g_table = mem;
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);
    bool ok = pthread_mutex_init(&g_table->lock, &attr) == 0 &&
              RAND_bytes(g_table->secret, sizeof(g_table->secret)) == 1;
    pthread_mutexattr_destroy(&attr);
    if (!ok) {
        munmap(mem, sizeof(*g_table));
        g_table = NULL;
        CWIST_LOG_ERROR("Traffic guard: initialization failed; disabled");
        return false;
    }
    CWIST_LOG_INFO("Traffic guard: read=%u/s burst=%u write=%u/s burst=%u action=%s slots=%u",
                   g_rates[TRAFFIC_READ], g_bursts[TRAFFIC_READ],
                   g_rates[TRAFFIC_WRITE], g_bursts[TRAFFIC_WRITE],
                   g_action == TRAFFIC_ACTION_CACHED ? "cached" :
                   g_action == TRAFFIC_ACTION_TARPIT ? "tarpit" : "429", TRAFFIC_SLOTS);
    return true;
}

traffic_action_t traffic_guard_action(void) {
    return g_action;
}

unsigned traffic_guard_tarpit_ms(void) {
    return g_tarpit_ms;
}

bool traffic_guard_check_key(const void *id, size_t id_len, traffic_class_t cls,
                             uint64_t now_ns, unsigned *retry_after) {
    if (!g_table || !id || !id_len || cls > TRAFFIC_WRITE || !g_rates[cls]) return true;
    uint8_t class_id = (uint8_t)cls;
    uint8_t mac[SHA256_DIGEST_LENGTH];
    uint8_t class_mac[SHA256_DIGEST_LENGTH];
    unsigned int mac_len = 0;
    HMAC(EVP_sha256(), g_table->secret, sizeof(g_table->secret), &class_id, 1, class_mac, &mac_len);
    HMAC(EVP_sha256(), g_table->secret, sizeof(g_table->secret), id, id_len, mac, &mac_len);
    for (size_t i = 0; i < sizeof(mac); i++) mac[i] ^= class_mac[i];
    uint64_t key;
    memcpy(&key, mac, sizeof(key));
    if (!key) key = 1;

    int lock_rc = pthread_mutex_lock(&g_table->lock);
    if (lock_rc == EOWNERDEAD) pthread_mutex_consistent(&g_table->lock);
    else if (lock_rc != 0) return true;

    size_t start = (size_t)(key % TRAFFIC_SLOTS);
    traffic_slot_t *slot = NULL;
    traffic_slot_t *oldest = &g_table->slots[start];
    for (size_t i = 0; i < TRAFFIC_PROBE; i++) {
        traffic_slot_t *candidate = &g_table->slots[(start + i) % TRAFFIC_SLOTS];
        if (candidate->key == key) {
            slot = candidate;
            break;
        }
        if (!candidate->key) {
            slot = candidate;
            break;
        }
        if (candidate->used_ns < oldest->used_ns) oldest = candidate;
    }
    if (!slot) slot = oldest;
    bool fresh = slot->key != key;
    if (fresh) {
        slot->key = key;
        slot->tokens = (int64_t)g_bursts[cls] * TOKEN_SCALE;
        slot->updated_ns = now_ns;
    } else {
        uint64_t elapsed = now_ns > slot->updated_ns ? now_ns - slot->updated_ns : 0;
        if (elapsed > MAX_REFILL_NS) elapsed = MAX_REFILL_NS;
        int64_t capacity = (int64_t)g_bursts[cls] * TOKEN_SCALE;
        int64_t refill = (int64_t)((elapsed * g_rates[cls]) / 1000000ULL);
        slot->tokens += refill;
        if (slot->tokens > capacity) slot->tokens = capacity;
        slot->updated_ns = now_ns;
    }
    bool allowed = slot->tokens >= (int64_t)TOKEN_SCALE;
    if (allowed) {
        slot->tokens -= TOKEN_SCALE;
        g_table->allowed++;
    } else {
        g_table->limited++;
        if (retry_after) {
            uint64_t missing = TOKEN_SCALE - (uint64_t)(slot->tokens > 0 ? slot->tokens : 0);
            uint64_t delay_ms = (missing + g_rates[cls] - 1) / g_rates[cls];
            unsigned seconds = (unsigned)((delay_ms + 999) / 1000);
            *retry_after = seconds ? seconds : 1;
        }
    }
    slot->used_ns = now_ns;
    pthread_mutex_unlock(&g_table->lock);
    return allowed;
}

static bool request_identity(cwist_http_request *req, unsigned char *out, size_t *out_len) {
    if (req->client_fd >= 0) {
        struct sockaddr_storage addr;
        socklen_t len = sizeof(addr);
        if (getpeername(req->client_fd, (struct sockaddr *)&addr, &len) == 0) {
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
        }
    }
    char session[2048];
    if (auth_get_cookie(req, SESSION_COOKIE_NAME, session, sizeof(session)) && session[0]) {
        size_t len = strlen(session);
        if (len > 512) len = 512;
        out[0] = 's';
        memcpy(out + 1, session, len);
        *out_len = len + 1;
        return true;
    }
    return false;
}

bool traffic_guard_check(cwist_http_request *req, unsigned *retry_after) {
    if (!req || !g_table) return true;
    traffic_class_t cls = (req->method == CWIST_HTTP_GET || req->method == CWIST_HTTP_HEAD) ?
                          TRAFFIC_READ : TRAFFIC_WRITE;
    if (!g_rates[cls]) return true;
    unsigned char id[514];
    size_t id_len = 0;
    if (!request_identity(req, id, &id_len)) return true;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return traffic_guard_check_key(id, id_len, cls,
                                   (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec,
                                   retry_after);
}

void traffic_guard_reject(cwist_http_response *res, unsigned retry_after) {
    char retry[16];
    snprintf(retry, sizeof(retry), "%u", retry_after ? retry_after : 1);
    res->status_code = (cwist_http_status_t)429;
    cwist_http_header_add(&res->headers, "Retry-After", retry);
    cwist_http_header_add(&res->headers, "Content-Type", "text/plain; charset=utf-8");
    cwist_http_header_add(&res->headers, "Cache-Control", "no-store");
    cwist_sstring_assign(res->body, "Traffic limit exceeded. Please retry later.");
}

void traffic_guard_stats(uint64_t *allowed, uint64_t *limited, size_t *used) {
    if (!g_table) return;
    int lock_rc = pthread_mutex_lock(&g_table->lock);
    if (lock_rc == EOWNERDEAD) pthread_mutex_consistent(&g_table->lock);
    else if (lock_rc != 0) return;
    if (allowed) *allowed = g_table->allowed;
    if (limited) *limited = g_table->limited;
    if (used) {
        *used = 0;
        for (size_t i = 0; i < TRAFFIC_SLOTS; i++) if (g_table->slots[i].key) (*used)++;
    }
    pthread_mutex_unlock(&g_table->lock);
}

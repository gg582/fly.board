#define _GNU_SOURCE
#include "fly_nats.h"
#include "../crypto/fly_crypto.h"
#include "../config/config.h"
#include "../db/db.h"
#if defined __has_include
#  if __has_include (<cwist/net/nats/cwist_nats.h>)
#    define HAVE_NATS 1
#    include <cwist/net/nats/cwist_nats.h>
#  endif
#endif
#include <cwist/core/mem/alloc.h>
#include <cwist/core/log.h>
#include <cjson/cJSON.h>
#include "../utils/cache.h"
#include <cwist/core/mem/alloc.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>

#ifdef HAVE_NATS
/* cnats is not fork-safe: a connection (and the library's own timer and
 * callback threads) created before cwist forks the workers exists in the
 * children only as copied memory, with none of its threads. Workers that
 * published on it blocked for the full 10 s flush timeout waiting for a PONG
 * no reader thread would ever see, shared the supervisor's socket, and
 * crashed on shutdown joining threads that did not exist.
 *
 * So every serving process opens its own connection, after the fork, from a
 * dedicated thread started on its first request (fly_nats_ensure()), and
 * nothing touches cnats before cwist_app_listen() has forked. Publishing only
 * queues: the thread connects (retrying with backoff), subscribes, and sends,
 * so a slow or absent NATS server never stalls a request. Each process
 * subscribes, so a post broadcast invalidates every worker's page cache. */

#define FLY_NATS_QUEUE_MAX 256

typedef struct fly_nats_msg {
    char *subject;
    char *data;
    struct fly_nats_msg *next;
} fly_nats_msg;

typedef struct {
    pid_t owner;                 /* process this state belongs to, 0 = none */
    bool started;                /* worker thread running in this process */
    bool stopping;
    pthread_t thread;
    pthread_mutex_t mtx;
    pthread_cond_t cond;
    fly_nats_msg *head, *tail;
    int queued;
} fly_nats_state;

static char g_nats_url[512];
static fly_nats_state g_ns;
/* Fast path for fly_nats_ensure(): the pid that already started its thread. */
static _Atomic pid_t g_ns_ready_pid = 0;
static pthread_mutex_t g_ns_init_mtx = PTHREAD_MUTEX_INITIALIZER;
/* Set by the first request a process serves. cwist forks every worker before
 * any process serves, so this is false in each child at fork time and the
 * NATS thread can only ever start after the fork. */
static _Atomic bool g_ns_serving = false;

static void on_post_msg(const char *subject, const char *data, size_t len, void *ctx) {
    (void)subject; (void)ctx;
    cJSON *obj = cJSON_ParseWithLength(data, len);
    const char *slug = NULL;
    bool has_sig = false;
    if (obj) {
        cJSON *slug_item = cJSON_GetObjectItemCaseSensitive(obj, "slug");
        cJSON *sig_item = cJSON_GetObjectItemCaseSensitive(obj, "sig_b64");
        if (cJSON_IsString(slug_item) && slug_item->valuestring) slug = slug_item->valuestring;
        has_sig = cJSON_IsString(sig_item) && sig_item->valuestring && sig_item->valuestring[0] != '\0';
    }
    FLY_LOG_DEBUG("[fly_nats] received post broadcast slug=%s sig=%s", slug ? slug : "(unknown)", has_sig ? "present" : "missing");
    if (slug && slug[0]) {
        page_cache_invalidate_post(slug);
    } else {
        page_cache_invalidate_all();
    }
    if (obj) cJSON_Delete(obj);
}

static char *build_signed_post_payload(const char *title, const char *slug, const char *summary) {
    char *sig_b64 = NULL;
    char *pubkey_b64 = NULL;
    char *signed_json = NULL;
    cJSON *canonical = cJSON_CreateObject();
    if (!canonical) return NULL;

    cJSON_AddStringToObject(canonical, "title", title ? title : "");
    cJSON_AddStringToObject(canonical, "slug", slug ? slug : "");
    cJSON_AddStringToObject(canonical, "summary", summary ? summary : "");

    char *canonical_json = cJSON_PrintUnformatted(canonical);
    cJSON_Delete(canonical);
    if (!canonical_json) return NULL;

    if (!fly_crypto_sign((const uint8_t *)canonical_json, strlen(canonical_json), &sig_b64)) {
        cwist_free(canonical_json);
        return NULL;
    }
    if (!fly_crypto_pubkey_export(&pubkey_b64)) {
        cwist_free(canonical_json);
        cwist_free(sig_b64);
        return NULL;
    }

    cJSON *envelope = cJSON_Parse(canonical_json);
    cwist_free(canonical_json);
    if (!envelope) {
        cwist_free(sig_b64);
        cwist_free(pubkey_b64);
        return NULL;
    }
    cJSON_AddStringToObject(envelope, "sig_alg", "ML-DSA-65");
    cJSON_AddStringToObject(envelope, "sig_b64", sig_b64);
    cJSON_AddStringToObject(envelope, "pubkey_b64", pubkey_b64);
    signed_json = cJSON_PrintUnformatted(envelope);
    cJSON_Delete(envelope);
    cwist_free(sig_b64);
    cwist_free(pubkey_b64);
    return signed_json;
}

static const char *json_str(cJSON *obj, const char *key) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return (cJSON_IsString(item) && item->valuestring) ? item->valuestring : "";
}

static void on_comment_msg(const char *subject, const char *data, size_t len, void *ctx) {
    (void)subject; (void)ctx;
    cJSON *obj = cJSON_ParseWithLength(data, len);
    if (!obj) return;
    const char *origin = json_str(obj, "origin");
    const char *actor_name = json_str(obj, "actor_name");
    const char *kind = json_str(obj, "kind");
    const char *post_slug = json_str(obj, "post_slug");
    const char *excerpt = json_str(obj, "excerpt");
    cJSON *sig_item = cJSON_GetObjectItemCaseSensitive(obj, "sig_b64");
    bool has_sig = cJSON_IsString(sig_item) && sig_item->valuestring && sig_item->valuestring[0] != '\0';
    cJSON *recipient_item = cJSON_GetObjectItemCaseSensitive(obj, "recipient_user_id");
    long recipient = cJSON_IsNumber(recipient_item) ? (long)recipient_item->valuedouble : 0;

    /* Events published by this node were already recorded locally when the
     * comment was created; only federated (remote-origin) events deliver. */
    if (g_config.root_url[0] && origin[0] && strcmp(origin, g_config.root_url) == 0) {
        cJSON_Delete(obj);
        return;
    }
    if (recipient > 0 && kind[0]) {
        if (db_notification_deliver_federated(recipient, actor_name, kind, post_slug, excerpt)) {
            FLY_LOG_DEBUG("[fly_nats] federated %s notification delivered to user=%ld sig=%s",
                          kind, recipient, has_sig ? "present" : "missing");
        }
    }
    cJSON_Delete(obj);
}

static char *build_signed_comment_payload(const char *origin, const char *actor_name, const char *kind, const char *post_slug, long recipient_user_id, const char *excerpt) {
    char *sig_b64 = NULL;
    char *pubkey_b64 = NULL;
    char *signed_json = NULL;
    cJSON *canonical = cJSON_CreateObject();
    if (!canonical) return NULL;

    cJSON_AddStringToObject(canonical, "origin", origin ? origin : "");
    cJSON_AddStringToObject(canonical, "actor_name", actor_name ? actor_name : "");
    cJSON_AddStringToObject(canonical, "kind", kind ? kind : "");
    cJSON_AddStringToObject(canonical, "post_slug", post_slug ? post_slug : "");
    cJSON_AddNumberToObject(canonical, "recipient_user_id", (double)recipient_user_id);
    cJSON_AddStringToObject(canonical, "excerpt", excerpt ? excerpt : "");

    char *canonical_json = cJSON_PrintUnformatted(canonical);
    cJSON_Delete(canonical);
    if (!canonical_json) return NULL;

    if (!fly_crypto_sign((const uint8_t *)canonical_json, strlen(canonical_json), &sig_b64)) {
        cwist_free(canonical_json);
        return NULL;
    }
    if (!fly_crypto_pubkey_export(&pubkey_b64)) {
        cwist_free(canonical_json);
        cwist_free(sig_b64);
        return NULL;
    }

    cJSON *envelope = cJSON_Parse(canonical_json);
    cwist_free(canonical_json);
    if (!envelope) {
        cwist_free(sig_b64);
        cwist_free(pubkey_b64);
        return NULL;
    }
    cJSON_AddStringToObject(envelope, "sig_alg", "ML-DSA-65");
    cJSON_AddStringToObject(envelope, "sig_b64", sig_b64);
    cJSON_AddStringToObject(envelope, "pubkey_b64", pubkey_b64);
    signed_json = cJSON_PrintUnformatted(envelope);
    cJSON_Delete(envelope);
    cwist_free(sig_b64);
    cwist_free(pubkey_b64);
    return signed_json;
}
#endif

#ifdef HAVE_NATS
static void ensure_process_state(void);

static void free_msg(fly_nats_msg *m) {
    if (!m) return;
    free(m->subject);
    free(m->data);
    free(m);
}

/* Reset the per-process state. State copied from the parent across fork
 * (including a mutex another thread may have held) is abandoned, not freed. */
static void state_reset_for_this_process(void) {
    memset(&g_ns, 0, sizeof(g_ns));
    pthread_mutex_init(&g_ns.mtx, NULL);
    pthread_cond_init(&g_ns.cond, NULL);
    g_ns.owner = getpid();
}

static cwist_nats_t *connect_and_subscribe(void) {
    cwist_nats_t *nc = NULL;
    cwist_error_t err = cwist_nats_connect(&nc, g_nats_url);
    if (!cwist_error_is_ok(&err) || !nc) return NULL;
    err = cwist_nats_subscribe(nc, "flyboard.posts", on_post_msg, NULL);
    if (cwist_error_is_ok(&err)) err = cwist_nats_subscribe(nc, "flyboard.comments", on_comment_msg, NULL);
    if (!cwist_error_is_ok(&err)) {
        cwist_nats_destroy(nc);
        return NULL;
    }
    return nc;
}

static void *nats_thread_main(void *arg) {
    (void)arg;
    cwist_nats_t *nc = NULL;
    int backoff = 1;
    pthread_mutex_lock(&g_ns.mtx);
    while (!g_ns.stopping) {
        if (!nc) {
            pthread_mutex_unlock(&g_ns.mtx);
            nc = connect_and_subscribe();
            pthread_mutex_lock(&g_ns.mtx);
            if (!nc) {
                FLY_LOG_ERROR("[fly_nats] connect to %s failed; retrying in %ds", g_nats_url, backoff);
                struct timespec until;
                clock_gettime(CLOCK_REALTIME, &until);
                until.tv_sec += backoff;
                while (!g_ns.stopping &&
                       pthread_cond_timedwait(&g_ns.cond, &g_ns.mtx, &until) != ETIMEDOUT) {
                }
                if (backoff < 30) backoff *= 2;
                continue;
            }
            backoff = 1;
            FLY_LOG_DEBUG("[fly_nats] pid %d connected to %s", (int)getpid(), g_nats_url);
        }
        if (!g_ns.head) {
            pthread_cond_wait(&g_ns.cond, &g_ns.mtx);
            continue;
        }
        fly_nats_msg *m = g_ns.head;
        g_ns.head = m->next;
        if (!g_ns.head) g_ns.tail = NULL;
        g_ns.queued--;
        pthread_mutex_unlock(&g_ns.mtx);
        cwist_error_t err = cwist_nats_publish_string(nc, m->subject, m->data);
        if (!cwist_error_is_ok(&err)) {
            FLY_LOG_ERROR("[fly_nats] publish to %s failed (status %d)", m->subject, (int)err.error.err_i16);
        }
        free_msg(m);
        /* No cwist_gc_scope_flush() here: it would retire the connection
         * object this thread allocated in cwist_nats_connect() and still
         * uses (use-after-free on the next publish). Nothing else this loop
         * allocates through cwist outlives an iteration. */
        pthread_mutex_lock(&g_ns.mtx);
    }
    pthread_mutex_unlock(&g_ns.mtx);
    if (nc) cwist_nats_destroy(nc);
    return NULL;
}

/* Copy the cwist-allocated payload into plain malloc memory owned by the
 * queue, then release the original on the calling thread. */
static bool enqueue(const char *subject, char *json) {
    if (!json) return false;
    fly_nats_msg *m = calloc(1, sizeof(*m));
    if (m) {
        m->subject = strdup(subject);
        m->data = strdup(json);
    }
    cwist_free(json);
    if (!m || !m->subject || !m->data) {
        free_msg(m);
        return false;
    }
    ensure_process_state();
    if (g_ns.owner != getpid()) {
        free_msg(m);
        return false;
    }
    pthread_mutex_lock(&g_ns.mtx);
    if (g_ns.queued >= FLY_NATS_QUEUE_MAX) {
        pthread_mutex_unlock(&g_ns.mtx);
        FLY_LOG_ERROR("[fly_nats] queue full, dropping %s message", subject);
        free_msg(m);
        return false;
    }
    if (g_ns.tail) g_ns.tail->next = m; else g_ns.head = m;
    g_ns.tail = m;
    g_ns.queued++;
    pthread_cond_signal(&g_ns.cond);
    pthread_mutex_unlock(&g_ns.mtx);
    return true;
}
#endif

bool fly_nats_init(const char *url) {
#ifdef HAVE_NATS
    snprintf(g_nats_url, sizeof(g_nats_url), "%s", url && url[0] ? url : "nats://localhost:4222");
    FLY_LOG_DEBUG("[fly_nats] configured %s; each serving process connects after the fork", g_nats_url);
    return true;
#else
    (void)url;
    return true;
#endif
}

#ifdef HAVE_NATS
/* Make g_ns this process's state; start the NATS thread once serving. */
static void ensure_process_state(void) {
    pid_t pid = getpid();
    if (atomic_load_explicit(&g_ns_ready_pid, memory_order_acquire) == pid) return;
    pthread_mutex_lock(&g_ns_init_mtx);
    if (g_ns.owner != pid) state_reset_for_this_process();
    if (atomic_load(&g_ns_serving) && !g_ns.started) {
        if (pthread_create(&g_ns.thread, NULL, nats_thread_main, NULL) == 0) {
            g_ns.started = true;
            atomic_store_explicit(&g_ns_ready_pid, pid, memory_order_release);
        } else {
            FLY_LOG_ERROR("[fly_nats] failed to start the NATS thread");
        }
    }
    pthread_mutex_unlock(&g_ns_init_mtx);
}
#endif

void fly_nats_ensure(void) {
#ifdef HAVE_NATS
    if (!g_nats_url[0]) return;
    if (!atomic_load_explicit(&g_ns_serving, memory_order_relaxed)) atomic_store(&g_ns_serving, true);
    ensure_process_state();
#endif
}

bool fly_nats_publish_post(const char *title, const char *slug, const char *summary) {
#ifdef HAVE_NATS
    if (!g_nats_url[0]) return false;
    return enqueue("flyboard.posts", build_signed_post_payload(title, slug, summary));
#else
    (void)title; (void)slug; (void)summary;
    return true;
#endif
}

bool fly_nats_publish_comment(const char *origin, const char *actor_name, const char *kind, const char *post_slug, long recipient_user_id, const char *excerpt) {
#ifdef HAVE_NATS
    if (!g_nats_url[0]) return false;
    return enqueue("flyboard.comments",
                   build_signed_comment_payload(origin, actor_name, kind, post_slug, recipient_user_id, excerpt));
#else
    (void)origin; (void)actor_name; (void)kind; (void)post_slug; (void)recipient_user_id; (void)excerpt;
    return true;
#endif
}

void fly_nats_close(void) {
#ifdef HAVE_NATS
    /* Only a thread this process started is stopped and joined; state
     * inherited from the parent is left alone. Queued messages not yet sent
     * are dropped. */
    if (g_ns.owner != getpid() || !g_ns.started) return;
    pthread_mutex_lock(&g_ns.mtx);
    g_ns.stopping = true;
    pthread_cond_broadcast(&g_ns.cond);
    pthread_mutex_unlock(&g_ns.mtx);
    pthread_join(g_ns.thread, NULL);
    g_ns.started = false;
    atomic_store_explicit(&g_ns_ready_pid, 0, memory_order_release);
    while (g_ns.head) {
        fly_nats_msg *m = g_ns.head;
        g_ns.head = m->next;
        free_msg(m);
    }
    g_ns.tail = NULL;
    g_ns.queued = 0;
#endif
}

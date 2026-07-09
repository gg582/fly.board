#define _POSIX_C_SOURCE 200809L
#include "reqshare.h"
#include <cwist/core/log.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Use malloc/free like the page cache so memory allocated by one thread can
 * be freed by another. cwist_alloc may be thread-local. */
#define rs_malloc malloc
#define rs_free free

#define RS_BUCKETS 1024

/* How long a finished coalescing entry remains available for late waiters.
 * Keep this short: the page cache already owns the durable copy. */
#define RS_RESULT_TTL_SEC 2

/* Safety timeout for a leader that never finishes. */
#define RS_LEADER_TTL_SEC 30

typedef enum {
    RS_IN_PROGRESS,
    RS_DONE
} rs_state_t;

typedef struct rs_entry {
    char *key;
    rs_state_t state;
    cwist_sstring *result;
    time_t expires_at;
    pthread_cond_t cond;
    struct rs_entry *next;
} rs_entry_t;

static rs_entry_t *g_buckets[RS_BUCKETS];
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;

static uint32_t hash_str(const char *s) {
    uint32_t h = 5381;
    while (*s) h = ((h << 5) + h) + (unsigned char)*s++;
    return h;
}

static cwist_sstring *copy_sstring(const cwist_sstring *src) {
    cwist_sstring *dst = cwist_sstring_create();
    if (!dst) return NULL;
    if (src && src->size > 0) {
        cwist_sstring_append_len(dst, src->data, src->size);
    }
    return dst;
}

static void free_entry(rs_entry_t *e) {
    if (!e) return;
    if (e->key) rs_free(e->key);
    if (e->result) cwist_sstring_destroy(e->result);
    pthread_cond_destroy(&e->cond);
    rs_free(e);
}

bool reqshare_init(void) {
    memset(g_buckets, 0, sizeof(g_buckets));
    CWIST_LOG_INFO("Request coalescer initialized");
    return true;
}

void reqshare_cleanup(void) {
    pthread_mutex_lock(&g_mutex);
    for (int i = 0; i < RS_BUCKETS; i++) {
        rs_entry_t *e = g_buckets[i];
        while (e) {
            rs_entry_t *next = e->next;
            free_entry(e);
            e = next;
        }
        g_buckets[i] = NULL;
    }
    pthread_mutex_unlock(&g_mutex);
}

cwist_sstring *reqshare_wait_or_start(const char *key, bool *leader) {
    if (!leader || !key) return NULL;
    *leader = false;

    pthread_mutex_lock(&g_mutex);
    uint32_t h = hash_str(key) % RS_BUCKETS;
    time_t now = time(NULL);

    rs_entry_t **prev = &g_buckets[h];
    while (*prev) {
        rs_entry_t *e = *prev;
        if (strcmp(e->key, key) == 0) {
            if (e->state == RS_DONE) {
                if (e->expires_at > now) {
                    cwist_sstring *copy = copy_sstring(e->result);
                    pthread_mutex_unlock(&g_mutex);
                    return copy;
                }
                /* Expired: drop it and let this request become the leader. */
                *prev = e->next;
                free_entry(e);
                break;
            }
            /* Another request is in flight.  Wait for it. */
            while (e->state == RS_IN_PROGRESS) {
                pthread_cond_wait(&e->cond, &g_mutex);
            }
            if (e->state == RS_DONE && e->expires_at > now) {
                cwist_sstring *copy = copy_sstring(e->result);
                pthread_mutex_unlock(&g_mutex);
                return copy;
            }
            /* The leader finished with an expired/empty result.  Start fresh. */
            *prev = e->next;
            free_entry(e);
            break;
        }
        prev = &e->next;
    }

    /* No usable entry: this request becomes the leader. */
    rs_entry_t *e = (rs_entry_t *)rs_malloc(sizeof(*e));
    if (!e) {
        pthread_mutex_unlock(&g_mutex);
        return NULL;
    }
    memset(e, 0, sizeof(*e));
    e->key = strdup(key);
    e->state = RS_IN_PROGRESS;
    e->expires_at = now + RS_LEADER_TTL_SEC;
    pthread_cond_init(&e->cond, NULL);
    e->next = g_buckets[h];
    g_buckets[h] = e;
    *leader = true;
    pthread_mutex_unlock(&g_mutex);
    return NULL;
}

void reqshare_finish(const char *key, cwist_sstring *html) {
    if (!key) return;
    pthread_mutex_lock(&g_mutex);
    uint32_t h = hash_str(key) % RS_BUCKETS;
    rs_entry_t **prev = &g_buckets[h];
    while (*prev) {
        rs_entry_t *e = *prev;
        if (strcmp(e->key, key) == 0) {
            if (e->state == RS_IN_PROGRESS) {
                e->result = copy_sstring(html);
                e->state = RS_DONE;
                e->expires_at = time(NULL) + RS_RESULT_TTL_SEC;
                pthread_cond_broadcast(&e->cond);
            }
            break;
        }
        prev = &e->next;
    }
    pthread_mutex_unlock(&g_mutex);
}

/* ---------------------------------------------------------------------------
 * Write-request deduplication
 *
 * A simple hash set of keys that are currently being written.  reqshare_write_lock_try
 * inserts the key if absent (returns true = lock acquired).  If the key is
 * already present another write is in flight; we return false immediately so
 * the duplicate request can be dropped without executing the DB mutation a
 * second time.  reqshare_write_lock_release removes the key.
 *
 * We reuse the same global mutex as the read coalescer to keep the
 * implementation self-contained.  Write operations are expected to be rare
 * compared to reads, so contention on the mutex is negligible.
 * ---------------------------------------------------------------------------*/

#define WL_BUCKETS 256

typedef struct wl_entry {
    char *key;
    struct wl_entry *next;
} wl_entry_t;

static wl_entry_t *g_wl_buckets[WL_BUCKETS];
/* g_mutex (defined above) is reused for the write-lock table. */

bool reqshare_write_lock_try(const char *key) {
    if (!key) return true; /* no key → don't deduplicate */
    pthread_mutex_lock(&g_mutex);
    uint32_t h = hash_str(key) % WL_BUCKETS;
    for (wl_entry_t *e = g_wl_buckets[h]; e; e = e->next) {
        if (strcmp(e->key, key) == 0) {
            /* Another write is already in flight for this key. */
            pthread_mutex_unlock(&g_mutex);
            return false;
        }
    }
    wl_entry_t *ne = (wl_entry_t *)rs_malloc(sizeof(*ne));
    if (!ne) {
        pthread_mutex_unlock(&g_mutex);
        return true; /* fail-open: allow the write rather than silently drop */
    }
    ne->key = strdup(key);
    if (!ne->key) {
        rs_free(ne);
        pthread_mutex_unlock(&g_mutex);
        return true;
    }
    ne->next = g_wl_buckets[h];
    g_wl_buckets[h] = ne;
    pthread_mutex_unlock(&g_mutex);
    return true;
}

void reqshare_write_lock_release(const char *key) {
    if (!key) return;
    pthread_mutex_lock(&g_mutex);
    uint32_t h = hash_str(key) % WL_BUCKETS;
    wl_entry_t **prev = &g_wl_buckets[h];
    while (*prev) {
        wl_entry_t *e = *prev;
        if (strcmp(e->key, key) == 0) {
            *prev = e->next;
            rs_free(e->key);
            rs_free(e);
            break;
        }
        prev = &e->next;
    }
    pthread_mutex_unlock(&g_mutex);
}

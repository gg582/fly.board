#define _POSIX_C_SOURCE 200809L
#include "cache.h"
#include <cwist/core/log.h>
#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* Use standard malloc/free for the shared cache so memory allocated by one
 * thread can safely be freed by another. cwist_alloc may be thread-local. */
#define cache_malloc malloc
#define cache_free free

#define CACHE_BUCKETS 1024

typedef struct cache_entry {
    char *key;
    char *data;
    size_t len;
    time_t expires_at;
    int pin_count;
    struct cache_entry *next;
} cache_entry_t;

static cache_entry_t *g_buckets[CACHE_BUCKETS];
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
static size_t g_total_bytes = 0;
static size_t g_max_bytes = 64 * 1024 * 1024; /* 64 MB default */

static uint32_t hash_str(const char *s) {
    uint32_t h = 5381;
    while (*s) h = ((h << 5) + h) + (unsigned char)*s++;
    return h;
}

bool page_cache_init(void) {
    const char *env = getenv("FLYBOARD_CACHE_MAX_MB");
    if (env) {
        char *end = NULL;
        long mb = strtol(env, &end, 10);
        if (end != env && *end == '\0' && mb > 0 && mb <= 1024) {
            g_max_bytes = (size_t)mb * 1024 * 1024;
        }
    }
    memset(g_buckets, 0, sizeof(g_buckets));
    g_total_bytes = 0;
    CWIST_LOG_INFO("Page cache initialized (max %zu MB)", g_max_bytes / (1024 * 1024));
    return true;
}

void page_cache_cleanup(void) {
    page_cache_clear();
}

static void free_entry(cache_entry_t *e) {
    if (!e) return;
    if (e->key) cache_free(e->key);
    if (e->data) cache_free(e->data);
    if (e->len <= g_total_bytes) g_total_bytes -= e->len;
    else g_total_bytes = 0;
    cache_free(e);
}

bool page_cache_set(const char *key, const char *data, size_t len, uint32_t ttl_sec) {
    if (!key || !data || len == 0 || ttl_sec == 0) return false;
    if (len > g_max_bytes / 4) {
        CWIST_LOG_DEBUG("Cache entry too large: %zu bytes for key %s", len, key);
        return false;
    }

    pthread_mutex_lock(&g_mutex);
    uint32_t h = hash_str(key) % CACHE_BUCKETS;

    /* Remove existing entry with the same key. */
    cache_entry_t **prev = &g_buckets[h];
    while (*prev) {
        if (strcmp((*prev)->key, key) == 0) {
            cache_entry_t *old = *prev;
            *prev = old->next;
            free_entry(old);
            break;
        }
        prev = &(*prev)->next;
    }

    /* Simple eviction: drop the oldest unpinned entry one bucket at a time
     * until there is room.  Pinned entries (currently being read by another
     * thread) must never be freed from under the reader. */
    while (g_total_bytes + len > g_max_bytes && g_total_bytes > 0) {
        bool evicted = false;
        for (int i = 0; i < CACHE_BUCKETS; i++) {
            cache_entry_t *chosen = NULL;
            cache_entry_t *chosen_prev = NULL;
            cache_entry_t *cur = g_buckets[i];
            cache_entry_t *prev = NULL;
            while (cur) {
                if (cur->pin_count == 0) {
                    chosen = cur;
                    chosen_prev = prev;
                }
                prev = cur;
                cur = cur->next;
            }
            if (chosen) {
                if (chosen_prev) chosen_prev->next = chosen->next;
                else g_buckets[i] = chosen->next;
                free_entry(chosen);
                evicted = true;
                break;
            }
        }
        if (!evicted) break;
    }

    if (g_total_bytes + len > g_max_bytes) {
        pthread_mutex_unlock(&g_mutex);
        CWIST_LOG_DEBUG("Cache full, dropping key %s", key);
        return false;
    }

    cache_entry_t *e = (cache_entry_t *)cache_malloc(sizeof(*e));
    if (!e) { pthread_mutex_unlock(&g_mutex); return false; }
    memset(e, 0, sizeof(*e));

    size_t key_len = strlen(key);
    e->key = (char *)cache_malloc(key_len + 1);
    e->data = (char *)cache_malloc(len);
    if (!e->key || !e->data) {
        if (e->key) cache_free(e->key);
        if (e->data) cache_free(e->data);
        cache_free(e);
        pthread_mutex_unlock(&g_mutex);
        return false;
    }
    memcpy(e->key, key, key_len + 1);
    memcpy(e->data, data, len);
    e->len = len;
    e->expires_at = time(NULL) + ttl_sec;
    e->next = g_buckets[h];
    g_buckets[h] = e;
    g_total_bytes += len;

    pthread_mutex_unlock(&g_mutex);
    return true;
}

bool page_cache_get(const char *key, const char **out_data, size_t *out_len, uint32_t *out_ttl_remaining) {
    if (!key) return false;
    pthread_mutex_lock(&g_mutex);
    uint32_t h = hash_str(key) % CACHE_BUCKETS;
    time_t now = time(NULL);
    cache_entry_t **prev = &g_buckets[h];
    while (*prev) {
        cache_entry_t *e = *prev;
        if (strcmp(e->key, key) == 0) {
            if (e->expires_at <= now) {
                if (e->pin_count > 0) {
                    /* In use by another thread; do not delete yet.  Report a miss
                     * so the caller computes its own copy and releases the pin
                     * later. */
                    pthread_mutex_unlock(&g_mutex);
                    return false;
                }
                *prev = e->next;
                free_entry(e);
                pthread_mutex_unlock(&g_mutex);
                return false;
            }
            e->pin_count++;
            if (out_data) *out_data = e->data;
            if (out_len) *out_len = e->len;
            if (out_ttl_remaining) *out_ttl_remaining = (uint32_t)(e->expires_at - now);
            pthread_mutex_unlock(&g_mutex);
            return true;
        }
        prev = &e->next;
    }
    pthread_mutex_unlock(&g_mutex);
    return false;
}

void page_cache_release(const char *key) {
    if (!key) return;
    pthread_mutex_lock(&g_mutex);
    uint32_t h = hash_str(key) % CACHE_BUCKETS;
    time_t now = time(NULL);
    cache_entry_t **prev = &g_buckets[h];
    while (*prev) {
        cache_entry_t *e = *prev;
        if (strcmp(e->key, key) == 0) {
            if (e->pin_count > 0) e->pin_count--;
            /* If the entry expired while pinned, free it now that the last pin
             * is gone. */
            if (e->pin_count == 0 && e->expires_at <= now) {
                *prev = e->next;
                free_entry(e);
            }
            break;
        }
        prev = &e->next;
    }
    pthread_mutex_unlock(&g_mutex);
}

void page_cache_delete(const char *key) {
    if (!key) return;
    pthread_mutex_lock(&g_mutex);
    uint32_t h = hash_str(key) % CACHE_BUCKETS;
    cache_entry_t **prev = &g_buckets[h];
    while (*prev) {
        if (strcmp((*prev)->key, key) == 0) {
            cache_entry_t *old = *prev;
            if (old->pin_count > 0) {
                /* Mark it expired so it is freed once the last reader releases. */
                old->expires_at = 0;
            } else {
                *prev = old->next;
                free_entry(old);
            }
            break;
        }
        prev = &(*prev)->next;
    }
    pthread_mutex_unlock(&g_mutex);
}

void page_cache_clear(void) {
    pthread_mutex_lock(&g_mutex);
    size_t remaining = 0;
    for (int i = 0; i < CACHE_BUCKETS; i++) {
        cache_entry_t *e = g_buckets[i];
        cache_entry_t *prev = NULL;
        while (e) {
            cache_entry_t *next = e->next;
            if (e->pin_count > 0) {
                e->expires_at = 0;
                remaining += e->len;
                prev = e;
            } else {
                if (prev) prev->next = next;
                else g_buckets[i] = next;
                cache_free(e->key);
                cache_free(e->data);
                cache_free(e);
            }
            e = next;
        }
    }
    g_total_bytes = remaining;
    pthread_mutex_unlock(&g_mutex);
}

void page_cache_clear_prefix(const char *prefix) {
    if (!prefix) return;
    size_t plen = strlen(prefix);
    pthread_mutex_lock(&g_mutex);
    for (int i = 0; i < CACHE_BUCKETS; i++) {
        cache_entry_t **prev = &g_buckets[i];
        while (*prev) {
            if (strncmp((*prev)->key, prefix, plen) == 0) {
                cache_entry_t *old = *prev;
                if (old->pin_count > 0) {
                    old->expires_at = 0;
                    prev = &old->next;
                } else {
                    *prev = old->next;
                    free_entry(old);
                }
            } else {
                prev = &(*prev)->next;
            }
        }
    }
    pthread_mutex_unlock(&g_mutex);
}

void page_cache_key_home(char *out, size_t out_len, bool dark, bool mobile, const char *role, int uid) {
    snprintf(out, out_len, "home:d=%d:m=%d:r=%s:u=%d",
             dark ? 1 : 0, mobile ? 1 : 0,
             role && role[0] ? role : "guest", uid);
}

void page_cache_key_post(char *out, size_t out_len, const char *slug, bool dark, bool mobile, const char *role, int uid) {
    snprintf(out, out_len, "post:%s:d=%d:m=%d:r=%s:u=%d",
             slug ? slug : "", dark ? 1 : 0, mobile ? 1 : 0,
             role && role[0] ? role : "guest", uid);
}

void page_cache_key_board(char *out, size_t out_len, const char *slug, int page,
                          bool dark, bool mobile, const char *role, int uid,
                          const char *search, const char *search_type) {
    snprintf(out, out_len, "board:%s:p=%d:d=%d:m=%d:r=%s:u=%d:s=%s:st=%s",
             slug ? slug : "", page, dark ? 1 : 0, mobile ? 1 : 0,
             role && role[0] ? role : "guest", uid,
             search ? search : "", search_type ? search_type : "");
}

void page_cache_key_board_list(char *out, size_t out_len, bool dark, bool mobile,
                               const char *role, int uid) {
    snprintf(out, out_len, "boards:d=%d:m=%d:r=%s:u=%d",
             dark ? 1 : 0, mobile ? 1 : 0,
             role && role[0] ? role : "guest", uid);
}

void page_cache_invalidate_post(const char *slug) {
    if (slug) {
        char prefix[256];
        snprintf(prefix, sizeof(prefix), "post:%s:", slug);
        page_cache_clear_prefix(prefix);
    }
    /* Home and board listings embed post titles/summaries/votes, so they must
     * be invalidated too. */
    page_cache_clear_prefix("home:");
    page_cache_clear_prefix("board:");
    page_cache_clear_prefix("boards:");
}

void page_cache_invalidate_board(const char *slug) {
    if (slug) {
        char prefix[256];
        snprintf(prefix, sizeof(prefix), "board:%s:", slug);
        page_cache_clear_prefix(prefix);
    }
    page_cache_clear_prefix("board:");
    page_cache_clear_prefix("boards:");
    page_cache_clear_prefix("home:");
}

void page_cache_invalidate_all(void) {
    page_cache_clear();
}

size_t page_cache_total_bytes(void) {
    size_t total;
    pthread_mutex_lock(&g_mutex);
    total = g_total_bytes;
    pthread_mutex_unlock(&g_mutex);
    return total;
}

#include "realtime.h"

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

#define REALTIME_SLOTS 2048

typedef struct {
    int post_id;
    char session[REALTIME_SESSION_HEX_LEN + 1];
    int64_t presence_expires;
    int64_t typing_expires;
    int64_t last_typing;
} realtime_entry_t;

typedef struct {
    realtime_entry_t entries[REALTIME_SLOTS];
    pthread_mutex_t mtx;
} realtime_shared_t;

static realtime_shared_t g_local = { .mtx = PTHREAD_MUTEX_INITIALIZER };
static realtime_shared_t *g_shared = &g_local;
#define g_entries (g_shared->entries)
#define g_realtime_mtx (g_shared->mtx)

void realtime_init(void) {
    if (g_shared != &g_local) return;
    realtime_shared_t *shared = mmap(NULL, sizeof(*shared), PROT_READ | PROT_WRITE,
                                     MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (shared == MAP_FAILED) return;
    memset(shared, 0, sizeof(*shared));
    bool ready = false;
    pthread_mutexattr_t attr;
    if (pthread_mutexattr_init(&attr) == 0) {
        if (pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED) == 0)
            ready = pthread_mutex_init(&shared->mtx, &attr) == 0;
        pthread_mutexattr_destroy(&attr);
    }
    if (!ready) {
        munmap(shared, sizeof(*shared));
        return;
    }
    g_shared = shared;
}

int64_t realtime_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

bool realtime_session_valid(const char *session) {
    if (!session || strlen(session) != REALTIME_SESSION_HEX_LEN) return false;
    for (size_t i = 0; i < REALTIME_SESSION_HEX_LEN; i++) {
        if (!isxdigit((unsigned char)session[i])) return false;
    }
    return true;
}

void realtime_reset(void) {
    pthread_mutex_lock(&g_realtime_mtx);
    memset(g_entries, 0, sizeof(g_local.entries));
    pthread_mutex_unlock(&g_realtime_mtx);
}

static size_t slot_for(int post_id, const char *session) {
    uint32_t hash = (uint32_t)post_id * 2166136261u;
    for (size_t i = 0; i < REALTIME_SESSION_HEX_LEN; i++) {
        hash ^= (unsigned char)session[i];
        hash *= 16777619u;
    }
    return hash % REALTIME_SLOTS;
}

static realtime_entry_t *lookup_locked(int post_id, const char *session, int64_t now_ms, bool create) {
    size_t start = slot_for(post_id, session);
    realtime_entry_t *free_slot = NULL;
    for (size_t n = 0; n < REALTIME_SLOTS; n++) {
        realtime_entry_t *entry = &g_entries[(start + n) % REALTIME_SLOTS];
        if (entry->post_id == post_id && strcmp(entry->session, session) == 0) return entry;
        if (!free_slot && (!entry->post_id ||
            (entry->presence_expires <= now_ms && entry->typing_expires <= now_ms)))
            free_slot = entry;
    }
    return create ? free_slot : NULL;
}

realtime_result_t realtime_presence_at(int post_id, const char *session, bool active, int64_t now_ms) {
    if (post_id <= 0 || !realtime_session_valid(session)) return REALTIME_INVALID;
    realtime_result_t result = REALTIME_OK;
    pthread_mutex_lock(&g_realtime_mtx);
    realtime_entry_t *entry = lookup_locked(post_id, session, now_ms, active);
    if (!entry && active) {
        result = REALTIME_FULL;
    } else if (entry) {
        if (active) {
            if (!entry->post_id) {
                entry->post_id = post_id;
                snprintf(entry->session, sizeof(entry->session), "%s", session);
            }
            entry->presence_expires = now_ms + REALTIME_PRESENCE_LEASE_MS;
        } else {
            memset(entry, 0, sizeof(*entry));
        }
    }
    pthread_mutex_unlock(&g_realtime_mtx);
    return result;
}

static realtime_result_t typing_locked(int post_id, const char *session, bool active, int64_t now_ms,
                                       bool rate_limit) {
    realtime_entry_t *entry = lookup_locked(post_id, session, now_ms, active);
    if (!entry) return active ? REALTIME_FULL : REALTIME_OK;
    if (active && rate_limit && entry->last_typing > 0 &&
        now_ms - entry->last_typing < REALTIME_TYPING_MIN_INTERVAL_MS)
        return REALTIME_RATE_LIMITED;
    if (active) {
        if (!entry->post_id) {
            entry->post_id = post_id;
            snprintf(entry->session, sizeof(entry->session), "%s", session);
        }
        entry->last_typing = now_ms;
        entry->typing_expires = now_ms + REALTIME_TYPING_LEASE_MS;
    } else {
        entry->typing_expires = 0;
        entry->last_typing = 0;
        if (entry->presence_expires <= now_ms) memset(entry, 0, sizeof(*entry));
    }
    return REALTIME_OK;
}

realtime_result_t realtime_typing_at(int post_id, const char *session, bool active, int64_t now_ms) {
    if (post_id <= 0 || !realtime_session_valid(session)) return REALTIME_INVALID;
    pthread_mutex_lock(&g_realtime_mtx);
    realtime_result_t result = typing_locked(post_id, session, active, now_ms, true);
    pthread_mutex_unlock(&g_realtime_mtx);
    return result;
}

void realtime_typing_event_at(int post_id, const char *session, bool active, int64_t now_ms) {
    if (post_id <= 0 || !realtime_session_valid(session)) return;
    pthread_mutex_lock(&g_realtime_mtx);
    (void)typing_locked(post_id, session, active, now_ms, false);
    pthread_mutex_unlock(&g_realtime_mtx);
}

static int count_at(int post_id, const char *exclude_session, int64_t now_ms, bool typing) {
    int count = 0;
    pthread_mutex_lock(&g_realtime_mtx);
    for (size_t i = 0; i < REALTIME_SLOTS; i++) {
        realtime_entry_t *entry = &g_entries[i];
        if (entry->post_id != post_id) continue;
        if (exclude_session && strcmp(entry->session, exclude_session) == 0) continue;
        int64_t expires = typing ? entry->typing_expires : entry->presence_expires;
        if (expires > now_ms) count++;
    }
    pthread_mutex_unlock(&g_realtime_mtx);
    return count;
}

int realtime_viewer_count_at(int post_id, int64_t now_ms) {
    return count_at(post_id, NULL, now_ms, false);
}

int realtime_typing_count_at(int post_id, const char *exclude_session, int64_t now_ms) {
    return count_at(post_id, exclude_session, now_ms, true);
}

void realtime_viewer_label(int count, char *out, size_t out_size) {
    if (!out || out_size == 0) return;
    if (count <= 0) snprintf(out, out_size, "No viewers");
    else if (count == 1) snprintf(out, out_size, "1 viewer");
    else if (count <= 5) snprintf(out, out_size, "2-5 viewers");
    else if (count <= 20) snprintf(out, out_size, "6-20 viewers");
    else snprintf(out, out_size, "21+ viewers");
}

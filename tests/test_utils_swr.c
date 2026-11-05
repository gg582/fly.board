#include "engine/refresh.h"
#include "utils/cache.h"
#include "utils/reqshare.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define THREADS 24

static int failures;
static pthread_mutex_t gate_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cond = PTHREAD_COND_INITIALIZER;
static int gate_open;
static atomic_int calls;
static atomic_int completed;
static atomic_int stale_hits;

#define CHECK(cond, name) do { \
    if (cond) printf("PASS: %s\n", name); \
    else { printf("FAIL: %s (line %d)\n", name, __LINE__); failures++; } \
} while (0)

typedef struct {
    char key[64];
    int delay_ms;
} refresh_arg;

static void refresh(void *ptr) {
    refresh_arg *arg = ptr;
    atomic_fetch_add(&calls, 1);
    if (arg->delay_ms) usleep((useconds_t)arg->delay_ms * 1000);
    pthread_mutex_lock(&gate_mtx);
    while (!gate_open) pthread_cond_wait(&gate_cond, &gate_mtx);
    pthread_mutex_unlock(&gate_mtx);
    atomic_fetch_add(&completed, 1);
}

static void open_gate(void) {
    pthread_mutex_lock(&gate_mtx);
    gate_open = 1;
    pthread_cond_broadcast(&gate_cond);
    pthread_mutex_unlock(&gate_mtx);
}

static void *stale_client(void *ptr) {
    const char *key = ptr;
    const char *data = NULL;
    size_t len = 0;
    if (page_cache_get_stale(key, &data, &len)) {
        if (len == 3 && memcmp(data, "old", 3) == 0) atomic_fetch_add(&stale_hits, 1);
        refresh_arg *arg = calloc(1, sizeof(*arg));
        if (arg) {
            snprintf(arg->key, sizeof(arg->key), "%s", key);
            arg->delay_ms = 100;
            engine_refresh_schedule(key, refresh, free, arg);
        }
        page_cache_release(key);
    }
    return NULL;
}

static void *cache_stress_client(void *ptr) {
    uintptr_t id = (uintptr_t)ptr;
    for (int i = 0; i < 1500; i++) {
        char key[64];
        snprintf(key, sizeof(key), "stress:%d", i % 16);
        if ((i + (int)id) % 7 == 0) {
            uint64_t generation = page_cache_generation();
            page_cache_set_if_generation(key, "new", 3, 1, generation);
        } else {
            const char *data = NULL;
            size_t len = 0;
            if (page_cache_get(key, &data, &len, NULL) ||
                page_cache_get_stale(key, &data, &len)) {
                if (len != 3) atomic_fetch_add(&calls, 1000000);
                page_cache_release(key);
            }
        }
        if ((i + (int)id) % 211 == 0) page_cache_clear_prefix("stress:");
    }
    return NULL;
}

int main(void) {
    setenv("FLYBOARD_CACHE_MAX_MB", "8", 1);
    setenv("FLY_PAGE_SWR_SECONDS", "30", 1);
    setenv("FLY_PAGE_REFRESH_WORKERS", "1", 1);
    setenv("FLY_PAGE_REFRESH_QUEUE_MAX", "1", 1);
    CHECK(page_cache_init(), "page_cache_init");
    CHECK(reqshare_init(), "reqshare_init");

    CHECK(page_cache_set("hot", "old", 3, 1), "seed expiring page");
    sleep(2);
    pthread_t clients[THREADS];
    for (int i = 0; i < THREADS; i++) pthread_create(&clients[i], NULL, stale_client, "hot");
    for (int i = 0; i < THREADS; i++) pthread_join(clients[i], NULL);
    CHECK(atomic_load(&stale_hits) == THREADS, "every stampede client receives stale bytes");
    CHECK(atomic_load(&calls) == 1, "stampede elects exactly one refresh leader");
    open_gate();
    for (int i = 0; i < 100 && atomic_load(&completed) != 1; i++) usleep(20000);
    CHECK(atomic_load(&completed) == 1, "refresh leader completes");

    uint64_t generation = page_cache_generation();
    page_cache_set("other", "x", 1, 30);
    page_cache_clear_prefix("other");
    CHECK(!page_cache_set_if_generation("hot", "bad", 3, 30, generation),
          "mutation invalidation rejects late background render");
    page_cache_delete("hot");
    CHECK(!page_cache_get_stale("hot", NULL, NULL), "invalidated page cannot be served stale");

    CHECK(page_cache_set("pinned", "old", 3, 1), "seed pinned page");
    sleep(2);
    const char *data = NULL;
    size_t len = 0;
    CHECK(page_cache_get_stale("pinned", &data, &len), "pin stale page");
    CHECK(page_cache_set("pinned", "new", 3, 30), "replace pinned stale page");
    CHECK(len == 3 && memcmp(data, "old", 3) == 0, "old pin remains valid after replacement");
    page_cache_release("pinned");
    CHECK(page_cache_get("pinned", &data, &len, NULL) && memcmp(data, "new", 3) == 0,
          "replacement visible to next request");
    page_cache_release("pinned");

    gate_open = 0;
    refresh_arg *first = calloc(1, sizeof(*first));
    refresh_arg *second = calloc(1, sizeof(*second));
    refresh_arg *third = calloc(1, sizeof(*third));
    snprintf(first->key, sizeof(first->key), "queue:a");
    snprintf(second->key, sizeof(second->key), "queue:b");
    snprintf(third->key, sizeof(third->key), "queue:c");
    CHECK(engine_refresh_schedule(first->key, refresh, free, first), "queue accepts active job");
    for (int i = 0; i < 100 && atomic_load(&calls) < 2; i++) usleep(10000);
    CHECK(engine_refresh_schedule(second->key, refresh, free, second), "queue accepts one waiting job");
    CHECK(!engine_refresh_schedule(third->key, refresh, free, third), "bounded queue rejects overflow");
    open_gate();
    for (int i = 0; i < 100 && atomic_load(&completed) < 3; i++) usleep(20000);
    CHECK(atomic_load(&completed) == 3, "accepted queue jobs drain");

    pthread_t stress[8];
    for (uintptr_t i = 0; i < 8; i++) pthread_create(&stress[i], NULL, cache_stress_client, (void *)i);
    for (int i = 0; i < 8; i++) pthread_join(stress[i], NULL);
    CHECK(atomic_load(&calls) < 1000000, "concurrent cache stress preserves payload lengths");
    CHECK(page_cache_total_bytes() <= 8 * 1024 * 1024, "stress remains within cache byte bound");

    page_cache_cleanup();
    reqshare_cleanup();
    printf("%s\n", failures == 0 ? "ALL PASS" : "SOME FAILED");
    return failures == 0 ? 0 : 1;
}

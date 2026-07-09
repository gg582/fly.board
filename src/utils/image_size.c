#define _POSIX_C_SOURCE 200809L
#include "cwist/image_size.h"
#include "stb_image.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <pthread.h>

#define CACHE_SIZE 16

typedef struct {
    char path[256];
    time_t mtime;
    int w;
    int h;
    int valid;
} cache_entry_t;

static pthread_mutex_t g_image_size_mutex = PTHREAD_MUTEX_INITIALIZER;
static cache_entry_t cache[CACHE_SIZE] = {0};
static int next_slot = 0;

bool get_image_dimensions(const char *path, int *w, int *h) {
    if (!path || !path[0]) return false;

    struct stat st;
    time_t mtime = 0;
    if (stat(path, &st) == 0) mtime = st.st_mtime;

    /* Check cache */
    pthread_mutex_lock(&g_image_size_mutex);
    for (int i = 0; i < CACHE_SIZE; i++) {
        if (cache[i].valid && strcmp(cache[i].path, path) == 0 && cache[i].mtime == mtime) {
            *w = cache[i].w;
            *h = cache[i].h;
            pthread_mutex_unlock(&g_image_size_mutex);
            return true;
        }
    }
    pthread_mutex_unlock(&g_image_size_mutex);

    int ww, hh, channels;
    unsigned char *data = stbi_load(path, &ww, &hh, &channels, 0);
    if (!data) return false;
    stbi_image_free(data);

    /* Find empty slot or evict oldest (simple round-robin) */
    pthread_mutex_lock(&g_image_size_mutex);
    int slot = next_slot;
    next_slot = (next_slot + 1) % CACHE_SIZE;

    snprintf(cache[slot].path, sizeof(cache[slot].path), "%s", path);
    cache[slot].mtime = mtime;
    cache[slot].w = ww;
    cache[slot].h = hh;
    cache[slot].valid = 1;

    *w = ww;
    *h = hh;
    pthread_mutex_unlock(&g_image_size_mutex);
    return true;
}

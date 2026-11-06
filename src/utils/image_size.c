#define _POSIX_C_SOURCE 200809L
#include "cwist/image_size.h"
#include "stb_image.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <pthread.h>

#define CACHE_SIZE 256

typedef struct {
    char path[512];
    time_t mtime;
    off_t size;
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
    off_t size = 0;
    if (stat(path, &st) == 0) {
        mtime = st.st_mtime;
        size = st.st_size;
    }

    pthread_mutex_lock(&g_image_size_mutex);
    for (int i = 0; i < CACHE_SIZE; i++) {
        if (cache[i].valid && strcmp(cache[i].path, path) == 0 &&
            cache[i].mtime == mtime && cache[i].size == size) {
            *w = cache[i].w;
            *h = cache[i].h;
            pthread_mutex_unlock(&g_image_size_mutex);
            return true;
        }
    }
    pthread_mutex_unlock(&g_image_size_mutex);

    int ww, hh, channels;
    if (!stbi_info(path, &ww, &hh, &channels)) return false;

    pthread_mutex_lock(&g_image_size_mutex);
    int slot = next_slot;
    next_slot = (next_slot + 1) % CACHE_SIZE;

    snprintf(cache[slot].path, sizeof(cache[slot].path), "%s", path);
    cache[slot].mtime = mtime;
    cache[slot].size = size;
    cache[slot].w = ww;
    cache[slot].h = hh;
    cache[slot].valid = 1;

    *w = ww;
    *h = hh;
    pthread_mutex_unlock(&g_image_size_mutex);
    return true;
}

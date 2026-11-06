#include "cwist/image_size.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define THREADS 8
#define ITERATIONS 200

static char g_path[256];
static atomic_int g_failures;

static int write_pgm(int w, int h) {
    FILE *f = fopen(g_path, "wb");
    if (!f) return 0;
    int ok = fprintf(f, "P5\n%d %d\n255\n", w, h) > 0;
    for (int i = 0; ok && i < w * h; i++) ok = fputc(i & 0xff, f) != EOF;
    return fclose(f) == 0 && ok;
}

static void *reader(void *arg) {
    (void)arg;
    for (int i = 0; i < ITERATIONS; i++) {
        int w = 0, h = 0;
        if (!get_image_dimensions(g_path, &w, &h) || w != 3 || h != 2)
            atomic_fetch_add(&g_failures, 1);
    }
    return NULL;
}

int main(void) {
    int fd = -1;
    snprintf(g_path, sizeof(g_path), "/tmp/fly-image-size-XXXXXX");
    fd = mkstemp(g_path);
    if (fd < 0) return 1;
    close(fd);
    unlink(g_path);

    if (!write_pgm(3, 2)) return 1;
    int w = 0, h = 0;
    if (!get_image_dimensions(g_path, &w, &h) || w != 3 || h != 2) return 1;

    pthread_t threads[THREADS];
    for (int i = 0; i < THREADS; i++) {
        if (pthread_create(&threads[i], NULL, reader, NULL) != 0) return 1;
    }
    for (int i = 0; i < THREADS; i++) pthread_join(threads[i], NULL);
    if (atomic_load(&g_failures) != 0) return 1;

    if (!write_pgm(5, 7) || !get_image_dimensions(g_path, &w, &h) || w != 5 || h != 7)
        return 1;
    if (get_image_dimensions("/tmp/fly-image-size-does-not-exist", &w, &h)) return 1;

    unlink(g_path);
    puts("image size: header reads, concurrent cache hits, and invalidation OK");
    return 0;
}

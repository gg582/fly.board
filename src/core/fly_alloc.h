#ifndef FLYBOARD_CORE_FLY_ALLOC_H
#define FLYBOARD_CORE_FLY_ALLOC_H

/* Zeroed array allocation that is never tracked by CWIST's scope GC.
 *
 * fly.board maps calloc() here. Much of that memory outlives the request
 * that allocates it: async job queues, NATS publish queues, caches, and
 * buffers handed to worker threads. Since CWIST v4.3.6, cwist_alloc_array()
 * registers its result with the calling thread's GC scope when full GC is
 * enabled, so the end-of-request flush freed such memory while its new owner
 * still used it (double free / use-after-free in the NATS thread and in
 * post_announce_worker). cwist_malloc() is the same zeroed allocation without
 * scope tracking, which is what these call sites were written for; every
 * release still goes through free()/cwist_free().
 *
 * Include this only after the system headers and <cwist/core/mem/alloc.h>:
 * it includes nothing itself, so it is safe inside a block that redefines
 * malloc/free. */
static inline void *fly_calloc_untracked(size_t count, size_t elem_size) {
    if (count && elem_size > __SIZE_MAX__ / count) return NULL;
    return cwist_malloc(count && elem_size ? count * elem_size : 1);
}

#endif

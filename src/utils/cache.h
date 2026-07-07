#ifndef FLYBOARD_CACHE_H
#define FLYBOARD_CACHE_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

/* In-memory page cache with TTL.
 *
 * Designed for rendered HTML pages. Keys are short ASCII strings; values are
 * opaque byte blobs (usually UTF-8 HTML). Thread-safe via a single mutex.
 *
 * NOTE: The cache lives in each worker process. CWIST uses multiple workers,
 * so the effective hit rate is split across processes. Even so, each worker
 * caches its own hot pages, which still eliminates repeated DB+render work
 * under load. Future improvement: shared memory or external cache. */

bool page_cache_init(void);
void page_cache_cleanup(void);

/* Store a value with a TTL in seconds. Returns false on allocation failure or
 * if the entry is too large (> max_size / 4). */
bool page_cache_set(const char *key, const char *data, size_t len, uint32_t ttl_sec);

/* Look up a value. On hit, sets *out_data and *out_len and returns true.
 * out_ttl_remaining is optional. The returned pointer remains valid only
 * until the next cache mutation call. */
bool page_cache_get(const char *key, const char **out_data, size_t *out_len, uint32_t *out_ttl_remaining);

void page_cache_delete(const char *key);
void page_cache_clear(void);
void page_cache_clear_prefix(const char *prefix);

/* Convenience key builders. All write a NUL-terminated key into out. */
void page_cache_key_home(char *out, size_t out_len, bool dark, bool mobile, const char *role, int uid);
void page_cache_key_post(char *out, size_t out_len, const char *slug, bool dark, bool mobile, const char *role, int uid);
void page_cache_key_board(char *out, size_t out_len, const char *slug, int page,
                          bool dark, bool mobile, const char *role, int uid,
                          const char *search, const char *search_type);

/* Invalidation helpers. These clear cache entries that depend on changed data. */
void page_cache_invalidate_post(const char *slug);
void page_cache_invalidate_board(const char *slug);
void page_cache_invalidate_all(void);

/* Current total cached payload size in bytes (approximate). */
size_t page_cache_total_bytes(void);

#endif

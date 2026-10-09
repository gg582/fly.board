/* Unit test: src/utils/cache.c page cache (set/get, TTL expiry, delete,
 * clear, clear_prefix, key builders, oversized value rejection).
 *
 * Build:
 *   gcc -O1 -Iinclude -Isrc -I/home/yjlee/cwist/include -I/home/yjlee/cwist/lib \
 *       tests/test_utils_cache.c tests/stubs/utils_test_stubs.c src/utils/cache.o \
 *       /home/yjlee/cwist/libcwist.a /home/yjlee/cwist/lib/libttak/lib/libttak.a /home/yjlee/cwist/lib/cjson/libcjson.a \
 *       -lpthread -lm -ldl -o /tmp/test_utils_cache
 * Run: /tmp/test_utils_cache
 */
#include "utils/cache.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>

static int failures = 0;
#define CHECK(cond, name) do { \
    if (cond) printf("PASS: %s\n", name); \
    else { printf("FAIL: %s (line %d)\n", name, __LINE__); failures++; } \
} while (0)

int main(void) {
    setenv("FLYBOARD_CACHE_MAX_MB", "1", 1); /* 1 MB cap -> 256 KB entry limit */
    CHECK(page_cache_init(), "page_cache_init");

    /* basic set/get */
    CHECK(page_cache_set("k1", "hello", 5, 60), "set k1");
    const char *data = NULL;
    size_t len = 0;
    uint32_t ttl = 0;
    CHECK(page_cache_get("k1", &data, &len, &ttl), "get k1");
    CHECK(len == 5 && memcmp(data, "hello", 5) == 0, "k1 payload intact");
    CHECK(ttl > 0 && ttl <= 60, "ttl reported");
    page_cache_release("k1");

    CHECK(!page_cache_get("missing", &data, &len, NULL), "miss on missing key");

    /* overwrite */
    CHECK(page_cache_set("k1", "world!", 6, 60), "overwrite k1");
    CHECK(page_cache_get("k1", &data, &len, NULL) && len == 6 &&
          memcmp(data, "world!", 6) == 0, "overwrite visible");
    page_cache_release("k1");

    /* TTL expiry */
    CHECK(page_cache_set("kttl", "x", 1, 1), "set short-ttl");
    sleep(2);
    CHECK(!page_cache_get("kttl", &data, &len, NULL), "expired entry missed");

    /* delete */
    CHECK(page_cache_set("kdel", "y", 1, 60), "set kdel");
    page_cache_delete("kdel");
    CHECK(!page_cache_get("kdel", &data, &len, NULL), "deleted entry missed");

    /* clear_prefix */
    page_cache_set("post/a", "1", 1, 60);
    page_cache_release("post/a");
    page_cache_set("post/b", "2", 1, 60);
    page_cache_release("post/b");
    page_cache_set("board/c", "3", 1, 60);
    page_cache_release("board/c");
    page_cache_clear_prefix("post/");
    CHECK(!page_cache_get("post/a", &data, &len, NULL), "prefix clear removes post/a");
    CHECK(!page_cache_get("post/b", &data, &len, NULL), "prefix clear removes post/b");
    CHECK(page_cache_get("board/c", &data, &len, NULL), "prefix clear keeps board/c");
    page_cache_release("board/c");

    /* clear */
    page_cache_clear();
    CHECK(!page_cache_get("k1", &data, &len, NULL), "clear empties cache");

    /* oversized value (> max/4) rejected */
    size_t big = 512 * 1024; /* > 1MB/4 */
    char *blob = malloc(big);
    memset(blob, 'z', big);
    CHECK(blob != NULL, "alloc blob");
    if (blob) {
        bool r = page_cache_set("huge", blob, big, 60);
        CHECK(!r || !page_cache_get("huge", &data, &len, NULL),
              "oversized value not cached");
        if (r) page_cache_release("huge");
        free(blob);
    }

    /* key builders */
    char key[256];
    page_cache_key_home(key, sizeof(key), false, false, "user", 7);
    CHECK(strstr(key, "7") != NULL, "home key embeds uid");
    page_cache_key_post(key, sizeof(key), "my-post", true, false, "", 0);
    CHECK(strstr(key, "my-post") != NULL && strstr(key, "d=1") != NULL,
          "post key embeds slug and dark flag");
    page_cache_key_board(key, sizeof(key), "dev", 2, false, true, "user", 3, "q", "title");
    CHECK(strstr(key, "dev") != NULL && strstr(key, "q") != NULL,
          "board key embeds slug and search");

    page_cache_cleanup();
    printf("%s\n", failures == 0 ? "ALL PASS" : "SOME FAILED");
    return failures == 0 ? 0 : 1;
}

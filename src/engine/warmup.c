#define _POSIX_C_SOURCE 200809L
#include "engine/warmup.h"
#include "utils/cache.h"
#include "render/render.h"
#include "db/db.h"
#include <cwist/core/log.h>
#include <cwist/core/sstring/sstring.h>
#include <cjson/cJSON.h>

void page_cache_warmup(cwist_db *db) {
    if (!db) return;
    CWIST_LOG_INFO("Warming up page cache...");

    struct { bool dark; bool mobile; } variants[] = {
        {false, false}, {true, false}, {false, true}, {true, true}
    };

    for (size_t i = 0; i < sizeof(variants) / sizeof(variants[0]); i++) {
        bool dark = variants[i].dark;
        bool mobile = variants[i].mobile;
        char key[256];
        page_cache_key_home(key, sizeof(key), dark, mobile, "", 0);
        if (page_cache_get(key, NULL, NULL, NULL)) continue;

        cJSON *posts = db_post_recent(db, 12);
        cwist_sstring *page = render_post_list(posts, NULL, dark, "", 1, 1, "", NULL, NULL, NULL, 0, mobile, NULL);
        if (page) {
            page_cache_set(key, page->data, page->size, 300);
            cwist_sstring_destroy(page);
        }
        if (posts) cJSON_Delete(posts);
    }

    CWIST_LOG_INFO("Page cache warmup complete (%zu bytes)", page_cache_total_bytes());
}

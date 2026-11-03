#define _POSIX_C_SOURCE 200809L
#include "handlers/tasfa/tasfa_internal.h"

extern finalize_slot_t g_finalize_slots[TASFA_FINALIZE_CACHE_SLOTS];

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "CHECK failed at line %d: %s\n", __LINE__, #condition); \
        return 1; \
    } \
} while (0)

static void clear_local_cache(void) {
    for (int i = 0; i < TASFA_FINALIZE_CACHE_SLOTS; i++) {
        free(g_finalize_slots[i].body);
        memset(&g_finalize_slots[i], 0, sizeof(g_finalize_slots[i]));
    }
}

int main(void) {
    char temp_root[] = "/tmp/fly-finalize-cache.XXXXXX";
    CHECK(mkdtemp(temp_root) != NULL);
    CHECK(chdir(temp_root) == 0);
    CHECK(ensure_tasfa_roots());
    CHECK(rmdir("data/tasfa/finalize") == 0);

    const char *id = "finalize-test-upload";
    const char *token = "test-token";
    int status = 0;
    bool active = false;
    char *body = NULL;

    CHECK(finalize_cache_mark_started(id, token));
    CHECK(!finalize_cache_mark_started(id, token));
    finalize_cache_update_status(id, "Processing media previews...");
    clear_local_cache();

    CHECK(finalize_cache_get(id, token, &status, &body, &active));
    CHECK(status == 202 && active);
    CHECK(body && strstr(body, "Processing media previews"));
    free(body);
    body = NULL;

    const char *done_id = "finalize-test-done";
    CHECK(finalize_cache_mark_started(done_id, token));
    finalize_cache_mark_done(done_id, 200, "{\"ok\":true,\"fid\":42}");
    clear_local_cache();
    CHECK(finalize_cache_get(done_id, token, &status, &body, &active));
    CHECK(status == 200 && !active);
    CHECK(body && strcmp(body, "{\"ok\":true,\"fid\":42}") == 0);
    free(body);
    body = NULL;

    CHECK(!finalize_cache_get(done_id, "wrong-token", &status, &body, &active));
    CHECK(!finalize_cache_mark_started(done_id, token));

    clear_local_cache();
    unlink("data/tasfa/finalize/finalize-test-upload.json");
    unlink("data/tasfa/finalize/finalize-test-done.json");
    rmdir("data/tasfa/finalize");
    rmdir("data/tasfa/uploads");
    rmdir("data/tasfa/downloads");
    rmdir("data/tasfa");
    rmdir("data");
    rmdir("public/uploads");
    rmdir("public");
    CHECK(chdir("/") == 0);
    CHECK(rmdir(temp_root) == 0);
    puts("PASS: finalize state survives local cache isolation");
    return 0;
}

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "tasfa_internal.h"

cache_slot_t g_tasfa_cache[TASFA_CACHE_SLOTS];
pthread_mutex_t g_tasfa_cache_mtx = PTHREAD_MUTEX_INITIALIZER;

finalize_slot_t g_finalize_slots[TASFA_FINALIZE_CACHE_SLOTS];
pthread_mutex_t g_finalize_mtx = PTHREAD_MUTEX_INITIALIZER;

static unsigned int cache_hash(const char *str) {
    unsigned int h = 5381;
    int c;
    while ((c = *str++))
        h = ((h << 5) + h) + c;
    return h % TASFA_CACHE_SLOTS;
}

static void cache_clear_slot(cache_slot_t *slot) {
    if (!slot->valid) return;
    if (slot->type == 3 && slot->data.json) {
        cJSON_Delete(slot->data.json);
        slot->data.json = NULL;
    }
    slot->valid = 0;
    slot->key[0] = '\0';
}

void cache_invalidate(const char *key) {
    unsigned int idx = cache_hash(key);
    pthread_mutex_lock(&g_tasfa_cache_mtx);
    for (int i = 0; i < TASFA_CACHE_SLOTS; i++) {
        int probe = (idx + i) % TASFA_CACHE_SLOTS;
        if (!g_tasfa_cache[probe].valid) break;
        if (strcmp(g_tasfa_cache[probe].key, key) == 0) {
            cache_clear_slot(&g_tasfa_cache[probe]);
            break;
        }
    }
    pthread_mutex_unlock(&g_tasfa_cache_mtx);
}

bool load_upload_session_meta_bin_cached(const char *upload_id, tasfa_meta_bin_t *out) {
    unsigned int idx = cache_hash(upload_id);
    pthread_mutex_lock(&g_tasfa_cache_mtx);
    for (int i = 0; i < TASFA_CACHE_SLOTS; i++) {
        int probe = (idx + i) % TASFA_CACHE_SLOTS;
        if (!g_tasfa_cache[probe].valid) break;
        if (g_tasfa_cache[probe].type == 1 && strcmp(g_tasfa_cache[probe].key, upload_id) == 0) {
            if (time(NULL) <= g_tasfa_cache[probe].expires) {
                memcpy(out, &g_tasfa_cache[probe].data.mbin, sizeof(tasfa_meta_bin_t));
                pthread_mutex_unlock(&g_tasfa_cache_mtx);
                return true;
            }
            cache_clear_slot(&g_tasfa_cache[probe]);
            break;
        }
    }
    pthread_mutex_unlock(&g_tasfa_cache_mtx);
    if (load_upload_session_meta_bin(upload_id, out)) {
        pthread_mutex_lock(&g_tasfa_cache_mtx);
        /* Re-check after loading: another thread may have inserted this entry
           while we were reading from disk. */
        bool already_cached = false;
        for (int i = 0; i < TASFA_CACHE_SLOTS; i++) {
            int probe = (idx + i) % TASFA_CACHE_SLOTS;
            if (!g_tasfa_cache[probe].valid) break;
            if (g_tasfa_cache[probe].type == 1 && strcmp(g_tasfa_cache[probe].key, upload_id) == 0) {
                if (time(NULL) <= g_tasfa_cache[probe].expires) {
                    memcpy(out, &g_tasfa_cache[probe].data.mbin, sizeof(tasfa_meta_bin_t));
                    already_cached = true;
                } else {
                    cache_clear_slot(&g_tasfa_cache[probe]);
                }
                break;
            }
        }
        if (!already_cached) {
            for (int i = 0; i < TASFA_CACHE_SLOTS; i++) {
                int probe = (idx + i) % TASFA_CACHE_SLOTS;
                if (!g_tasfa_cache[probe].valid) {
                    g_tasfa_cache[probe].valid = 1;
                    g_tasfa_cache[probe].type = 1;
                    strncpy(g_tasfa_cache[probe].key, upload_id, sizeof(g_tasfa_cache[probe].key)-1);
                    g_tasfa_cache[probe].key[sizeof(g_tasfa_cache[probe].key)-1] = '\0';
                    memcpy(&g_tasfa_cache[probe].data.mbin, out, sizeof(tasfa_meta_bin_t));
                    g_tasfa_cache[probe].expires = time(NULL) + TASFA_UPLOAD_TTL;
                    break;
                }
            }
        }
        pthread_mutex_unlock(&g_tasfa_cache_mtx);
        return true;
    }
    return false;
}

cJSON *load_download_session_cached(const char *session_id) {
    unsigned int idx = cache_hash(session_id);
    pthread_mutex_lock(&g_tasfa_cache_mtx);
    for (int i = 0; i < TASFA_CACHE_SLOTS; i++) {
        int probe = (idx + i) % TASFA_CACHE_SLOTS;
        if (!g_tasfa_cache[probe].valid) break;
        if (g_tasfa_cache[probe].type == 3 && strcmp(g_tasfa_cache[probe].key, session_id) == 0) {
            if (time(NULL) <= g_tasfa_cache[probe].expires) {
                cJSON *copy = cJSON_Duplicate(g_tasfa_cache[probe].data.json, 1);
                pthread_mutex_unlock(&g_tasfa_cache_mtx);
                return copy;
            }
            cache_clear_slot(&g_tasfa_cache[probe]);
            break;
        }
    }
    pthread_mutex_unlock(&g_tasfa_cache_mtx);
    cJSON *meta = load_download_session(session_id);
    if (meta) {
        pthread_mutex_lock(&g_tasfa_cache_mtx);
        /* Re-check after loading: another thread may have inserted this entry
           while we were reading from disk. */
        bool already_cached = false;
        for (int i = 0; i < TASFA_CACHE_SLOTS; i++) {
            int probe = (idx + i) % TASFA_CACHE_SLOTS;
            if (!g_tasfa_cache[probe].valid) break;
            if (g_tasfa_cache[probe].type == 3 && strcmp(g_tasfa_cache[probe].key, session_id) == 0) {
                if (time(NULL) <= g_tasfa_cache[probe].expires) {
                    cJSON_Delete(meta);
                    meta = cJSON_Duplicate(g_tasfa_cache[probe].data.json, 1);
                    already_cached = true;
                } else {
                    cache_clear_slot(&g_tasfa_cache[probe]);
                }
                break;
            }
        }
        if (!already_cached) {
            for (int i = 0; i < TASFA_CACHE_SLOTS; i++) {
                int probe = (idx + i) % TASFA_CACHE_SLOTS;
                if (!g_tasfa_cache[probe].valid) {
                    g_tasfa_cache[probe].valid = 1;
                    g_tasfa_cache[probe].type = 3;
                    strncpy(g_tasfa_cache[probe].key, session_id, sizeof(g_tasfa_cache[probe].key)-1);
                    g_tasfa_cache[probe].key[sizeof(g_tasfa_cache[probe].key)-1] = '\0';
                    g_tasfa_cache[probe].data.json = cJSON_Duplicate(meta, 1);
                    g_tasfa_cache[probe].expires = time(NULL) + TASFA_DOWNLOAD_TTL;
                    break;
                }
            }
        }
        pthread_mutex_unlock(&g_tasfa_cache_mtx);
    }
    return meta;
}

static void finalize_cache_sweep_locked(void) {
    time_t now = time(NULL);
    for (int i = 0; i < TASFA_FINALIZE_CACHE_SLOTS; i++) {
        if (g_finalize_slots[i].upload_id[0] && g_finalize_slots[i].expires > 0 && g_finalize_slots[i].expires < now) {
            free(g_finalize_slots[i].body);
            memset(&g_finalize_slots[i], 0, sizeof(g_finalize_slots[i]));
        }
    }
}

static finalize_slot_t *finalize_cache_find_locked(const char *upload_id) {
    if (!upload_id || !upload_id[0]) return NULL;
    for (int i = 0; i < TASFA_FINALIZE_CACHE_SLOTS; i++) {
        if (strcmp(g_finalize_slots[i].upload_id, upload_id) == 0) return &g_finalize_slots[i];
    }
    return NULL;
}

bool finalize_cache_get(const char *upload_id, const char *upload_token, int *status_code, char **body, bool *active) {
    bool found = false;
    pthread_mutex_lock(&g_finalize_mtx);
    finalize_cache_sweep_locked();
    finalize_slot_t *slot = finalize_cache_find_locked(upload_id);
    if (slot && secure_str_eq(upload_token, slot->upload_token)) {
        if (status_code) *status_code = slot->status_code;
        if (body) *body = slot->body ? strdup(slot->body) : strdup("{}");
        if (active) *active = slot->active && !slot->done;
        found = true;
    }
    pthread_mutex_unlock(&g_finalize_mtx);
    return found;
}

bool finalize_cache_mark_started(const char *upload_id, const char *upload_token) {
    bool ok = false;
    pthread_mutex_lock(&g_finalize_mtx);
    finalize_cache_sweep_locked();
    finalize_slot_t *slot = finalize_cache_find_locked(upload_id);
    if (slot) {
        /* Already finalizing this upload; do not overwrite and do not schedule
           a duplicate worker. The caller should re-check the cached status. */
        pthread_mutex_unlock(&g_finalize_mtx);
        return false;
    }
    for (int i = 0; i < TASFA_FINALIZE_CACHE_SLOTS; i++) {
        if (!g_finalize_slots[i].upload_id[0]) {
            slot = &g_finalize_slots[i];
            break;
        }
    }
    if (slot) {
        memset(slot, 0, sizeof(*slot));
        snprintf(slot->upload_id, sizeof(slot->upload_id), "%s", upload_id);
        snprintf(slot->upload_token, sizeof(slot->upload_token), "%s", upload_token);
        slot->active = true;
        slot->done = false;
        slot->status_code = 202;
        slot->body = strdup("{\"ok\":false,\"processing\":true}");
        slot->expires = time(NULL) + 3600;
        ok = true;
    }
    pthread_mutex_unlock(&g_finalize_mtx);
    return ok;
}

void finalize_cache_mark_done(const char *upload_id, int status_code, const char *body) {
    pthread_mutex_lock(&g_finalize_mtx);
    finalize_slot_t *slot = finalize_cache_find_locked(upload_id);
    if (slot) {
        free(slot->body);
        slot->body = strdup(body ? body : "{}");
        slot->status_code = status_code > 0 ? status_code : 500;
        slot->active = false;
        slot->done = true;
        slot->expires = time(NULL) + 3600;
    }
    pthread_mutex_unlock(&g_finalize_mtx);
}

void finalize_cache_update_status(const char *upload_id, const char *msg) {
    pthread_mutex_lock(&g_finalize_mtx);
    finalize_slot_t *slot = finalize_cache_find_locked(upload_id);
    if (slot) {
        free(slot->body);
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddBoolToObject(obj, "ok", false);
        cJSON_AddBoolToObject(obj, "processing", true);
        cJSON_AddStringToObject(obj, "status", msg ? msg : "");
        slot->body = cJSON_PrintUnformatted(obj);
        cJSON_Delete(obj);
    }
    pthread_mutex_unlock(&g_finalize_mtx);
}

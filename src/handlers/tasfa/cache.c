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
    if (slot->type == 3 && slot->data.json_str) {
        free(slot->data.json_str);
        slot->data.json_str = NULL;
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
                cJSON *copy = cJSON_Parse(g_tasfa_cache[probe].data.json_str);
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
                    meta = cJSON_Parse(g_tasfa_cache[probe].data.json_str);
                    already_cached = true;
                } else {
                    cache_clear_slot(&g_tasfa_cache[probe]);
                }
                break;
            }
        }
        if (!already_cached) {
            char *serialized = cJSON_PrintUnformatted(meta);
            if (serialized) cwist_gc_scope_disown(serialized);
            for (int i = 0; i < TASFA_CACHE_SLOTS; i++) {
                int probe = (idx + i) % TASFA_CACHE_SLOTS;
                if (!g_tasfa_cache[probe].valid && serialized) {
                    g_tasfa_cache[probe].valid = 1;
                    g_tasfa_cache[probe].type = 3;
                    strncpy(g_tasfa_cache[probe].key, session_id, sizeof(g_tasfa_cache[probe].key)-1);
                    g_tasfa_cache[probe].key[sizeof(g_tasfa_cache[probe].key)-1] = '\0';
                    g_tasfa_cache[probe].data.json_str = serialized;
                    g_tasfa_cache[probe].expires = time(NULL) + TASFA_DOWNLOAD_TTL;
                    serialized = NULL;
                    break;
                }
            }
            free(serialized); /* no free slot: drop the entry */
        }
        pthread_mutex_unlock(&g_tasfa_cache_mtx);
    }
    return meta;
}

#define TASFA_FINALIZE_DIR "data/tasfa/finalize"

static void finalize_disk_path(char *out, size_t out_len, const char *upload_id) {
    snprintf(out, out_len, "%s/%s.json", TASFA_FINALIZE_DIR, upload_id);
}

static bool finalize_disk_write(const char *upload_id, const char *upload_token, int status_code,
                                const char *body, bool active) {
    if (!is_safe_segment(upload_id) || !upload_token) return false;
    cJSON *obj = cJSON_CreateObject();
    if (!obj) return false;
    cJSON_AddStringToObject(obj, "upload_token", upload_token);
    cJSON_AddNumberToObject(obj, "status_code", status_code);
    cJSON_AddStringToObject(obj, "body", body ? body : "{}");
    cJSON_AddBoolToObject(obj, "active", active);
    cJSON_AddNumberToObject(obj, "expires", (double)(time(NULL) + 3600));
    char *serialized = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (!serialized) return false;

    char path[PATH_MAX], temp_path[PATH_MAX];
    finalize_disk_path(path, sizeof(path), upload_id);
    snprintf(temp_path, sizeof(temp_path), "%s.%ld.tmp", path, (long)getpid());
    bool ok = file_write(temp_path, serialized, strlen(serialized)) && rename(temp_path, path) == 0;
    if (!ok) unlink(temp_path);
    cwist_free(serialized);
    return ok;
}

static bool finalize_disk_get(const char *upload_id, const char *upload_token, int *status_code,
                              char **body, bool *active) {
    if (!is_safe_segment(upload_id) || !upload_token) return false;
    char path[PATH_MAX];
    finalize_disk_path(path, sizeof(path), upload_id);
    size_t len = 0;
    char *serialized = file_read(path, &len);
    if (!serialized) return false;
    cJSON *obj = cJSON_Parse(serialized);
    cwist_free(serialized);
    if (!obj) return false;

    const char *expected_token = json_string(obj, "upload_token", "");
    const char *stored_body = json_string(obj, "body", "{}");
    long long expires = (long long)cJSON_GetNumberValue(cJSON_GetObjectItem(obj, "expires"));
    bool valid = expires >= (long long)time(NULL) && secure_str_eq(upload_token, expected_token);
    if (valid) {
        if (status_code) *status_code = json_int(obj, "status_code", 500);
        if (body) *body = strdup(stored_body);
        cJSON *is_active = cJSON_GetObjectItem(obj, "active");
        if (active) *active = cJSON_IsTrue(is_active);
    }
    cJSON_Delete(obj);
    return valid;
}

static void finalize_disk_sweep(void) {
    static time_t last_sweep;
    time_t now = time(NULL);
    if (last_sweep && now - last_sweep < 60) return;
    last_sweep = now;
    DIR *dir = opendir(TASFA_FINALIZE_DIR);
    if (!dir) return;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        const char *suffix = strstr(ent->d_name, ".json");
        if (!suffix || suffix == ent->d_name || strcmp(suffix, ".json") != 0) continue;
        char id[256];
        size_t id_len = (size_t)(suffix - ent->d_name);
        if (id_len >= sizeof(id)) continue;
        memcpy(id, ent->d_name, id_len);
        id[id_len] = '\0';
        if (!is_safe_segment(id)) continue;
        char path[PATH_MAX];
        finalize_disk_path(path, sizeof(path), id);
        struct stat st;
        if (stat(path, &st) == 0 && st.st_mtime + 3600 < now) unlink(path);
    }
    closedir(dir);
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
    finalize_disk_sweep();
    pthread_mutex_unlock(&g_finalize_mtx);
    return found || finalize_disk_get(upload_id, upload_token, status_code, body, active);
}

bool finalize_cache_mark_started(const char *upload_id, const char *upload_token) {
    int disk_status = 0;
    bool disk_active = false;
    if (finalize_disk_get(upload_id, upload_token, &disk_status, NULL, &disk_active)) return false;

    bool ok = false;
    pthread_mutex_lock(&g_finalize_mtx);
    finalize_cache_sweep_locked();
    finalize_slot_t *slot = finalize_cache_find_locked(upload_id);
    if (slot) {
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
        ok = finalize_disk_write(upload_id, upload_token, 202, slot->body, true);
        if (!ok) {
            free(slot->body);
            memset(slot, 0, sizeof(*slot));
        }
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
        finalize_disk_write(upload_id, slot->upload_token, slot->status_code, slot->body, false);
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
        if (slot->body) cwist_gc_scope_disown(slot->body);
        cJSON_Delete(obj);
        if (slot->body) finalize_disk_write(upload_id, slot->upload_token, 202, slot->body, true);
    }
    pthread_mutex_unlock(&g_finalize_mtx);
}

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "tasfa_internal.h"

queue_slot_t g_q_uploads[TASFA_MAX_CONCURRENT_UPLOADS];
queue_slot_t g_q_downloads[TASFA_MAX_CONCURRENT_DOWNLOADS];
pthread_mutex_t g_tasfa_queue_mtx = PTHREAD_MUTEX_INITIALIZER;

bool tasfa_queue_try_enter(queue_slot_t *slots, int max_slots, const char *id) {
    pthread_mutex_lock(&g_tasfa_queue_mtx);
    int empty = -1;
    for (int i = 0; i < max_slots; i++) {
        if (slots[i].id[0] == '\0') { empty = i; break; }
    }
    if (empty < 0) { pthread_mutex_unlock(&g_tasfa_queue_mtx); return false; }
    snprintf(slots[empty].id, sizeof(slots[empty].id), "%s", id ? id : "");
    slots[empty].ts = time(NULL);
    pthread_mutex_unlock(&g_tasfa_queue_mtx);
    return true;
}

void tasfa_queue_leave(queue_slot_t *slots, int max_slots, const char *id) {
    if (!id || !id[0]) return;
    pthread_mutex_lock(&g_tasfa_queue_mtx);
    for (int i = 0; i < max_slots; i++) {
        if (strcmp(slots[i].id, id) == 0) {
            slots[i].id[0] = '\0';
            slots[i].ts = 0;
            break;
        }
    }
    pthread_mutex_unlock(&g_tasfa_queue_mtx);
}

void tasfa_queue_touch(queue_slot_t *slots, int max_slots, const char *id) {
    if (!id || !id[0]) return;
    pthread_mutex_lock(&g_tasfa_queue_mtx);
    for (int i = 0; i < max_slots; i++) {
        if (strcmp(slots[i].id, id) == 0) {
            slots[i].ts = time(NULL);
            break;
        }
    }
    pthread_mutex_unlock(&g_tasfa_queue_mtx);
}

void tasfa_queue_sweep(queue_slot_t *slots, int max_slots, int ttl) {
    time_t now = time(NULL);
    pthread_mutex_lock(&g_tasfa_queue_mtx);
    for (int i = 0; i < max_slots; i++) {
        if (slots[i].id[0] && (now - slots[i].ts) > ttl) {
            slots[i].id[0] = '\0';
            slots[i].ts = 0;
        }
    }
    pthread_mutex_unlock(&g_tasfa_queue_mtx);
}

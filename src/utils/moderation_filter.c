#include "moderation_filter.h"
#include "db/db.h"
#include <cwist/core/log.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define KEY_ENABLED "moderation_filter_enabled"
#define KEY_ACTION "moderation_filter_action"
#define KEY_CASE_FOLD "moderation_filter_case_fold"
#define KEY_MATCH_LIMIT "moderation_filter_match_limit"
#define KEY_RULES "moderation_filter_rules"

typedef struct {
    pthread_mutex_t lock;
    unsigned generation;
    bool enabled;
    moderation_filter_action_t action;
    bool case_fold;
    size_t match_limit;
    char rules[AHO_MODERATION_RULES_MAX_BYTES + 1];
} shared_filter_t;

static shared_filter_t fallback = {.lock = PTHREAD_MUTEX_INITIALIZER};
static shared_filter_t *shared = &fallback;
static pthread_rwlock_t matcher_lock = PTHREAD_RWLOCK_INITIALIZER;
static aho_moderation_t *matcher;
static unsigned loaded_generation;
static bool have_generation;

static void publish(const moderation_filter_policy_t *policy) {
    pthread_mutex_lock(&shared->lock);
    shared->enabled = policy->enabled;
    shared->action = policy->action;
    shared->case_fold = policy->case_fold;
    shared->match_limit = policy->match_limit;
    memcpy(shared->rules, policy->rules, sizeof(shared->rules));
    shared->generation++;
    pthread_mutex_unlock(&shared->lock);
}

static moderation_filter_policy_t snapshot(unsigned *generation) {
    moderation_filter_policy_t policy;
    pthread_mutex_lock(&shared->lock);
    policy.enabled = shared->enabled;
    policy.action = shared->action;
    policy.case_fold = shared->case_fold;
    policy.match_limit = shared->match_limit;
    memcpy(policy.rules, shared->rules, sizeof(policy.rules));
    if (generation) *generation = shared->generation;
    pthread_mutex_unlock(&shared->lock);
    return policy;
}

moderation_filter_policy_t moderation_filter_get(void) {
    return snapshot(NULL);
}

static bool valid_policy(const moderation_filter_policy_t *policy) {
    if (!policy || (policy->action != MODERATION_FILTER_PENDING &&
                    policy->action != MODERATION_FILTER_REJECT) ||
        !policy->match_limit || policy->match_limit > MODERATION_FILTER_MATCH_LIMIT_MAX ||
        policy->rules[AHO_MODERATION_RULES_MAX_BYTES]) return false;
    aho_moderation_t *a = aho_moderation_build(policy->rules, policy->case_fold);
    if (!a) return false;
    aho_moderation_destroy(a);
    return true;
}

bool moderation_filter_init(cwist_db *db) {
    void *mem = mmap(NULL, sizeof(*shared), PROT_READ | PROT_WRITE,
                     MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) {
        FLY_LOG_ERROR("Moderation filter: shared memory unavailable; changes apply per process");
    } else {
        shared = mem;
    }
    pthread_mutexattr_t attr;
    bool lock_ready = pthread_mutexattr_init(&attr) == 0;
    if (lock_ready) {
        lock_ready = pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED) == 0 &&
                     pthread_mutex_init(&shared->lock, &attr) == 0;
        pthread_mutexattr_destroy(&attr);
    }
    if (!lock_ready) {
        shared = &fallback;
        pthread_mutex_init(&shared->lock, NULL);
        FLY_LOG_ERROR("Moderation filter: process-shared lock unavailable; changes apply per process");
    }
    moderation_filter_policy_t policy = {
        .match_limit = MODERATION_FILTER_MATCH_LIMIT_DEFAULT
    };
    char value[32];
    if (db_site_setting_get(db, KEY_ENABLED, value, sizeof(value))) policy.enabled = strcmp(value, "1") == 0;
    if (db_site_setting_get(db, KEY_ACTION, value, sizeof(value))) {
        if (strcmp(value, "reject") == 0) policy.action = MODERATION_FILTER_REJECT;
        else if (strcmp(value, "pending") != 0) policy.enabled = false;
    }
    if (db_site_setting_get(db, KEY_CASE_FOLD, value, sizeof(value))) policy.case_fold = strcmp(value, "simple") == 0;
    if (db_site_setting_get(db, KEY_MATCH_LIMIT, value, sizeof(value))) {
        int limit = atoi(value);
        if (limit <= 0 || limit > MODERATION_FILTER_MATCH_LIMIT_MAX) policy.enabled = false;
        else policy.match_limit = (size_t)limit;
    }
    if (!db_site_setting_get(db, KEY_RULES, policy.rules, sizeof(policy.rules))) policy.rules[0] = '\0';
    if (!valid_policy(&policy)) {
        CWIST_LOG_WARN("Moderation filter: invalid stored rules; disabling filter");
        policy.enabled = false;
    }
    publish(&policy);
    CWIST_LOG_INFO("Moderation filter: %s action=%s case_fold=%s match_limit=%zu",
                   policy.enabled ? "enabled" : "disabled",
                   policy.action == MODERATION_FILTER_REJECT ? "reject" : "pending",
                   policy.case_fold ? "simple" : "exact", policy.match_limit);
    return true;
}

bool moderation_filter_set(cwist_db *db, const moderation_filter_policy_t *policy) {
    if (!valid_policy(policy)) return false;
    char limit[16];
    snprintf(limit, sizeof(limit), "%zu", policy->match_limit);
    bool ok = db_site_setting_set(db, KEY_ENABLED, policy->enabled ? "1" : "0") &&
              db_site_setting_set(db, KEY_ACTION, policy->action == MODERATION_FILTER_REJECT ? "reject" : "pending") &&
              db_site_setting_set(db, KEY_CASE_FOLD, policy->case_fold ? "simple" : "exact") &&
              db_site_setting_set(db, KEY_MATCH_LIMIT, limit) &&
              db_site_setting_set(db, KEY_RULES, policy->rules);
    if (!ok) return false;
    publish(policy);
    return true;
}

static void reload_if_needed(const moderation_filter_policy_t *policy, unsigned generation) {
    pthread_rwlock_wrlock(&matcher_lock);
    if (!have_generation || loaded_generation != generation) {
        aho_moderation_t *next = aho_moderation_build(policy->rules, policy->case_fold);
        if (next) {
            aho_moderation_destroy(matcher);
            matcher = next;
            loaded_generation = generation;
            have_generation = true;
        } else {
            aho_moderation_destroy(matcher);
            matcher = NULL;
            loaded_generation = generation;
            have_generation = true;
            CWIST_LOG_WARN("Moderation filter: automaton reload failed; filter is inactive");
        }
    }
    pthread_rwlock_unlock(&matcher_lock);
}

bool moderation_filter_check(const char *content, moderation_filter_action_t *action) {
    unsigned generation;
    moderation_filter_policy_t policy = snapshot(&generation);
    if (!policy.enabled || !content) return false;
    reload_if_needed(&policy, generation);
    bool matched = false;
    pthread_rwlock_rdlock(&matcher_lock);
    if (matcher) matched = aho_moderation_search(matcher, content, policy.match_limit).count > 0;
    pthread_rwlock_unlock(&matcher_lock);
    if (matched && action) *action = policy.action;
    return matched;
}

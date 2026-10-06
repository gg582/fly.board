#include "config/write_policy.h"
#include "db/db.h"
#include <cwist/core/log.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define KEY_POST_SCOPE "write_post_scope"
#define KEY_COMMENT_SCOPE "write_comment_scope"
#define KEY_REQUIRE_BOARD "write_require_board"
#define KEY_SPAM_HONEYPOT "spam_honeypot"
#define KEY_SPAM_RATE_LIMIT "spam_rate_limit"

/* MAP_SHARED before fork (same pattern as the route BDR version counter):
 * the process that handles the dashboard POST updates every worker. */
typedef struct {
    _Atomic int post;
    _Atomic int comment;
    _Atomic bool require_board;
    _Atomic bool spam_honeypot;
    _Atomic int spam_rate_limit;
    _Atomic unsigned generation;
} shared_policy_t;

static shared_policy_t g_fallback;
static shared_policy_t *g_policy = &g_fallback;

const char *write_scope_name(write_scope_t scope) {
    switch (scope) {
    case WRITE_SCOPE_MEMBER: return "member";
    case WRITE_SCOPE_ADMIN: return "admin";
    default: return "guest";
    }
}

bool write_scope_parse(const char *name, write_scope_t *out) {
    if (!name || !out) return false;
    if (strcmp(name, "guest") == 0) *out = WRITE_SCOPE_GUEST;
    else if (strcmp(name, "member") == 0) *out = WRITE_SCOPE_MEMBER;
    else if (strcmp(name, "admin") == 0) *out = WRITE_SCOPE_ADMIN;
    else return false;
    return true;
}

static write_scope_t load_scope(cwist_db *db, const char *key) {
    char value[32];
    write_scope_t scope = WRITE_SCOPE_GUEST;
    if (db_site_setting_get(db, key, value, sizeof(value)) && !write_scope_parse(value, &scope)) {
        CWIST_LOG_WARN("Unknown %s value '%s', falling back to guest", key, value);
    }
    return scope;
}

static void publish(const write_policy_t *policy) {
    atomic_store(&g_policy->post, (int)policy->post);
    atomic_store(&g_policy->comment, (int)policy->comment);
    atomic_store(&g_policy->require_board, policy->require_board);
    atomic_store(&g_policy->spam_honeypot, policy->spam_honeypot);
    atomic_store(&g_policy->spam_rate_limit, policy->spam_rate_limit);
}

bool write_policy_init(cwist_db *db) {
    void *mem = mmap(NULL, sizeof(shared_policy_t), PROT_READ | PROT_WRITE,
                     MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) {
        FLY_LOG_ERROR("Write policy: shared memory unavailable; changes apply per process");
    } else {
        g_policy = mem;
    }
    char value[8];
    write_policy_t policy = {
        .post = load_scope(db, KEY_POST_SCOPE),
        .comment = load_scope(db, KEY_COMMENT_SCOPE),
        .require_board = db_site_setting_get(db, KEY_REQUIRE_BOARD, value, sizeof(value)) &&
                         strcmp(value, "1") == 0,
        .spam_honeypot = db_site_setting_get(db, KEY_SPAM_HONEYPOT, value, sizeof(value)) &&
                         strcmp(value, "1") == 0,
    };
    if (db_site_setting_get(db, KEY_SPAM_RATE_LIMIT, value, sizeof(value))) {
        int limit = atoi(value);
        policy.spam_rate_limit = limit < 0 ? 0 : limit > SPAM_RATE_LIMIT_MAX ? SPAM_RATE_LIMIT_MAX : limit;
    }
    publish(&policy);
    CWIST_LOG_INFO("Write policy: posts=%s comments=%s require_board=%s spam_honeypot=%s spam_rate_limit=%d",
                   write_scope_name(policy.post), write_scope_name(policy.comment),
                   policy.require_board ? "yes" : "no", policy.spam_honeypot ? "yes" : "no",
                   policy.spam_rate_limit);
    return true;
}

write_policy_t write_policy_get(void) {
    write_policy_t policy = {
        .post = (write_scope_t)atomic_load(&g_policy->post),
        .comment = (write_scope_t)atomic_load(&g_policy->comment),
        .require_board = atomic_load(&g_policy->require_board),
        .spam_honeypot = atomic_load(&g_policy->spam_honeypot),
        .spam_rate_limit = atomic_load(&g_policy->spam_rate_limit),
    };
    return policy;
}

unsigned write_policy_generation(void) {
    return atomic_load(&g_policy->generation);
}

bool write_policy_set(cwist_db *db, const write_policy_t *policy) {
    if (!policy) return false;
    bool ok = db_site_setting_set(db, KEY_POST_SCOPE, write_scope_name(policy->post)) &&
              db_site_setting_set(db, KEY_COMMENT_SCOPE, write_scope_name(policy->comment)) &&
              db_site_setting_set(db, KEY_REQUIRE_BOARD, policy->require_board ? "1" : "0");
    char limit[16];
    snprintf(limit, sizeof(limit), "%d", policy->spam_rate_limit);
    ok = ok && db_site_setting_set(db, KEY_SPAM_HONEYPOT, policy->spam_honeypot ? "1" : "0") &&
         db_site_setting_set(db, KEY_SPAM_RATE_LIMIT, limit);
    if (!ok) return false;
    publish(policy);
    atomic_fetch_add(&g_policy->generation, 1);
    return true;
}

bool write_scope_allows(write_scope_t scope, const char *role) {
    bool logged_in = role && role[0];
    switch (scope) {
    case WRITE_SCOPE_MEMBER: return logged_in;
    case WRITE_SCOPE_ADMIN: return logged_in && strcmp(role, "admin") == 0;
    default: return true;
    }
}

bool write_policy_can_post(const char *role) {
    return write_scope_allows(write_policy_get().post, role);
}

bool write_policy_can_comment(const char *role) {
    return write_scope_allows(write_policy_get().comment, role);
}

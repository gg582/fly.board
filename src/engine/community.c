#define _POSIX_C_SOURCE 200809L
#include "community.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>

community_config_t community = {0};

static bool parse_bool(const char *value, bool *out) {
    if (strcasecmp(value, "true") == 0 || strcmp(value, "1") == 0) {
        *out = true;
        return true;
    }
    if (strcasecmp(value, "false") == 0 || strcmp(value, "0") == 0) {
        *out = false;
        return true;
    }
    return false;
}

bool community_config_load(const char *path) {
    memset(&community, 0, sizeof(community));
    if (!path) return false;
    FILE *f = fopen(path, "r");
    if (!f) return true;

    char line[256];
    bool ok = true;
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0' || *p == '\r' || *p == '\n' || *p == '#') continue;
        char *newline = strpbrk(p, "\r\n");
        if (newline) *newline = '\0';
        char *eq = strchr(p, '=');
        if (!eq) {
            ok = false;
            continue;
        }
        *eq = '\0';
        char *key = p;
        char *value = eq + 1;
        char *end = key + strlen(key);
        while (end > key && (end[-1] == ' ' || end[-1] == '\t')) *--end = '\0';
        while (*value == ' ' || *value == '\t') value++;
        end = value + strlen(value);
        while (end > value && (end[-1] == ' ' || end[-1] == '\t')) *--end = '\0';
        bool *target = NULL;
        if (strcmp(key, "roles") == 0) target = &community.roles;
        else if (strcmp(key, "new_user_limits") == 0) target = &community.new_user_limits;
        else if (strcmp(key, "trust") == 0) target = &community.trust;
        else if (strcmp(key, "spam_detection") == 0) target = &community.spam_detection;
        else if (strcmp(key, "approval_queue") == 0) target = &community.approval_queue;
        if (target && !parse_bool(value, target)) ok = false;
    }
    fclose(f);
    if (!ok) memset(&community, 0, sizeof(community));
    return ok;
}

bool community_roles_enabled(void) {
    return community.roles;
}

bool community_role_can_moderate(const char *role) {
    return role && (strcmp(role, "admin") == 0 ||
                    (community.roles && strcmp(role, "moderator") == 0));
}

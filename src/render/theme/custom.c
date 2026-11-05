#define _POSIX_C_SOURCE 200809L
#include "../theme.h"
#include <cwist/core/log.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define THEME_MAX_BYTES (512 * 1024)

static cJSON *g_custom_theme;
static pthread_rwlock_t g_custom_lock = PTHREAD_RWLOCK_INITIALIZER;

static bool valid_css_text(const char *value, bool selector) {
    if (!value || !value[0] || strlen(value) > 2048) return false;
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        if (*p == '{' || *p == '}' || *p == '<' || (!selector && *p == '>') || *p == '\0') return false;
        if (selector && *p == '@' && strncmp(value, "@keyframes ", 11) != 0) return false;
        if (*p < 32 && *p != '\t') return false;
    }
    return true;
}

static bool valid_declarations(cJSON *decls) {
    if (!cJSON_IsObject(decls)) return false;
    for (cJSON *item = decls->child; item; item = item->next) {
        if (!item->string || !item->string[0] || !cJSON_IsString(item) ||
            !valid_css_text(item->valuestring, false)) return false;
        for (const unsigned char *p = (const unsigned char *)item->string; *p; p++) {
            if (!(isalnum(*p) || *p == '-' || *p == '%')) return false;
        }
    }
    return true;
}

static bool validate_theme(cJSON *root, char *error, size_t error_size) {
    cJSON *vars = cJSON_GetObjectItemCaseSensitive(root, "vars");
    cJSON *rules = cJSON_GetObjectItemCaseSensitive(root, "rules");
    if (!cJSON_IsObject(root) || !valid_declarations(vars) || !cJSON_IsArray(rules)) {
        snprintf(error, error_size, "Expected an object with vars and rules");
        return false;
    }
    cJSON *mode = cJSON_GetObjectItemCaseSensitive(root, "mode");
    if (mode && (!cJSON_IsString(mode) ||
                 (strcmp(mode->valuestring, "light") != 0 && strcmp(mode->valuestring, "dark") != 0))) {
        snprintf(error, error_size, "mode must be light or dark");
        return false;
    }
    int count = 0;
    cJSON *rule = NULL;
    cJSON_ArrayForEach(rule, rules) {
        cJSON *selector = cJSON_GetObjectItemCaseSensitive(rule, "sel");
        cJSON *decls = cJSON_GetObjectItemCaseSensitive(rule, "decls");
        if (!cJSON_IsObject(rule) || !cJSON_IsString(selector) ||
            !valid_css_text(selector->valuestring, true) || !valid_declarations(decls)) {
            snprintf(error, error_size, "Invalid selector or declaration in rule %d: %.80s", count + 1,
                     cJSON_IsString(selector) ? selector->valuestring : "(missing)");
            return false;
        }
        if (++count > 4096) {
            snprintf(error, error_size, "Too many rules");
            return false;
        }
    }
    return true;
}

static cJSON *parse_theme(const char *text, size_t length, char *error, size_t error_size) {
    if (!text || length == 0 || length > THEME_MAX_BYTES) {
        snprintf(error, error_size, "Theme must be between 1 byte and 512 KiB");
        return NULL;
    }
    cJSON *root = cJSON_ParseWithLength(text, length);
    if (!root) {
        snprintf(error, error_size, "Invalid JSON");
        return NULL;
    }
    if (!validate_theme(root, error, error_size)) {
        cJSON_Delete(root);
        return NULL;
    }
    cJSON *name = cJSON_GetObjectItemCaseSensitive(root, "name");
    if (cJSON_IsString(name)) cJSON_SetValuestring(name, "custom");
    else if (name) cJSON_ReplaceItemInObject(root, "name", cJSON_CreateString("custom"));
    else cJSON_AddStringToObject(root, "name", "custom");
    return root;
}

bool theme_custom_load(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) return errno == ENOENT;
    char *text = malloc(THEME_MAX_BYTES + 1);
    if (!text) { fclose(file); return false; }
    size_t length = fread(text, 1, THEME_MAX_BYTES + 1, file);
    bool read_ok = !ferror(file) && feof(file);
    fclose(file);
    if (!read_ok) { free(text); return false; }
    char error[128] = {0};
    cJSON *root = parse_theme(text, length, error, sizeof(error));
    free(text);
    if (!root) {
        CWIST_LOG_WARN("Ignoring invalid custom theme: %s", error);
        return false;
    }
    pthread_rwlock_wrlock(&g_custom_lock);
    cJSON_Delete(g_custom_theme);
    g_custom_theme = root;
    pthread_rwlock_unlock(&g_custom_lock);
    return true;
}

bool theme_custom_validate(const char *text, size_t length, char *error, size_t error_size) {
    cJSON *root = parse_theme(text, length, error, error_size);
    if (!root) return false;
    cJSON_Delete(root);
    return true;
}

bool theme_custom_save(const char *path, const char *text, size_t length, char *error, size_t error_size) {
    cJSON *root = parse_theme(text, length, error, error_size);
    if (!root) return false;
    char *printed = cJSON_PrintUnformatted(root);
    if (!printed) { cJSON_Delete(root); return false; }
    char temporary[512];
    int length_needed = snprintf(temporary, sizeof(temporary), "%s.tmp.%ld", path, (long)getpid());
    if (length_needed < 0 || (size_t)length_needed >= sizeof(temporary)) {
        free(printed); cJSON_Delete(root); return false;
    }
    int fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) { free(printed); cJSON_Delete(root); return false; }
    size_t bytes = strlen(printed);
    bool ok = write(fd, printed, bytes) == (ssize_t)bytes && fsync(fd) == 0 && close(fd) == 0;
    if (!ok || rename(temporary, path) != 0) {
        if (fd >= 0) close(fd);
        unlink(temporary);
        ok = false;
    }
    free(printed);
    if (!ok) { cJSON_Delete(root); snprintf(error, error_size, "Could not save theme"); return false; }
    pthread_rwlock_wrlock(&g_custom_lock);
    cJSON_Delete(g_custom_theme);
    g_custom_theme = root;
    pthread_rwlock_unlock(&g_custom_lock);
    return true;
}

cJSON *theme_custom_object(void) {
    pthread_rwlock_rdlock(&g_custom_lock);
    cJSON *copy = g_custom_theme ? cJSON_Duplicate(g_custom_theme, true) : NULL;
    pthread_rwlock_unlock(&g_custom_lock);
    return copy;
}

bool theme_custom_available(void) {
    pthread_rwlock_rdlock(&g_custom_lock);
    bool available = g_custom_theme != NULL;
    pthread_rwlock_unlock(&g_custom_lock);
    return available;
}

bool theme_custom_is_dark(void) {
    pthread_rwlock_rdlock(&g_custom_lock);
    cJSON *mode = g_custom_theme ? cJSON_GetObjectItemCaseSensitive(g_custom_theme, "mode") : NULL;
    bool dark = cJSON_IsString(mode) && strcmp(mode->valuestring, "dark") == 0;
    pthread_rwlock_unlock(&g_custom_lock);
    return dark;
}

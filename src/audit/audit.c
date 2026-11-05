#define _POSIX_C_SOURCE 200809L
#include "audit.h"
#include "crypto/fly_crypto.h"
#include <cjson/cJSON.h>
#include <cwist/core/log.h>
#include <cwist/core/mem/alloc.h>
#include <openssl/sha.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#define AUDIT_HASH_HEX_LEN 64
#define AUDIT_LINE_MAX 65536

static bool g_enabled;
static bool g_sign;
static char g_path[4096];

static bool setting_true(const char *value) {
    return strcmp(value, "1") == 0 || strcmp(value, "true") == 0 || strcmp(value, "yes") == 0;
}

static void set_error(char *error, size_t size, const char *message) {
    if (error && size) snprintf(error, size, "%s", message);
}

static const char *json_string(cJSON *object, const char *key) {
    cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(value) ? value->valuestring : NULL;
}

static uint64_t json_u64(cJSON *object, const char *key) {
    cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsNumber(value) && value->valuedouble >= 0 ? (uint64_t)value->valuedouble : 0;
}

static void hash_hex(const char *message, char out[AUDIT_HASH_HEX_LEN + 1]) {
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256((const unsigned char *)message, strlen(message), digest);
    for (size_t i = 0; i < sizeof(digest); i++) snprintf(out + i * 2, 3, "%02x", digest[i]);
}

static char *payload(cJSON *entry) {
    cJSON *copy = cJSON_Duplicate(entry, true);
    if (!copy) return NULL;
    cJSON_DeleteItemFromObjectCaseSensitive(copy, "hash");
    cJSON_DeleteItemFromObjectCaseSensitive(copy, "signature");
    char *printed = cJSON_PrintUnformatted(copy);
    cJSON_Delete(copy);
    return printed;
}

static bool entry_valid(cJSON *entry, uint64_t expected_seq, const char *expected_previous,
                        bool verify_signatures, char *computed_hash) {
    if (!cJSON_IsObject(entry) || json_u64(entry, "version") != 1 ||
        json_u64(entry, "seq") != expected_seq) return false;
    const char *previous = json_string(entry, "previous_hash");
    const char *hash = json_string(entry, "hash");
    const char *signature = json_string(entry, "signature");
    if (!previous || strcmp(previous, expected_previous) != 0 || !hash || strlen(hash) != AUDIT_HASH_HEX_LEN ||
        !json_u64(entry, "timestamp") || !json_string(entry, "action") || !json_string(entry, "target") ||
        !json_string(entry, "outcome") || !json_string(entry, "detail") ||
        !cJSON_GetObjectItemCaseSensitive(entry, "actor_uid")) return false;
    char *message = payload(entry);
    if (!message) return false;
    hash_hex(message, computed_hash);
    cwist_free(message);
    if (strcmp(hash, computed_hash) != 0) return false;
    if (signature && signature[0]) {
        if (!verify_signatures || !fly_crypto_verify_audit((const uint8_t *)hash, strlen(hash), signature)) return false;
    }
    return true;
}

bool audit_verify(const char *path, uint64_t *entries, char *error, size_t error_size) {
    if (entries) *entries = 0;
    if (!path || !path[0]) {
        set_error(error, error_size, "audit log path is empty");
        return false;
    }
    FILE *file = fopen(path, "r");
    if (!file) {
        if (errno == ENOENT) return true;
        set_error(error, error_size, "cannot open audit log");
        return false;
    }
    char *line = NULL;
    size_t capacity = 0;
    ssize_t length;
    uint64_t expected_seq = 1;
    char previous[AUDIT_HASH_HEX_LEN + 1] = {0};
    bool ok = true;
    while ((length = getline(&line, &capacity, file)) >= 0) {
        if (length == 0 || length > AUDIT_LINE_MAX || line[length - 1] != '\n') {
            set_error(error, error_size, "unterminated or oversized audit entry");
            ok = false;
            break;
        }
        line[--length] = '\0';
        cJSON *entry = cJSON_ParseWithLength(line, (size_t)length);
        char computed[AUDIT_HASH_HEX_LEN + 1] = {0};
        if (!entry || !entry_valid(entry, expected_seq, previous, true, computed)) {
            if (error && error_size) snprintf(error, error_size, "audit chain or signature mismatch at entry %llu",
                                              (unsigned long long)expected_seq);
            cJSON_Delete(entry);
            ok = false;
            break;
        }
        cJSON_Delete(entry);
        snprintf(previous, sizeof(previous), "%s", computed);
        expected_seq++;
    }
    if (ok && ferror(file)) {
        set_error(error, error_size, "cannot read audit log");
        ok = false;
    }
    free(line);
    fclose(file);
    if (ok && entries) *entries = expected_seq - 1;
    return ok;
}

bool audit_init(const char *settings_path) {
    g_enabled = false;
    g_sign = false;
    snprintf(g_path, sizeof(g_path), "data/audit.log");
    FILE *file = fopen(settings_path, "r");
    if (!file) return errno == ENOENT;
    char line[4608];
    while (fgets(line, sizeof(line), file)) {
        line[strcspn(line, "\r\n")] = '\0';
        char *equals = strchr(line, '=');
        if (!equals) continue;
        *equals = '\0';
        if (strcmp(line, "enabled") == 0) g_enabled = setting_true(equals + 1);
        else if (strcmp(line, "sign") == 0) g_sign = setting_true(equals + 1);
        else if (strcmp(line, "path") == 0 && equals[1]) snprintf(g_path, sizeof(g_path), "%s", equals + 1);
    }
    fclose(file);
    if (!g_enabled) return true;
    char error[256] = {0};
    uint64_t entries = 0;
    if (!audit_verify(g_path, &entries, error, sizeof(error))) {
        CWIST_LOG_ERROR("Audit log verification failed: %s", error);
        g_enabled = false;
        return false;
    }
    CWIST_LOG_INFO("Audit log enabled: path=%s entries=%llu ML-DSA=%s", g_path,
                   (unsigned long long)entries, g_sign ? "on" : "off");
    return true;
}

bool audit_enabled(void) {
    return g_enabled;
}

static bool last_entry(int fd, uint64_t *seq, char previous[AUDIT_HASH_HEX_LEN + 1]) {
    FILE *file = fdopen(dup(fd), "r");
    if (!file) return false;
    char *line = NULL;
    size_t capacity = 0;
    ssize_t length;
    *seq = 1;
    previous[0] = '\0';
    bool ok = true;
    while ((length = getline(&line, &capacity, file)) >= 0) {
        if (length == 0 || length > AUDIT_LINE_MAX || line[length - 1] != '\n') {
            ok = false;
            break;
        }
        line[--length] = '\0';
        cJSON *entry = cJSON_ParseWithLength(line, (size_t)length);
        char computed[AUDIT_HASH_HEX_LEN + 1] = {0};
        if (!entry || !entry_valid(entry, *seq, previous, true, computed)) {
            cJSON_Delete(entry);
            ok = false;
            break;
        }
        cJSON_Delete(entry);
        snprintf(previous, AUDIT_HASH_HEX_LEN + 1, "%s", computed);
        (*seq)++;
    }
    if (ferror(file)) ok = false;
    free(line);
    fclose(file);
    return ok;
}

bool audit_record(int actor_uid, const char *action, const char *target,
                  const char *outcome, const char *detail) {
    if (!g_enabled) return true;
    if (!action || !action[0] || !target || !outcome) return false;
    int fd = open(g_path, O_RDWR | O_APPEND | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) {
        CWIST_LOG_ERROR("Audit append failed: cannot open %s", g_path);
        return false;
    }
    bool ok = flock(fd, LOCK_EX) == 0;
    uint64_t seq = 1;
    char previous[AUDIT_HASH_HEX_LEN + 1] = {0};
    cJSON *entry = NULL;
    char *message = NULL;
    char *printed = NULL;
    char *signature = NULL;
    char hash[AUDIT_HASH_HEX_LEN + 1];
    if (ok) ok = last_entry(fd, &seq, previous);
    if (ok) {
        entry = cJSON_CreateObject();
        ok = entry != NULL;
    }
    if (ok) {
        cJSON_AddNumberToObject(entry, "version", 1);
        cJSON_AddNumberToObject(entry, "seq", (double)seq);
        cJSON_AddNumberToObject(entry, "timestamp", (double)time(NULL));
        cJSON_AddNumberToObject(entry, "actor_uid", actor_uid);
        cJSON_AddStringToObject(entry, "action", action);
        cJSON_AddStringToObject(entry, "target", target);
        cJSON_AddStringToObject(entry, "outcome", outcome);
        cJSON_AddStringToObject(entry, "detail", detail ? detail : "");
        cJSON_AddStringToObject(entry, "previous_hash", previous);
        message = payload(entry);
        ok = message != NULL;
    }
    if (ok) {
        hash_hex(message, hash);
        cJSON_AddStringToObject(entry, "hash", hash);
        if (g_sign) {
            ok = fly_crypto_sign_audit((const uint8_t *)hash, strlen(hash), &signature);
            if (ok) cJSON_AddStringToObject(entry, "signature", signature);
        }
    }
    if (ok) {
        printed = cJSON_PrintUnformatted(entry);
        ok = printed && strlen(printed) + 1 <= AUDIT_LINE_MAX;
    }
    if (ok) {
        size_t length = strlen(printed);
        struct iovec parts[] = {{.iov_base = printed, .iov_len = length}, {.iov_base = "\n", .iov_len = 1}};
        ssize_t written = writev(fd, parts, 2);
        ok = written == (ssize_t)(length + 1) && fsync(fd) == 0;
    }
    if (!ok) CWIST_LOG_ERROR("Audit append failed: action=%s target=%s", action, target);
    if (signature) cwist_free(signature);
    if (printed) cwist_free(printed);
    if (message) cwist_free(message);
    cJSON_Delete(entry);
    flock(fd, LOCK_UN);
    close(fd);
    return ok;
}

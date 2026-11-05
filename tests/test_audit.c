#define _POSIX_C_SOURCE 200809L
#include "audit/audit.h"
#include "crypto/fly_crypto.h"
#include <cjson/cJSON.h>
#include <cwist/core/mem/gc.h>
#include <assert.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void write_text(const char *path, const char *text) {
    FILE *file = fopen(path, "w");
    assert(file);
    assert(fputs(text, file) >= 0);
    assert(fclose(file) == 0);
}

static char *read_text(const char *path) {
    FILE *file = fopen(path, "r");
    assert(file);
    assert(fseek(file, 0, SEEK_END) == 0);
    long size = ftell(file);
    assert(size >= 0);
    rewind(file);
    char *text = malloc((size_t)size + 1);
    assert(text);
    assert(fread(text, 1, (size_t)size, file) == (size_t)size);
    text[size] = '\0';
    fclose(file);
    return text;
}

static void copy_file(const char *source, const char *destination) {
    char *text = read_text(source);
    write_text(destination, text);
    free(text);
}

static void assert_valid(const char *path, uint64_t expected) {
    uint64_t entries = 0;
    char error[256] = {0};
    bool ok = audit_verify(path, &entries, error, sizeof(error));
    if (!ok) fprintf(stderr, "unexpected verification error: %s\n", error);
    assert(ok);
    assert(entries == expected);
}

static void assert_invalid(const char *path) {
    uint64_t entries = 0;
    char error[256] = {0};
    assert(!audit_verify(path, &entries, error, sizeof(error)));
    assert(error[0]);
}

int main(void) {
    cwist_full_gc(true);
    char root[] = "/tmp/flyboard-audit-test.XXXXXX";
    assert(mkdtemp(root));
    char settings[PATH_MAX], log[PATH_MAX], seed[PATH_MAX], original[PATH_MAX];
    snprintf(settings, sizeof(settings), "%s/audit.settings", root);
    snprintf(log, sizeof(log), "%s/audit.log", root);
    snprintf(seed, sizeof(seed), "%s/pqc.seed", root);
    snprintf(original, sizeof(original), "%s/original.log", root);
    snprintf(settings, sizeof(settings), "%s/disabled.settings", root);
    char disabled_log[PATH_MAX];
    snprintf(disabled_log, sizeof(disabled_log), "%s/disabled.log", root);
    char config[PATH_MAX + 64];
    snprintf(config, sizeof(config), "enabled=false\npath=%s\n", disabled_log);
    write_text(settings, config);
    assert(audit_init(settings));
    assert(!audit_enabled());
    assert(audit_record(7, "admin.user.role", "user:9", "success", "moderator"));
    assert(access(disabled_log, F_OK) != 0);
    assert_valid(disabled_log, 0);

    assert(fly_crypto_init(seed));
    snprintf(settings, sizeof(settings), "%s/audit.settings", root);
    snprintf(config, sizeof(config), "enabled=true\nsign=true\npath=%s\n", log);
    write_text(settings, config);
    assert(audit_init(settings));
    assert(audit_enabled());
    assert(audit_record(1, "admin.user.role", "user:2", "success", "moderator"));
    assert(audit_record(1, "moderation.decision", "post:3", "approve", "approval queue"));
    assert_valid(log, 2);

    char *text = read_text(log);
    char *second = strstr(text, "\n{");
    assert(second);
    *second = '\0';
    second++;
    char *newline = strchr(second, '\n');
    assert(newline);
    *newline = '\0';
    cJSON *first_entry = cJSON_Parse(text);
    cJSON *second_entry = cJSON_Parse(second);
    assert(first_entry && second_entry);
    cJSON *first_hash = cJSON_GetObjectItem(first_entry, "hash");
    cJSON *previous = cJSON_GetObjectItem(second_entry, "previous_hash");
    cJSON *signature = cJSON_GetObjectItem(second_entry, "signature");
    assert(cJSON_IsString(first_hash) && cJSON_IsString(previous) && cJSON_IsString(signature));
    assert(strcmp(first_hash->valuestring, previous->valuestring) == 0);
    cJSON_Delete(first_entry);
    cJSON_Delete(second_entry);
    free(text);

    text = read_text(log);
    write_text(original, text);
    char *changed = strstr(text, "moderator");
    assert(changed);
    memcpy(changed, "moderatoR", strlen("moderator"));
    write_text(log, text);
    free(text);
    assert_invalid(log);
    assert(!audit_record(1, "admin.user.role", "user:4", "success", "user"));
    copy_file(original, log);
    text = read_text(log);
    char *sig = strstr(text, "\"signature\":\"");
    assert(sig);
    sig += strlen("\"signature\":\"");
    char *sig_end = strchr(sig, '"');
    assert(sig_end && sig_end > sig);
    sig_end[-1] = sig_end[-1] == 'A' ? 'B' : 'A';
    write_text(log, text);
    free(text);
    assert_invalid(log);
    copy_file(original, log);
    assert_valid(log, 2);
    assert(audit_record(1, "content.secret.read", "post:4", "success", "direct post view"));
    assert_valid(log, 3);

    int fd = open(log, O_WRONLY | O_TRUNC);
    assert(fd >= 0);
    close(fd);
    assert_valid(log, 0);
    copy_file(original, log);
    text = read_text(original);
    size_t original_size = strlen(text);
    free(text);
    assert(truncate(log, (off_t)(original_size - 1)) == 0);
    assert_invalid(log);

    fly_crypto_cleanup();
    unlink(settings);
    unlink(log);
    unlink(seed);
    unlink(original);
    char disabled_settings[PATH_MAX];
    snprintf(disabled_settings, sizeof(disabled_settings), "%s/disabled.settings", root);
    unlink(disabled_settings);
    rmdir(root);
    cwist_gc_scope_flush();
    puts("audit chain, signature, tamper detection, and disabled-mode tests passed");
    return 0;
}

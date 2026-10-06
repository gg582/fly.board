#include "backup.h"
#include "config/config.h"
#include "utils/s3_client.h"
#include <cjson/cJSON.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <openssl/mem.h>
#include <signal.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Scheduled backups to an external target.
 *
 * Nothing runs until a target is registered in backup.settings (from the
 * admin dashboard or by hand). Each run writes a signed archive (see
 * backup.c) into data/.backup-out, ships it to the target, prunes the
 * target to the newest `keep` archives this site made, and records the
 * outcome in site_settings for the dashboard. */

#define SETTINGS_PATH "backup.settings"
#define OUT_DIR "data/.backup-out"
#define LOCK_PATH "data/.backup.lock"
#define NAME_PREFIX "flyboard-"
#define DEFAULT_KEEP 14

static void copy_field(char *dst, size_t size, const char *v) {
    snprintf(dst, size, "%s", v ? v : "");
}

bool backup_settings_load(backup_settings_t *out) {
    memset(out, 0, sizeof(*out));
    out->keep = DEFAULT_KEEP;
    out->enabled = true;
    FILE *f = fopen(SETTINGS_PATH, "r");
    if (!f) return false;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (!line[0] || line[0] == '#') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        const char *k = line, *v = eq + 1;
        if (!strcmp(k, "target")) copy_field(out->target, sizeof(out->target), v);
        else if (!strcmp(k, "enabled")) out->enabled = strcmp(v, "false") != 0 && strcmp(v, "0") != 0;
        else if (!strcmp(k, "endpoint")) copy_field(out->endpoint, sizeof(out->endpoint), v);
        else if (!strcmp(k, "region")) copy_field(out->region, sizeof(out->region), v);
        else if (!strcmp(k, "bucket")) copy_field(out->bucket, sizeof(out->bucket), v);
        else if (!strcmp(k, "access_key")) copy_field(out->access_key, sizeof(out->access_key), v);
        else if (!strcmp(k, "secret_key")) copy_field(out->secret_key, sizeof(out->secret_key), v);
        else if (!strcmp(k, "prefix")) copy_field(out->prefix, sizeof(out->prefix), v);
        else if (!strcmp(k, "use_path_style")) out->use_path_style = !strcmp(v, "true") || !strcmp(v, "1");
        else if (!strcmp(k, "path")) copy_field(out->path, sizeof(out->path), v);
        else if (!strcmp(k, "keep")) out->keep = atoi(v) > 0 ? atoi(v) : DEFAULT_KEEP;
        else if (!strcmp(k, "passphrase")) copy_field(out->passphrase, sizeof(out->passphrase), v);
    }
    OPENSSL_cleanse(line, sizeof(line));
    fclose(f);
    return true;
}

static bool clean_value(const char *v) {
    return !strpbrk(v, "\r\n");
}

bool backup_settings_save(const backup_settings_t *in) {
    const char *vals[] = {in->target, in->endpoint, in->region, in->bucket, in->access_key, in->secret_key,
                          in->prefix, in->path, in->passphrase};
    for (size_t i = 0; i < sizeof(vals) / sizeof(vals[0]); i++) {
        if (!clean_value(vals[i])) return false;
    }
    char tmp[] = SETTINGS_PATH ".XXXXXX";
    int fd = mkstemp(tmp);
    if (fd < 0) return false;
    fchmod(fd, 0600);
    FILE *f = fdopen(fd, "w");
    if (!f) { close(fd); unlink(tmp); return false; }
    fprintf(f, "# Scheduled backup target (written by the admin dashboard). Keep this file private.\n");
    fprintf(f, "target=%s\nenabled=%s\n", in->target, in->enabled ? "true" : "false");
    fprintf(f, "endpoint=%s\nregion=%s\nbucket=%s\naccess_key=%s\nsecret_key=%s\nprefix=%s\nuse_path_style=%s\n",
            in->endpoint, in->region, in->bucket, in->access_key, in->secret_key, in->prefix,
            in->use_path_style ? "true" : "false");
    fprintf(f, "path=%s\nkeep=%d\npassphrase=%s\n", in->path, in->keep > 0 ? in->keep : DEFAULT_KEEP, in->passphrase);
    bool ok = fflush(f) == 0 && fsync(fileno(f)) == 0;
    ok = fclose(f) == 0 && ok;
    if (!ok || rename(tmp, SETTINGS_PATH) != 0) {
        unlink(tmp);
        return false;
    }
    return true;
}

bool backup_settings_remove(void) {
    return unlink(SETTINGS_PATH) == 0 || errno == ENOENT;
}

bool backup_settings_ready(const backup_settings_t *s) {
    if (!s->enabled) return false;
    if (!strcmp(s->target, "s3")) return s->endpoint[0] && s->bucket[0] && s->access_key[0] && s->secret_key[0];
    if (!strcmp(s->target, "dir")) return s->path[0] == '/';
    return false;
}

/* ---- Status in site_settings ---- */

static void status_set(sqlite3 *db, const char *key, const char *value) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, "INSERT INTO site_settings (key, value) VALUES (?, ?) ON CONFLICT(key) DO UPDATE SET value=excluded.value", -1, &st, NULL) != SQLITE_OK) return;
    sqlite3_bind_text(st, 1, key, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, value ? value : "", -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

static char *status_get(sqlite3 *db, const char *key) {
    sqlite3_stmt *st = NULL;
    char *out = NULL;
    if (sqlite3_prepare_v2(db, "SELECT value FROM site_settings WHERE key=?", -1, &st, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_text(st, 1, key, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) out = strdup((const char *)sqlite3_column_text(st, 0));
    sqlite3_finalize(st);
    return out;
}

static void record(const char *result, const char *name, long long size, const char *error) {
    sqlite3 *db = NULL;
    if (sqlite3_open("data/blog.db", &db) != SQLITE_OK) { sqlite3_close(db); return; }
    sqlite3_busy_timeout(db, 10000);
    char now[32], sz[32];
    time_t t = time(NULL);
    strftime(now, sizeof(now), "%Y-%m-%d %H:%M:%S", gmtime(&t));
    snprintf(sz, sizeof(sz), "%lld", size);
    status_set(db, "backup_last_at", now);
    status_set(db, "backup_last_result", result);
    status_set(db, "backup_last_error", error ? error : "");
    if (name) {
        status_set(db, "backup_last_name", name);
        status_set(db, "backup_last_size", sz);
    }
    sqlite3_close(db);
}

/* ---- Targets ---- */

static void s3_cfg_from(const backup_settings_t *s, s3_config_t *c) {
    memset(c, 0, sizeof(*c));
    snprintf(c->endpoint, sizeof(c->endpoint), "%s", s->endpoint);
    snprintf(c->region, sizeof(c->region), "%s", s->region);
    snprintf(c->bucket, sizeof(c->bucket), "%s", s->bucket);
    snprintf(c->access_key, sizeof(c->access_key), "%s", s->access_key);
    snprintf(c->secret_key, sizeof(c->secret_key), "%s", s->secret_key);
    c->use_path_style = s->use_path_style;
}

static bool copy_to_dir(const char *src, const char *dir, const char *name) {
    char dst[PATH_MAX], tmp[PATH_MAX + 16];
    snprintf(dst, sizeof(dst), "%s/%s", dir, name);
    snprintf(tmp, sizeof(tmp), "%s.partial", dst);
    FILE *in = fopen(src, "rb");
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    FILE *out = fd >= 0 ? fdopen(fd, "wb") : NULL;
    bool ok = in && out;
    static char buf[256 * 1024];
    size_t n;
    while (ok && (n = fread(buf, 1, sizeof(buf), in)) > 0) ok = fwrite(buf, 1, n, out) == n;
    if (in) fclose(in);
    if (out) {
        ok = fflush(out) == 0 && fsync(fileno(out)) == 0 && ok;
        ok = fclose(out) == 0 && ok;
    } else if (fd >= 0) {
        close(fd);
    }
    if (!ok || rename(tmp, dst) != 0) {
        unlink(tmp);
        return false;
    }
    return true;
}

static int cmp_str(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* Keep the newest `keep` archives of this site in a directory target. */
static void prune_dir(const char *dir, int keep) {
    DIR *d = opendir(dir);
    if (!d) return;
    char *names[4096];
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < 4096) {
        size_t len = strlen(e->d_name);
        if (!strncmp(e->d_name, NAME_PREFIX, strlen(NAME_PREFIX)) && len > 4 && !strcmp(e->d_name + len - 4, ".fbk")) {
            names[n++] = strdup(e->d_name);
        }
    }
    closedir(d);
    qsort(names, (size_t)n, sizeof(char *), cmp_str); /* names sort by time */
    for (int i = 0; i < n; i++) {
        if (i < n - keep) {
            char p[PATH_MAX];
            snprintf(p, sizeof(p), "%s/%s", dir, names[i]);
            if (unlink(p) == 0) fprintf(stderr, "[backup] pruned %s\n", p);
        }
        free(names[i]);
    }
}

/* Keep the newest `keep` uploaded keys; the list lives in site_settings
 * because listing a bucket would need more of the S3 API than we carry. */
static void prune_s3(const s3_config_t *c, const char *new_key, int keep) {
    sqlite3 *db = NULL;
    if (sqlite3_open("data/blog.db", &db) != SQLITE_OK) { sqlite3_close(db); return; }
    sqlite3_busy_timeout(db, 10000);
    char *raw = status_get(db, "backup_s3_keys");
    cJSON *keys = raw ? cJSON_Parse(raw) : NULL;
    free(raw);
    if (!cJSON_IsArray(keys)) { cJSON_Delete(keys); keys = cJSON_CreateArray(); }
    cJSON_AddItemToArray(keys, cJSON_CreateString(new_key));
    while (cJSON_GetArraySize(keys) > keep) {
        cJSON *old = cJSON_GetArrayItem(keys, 0);
        if (cJSON_IsString(old) && s3_delete_object_with(c, old->valuestring)) {
            fprintf(stderr, "[backup] pruned s3://%s/%s\n", c->bucket, old->valuestring);
        }
        cJSON_DeleteItemFromArray(keys, 0);
    }
    char *js = cJSON_PrintUnformatted(keys);
    status_set(db, "backup_s3_keys", js);
    free(js);
    cJSON_Delete(keys);
    sqlite3_close(db);
}

int backup_scheduled_run(void) {
    backup_settings_t s;
    if (!backup_settings_load(&s) || !backup_settings_ready(&s)) {
        fprintf(stderr, "[backup] no backup target registered (backup.settings); nothing to do\n");
        return 0;
    }
    mkdir("data", 0755);
    int lock = open(LOCK_PATH, O_CREAT | O_RDWR, 0600);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) != 0) {
        fprintf(stderr, "[backup] another backup is running\n");
        if (lock >= 0) close(lock);
        return 1;
    }
    mkdir(OUT_DIR, 0700);
    char host[128] = {0}, stamp[32], name[256], local[PATH_MAX];
    gethostname(host, sizeof(host) - 1);
    for (char *h = host; *h; h++) if (!((*h >= 'a' && *h <= 'z') || (*h >= 'A' && *h <= 'Z') || (*h >= '0' && *h <= '9') || *h == '-')) *h = '-';
    time_t t = time(NULL);
    strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%SZ", gmtime(&t));
    snprintf(name, sizeof(name), NAME_PREFIX "%s-%s.fbk", host[0] ? host : "site", stamp);
    snprintf(local, sizeof(local), OUT_DIR "/%s", name);

    int rc = fly_backup_write(local, s.passphrase[0] ? s.passphrase : NULL);
    const char *error = NULL;
    struct stat st;
    long long size = (rc == 0 && stat(local, &st) == 0) ? (long long)st.st_size : 0;
    if (rc != 0) {
        error = "creating the archive failed";
    } else if (!strcmp(s.target, "s3")) {
        s3_config_t c;
        s3_cfg_from(&s, &c);
        char key[768];
        snprintf(key, sizeof(key), "%s%s", s.prefix, name);
        if (s3_upload_file_with(&c, local, key, "application/zstd")) {
            fprintf(stderr, "[backup] uploaded s3://%s/%s\n", s.bucket, key);
            prune_s3(&c, key, s.keep);
        } else {
            error = "upload to the S3 target failed";
        }
        OPENSSL_cleanse(&c, sizeof(c));
    } else {
        if (copy_to_dir(local, s.path, name)) {
            fprintf(stderr, "[backup] copied to %s/%s\n", s.path, name);
            prune_dir(s.path, s.keep);
        } else {
            error = "copying to the backup directory failed";
        }
    }
    unlink(local);
    OPENSSL_cleanse(&s, sizeof(s));
    record(error ? "failed" : "ok", error ? NULL : name, size, error);
    if (error) fprintf(stderr, "[backup] %s\n", error);
    flock(lock, LOCK_UN);
    close(lock);
    return error ? 1 : 0;
}

bool backup_spawn(void) {
    char exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0) return false;
    exe[n] = '\0';
    /* Double fork: the backup runs detached and is reaped by init, so no
     * server process has to wait for it. */
    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        if (fork() != 0) _exit(0);
        setsid();
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) { dup2(devnull, STDIN_FILENO); close(devnull); }
        /* Drop every inherited descriptor but stdio: a listening socket
         * kept open here would hold the port across a server restart. */
        if (close_range(3, ~0U, 0) != 0) {
            for (int fd = 3; fd < 65536; fd++) close(fd);
        }
        execl(exe, exe, "--scheduled-backup", (char *)NULL);
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

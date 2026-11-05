#define _POSIX_C_SOURCE 200809L
#include "privacy.h"
#include "db/db_internal.h"
#include "db/db_portability.h"
#include <openssl/rand.h>
#include <zstd.h>
#include <cjson/cJSON.h>
#include <cwist/core/log.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define EXPORT_DEFAULT_DIR "data/exports"
#define EXPORT_DEFAULT_TTL 900
#define EXPORT_BUFFER_SIZE (128 * 1024)

typedef struct {
    FILE *file;
    ZSTD_CStream *stream;
    unsigned char *compressed;
    size_t compressed_size;
    bool failed;
} tar_writer;

static const char *export_dir(void) {
    const char *configured = getenv("FLY_EXPORT_DIR");
    return configured && configured[0] ? configured : EXPORT_DEFAULT_DIR;
}

static long export_ttl(void) {
    const char *configured = getenv("FLY_EXPORT_TTL_SECONDS");
    long value = configured ? strtol(configured, NULL, 10) : EXPORT_DEFAULT_TTL;
    if (value < 60) return 60;
    if (value > 3600) return 3600;
    return value;
}

static bool valid_id(const char *id) {
    if (!id || strlen(id) != PRIVACY_EXPORT_ID_LEN) return false;
    for (size_t i = 0; i < PRIVACY_EXPORT_ID_LEN; i++)
        if (!isxdigit((unsigned char)id[i])) return false;
    return true;
}

static bool ensure_export_dir(void) {
    if (mkdir(export_dir(), 0700) == 0 || errno == EEXIST) {
        chmod(export_dir(), 0700);
        return true;
    }
    return false;
}

static void job_path(const char *id, const char *suffix, char *out, size_t out_len) {
    snprintf(out, out_len, "%s/%s%s", export_dir(), id, suffix);
}

static bool write_all(int fd, const char *data, size_t len) {
    size_t offset = 0;
    while (offset < len) {
        ssize_t n = write(fd, data + offset, len - offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        offset += (size_t)n;
    }
    return true;
}

static bool job_write(const char *id, int uid, const char *state) {
    if (!valid_id(id) || !ensure_export_dir()) return false;
    char path[PATH_MAX], temp[PATH_MAX + 64];
    job_path(id, ".job", path, sizeof(path));
    if (strlen(path) + 32 >= sizeof(temp)) return false;
    snprintf(temp, sizeof(temp), "%s.tmp.%ld", path, (long)getpid());
    int fd = open(temp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (fd < 0) return false;
    char contents[256];
    int len = snprintf(contents, sizeof(contents), "owner=%d\nstate=%s\nexpires=%ld\n",
                       uid, state, (long)time(NULL) + export_ttl());
    bool ok = len > 0 && (size_t)len < sizeof(contents) && write_all(fd, contents, (size_t)len);
    if (fsync(fd) != 0) ok = false;
    if (close(fd) != 0) ok = false;
    if (ok && rename(temp, path) != 0) ok = false;
    if (!ok) unlink(temp);
    return ok;
}

typedef struct {
    int owner;
    char state[16];
    time_t expires;
    bool found;
} export_job;

static export_job job_read(const char *id) {
    export_job job = {.owner = 0, .state = "", .expires = 0, .found = false};
    if (!valid_id(id)) return job;
    char path[PATH_MAX];
    job_path(id, ".job", path, sizeof(path));
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return job;
    char contents[256];
    ssize_t len = read(fd, contents, sizeof(contents) - 1);
    close(fd);
    if (len <= 0) return job;
    contents[len] = '\0';
    char *save = NULL;
    for (char *line = strtok_r(contents, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        if (strncmp(line, "owner=", 6) == 0) job.owner = atoi(line + 6);
        else if (strncmp(line, "state=", 6) == 0) snprintf(job.state, sizeof(job.state), "%s", line + 6);
        else if (strncmp(line, "expires=", 8) == 0) job.expires = (time_t)strtol(line + 8, NULL, 10);
    }
    job.found = job.owner > 0 && job.state[0] && job.expires > 0;
    if (job.found && job.expires <= time(NULL)) {
        job.found = false;
        unlink(path);
        char archive[PATH_MAX];
        job_path(id, ".tar.zst", archive, sizeof(archive));
        unlink(archive);
    }
    return job;
}

static void job_remove(const char *id) {
    if (!valid_id(id)) return;
    char path[PATH_MAX];
    job_path(id, ".job", path, sizeof(path));
    unlink(path);
    job_path(id, ".tar.zst", path, sizeof(path));
    unlink(path);
    job_path(id, ".tar.zst.tmp", path, sizeof(path));
    unlink(path);
}

static bool tar_octal(char *out, size_t len, unsigned long long value) {
    if (len < 2) return false;
    memset(out, '0', len - 1);
    out[len - 1] = '\0';
    size_t pos = len - 1;
    do {
        if (pos == 0) return false;
        out[--pos] = (char)('0' + (value & 7));
        value >>= 3;
    } while (value);
    return true;
}

static bool tar_compress(tar_writer *writer, const void *data, size_t len, bool finish) {
    ZSTD_inBuffer input = {.src = data, .size = len, .pos = 0};
    ZSTD_EndDirective directive = finish ? ZSTD_e_end : ZSTD_e_continue;
    do {
        ZSTD_outBuffer output = {.dst = writer->compressed, .size = writer->compressed_size, .pos = 0};
        size_t rc = ZSTD_compressStream2(writer->stream, &output, &input, directive);
        if (ZSTD_isError(rc) || fwrite(writer->compressed, 1, output.pos, writer->file) != output.pos) {
            writer->failed = true;
            return false;
        }
        if (finish && rc == 0 && input.pos == input.size) return true;
        if (!finish && input.pos == input.size) return true;
    } while (!writer->failed);
    return false;
}

static bool tar_bytes(tar_writer *writer, const void *data, size_t len) {
    return !writer->failed && tar_compress(writer, data, len, false);
}

static bool tar_header(tar_writer *writer, const char *name, size_t size) {
    unsigned char header[512] = {0};
    size_t name_len = strlen(name);
    if (name_len > 100) return false;
    memcpy(header, name, name_len);
    if (!tar_octal((char *)header + 100, 8, 0644) ||
        !tar_octal((char *)header + 108, 8, 0) ||
        !tar_octal((char *)header + 116, 8, 0) ||
        !tar_octal((char *)header + 124, 12, size) ||
        !tar_octal((char *)header + 136, 12, (unsigned long long)time(NULL))) return false;
    header[156] = '0';
    memcpy(header + 257, "ustar", 5);
    header[263] = '0'; header[264] = '0';
    memcpy(header + 265, "flyboard", 8);
    memcpy(header + 297, "flyboard", 8);
    memset(header + 148, ' ', 8);
    unsigned int checksum = 0;
    for (size_t i = 0; i < sizeof(header); i++) checksum += header[i];
    if (!tar_octal((char *)header + 148, 7, checksum)) return false;
    header[155] = ' ';
    return tar_bytes(writer, header, sizeof(header));
}

static bool tar_padding(tar_writer *writer, size_t size) {
    unsigned char zeros[512] = {0};
    size_t padding = (512 - (size % 512)) % 512;
    return padding == 0 || tar_bytes(writer, zeros, padding);
}

static bool tar_memory(tar_writer *writer, const char *name, const char *data, size_t len) {
    return tar_header(writer, name, len) && tar_bytes(writer, data, len) && tar_padding(writer, len);
}

static bool tar_json(tar_writer *writer, const char *name, cJSON *json) {
    char *text = cJSON_PrintUnformatted(json);
    if (!text) return false;
    bool ok = tar_memory(writer, name, text, strlen(text));
    cJSON_free(text);
    return ok;
}

static bool tar_disk_file(tar_writer *writer, const char *name, const char *path) {
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return true;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0) {
        close(fd);
        return false;
    }
    bool ok = tar_header(writer, name, (size_t)st.st_size);
    unsigned char buffer[EXPORT_BUFFER_SIZE];
    size_t remaining = (size_t)st.st_size;
    while (ok && remaining > 0) {
        size_t wanted = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
        ssize_t n = read(fd, buffer, wanted);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { ok = false; break; }
        ok = tar_bytes(writer, buffer, (size_t)n);
        remaining -= (size_t)n;
    }
    close(fd);
    return ok && tar_padding(writer, (size_t)st.st_size);
}

static bool upload_path_allowed(const char *path) {
    char root[PATH_MAX], resolved[PATH_MAX];
    if (!path || !realpath("public/uploads", root) || !realpath(path, resolved)) return false;
    size_t len = strlen(root);
    return strncmp(root, resolved, len) == 0 && resolved[len] == '/';
}

static cJSON *query_rows(sqlite3 *db, const char *sql, int uid) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_int(stmt, 1, uid);
    return db_sqlite3_rows_to_json(stmt);
}

static cJSON *query_profile(sqlite3 *db, int uid) {
    const char *sql = "SELECT id, username, email, role, nickname, bio, profile_pic, created_at, "
                      "email_verified, guestbook_anon FROM users "
                      "WHERE id=? AND active=1 AND ghosted=0";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_int(stmt, 1, uid);
    return db_sqlite3_row_to_json(stmt);
}

bool privacy_export_build(sqlite3 *main_db, sqlite3 *comments_db, int user_id,
                          const char *archive_path) {
    if (!main_db || !comments_db || user_id <= 0 || !archive_path) return false;
    cJSON *profile = query_profile(main_db, user_id);
    cJSON *posts = query_rows(main_db,
        "SELECT id, board_id, user_id, title, slug, content, summary, pqc_signature, "
        "created_at, updated_at, view_count, is_notice, is_secret, category, status FROM posts "
        "WHERE user_id=? ORDER BY id", user_id);
    cJSON *comments = query_rows(comments_db,
        "SELECT id, target_type, target_id, user_id, parent_id, content, created_at, updated_at, deleted "
        "FROM comments WHERE user_id=? ORDER BY id", user_id);
    cJSON *files = query_rows(main_db,
        "SELECT id, post_id, user_id, filename, mime_type, file_path, size, created_at, is_inline "
        "FROM files WHERE user_id=? ORDER BY id", user_id);
    bool ok = profile && posts && comments && files;
    FILE *output = NULL;
    ZSTD_CStream *stream = NULL;
    tar_writer writer = {0};
    if (ok) {
        int fd = open(archive_path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
        if (fd >= 0) output = fdopen(fd, "wb");
        if (fd >= 0 && !output) close(fd);
        stream = ZSTD_createCStream();
        writer.compressed_size = ZSTD_CStreamOutSize();
        writer.compressed = malloc(writer.compressed_size);
        writer.file = output;
        writer.stream = stream;
        ok = output && stream && writer.compressed && ZSTD_initCStream(stream, 3) == 0;
    }
    if (ok) ok = tar_json(&writer, "profile.json", profile);
    if (ok && !writer.failed) {
        cJSON *item;
        int index = 0;
        cJSON_ArrayForEach(item, posts) {
            char name[64];
            snprintf(name, sizeof(name), "posts/%08d.json", ++index);
            if (!tar_json(&writer, name, item)) { ok = false; break; }
        }
    }
    if (ok && !writer.failed) {
        cJSON *item;
        int index = 0;
        cJSON_ArrayForEach(item, comments) {
            char name[64];
            snprintf(name, sizeof(name), "comments/%08d.json", ++index);
            if (!tar_json(&writer, name, item)) { ok = false; break; }
        }
    }
    if (ok && !writer.failed) {
        cJSON *item;
        cJSON_ArrayForEach(item, files) {
            cJSON *id_item = cJSON_GetObjectItem(item, "id");
            int id = cJSON_IsNumber(id_item) ? id_item->valueint : 0;
            char name[128];
            snprintf(name, sizeof(name), "uploads/%08d.json", id);
            if (!tar_json(&writer, name, item)) { ok = false; break; }
            const char *path = cJSON_GetStringValue(cJSON_GetObjectItem(item, "file_path"));
            const char *original = cJSON_GetStringValue(cJSON_GetObjectItem(item, "filename"));
            char safe[65];
            size_t n = 0;
            for (const char *p = original ? original : "file"; *p && n < sizeof(safe) - 1; p++)
                safe[n++] = (char)(isalnum((unsigned char)*p) || *p == '.' || *p == '-' || *p == '_' ? *p : '_');
            safe[n] = '\0';
            if (upload_path_allowed(path)) {
                snprintf(name, sizeof(name), "uploads/%08d-%s", id, safe[0] ? safe : "file");
                if (!tar_disk_file(&writer, name, path)) { ok = false; break; }
            }
        }
    }
    unsigned char zeros[1024] = {0};
    if (ok && !writer.failed) ok = tar_bytes(&writer, zeros, sizeof(zeros));
    if (ok && !writer.failed) ok = tar_compress(&writer, "", 0, true);
    if (stream) ZSTD_freeCStream(stream);
    free(writer.compressed);
    if (output && fclose(output) != 0) ok = false;
    if (!ok && archive_path) unlink(archive_path);
    cJSON_Delete(profile);
    cJSON_Delete(posts);
    cJSON_Delete(comments);
    cJSON_Delete(files);
    return ok && !writer.failed;
}

typedef struct {
    int uid;
    char id[PRIVACY_EXPORT_ID_LEN + 1];
} export_work;

static void *export_worker(void *argument) {
    export_work *work = argument;
    char temp[PATH_MAX], final[PATH_MAX];
    job_path(work->id, ".tar.zst.tmp", temp, sizeof(temp));
    job_path(work->id, ".tar.zst", final, sizeof(final));
    sqlite3 *main_db = NULL, *comments_db = NULL;
    bool ok = fly_db_open_main(&main_db) == SQLITE_OK &&
              sqlite3_open_v2("data/comments.db", &comments_db, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK;
    if (ok) ok = db_configure_connection(main_db);
    if (ok) ok = privacy_export_build(main_db, comments_db, work->uid, temp);
    if (ok) {
        cJSON *user = query_profile(main_db, work->uid);
        ok = user != NULL;
        cJSON_Delete(user);
    }
    if (ok && rename(temp, final) != 0) ok = false;
    if (ok) ok = job_write(work->id, work->uid, "ready");
    if (ok) {
        cJSON *user = query_profile(main_db, work->uid);
        if (!user) {
            job_remove(work->id);
            ok = false;
        }
        cJSON_Delete(user);
    }
    if (!ok) {
        export_job job = job_read(work->id);
        if (job.found && job.owner == work->uid) job_write(work->id, work->uid, "failed");
        unlink(temp);
        unlink(final);
    }
    if (main_db) sqlite3_close(main_db);
    if (comments_db) sqlite3_close(comments_db);
    free(work);
    return NULL;
}

bool privacy_export_start(int user_id, char out_id[PRIVACY_EXPORT_ID_LEN + 1]) {
    if (user_id <= 0 || !out_id || !ensure_export_dir()) return false;
    unsigned char random[32];
    if (RAND_bytes(random, sizeof(random)) != 1) return false;
    for (size_t i = 0; i < sizeof(random); i++) snprintf(out_id + i * 2, 3, "%02x", random[i]);
    out_id[PRIVACY_EXPORT_ID_LEN] = '\0';
    if (!job_write(out_id, user_id, "queued")) return false;
    export_work *work = calloc(1, sizeof(*work));
    if (!work) { job_write(out_id, user_id, "failed"); return false; }
    work->uid = user_id;
    memcpy(work->id, out_id, sizeof(work->id));
    pthread_t thread;
    int rc = pthread_create(&thread, NULL, export_worker, work);
    if (rc != 0) {
        free(work);
        job_write(out_id, user_id, "failed");
        return false;
    }
    pthread_detach(thread);
    return true;
}

cJSON *privacy_export_status(int user_id, const char *id) {
    export_job job = job_read(id);
    if (!job.found || job.owner != user_id) return NULL;
    cJSON *status = cJSON_CreateObject();
    if (!status) return NULL;
    cJSON_AddStringToObject(status, "id", id);
    cJSON_AddStringToObject(status, "state", job.state);
    cJSON_AddNumberToObject(status, "expires_at", (double)job.expires);
    if (strcmp(job.state, "ready") == 0) {
        char url[160];
        snprintf(url, sizeof(url), "/account/export/%s/download", id);
        cJSON_AddStringToObject(status, "download_url", url);
    }
    return status;
}

bool privacy_export_download_path(int user_id, const char *id, char *out, size_t out_len) {
    export_job job = job_read(id);
    if (!job.found || job.owner != user_id || strcmp(job.state, "ready") != 0) return false;
    char path[PATH_MAX];
    job_path(id, ".tar.zst", path, sizeof(path));
    struct stat st;
    if (lstat(path, &st) != 0 || !S_ISREG(st.st_mode)) return false;
    int written = snprintf(out, out_len, "%s", path);
    return written >= 0 && (size_t)written < out_len;
}

void privacy_export_revoke_user(int user_id) {
    DIR *dir = opendir(export_dir());
    if (!dir) return;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        size_t len = strlen(entry->d_name);
        if (len != PRIVACY_EXPORT_ID_LEN + 4 || strcmp(entry->d_name + PRIVACY_EXPORT_ID_LEN, ".job") != 0) continue;
        char id[PRIVACY_EXPORT_ID_LEN + 1];
        memcpy(id, entry->d_name, PRIVACY_EXPORT_ID_LEN);
        id[PRIVACY_EXPORT_ID_LEN] = '\0';
        export_job job = job_read(id);
        if (job.found && job.owner == user_id) job_remove(id);
    }
    closedir(dir);
}

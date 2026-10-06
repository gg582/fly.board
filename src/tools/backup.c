#include "backup.h"
#include "crypto/fly_crypto.h"
#include "db/db_internal.h"
#include <cjson/cJSON.h>
#include <cwist/core/mem/alloc.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <openssl/aead.h>
#include <openssl/evp.h>
#include <openssl/mem.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <sqlite3.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <zstd.h>

/* Archive layout: a zstd-compressed ustar stream of
 *
 *   b/000001 ... b/NNNNNN   file contents, one blob per backed-up path
 *   secrets.enc             optional, encrypted (see below)
 *   manifest.json           paths, sizes, SHA-256 of every blob, key info
 *   manifest.sig            ML-DSA-65 signature over manifest.json
 *
 * Blobs carry numbers, not paths: the manifest alone decides where a blob
 * goes, and restore checks every path against an allow-list before writing.
 * The manifest comes last so the archive is written in one pass; readers
 * stage the blobs, then check them against it.
 *
 * Secrets (signing seed, JWT secret, admin.settings, s3.settings) are only
 * included with --with-secrets, as JSON sealed with AES-256-GCM under a key
 * from scrypt(passphrase). scrypt and AES-GCM come from the BoringSSL that
 * is linked into this binary, so any fly.board build can open the archive. */

#define FORMAT_NAME "fly.board-backup"
#define FORMAT_VERSION 1
#define SEED_PATH "data/.pqc_mldsa65_seed"
#define SECRETS_AD "fly.board-backup-secrets-v1"
#define SCRYPT_N (1u << 17)
#define SCRYPT_R 8
#define SCRYPT_P 1
#define SCRYPT_MAX_MEM (256u * 1024 * 1024)
#define IO_CHUNK (256 * 1024)
#define MAX_MANIFEST (64u * 1024 * 1024)
#define MAX_SECRETS (1u * 1024 * 1024)

static const char *const k_db_paths[] = {"data/blog.db", "data/comments.db", "data/board_tree.db"};
static const char *const k_file_dirs[] = {"public/uploads", "public/profile", "public/img"};
static const char *const k_settings[] = {"blog.settings", "fonts.settings", "robots.settings"};
static const char *const k_secret_paths[] = {SEED_PATH, "data/.jwt_secret", "admin.settings", "s3.settings"};

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static int jint(cJSON *o, const char *key, int def) {
    cJSON *v = o ? cJSON_GetObjectItem(o, key) : NULL;
    return cJSON_IsNumber(v) ? (int)v->valuedouble : def;
}

static void hex_of(const uint8_t *d, size_t n, char *out) {
    for (size_t i = 0; i < n; i++) snprintf(out + 2 * i, 3, "%02x", d[i]);
}

static char *b64_encode(const uint8_t *d, size_t n) {
    size_t len = 4 * ((n + 2) / 3);
    char *out = malloc(len + 1);
    if (!out) return NULL;
    EVP_EncodeBlock((uint8_t *)out, d, n);
    out[len] = '\0';
    return out;
}

static uint8_t *b64_decode(const char *s, size_t *out_len) {
    size_t len = s ? strlen(s) : 0;
    if (len == 0 || len % 4) return NULL;
    uint8_t *out = malloc(3 * len / 4 + 1);
    if (!out) return NULL;
    int n = EVP_DecodeBlock(out, (const uint8_t *)s, len);
    if (n < 0) { free(out); return NULL; }
    if (s[len - 1] == '=') n--;
    if (s[len - 2] == '=') n--;
    *out_len = (size_t)n;
    return out;
}

static bool read_file(const char *path, uint8_t **out, size_t *out_len, size_t max) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    struct stat st;
    if (fstat(fileno(f), &st) != 0 || !S_ISREG(st.st_mode) || (size_t)st.st_size > max) {
        fclose(f);
        return false;
    }
    uint8_t *buf = malloc((size_t)st.st_size + 1);
    bool ok = buf && fread(buf, 1, (size_t)st.st_size, f) == (size_t)st.st_size;
    fclose(f);
    if (!ok) { free(buf); return false; }
    buf[st.st_size] = '\0';
    *out = buf;
    *out_len = (size_t)st.st_size;
    return true;
}

static bool mkdirs_for(const char *path) {
    char buf[PATH_MAX];
    if ((size_t)snprintf(buf, sizeof(buf), "%s", path) >= sizeof(buf)) return false;
    for (char *p = buf + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(buf, 0755) != 0 && errno != EEXIST) return false;
        *p = '/';
    }
    return true;
}

static void rm_tree(const char *path) {
    DIR *d = opendir(path);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char child[PATH_MAX];
            snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
            rm_tree(child);
        }
        closedir(d);
        rmdir(path);
    } else {
        unlink(path);
    }
}

/* ---- Passphrase ---- */

static char *read_passphrase(const char *file, bool confirm) {
    char buf[1024] = {0};
    if (file) {
        FILE *f = fopen(file, "r");
        if (!f || !fgets(buf, sizeof(buf), f)) {
            if (f) fclose(f);
            say("cannot read passphrase file %s", file);
            return NULL;
        }
        fclose(f);
    } else if (getenv("FLY_BACKUP_PASSPHRASE")) {
        snprintf(buf, sizeof(buf), "%s", getenv("FLY_BACKUP_PASSPHRASE"));
    } else if (isatty(STDIN_FILENO)) {
        struct termios old, quiet;
        tcgetattr(STDIN_FILENO, &old);
        quiet = old;
        quiet.c_lflag &= ~(tcflag_t)ECHO;
        for (int round = 0; round < (confirm ? 2 : 1); round++) {
            char line[1024] = {0};
            fprintf(stderr, round ? "Repeat passphrase: " : "Backup passphrase: ");
            tcsetattr(STDIN_FILENO, TCSAFLUSH, &quiet);
            char *got = fgets(line, sizeof(line), stdin);
            tcsetattr(STDIN_FILENO, TCSAFLUSH, &old);
            fputc('\n', stderr);
            if (!got) return NULL;
            line[strcspn(line, "\r\n")] = '\0';
            if (round == 0) {
                snprintf(buf, sizeof(buf), "%s", line);
            } else if (strcmp(buf, line) != 0) {
                say("passphrases do not match");
                OPENSSL_cleanse(line, sizeof(line));
                OPENSSL_cleanse(buf, sizeof(buf));
                return NULL;
            }
            OPENSSL_cleanse(line, sizeof(line));
        }
    } else {
        return NULL;
    }
    buf[strcspn(buf, "\r\n")] = '\0';
    char *out = strdup(buf);
    OPENSSL_cleanse(buf, sizeof(buf));
    return out;
}

static void free_passphrase(char *p) {
    if (!p) return;
    OPENSSL_cleanse(p, strlen(p));
    free(p);
}

/* ---- zstd stream ---- */

typedef struct {
    FILE *f;
    ZSTD_CCtx *cctx;
    uint8_t out[IO_CHUNK];
    bool failed;
} zwriter;

static void zw_write(zwriter *w, const void *data, size_t len) {
    ZSTD_inBuffer in = {data, len, 0};
    while (!w->failed && in.pos < in.size) {
        ZSTD_outBuffer out = {w->out, sizeof(w->out), 0};
        size_t rc = ZSTD_compressStream2(w->cctx, &out, &in, ZSTD_e_continue);
        if (ZSTD_isError(rc) || fwrite(w->out, 1, out.pos, w->f) != out.pos) w->failed = true;
    }
}

static bool zw_finish(zwriter *w) {
    size_t remaining;
    do {
        ZSTD_inBuffer in = {NULL, 0, 0};
        ZSTD_outBuffer out = {w->out, sizeof(w->out), 0};
        remaining = ZSTD_compressStream2(w->cctx, &out, &in, ZSTD_e_end);
        if (ZSTD_isError(remaining) || fwrite(w->out, 1, out.pos, w->f) != out.pos) return false;
    } while (remaining != 0);
    return !w->failed;
}

typedef struct {
    FILE *f;
    ZSTD_DCtx *dctx;
    uint8_t in[IO_CHUNK];
    ZSTD_inBuffer ib;
    bool failed;
} zreader;

/* Read exactly len bytes; false on a short or corrupt stream. */
static bool zr_read(zreader *r, void *dst, size_t len) {
    ZSTD_outBuffer out = {dst, len, 0};
    while (out.pos < out.size) {
        if (r->ib.pos == r->ib.size) {
            size_t n = fread(r->in, 1, sizeof(r->in), r->f);
            if (n == 0) return false;
            r->ib.src = r->in;
            r->ib.size = n;
            r->ib.pos = 0;
        }
        size_t rc = ZSTD_decompressStream(r->dctx, &out, &r->ib);
        if (ZSTD_isError(rc)) {
            r->failed = true;
            return false;
        }
    }
    return true;
}

/* ---- ustar ---- */

static void tar_header(uint8_t h[512], const char *name, uint64_t size, time_t mtime) {
    memset(h, 0, 512);
    snprintf((char *)h, 100, "%s", name);
    snprintf((char *)h + 100, 8, "%07o", 0644);
    snprintf((char *)h + 108, 8, "%07o", 0);
    snprintf((char *)h + 116, 8, "%07o", 0);
    /* Both fields hold 11 octal digits; callers keep sizes below 8 GiB. */
    snprintf((char *)h + 124, 12, "%011llo", (unsigned long long)size & 077777777777ULL);
    snprintf((char *)h + 136, 12, "%011llo", (unsigned long long)(mtime > 0 ? mtime : 0) & 077777777777ULL);
    h[156] = '0';
    memcpy(h + 257, "ustar", 6);
    memcpy(h + 263, "00", 2);
    memset(h + 148, ' ', 8);
    unsigned sum = 0;
    for (int i = 0; i < 512; i++) sum += h[i];
    snprintf((char *)h + 148, 8, "%06o", sum);
    h[155] = ' ';
}

static const uint8_t k_zero[512];

static void tar_pad(zwriter *w, uint64_t size) {
    size_t pad = (size_t)((512 - size % 512) % 512);
    if (pad) zw_write(w, k_zero, pad);
}

/* Write one member from memory. */
static void tar_put_mem(zwriter *w, const char *name, const void *data, size_t len) {
    uint8_t h[512];
    tar_header(h, name, len, time(NULL));
    zw_write(w, h, 512);
    zw_write(w, data, len);
    tar_pad(w, len);
}

/* Stream one file into a member, hashing it on the way. */
static bool tar_put_file(zwriter *w, const char *name, const char *path, uint64_t *size_out, char sha_hex[65]) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    struct stat st;
    if (fstat(fileno(f), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size > 077777777777LL) {
        fclose(f);
        return false;
    }
    uint64_t size = (uint64_t)st.st_size;
    uint8_t h[512];
    tar_header(h, name, size, st.st_mtime);
    zw_write(w, h, 512);
    SHA256_CTX sha;
    SHA256_Init(&sha);
    static uint8_t buf[IO_CHUNK];
    uint64_t left = size;
    while (left > 0) {
        size_t want = left > sizeof(buf) ? sizeof(buf) : (size_t)left;
        size_t n = fread(buf, 1, want, f);
        if (n != want) {
            /* The file shrank while being read (a live upload). */
            fclose(f);
            say("%s changed while it was being read", path);
            w->failed = true;
            return false;
        }
        SHA256_Update(&sha, buf, n);
        zw_write(w, buf, n);
        left -= n;
    }
    fclose(f);
    tar_pad(w, size);
    uint8_t digest[SHA256_DIGEST_LENGTH];
    SHA256_Final(digest, &sha);
    hex_of(digest, sizeof(digest), sha_hex);
    *size_out = size;
    return !w->failed;
}

static uint64_t parse_octal(const uint8_t *p, size_t n) {
    uint64_t v = 0;
    for (size_t i = 0; i < n && p[i] >= '0' && p[i] <= '7'; i++) v = v * 8 + (uint64_t)(p[i] - '0');
    return v;
}

static bool tar_header_valid(const uint8_t h[512]) {
    unsigned sum = 0;
    for (int i = 0; i < 512; i++) sum += (i >= 148 && i < 156) ? ' ' : h[i];
    return sum == (unsigned)parse_octal(h + 148, 8) && memcmp(h + 257, "ustar", 5) == 0;
}

/* ---- Archive reading (verify / restore) ---- */

typedef struct {
    cJSON *blobs;           /* name -> {"sha256","size"} as seen in the stream */
    uint8_t *manifest;
    size_t manifest_len;
    char *signature;
    uint8_t *secrets;
    size_t secrets_len;
    const char *stage_dir;  /* restore: blobs are written here; verify: NULL */
} archive_read_t;

static void archive_read_free(archive_read_t *a) {
    if (a->blobs) cJSON_Delete(a->blobs);
    free(a->manifest);
    free(a->signature);
    if (a->secrets) OPENSSL_cleanse(a->secrets, a->secrets_len);
    free(a->secrets);
    memset(a, 0, sizeof(*a));
}

static bool blob_name_ok(const char *name) {
    if (strncmp(name, "b/", 2) != 0 || strlen(name) != 8) return false;
    for (int i = 2; i < 8; i++) if (name[i] < '0' || name[i] > '9') return false;
    return true;
}

static bool read_archive(const char *path, archive_read_t *a, const char *stage_dir) {
    memset(a, 0, sizeof(*a));
    a->stage_dir = stage_dir;
    a->blobs = cJSON_CreateObject();
    FILE *f = fopen(path, "rb");
    if (!f) {
        say("cannot open %s: %s", path, strerror(errno));
        return false;
    }
    zreader r = {.f = f, .dctx = ZSTD_createDCtx()};
    bool ok = r.dctx != NULL;
    static uint8_t buf[IO_CHUNK];
    while (ok) {
        uint8_t h[512];
        if (!zr_read(&r, h, 512)) { ok = false; say("archive ends early or is corrupt"); break; }
        if (memcmp(h, k_zero, 512) == 0) break; /* end of archive */
        if (!tar_header_valid(h)) { ok = false; say("bad tar header"); break; }
        char name[101];
        memcpy(name, h, 100);
        name[100] = '\0';
        uint64_t size = parse_octal(h + 124, 12);
        bool is_blob = blob_name_ok(name);
        bool is_secrets = strcmp(name, "secrets.enc") == 0;
        bool is_manifest = strcmp(name, "manifest.json") == 0;
        bool is_sig = strcmp(name, "manifest.sig") == 0;
        if (!is_blob && !is_secrets && !is_manifest && !is_sig) { ok = false; say("unexpected member %s", name); break; }
        if (cJSON_GetObjectItem(a->blobs, name) || (is_manifest && a->manifest) || (is_sig && a->signature) ||
            (is_secrets && a->secrets)) {
            ok = false; say("duplicate member %s", name); break;
        }
        if ((is_manifest && size > MAX_MANIFEST) || (is_secrets && size > MAX_SECRETS) || (is_sig && size > 16384)) {
            ok = false; say("member %s is too large", name); break;
        }

        FILE *out = NULL;
        uint8_t *mem = NULL;
        if (is_blob && a->stage_dir) {
            char stage[PATH_MAX];
            snprintf(stage, sizeof(stage), "%s/%s", a->stage_dir, name + 2);
            out = fopen(stage, "wb");
            if (!out) { ok = false; say("cannot stage %s: %s", name, strerror(errno)); break; }
        } else if (!is_blob) {
            mem = malloc((size_t)size + 1);
            if (!mem) { ok = false; break; }
        }
        SHA256_CTX sha;
        SHA256_Init(&sha);
        uint64_t left = size, off = 0;
        while (ok && left > 0) {
            size_t want = left > sizeof(buf) ? sizeof(buf) : (size_t)left;
            if (!zr_read(&r, buf, want)) { ok = false; say("archive ends inside %s", name); break; }
            SHA256_Update(&sha, buf, want);
            if (out && fwrite(buf, 1, want, out) != want) { ok = false; say("write failed while staging"); }
            if (mem) memcpy(mem + off, buf, want);
            left -= want;
            off += want;
        }
        if (out && fclose(out) != 0) ok = false;
        size_t pad = (size_t)((512 - size % 512) % 512);
        if (ok && pad && !zr_read(&r, buf, pad)) { ok = false; say("archive ends early"); }
        if (!ok) { free(mem); break; }

        if (is_blob) {
            uint8_t digest[SHA256_DIGEST_LENGTH];
            char hex[65];
            SHA256_Final(digest, &sha);
            hex_of(digest, sizeof(digest), hex);
            cJSON *rec = cJSON_CreateObject();
            cJSON_AddStringToObject(rec, "sha256", hex);
            cJSON_AddNumberToObject(rec, "size", (double)size);
            cJSON_AddItemToObject(a->blobs, name, rec);
        } else {
            mem[size] = '\0';
            if (is_manifest) { a->manifest = mem; a->manifest_len = (size_t)size; }
            else if (is_sig) { a->signature = (char *)mem; a->signature[strcspn(a->signature, "\r\n")] = '\0'; }
            else { a->secrets = mem; a->secrets_len = (size_t)size; }
        }
    }
    ZSTD_freeDCtx(r.dctx);
    fclose(f);
    if (ok && (!a->manifest || !a->signature)) {
        say("archive has no manifest or signature");
        ok = false;
    }
    return ok;
}

/* Manifest paths must stay inside the places a backup writes. */
static bool restore_path_ok(const char *p) {
    if (!p || !p[0] || p[0] == '/' || strstr(p, "..") || strstr(p, "//") || strchr(p, '\\')) return false;
    for (size_t i = 0; i < ARRAY_LEN(k_db_paths); i++) if (!strcmp(p, k_db_paths[i])) return true;
    for (size_t i = 0; i < ARRAY_LEN(k_settings); i++) if (!strcmp(p, k_settings[i])) return true;
    for (size_t i = 0; i < ARRAY_LEN(k_secret_paths); i++) if (!strcmp(p, k_secret_paths[i])) return true;
    for (size_t i = 0; i < ARRAY_LEN(k_file_dirs); i++) {
        size_t n = strlen(k_file_dirs[i]);
        if (!strncmp(p, k_file_dirs[i], n) && p[n] == '/' && p[n + 1]) return true;
    }
    return false;
}

static bool secret_path_ok(const char *p) {
    for (size_t i = 0; i < ARRAY_LEN(k_secret_paths); i++) if (p && !strcmp(p, k_secret_paths[i])) return true;
    return false;
}

/* Check the signature and every blob against the manifest. Returns the
 * parsed manifest, or NULL. */
static cJSON *check_archive(archive_read_t *a) {
    cJSON *m = cJSON_ParseWithLength((const char *)a->manifest, a->manifest_len);
    cJSON *fmt = m ? cJSON_GetObjectItem(m, "format") : NULL;
    if (!m || !cJSON_IsString(fmt) || strcmp(fmt->valuestring, FORMAT_NAME) != 0) {
        say("not a fly.board backup manifest");
        if (m) cJSON_Delete(m);
        return NULL;
    }
    if (jint(m, "version", 0) != FORMAT_VERSION) {
        say("unsupported backup format version %d", jint(m, "version", 0));
        cJSON_Delete(m);
        return NULL;
    }
    cJSON *key = cJSON_GetObjectItem(m, "signing_key");
    cJSON *pk = key ? cJSON_GetObjectItem(key, "public_key") : NULL;
    char kid[FLY_PQC_KEY_ID_LEN + 1] = {0};
    if (!cJSON_IsString(pk) || !fly_crypto_key_id_of(pk->valuestring, kid) ||
        !fly_crypto_verify_manifest(pk->valuestring, a->manifest, a->manifest_len, a->signature)) {
        say("manifest signature does NOT verify: the archive was altered or damaged");
        cJSON_Delete(m);
        return NULL;
    }
    say("manifest signature ok (ML-DSA-65 key %s)", kid);

    int bad = 0, n = 0;
    cJSON *e = NULL;
    cJSON_ArrayForEach(e, cJSON_GetObjectItem(m, "entries")) {
        n++;
        cJSON *path = cJSON_GetObjectItem(e, "path");
        cJSON *blob = cJSON_GetObjectItem(e, "blob");
        cJSON *sha = cJSON_GetObjectItem(e, "sha256");
        cJSON *seen = cJSON_IsString(blob) ? cJSON_GetObjectItem(a->blobs, blob->valuestring) : NULL;
        if (!cJSON_IsString(path) || !restore_path_ok(path->valuestring) || secret_path_ok(path->valuestring)) {
            say("entry with a path outside the allowed set: %s", cJSON_IsString(path) ? path->valuestring : "?");
            bad++;
            continue;
        }
        if (!seen || !cJSON_IsString(sha) || strcmp(sha->valuestring, cJSON_GetObjectItem(seen, "sha256")->valuestring) != 0 ||
            jint(seen, "size", -1) != jint(e, "size", -2)) {
            say("content mismatch for %s", path->valuestring);
            bad++;
        }
    }
    cJSON *sec = cJSON_GetObjectItem(m, "secrets");
    if (cJSON_IsObject(sec)) {
        cJSON *sha = cJSON_GetObjectItem(sec, "sha256");
        uint8_t digest[SHA256_DIGEST_LENGTH];
        char hex[65] = {0};
        if (a->secrets) {
            SHA256(a->secrets, a->secrets_len, digest);
            hex_of(digest, sizeof(digest), hex);
        }
        if (!a->secrets || !cJSON_IsString(sha) || strcmp(hex, sha->valuestring) != 0) {
            say("secrets block missing or altered");
            bad++;
        }
    }
    if (bad) {
        say("%d of %d entries failed the check", bad, n);
        cJSON_Delete(m);
        return NULL;
    }
    say("all %d entries match their SHA-256", n);
    return m;
}

/* ---- Secrets ---- */

static bool derive_key(const char *pass, const uint8_t *salt, size_t salt_len, uint8_t key[32]) {
    return EVP_PBE_scrypt(pass, strlen(pass), salt, salt_len, SCRYPT_N, SCRYPT_R, SCRYPT_P,
                          SCRYPT_MAX_MEM, key, 32) == 1;
}

/* Seal the secret files; fills the manifest's "secrets" object. */
static uint8_t *seal_secrets(const char *pass, cJSON *desc, size_t *out_len, int *count) {
    cJSON *doc = cJSON_CreateObject();
    cJSON *files = cJSON_AddArrayToObject(doc, "files");
    *count = 0;
    for (size_t i = 0; i < ARRAY_LEN(k_secret_paths); i++) {
        uint8_t *data = NULL;
        size_t len = 0;
        if (!read_file(k_secret_paths[i], &data, &len, MAX_SECRETS / 4)) continue;
        char *b64 = b64_encode(data, len);
        OPENSSL_cleanse(data, len);
        free(data);
        if (!b64) continue;
        cJSON *f = cJSON_CreateObject();
        cJSON_AddStringToObject(f, "path", k_secret_paths[i]);
        cJSON_AddStringToObject(f, "data", b64);
        cJSON_AddItemToArray(files, f);
        OPENSSL_cleanse(b64, strlen(b64));
        free(b64);
        (*count)++;
    }
    char *plain = cJSON_PrintUnformatted(doc);
    cJSON_Delete(doc);
    if (!plain) return NULL;
    size_t plain_len = strlen(plain);

    uint8_t salt[16], nonce[12], key[32];
    uint8_t *sealed = NULL;
    EVP_AEAD_CTX ctx;
    bool ok = RAND_bytes(salt, sizeof(salt)) == 1 && RAND_bytes(nonce, sizeof(nonce)) == 1 &&
              derive_key(pass, salt, sizeof(salt), key) &&
              EVP_AEAD_CTX_init(&ctx, EVP_aead_aes_256_gcm(), key, sizeof(key), EVP_AEAD_DEFAULT_TAG_LENGTH, NULL);
    OPENSSL_cleanse(key, sizeof(key));
    if (ok) {
        size_t max = plain_len + EVP_AEAD_max_overhead(EVP_aead_aes_256_gcm());
        sealed = malloc(max);
        ok = sealed && EVP_AEAD_CTX_seal(&ctx, sealed, out_len, max, nonce, sizeof(nonce), (const uint8_t *)plain,
                                         plain_len, (const uint8_t *)SECRETS_AD, strlen(SECRETS_AD));
        EVP_AEAD_CTX_cleanup(&ctx);
    }
    OPENSSL_cleanse(plain, plain_len);
    free(plain);
    if (!ok) { free(sealed); return NULL; }
    char *s64 = b64_encode(salt, sizeof(salt)), *n64 = b64_encode(nonce, sizeof(nonce));
    cJSON_AddStringToObject(desc, "cipher", "aes-256-gcm");
    cJSON_AddStringToObject(desc, "kdf", "scrypt");
    cJSON_AddNumberToObject(desc, "N", SCRYPT_N);
    cJSON_AddNumberToObject(desc, "r", SCRYPT_R);
    cJSON_AddNumberToObject(desc, "p", SCRYPT_P);
    cJSON_AddStringToObject(desc, "salt", s64);
    cJSON_AddStringToObject(desc, "nonce", n64);
    free(s64);
    free(n64);
    return sealed;
}

/* Returns the decrypted {"files":[...]} document, or NULL (wrong passphrase
 * or damaged block). */
static cJSON *open_secrets(const char *pass, cJSON *desc, const uint8_t *sealed, size_t sealed_len) {
    if (jint(desc, "N", 0) != (int)SCRYPT_N || jint(desc, "r", 0) != SCRYPT_R || jint(desc, "p", 0) != SCRYPT_P) {
        say("unsupported secrets parameters");
        return NULL;
    }
    cJSON *js = cJSON_GetObjectItem(desc, "salt"), *jn = cJSON_GetObjectItem(desc, "nonce");
    size_t salt_len = 0, nonce_len = 0;
    uint8_t *salt = cJSON_IsString(js) ? b64_decode(js->valuestring, &salt_len) : NULL;
    uint8_t *nonce = cJSON_IsString(jn) ? b64_decode(jn->valuestring, &nonce_len) : NULL;
    uint8_t key[32];
    uint8_t *plain = NULL;
    size_t plain_len = 0;
    EVP_AEAD_CTX ctx;
    bool ok = salt && nonce && nonce_len == 12 && derive_key(pass, salt, salt_len, key) &&
              EVP_AEAD_CTX_init(&ctx, EVP_aead_aes_256_gcm(), key, sizeof(key), EVP_AEAD_DEFAULT_TAG_LENGTH, NULL);
    OPENSSL_cleanse(key, sizeof(key));
    if (ok) {
        plain = malloc(sealed_len + 1);
        ok = plain && EVP_AEAD_CTX_open(&ctx, plain, &plain_len, sealed_len, nonce, nonce_len, sealed, sealed_len,
                                        (const uint8_t *)SECRETS_AD, strlen(SECRETS_AD));
        EVP_AEAD_CTX_cleanup(&ctx);
    }
    free(salt);
    free(nonce);
    cJSON *doc = ok ? cJSON_ParseWithLength((const char *)plain, plain_len) : NULL;
    if (plain) {
        OPENSSL_cleanse(plain, sealed_len);
        free(plain);
    }
    return doc;
}

/* ---- Signatures report ---- */

/* Count posts and posts whose signature verifies with the keys loaded. */
static void count_signatures(sqlite3 *conn, int *posts, int *valid) {
    *posts = *valid = 0;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(conn, "SELECT title, content, pqc_signature FROM posts", -1, &st, NULL) != SQLITE_OK) return;
    while (sqlite3_step(st) == SQLITE_ROW) {
        (*posts)++;
        const char *t = (const char *)sqlite3_column_text(st, 0);
        const char *c = (const char *)sqlite3_column_text(st, 1);
        const char *s = (const char *)sqlite3_column_text(st, 2);
        if (!s || !s[0]) continue;
        t = t ? t : "";
        c = c ? c : "";
        size_t mlen = strlen(t) + 1 + strlen(c);
        char *msg = malloc(mlen + 1);
        if (!msg) continue;
        snprintf(msg, mlen + 1, "%s\n%s", t, c);
        if (fly_crypto_verify((const uint8_t *)msg, mlen, s)) (*valid)++;
        free(msg);
    }
    sqlite3_finalize(st);
}

/* ---- --backup ---- */

typedef struct {
    zwriter *w;
    cJSON *entries;
    int next_blob;
    uint64_t bytes;
    int files;
    bool failed;
} backup_ctx;

static bool add_entry(backup_ctx *c, const char *rel_path, const char *src_path, const char *kind) {
    char blob[16];
    snprintf(blob, sizeof(blob), "b/%06d", ++c->next_blob);
    if (c->next_blob > 999999) { say("too many files for one archive"); return false; }
    uint64_t size = 0;
    char sha[65];
    if (!tar_put_file(c->w, blob, src_path, &size, sha)) {
        say("failed to add %s", rel_path);
        return false;
    }
    cJSON *e = cJSON_CreateObject();
    cJSON_AddStringToObject(e, "path", rel_path);
    cJSON_AddStringToObject(e, "blob", blob);
    cJSON_AddNumberToObject(e, "size", (double)size);
    cJSON_AddStringToObject(e, "sha256", sha);
    cJSON_AddStringToObject(e, "kind", kind);
    cJSON_AddItemToArray(c->entries, e);
    c->bytes += size;
    c->files++;
    return true;
}

static bool add_dir(backup_ctx *c, const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return errno == ENOENT; /* optional directory */
    struct dirent *e;
    bool ok = true;
    /* Sorted order keeps archives of the same site comparable. */
    struct dirent **list = NULL;
    int n = scandir(dir, &list, NULL, alphasort);
    closedir(d);
    for (int i = 0; i < n; i++) {
        e = list[i];
        if (ok && strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) {
            char path[PATH_MAX];
            snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
            struct stat st;
            if (lstat(path, &st) == 0) {
                if (S_ISDIR(st.st_mode)) ok = add_dir(c, path);
                else if (S_ISREG(st.st_mode)) ok = add_entry(c, path, path, "file");
                /* symlinks and special files are skipped */
            }
        }
        free(list[i]);
    }
    free(list);
    return ok;
}

static bool snapshot_db(const char *src, const char *dst) {
    sqlite3 *in = NULL, *out = NULL;
    bool ok = sqlite3_open_v2(src, &in, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK &&
              sqlite3_open(dst, &out) == SQLITE_OK;
    if (ok) {
        sqlite3_busy_timeout(in, 10000);
        sqlite3_backup *b = sqlite3_backup_init(out, "main", in, "main");
        ok = b && sqlite3_backup_step(b, -1) == SQLITE_DONE;
        if (b) sqlite3_backup_finish(b);
        /* A restored file should open as an ordinary database. */
        ok = ok && sqlite3_exec(out, "PRAGMA journal_mode=DELETE", NULL, NULL, NULL) == SQLITE_OK;
    }
    if (!ok) say("snapshot of %s failed: %s", src, in ? sqlite3_errmsg(in) : "open");
    sqlite3_close(in);
    sqlite3_close(out);
    return ok;
}

/* Refuse to write an archive (user data, password hashes) under public/,
 * where the server might hand it out. */
static bool output_path_safe(const char *out) {
    char dir[PATH_MAX], real_dir[PATH_MAX], real_public[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", out);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = '\0';
    else snprintf(dir, sizeof(dir), ".");
    if (!realpath(dir, real_dir)) {
        say("output directory %s does not exist", dir);
        return false;
    }
    if (realpath("public", real_public)) {
        size_t n = strlen(real_public);
        if (!strncmp(real_dir, real_public, n) && (real_dir[n] == '/' || real_dir[n] == '\0')) {
            say("refusing to write a backup under public/, which is served to visitors");
            return false;
        }
    }
    return true;
}

static int cmd_backup(const char *out_path, bool with_secrets, const char *pass_file) {
    if (!output_path_safe(out_path)) return 1;
    if (access("data/blog.db", R_OK) != 0) {
        say("data/blog.db not found; run from the site root");
        return 1;
    }
    char *pass = NULL;
    if (with_secrets) {
        pass = read_passphrase(pass_file, true);
        if (!pass || strlen(pass) < 12) {
            say("--with-secrets needs a passphrase of at least 12 characters "
                "(--passphrase-file, FLY_BACKUP_PASSPHRASE, or a terminal prompt)");
            free_passphrase(pass);
            return 1;
        }
    }
    if (!fly_crypto_init(SEED_PATH)) {
        say("cannot load the signing key %s", SEED_PATH);
        free_passphrase(pass);
        return 1;
    }

    /* Make sure the live database lists the current key and its signatures
     * carry key ids before the snapshot is taken. */
    sqlite3 *live = NULL;
    int posts = 0, valid = 0;
    if (sqlite3_open("data/blog.db", &live) != SQLITE_OK) {
        say("cannot open data/blog.db");
        free_passphrase(pass);
        return 1;
    }
    sqlite3_busy_timeout(live, 10000);
    bool synced = pqc_keys_sync_conn(live, true);
    if (synced) count_signatures(live, &posts, &valid);
    sqlite3_close(live);
    if (!synced) {
        say("cannot register the signing key in data/blog.db");
        free_passphrase(pass);
        return 1;
    }

    char stage[] = "data/.backup-XXXXXX";
    if (!mkdtemp(stage)) {
        say("cannot create a staging directory: %s", strerror(errno));
        free_passphrase(pass);
        return 1;
    }
    char tmp_out[PATH_MAX];
    snprintf(tmp_out, sizeof(tmp_out), "%s.partial", out_path);
    int fd = open(tmp_out, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    FILE *f = fd >= 0 ? fdopen(fd, "wb") : NULL;
    if (!f) {
        say("cannot write %s: %s", tmp_out, strerror(errno));
        rm_tree(stage);
        free_passphrase(pass);
        return 1;
    }
    zwriter w = {.f = f, .cctx = ZSTD_createCCtx()};
    ZSTD_CCtx_setParameter(w.cctx, ZSTD_c_compressionLevel, 6);
    ZSTD_CCtx_setParameter(w.cctx, ZSTD_c_checksumFlag, 1);
    backup_ctx c = {.w = &w, .entries = cJSON_CreateArray()};
    bool ok = true;

    for (size_t i = 0; ok && i < ARRAY_LEN(k_db_paths); i++) {
        if (access(k_db_paths[i], R_OK) != 0) continue;
        char snap[PATH_MAX];
        snprintf(snap, sizeof(snap), "%s/%s", stage, strrchr(k_db_paths[i], '/') + 1);
        ok = snapshot_db(k_db_paths[i], snap) && add_entry(&c, k_db_paths[i], snap, "db");
    }
    for (size_t i = 0; ok && i < ARRAY_LEN(k_settings); i++) {
        if (access(k_settings[i], R_OK) == 0) ok = add_entry(&c, k_settings[i], k_settings[i], "settings");
    }
    for (size_t i = 0; ok && i < ARRAY_LEN(k_file_dirs); i++) ok = add_dir(&c, k_file_dirs[i]);

    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "format", FORMAT_NAME);
    cJSON_AddNumberToObject(m, "version", FORMAT_VERSION);
    char now[32];
    time_t t = time(NULL);
    strftime(now, sizeof(now), "%Y-%m-%dT%H:%M:%SZ", gmtime(&t));
    cJSON_AddStringToObject(m, "created_at", now);
    char host[256] = {0};
    gethostname(host, sizeof(host) - 1);
    cJSON_AddStringToObject(m, "source_host", host);
    char *pk = NULL;
    fly_crypto_pubkey_export(&pk);
    cJSON *key = cJSON_AddObjectToObject(m, "signing_key");
    cJSON_AddStringToObject(key, "key_id", fly_crypto_key_id());
    cJSON_AddStringToObject(key, "public_key", pk ? pk : "");
    cwist_free(pk);
    cJSON *counts = cJSON_AddObjectToObject(m, "counts");
    cJSON_AddNumberToObject(counts, "posts", posts);
    cJSON_AddNumberToObject(counts, "signed_posts", valid);
    cJSON_AddNumberToObject(counts, "files", c.files);
    cJSON_AddNumberToObject(counts, "bytes", (double)c.bytes);

    int nsecrets = 0;
    if (ok && with_secrets) {
        cJSON *desc = cJSON_CreateObject();
        size_t sealed_len = 0;
        uint8_t *sealed = seal_secrets(pass, desc, &sealed_len, &nsecrets);
        if (sealed) {
            uint8_t digest[SHA256_DIGEST_LENGTH];
            char hex[65];
            SHA256(sealed, sealed_len, digest);
            hex_of(digest, sizeof(digest), hex);
            cJSON_AddStringToObject(desc, "sha256", hex);
            cJSON_AddNumberToObject(desc, "files", nsecrets);
            tar_put_mem(&w, "secrets.enc", sealed, sealed_len);
            cJSON_AddItemToObject(m, "secrets", desc);
            free(sealed);
        } else {
            cJSON_Delete(desc);
            say("sealing the secrets failed");
            ok = false;
        }
    }
    cJSON_AddItemToObject(m, "entries", c.entries);

    char *manifest = ok ? cJSON_Print(m) : NULL;
    cJSON_Delete(m);
    char *sig = NULL;
    if (manifest && fly_crypto_sign_manifest((const uint8_t *)manifest, strlen(manifest), &sig)) {
        tar_put_mem(&w, "manifest.json", manifest, strlen(manifest));
        tar_put_mem(&w, "manifest.sig", sig, strlen(sig));
        zw_write(&w, k_zero, 512);
        zw_write(&w, k_zero, 512);
    } else {
        ok = false;
    }
    free(manifest);
    cwist_free(sig);
    ok = zw_finish(&w) && ok;
    ok = (fflush(f) == 0 && fsync(fileno(f)) == 0) && ok;
    fclose(f);
    ZSTD_freeCCtx(w.cctx);
    rm_tree(stage);
    free_passphrase(pass);

    if (!ok || rename(tmp_out, out_path) != 0) {
        unlink(tmp_out);
        say("backup FAILED");
        return 1;
    }
    struct stat st;
    stat(out_path, &st);
    say("backup written to %s (%.1f MB)", out_path, st.st_size / 1048576.0);
    say("  %d files, %.1f MB before compression; %d posts, %d with a valid signature",
        c.files, c.bytes / 1048576.0, posts, valid);
    say("  signed by key %s", fly_crypto_key_id());
    if (with_secrets) say("  %d secret files sealed with the passphrase", nsecrets);
    else say("  secrets not included (signing seed, JWT secret, admin.settings, s3.settings); use --with-secrets to carry them");
    return 0;
}

/* ---- --verify ---- */

static int cmd_verify(const char *in_path, const char *pass_file) {
    archive_read_t a;
    bool ok = read_archive(in_path, &a, NULL);
    cJSON *m = ok ? check_archive(&a) : NULL;
    if (!m) {
        archive_read_free(&a);
        say("verify FAILED");
        return 1;
    }
    cJSON *counts = cJSON_GetObjectItem(m, "counts");
    cJSON *created = cJSON_GetObjectItem(m, "created_at");
    say("backup of %s, created %s: %d posts (%d signed), %d files",
        cJSON_IsString(cJSON_GetObjectItem(m, "source_host")) ? cJSON_GetObjectItem(m, "source_host")->valuestring : "?",
        cJSON_IsString(created) ? created->valuestring : "?", jint(counts, "posts", 0),
        jint(counts, "signed_posts", 0), jint(counts, "files", 0));
    int rc = 0;
    cJSON *sec = cJSON_GetObjectItem(m, "secrets");
    if (cJSON_IsObject(sec)) {
        char *pass = read_passphrase(pass_file, false);
        if (pass) {
            cJSON *doc = open_secrets(pass, sec, a.secrets, a.secrets_len);
            free_passphrase(pass);
            if (doc) {
                say("secrets: passphrase ok, %d files", cJSON_GetArraySize(cJSON_GetObjectItem(doc, "files")));
                cJSON_Delete(doc);
            } else {
                say("secrets: wrong passphrase or damaged block");
                rc = 1;
            }
        } else {
            say("secrets: present (give a passphrase to check them)");
        }
    } else {
        say("secrets: not included");
    }
    cJSON_Delete(m);
    archive_read_free(&a);
    say(rc ? "verify FAILED" : "verify ok");
    return rc;
}

/* ---- --restore ---- */

/* Move an existing path aside so a forced restore never destroys data. */
static bool move_aside(const char *path, const char *suffix) {
    if (access(path, F_OK) != 0) return true;
    char dst[PATH_MAX + 64];
    snprintf(dst, sizeof(dst), "%s.pre-restore-%s", path, suffix);
    if (rename(path, dst) != 0) {
        say("cannot move %s aside: %s", path, strerror(errno));
        return false;
    }
    return true;
}

static bool place_file(const char *src, const char *dst) {
    if (!mkdirs_for(dst)) return false;
    if (rename(src, dst) == 0) return true;
    if (errno != EXDEV) return false;
    /* Staging and target on different filesystems: copy. */
    FILE *in = fopen(src, "rb"), *out = fopen(dst, "wb");
    bool ok = in && out;
    static uint8_t buf[IO_CHUNK];
    size_t n;
    while (ok && (n = fread(buf, 1, sizeof(buf), in)) > 0) ok = fwrite(buf, 1, n, out) == n;
    if (in) fclose(in);
    if (out && fclose(out) != 0) ok = false;
    unlink(src);
    return ok;
}

static bool write_secret(const char *path, const uint8_t *data, size_t len) {
    if (!mkdirs_for(path)) return false;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return false;
    bool ok = write(fd, data, len) == (ssize_t)len && fsync(fd) == 0;
    close(fd);
    chmod(path, 0600);
    return ok;
}

static int cmd_restore(const char *in_path, bool force, const char *pass_file) {
    bool existing = access("data/blog.db", F_OK) == 0;
    if (existing && !force) {
        say("this site already has data/blog.db. Restore into a fresh install, or pass --force;");
        say("existing files the archive replaces are then kept as <name>.pre-restore-<time>.");
        return 1;
    }
    if (mkdir("data", 0755) != 0 && errno != EEXIST) {
        say("cannot create data/: %s", strerror(errno));
        return 1;
    }
    char stage[] = "data/.restore-XXXXXX";
    if (!mkdtemp(stage)) {
        say("cannot create a staging directory: %s", strerror(errno));
        return 1;
    }
    char b_dir[PATH_MAX];
    snprintf(b_dir, sizeof(b_dir), "%s/b", stage);
    mkdir(b_dir, 0700);

    archive_read_t a;
    bool ok = read_archive(in_path, &a, stage);
    cJSON *m = ok ? check_archive(&a) : NULL;
    if (!m) {
        archive_read_free(&a);
        rm_tree(stage);
        say("restore ABORTED: nothing was changed");
        return 1;
    }

    /* Open the secrets before touching anything, so a wrong passphrase
     * does not leave a half-restored site. */
    cJSON *secrets = NULL;
    cJSON *sec_desc = cJSON_GetObjectItem(m, "secrets");
    if (cJSON_IsObject(sec_desc)) {
        char *pass = read_passphrase(pass_file, false);
        if (pass) {
            secrets = open_secrets(pass, sec_desc, a.secrets, a.secrets_len);
            free_passphrase(pass);
            if (!secrets) {
                say("wrong passphrase for the secrets; restore ABORTED, nothing was changed");
                cJSON_Delete(m);
                archive_read_free(&a);
                rm_tree(stage);
                return 1;
            }
        } else {
            say("the archive carries secrets but no passphrase was given; restoring without them");
        }
    }

    char suffix[32];
    time_t t = time(NULL);
    strftime(suffix, sizeof(suffix), "%Y%m%d%H%M%S", gmtime(&t));
    int placed = 0, failed = 0;
    cJSON *e = NULL;
    cJSON_ArrayForEach(e, cJSON_GetObjectItem(m, "entries")) {
        const char *path = cJSON_GetObjectItem(e, "path")->valuestring;
        const char *blob = cJSON_GetObjectItem(e, "blob")->valuestring;
        char src[PATH_MAX];
        snprintf(src, sizeof(src), "%s/%s", stage, blob + 2);
        bool is_db = cJSON_IsString(cJSON_GetObjectItem(e, "kind")) &&
                     !strcmp(cJSON_GetObjectItem(e, "kind")->valuestring, "db");
        bool step_ok = true;
        if (existing) {
            step_ok = move_aside(path, suffix);
            if (step_ok && is_db) {
                char side[PATH_MAX];
                snprintf(side, sizeof(side), "%s-wal", path);
                step_ok = move_aside(side, suffix);
                snprintf(side, sizeof(side), "%s-shm", path);
                step_ok = step_ok && move_aside(side, suffix);
            }
        } else if (is_db) {
            /* Leftovers of an empty install must not be replayed onto the
             * restored database. */
            char side[PATH_MAX];
            snprintf(side, sizeof(side), "%s-wal", path);
            unlink(side);
            snprintf(side, sizeof(side), "%s-shm", path);
            unlink(side);
        }
        if (step_ok && place_file(src, path)) placed++;
        else { failed++; say("could not restore %s", path); }
    }

    int nsecrets = 0;
    cJSON *sf = NULL;
    cJSON *secret_files = secrets ? cJSON_GetObjectItem(secrets, "files") : NULL;
    cJSON_ArrayForEach(sf, secret_files) {
        cJSON *path = cJSON_GetObjectItem(sf, "path");
        cJSON *data = cJSON_GetObjectItem(sf, "data");
        size_t len = 0;
        uint8_t *raw = cJSON_IsString(data) ? b64_decode(data->valuestring, &len) : NULL;
        if (!cJSON_IsString(path) || !secret_path_ok(path->valuestring) || !raw) {
            failed++;
            free(raw);
            continue;
        }
        if ((existing && !move_aside(path->valuestring, suffix)) || !write_secret(path->valuestring, raw, len)) {
            failed++;
            say("could not restore %s", path->valuestring);
        } else {
            nsecrets++;
        }
        OPENSSL_cleanse(raw, len);
        free(raw);
    }
    if (secrets) cJSON_Delete(secrets);
    archive_read_free(&a);
    rm_tree(stage);

    /* Check what landed: database integrity, then every post signature
     * against the public keys the database carries. */
    sqlite3 *conn = NULL;
    int posts = 0, valid = 0;
    bool db_ok = false;
    if (sqlite3_open_v2("data/blog.db", &conn, SQLITE_OPEN_READWRITE, NULL) == SQLITE_OK) {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(conn, "PRAGMA integrity_check", -1, &st, NULL) == SQLITE_OK &&
            sqlite3_step(st) == SQLITE_ROW) {
            const char *res = (const char *)sqlite3_column_text(st, 0);
            db_ok = res && !strcmp(res, "ok");
        }
        sqlite3_finalize(st);
        /* Verification only: no seed is created here. A restored seed is
         * used when present; otherwise the stored public keys suffice. */
        if (access(SEED_PATH, R_OK) == 0) fly_crypto_init(SEED_PATH);
        pqc_keys_sync_conn(conn, false);
        count_signatures(conn, &posts, &valid);
    }
    sqlite3_close(conn);

    say("restored %d files", placed);
    if (failed) say("%d items could not be restored", failed);
    say("  database integrity: %s", db_ok ? "ok" : "FAILED");
    say("  post signatures: %d of %d verify", valid, posts);
    if (cJSON_IsObject(sec_desc)) {
        say("  secrets: %d restored", nsecrets);
    }
    if (access(SEED_PATH, F_OK) != 0) {
        say("  no signing seed restored: the server will create a new key on start;");
        say("  earlier posts keep verifying with the public keys stored in the database.");
    }
    if (existing) say("  replaced files were kept with the suffix .pre-restore-%s", suffix);
    cJSON_Delete(m);
    return (failed || !db_ok) ? 1 : 0;
}

/* ---- --export-markdown / --verify-markdown ----
 *
 * One Markdown file per post with YAML front matter, the attachments next
 * to them, and the public keys, for moving to a static site generator or
 * for checking signatures outside fly.board. Front matter values are JSON
 * strings, which YAML reads as double-quoted scalars.
 *
 * A signature covers title + "\n" + body exactly as stored. By default the
 * body after the front matter is that stored body. With --rewrite-links
 * the body's attachment links point at the exported files instead, and the
 * signed original goes to originals/<slug>.md (named by pqc_signed_body). */

static bool copy_file(const char *src, const char *dst) {
    if (!mkdirs_for(dst)) return false;
    FILE *in = fopen(src, "rb");
    if (!in) return false;
    FILE *out = fopen(dst, "wb");
    if (!out) { fclose(in); return false; }
    static uint8_t buf[IO_CHUNK];
    size_t n;
    bool ok = true;
    while (ok && (n = fread(buf, 1, sizeof(buf), in)) > 0) ok = fwrite(buf, 1, n, out) == n;
    fclose(in);
    if (fclose(out) != 0) ok = false;
    return ok;
}

static bool write_text(const char *path, const char *text, size_t len) {
    if (!mkdirs_for(path)) return false;
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    bool ok = fwrite(text, 1, len, f) == len;
    if (fclose(f) != 0) ok = false;
    return ok;
}

static void yaml_str(FILE *f, const char *key, const char *value) {
    cJSON *v = cJSON_CreateString(value ? value : "");
    char *js = cJSON_PrintUnformatted(v);
    fprintf(f, "%s: %s\n", key, js ? js : "\"\"");
    free(js);
    cJSON_Delete(v);
}

/* "YYYY-MM-DD HH:MM:SS" (UTC) -> "YYYY-MM-DDTHH:MM:SSZ" */
static void iso_time(const char *t, char out[32]) {
    if (t && strlen(t) >= 19) snprintf(out, 32, "%.10sT%.8sZ", t, t + 11);
    else snprintf(out, 32, "1970-01-01T00:00:00Z");
}

/* A filename safe as one path segment. */
static void safe_name(const char *in, char *out, size_t out_size) {
    size_t o = 0;
    for (const char *p = in ? in : ""; *p && o + 1 < out_size; p++) {
        char c = *p;
        /* Spaces too: Markdown link targets end at whitespace. */
        out[o++] = (c == '/' || c == '\\' || c == ' ' || c == '(' || c == ')' || (unsigned char)c < 0x20) ? '_' : c;
    }
    out[o] = '\0';
    if (o == 0 || out[0] == '.') out[0] = '_';
    if (o == 0) out[1] = '\0';
}

typedef struct {
    sqlite3 *db;
    const char *out_dir;
    cJSON *links;      /* original URL -> exported path */
    int files_copied;
} export_ctx;

/* Exported path of attachment @p id ("files/<id>/<name>"), copying it on
 * first use. */
static bool export_attachment(export_ctx *x, int id, char *rel, size_t rel_size) {
    sqlite3_stmt *st = NULL;
    bool ok = false;
    if (sqlite3_prepare_v2(x->db, "SELECT filename, file_path FROM files WHERE id=?", -1, &st, NULL) != SQLITE_OK) return false;
    sqlite3_bind_int(st, 1, id);
    if (sqlite3_step(st) == SQLITE_ROW) {
        char name[256];
        safe_name((const char *)sqlite3_column_text(st, 0), name, sizeof(name));
        const char *src = (const char *)sqlite3_column_text(st, 1);
        snprintf(rel, rel_size, "files/%d/%s", id, name);
        char dst[PATH_MAX];
        snprintf(dst, sizeof(dst), "%s/%s", x->out_dir, rel);
        ok = access(dst, F_OK) == 0 || (src && restore_path_ok(src) && copy_file(src, dst));
        if (ok && access(dst, F_OK) == 0) {
            char url[64];
            snprintf(url, sizeof(url), "/file/download/%d", id);
            if (!cJSON_GetObjectItem(x->links, url)) {
                cJSON_AddStringToObject(x->links, url, rel);
                x->files_copied++;
            }
        }
    }
    sqlite3_finalize(st);
    return ok;
}

static bool link_end(char c) {
    return !c || c == ')' || c == '"' || c == '\'' || c == ' ' || c == '\n' || c == '\r' || c == '<' ||
           c == '>' || c == '?' || c == '#' || c == ']';
}

/* Body with /file/download|preview/<id> and /assets/uploads/<path> links
 * pointing at the exported copies. */
static char *rewrite_links(export_ctx *x, const char *body) {
    size_t cap = strlen(body) * 2 + 1024, o = 0;
    char *out = malloc(cap);
    if (!out) return NULL;
    const char *p = body;
    while (*p) {
        char rel[PATH_MAX] = {0};
        size_t consumed = 0;
        if (!strncmp(p, "/file/download/", 15) || !strncmp(p, "/file/preview/", 14)) {
            const char *num = p + (p[6] == 'd' ? 15 : 14);
            char *end = NULL;
            long id = strtol(num, &end, 10);
            if (end != num && id > 0 && export_attachment(x, (int)id, rel, sizeof(rel))) {
                while (*end && !link_end(*end)) end++;
                if (*end == '?') while (*end && !(link_end(*end) && *end != '?')) end++; /* drop ?preview=1 etc. */
                consumed = (size_t)(end - p);
            }
        } else if (!strncmp(p, "/assets/uploads/", 16)) {
            const char *q = p + 16;
            size_t n = 0;
            while (!link_end(q[n])) n++;
            char sub[PATH_MAX];
            if (n > 0 && n < 900) {
                snprintf(sub, sizeof(sub), "public/uploads/%.*s", (int)n, q);
                snprintf(rel, sizeof(rel), "files/uploads/%.*s", (int)n, q);
                char dst[PATH_MAX + 512];
                snprintf(dst, sizeof(dst), "%s/%s", x->out_dir, rel);
                if (restore_path_ok(sub) && (access(dst, F_OK) == 0 || copy_file(sub, dst))) {
                    char url[PATH_MAX];
                    snprintf(url, sizeof(url), "/assets/uploads/%.*s", (int)n, q);
                    if (!cJSON_GetObjectItem(x->links, url)) {
                        cJSON_AddStringToObject(x->links, url, rel);
                        x->files_copied++;
                    }
                    consumed = 16 + n;
                } else {
                    rel[0] = '\0';
                }
            }
        }
        if (consumed) {
            size_t rl = strlen(rel) + 1;
            if (o + rl + 1 >= cap) { cap = cap * 2 + rl; char *g = realloc(out, cap); if (!g) { free(out); return NULL; } out = g; }
            out[o++] = '/';
            memcpy(out + o, rel, rl - 1);
            o += rl - 1;
            p += consumed;
        } else {
            if (o + 2 >= cap) { cap *= 2; char *g = realloc(out, cap); if (!g) { free(out); return NULL; } out = g; }
            out[o++] = *p++;
        }
    }
    out[o] = '\0';
    return out;
}

static int cmd_export_markdown(const char *out_dir, bool rewrite) {
    if (!output_path_safe(out_dir)) return 1;
    DIR *d = opendir(out_dir);
    if (d) {
        struct dirent *e;
        int n = 0;
        while ((e = readdir(d))) if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) n++;
        closedir(d);
        if (n) { say("%s is not empty", out_dir); return 1; }
    } else if (mkdir(out_dir, 0700) != 0) {
        say("cannot create %s: %s", out_dir, strerror(errno));
        return 1;
    }
    export_ctx x = {.out_dir = out_dir, .links = cJSON_CreateObject()};
    if (sqlite3_open_v2("data/blog.db", &x.db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        say("cannot open data/blog.db; run from the site root");
        sqlite3_close(x.db);
        return 1;
    }
    sqlite3_busy_timeout(x.db, 10000);

    /* Public keys, so signatures can be checked without this site. */
    cJSON *keys = cJSON_CreateArray();
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(x.db, "SELECT key_id, public_key, created_at FROM pqc_keys ORDER BY created_at", -1, &st, NULL) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            cJSON *k = cJSON_CreateObject();
            cJSON_AddStringToObject(k, "key_id", (const char *)sqlite3_column_text(st, 0));
            cJSON_AddStringToObject(k, "algorithm", "ML-DSA-65 (FIPS 204)");
            cJSON_AddStringToObject(k, "context", "fly.board/post/v1");
            cJSON_AddStringToObject(k, "public_key", (const char *)sqlite3_column_text(st, 1));
            cJSON_AddItemToArray(keys, k);
        }
        sqlite3_finalize(st);
    }

    const char *sql =
        "SELECT p.id, p.title, p.slug, p.content, p.summary, p.pqc_signature, p.created_at, p.updated_at,"
        " p.status, u.username, b.slug, b.name,"
        " (SELECT group_concat(t.name, char(31)) FROM post_tags pt JOIN tags t ON t.id=pt.tag_id WHERE pt.post_id=p.id)"
        " FROM posts p LEFT JOIN users u ON u.id=p.user_id LEFT JOIN boards b ON b.id=p.board_id ORDER BY p.created_at";
    int posts = 0, signed_posts = 0;
    bool ok = sqlite3_prepare_v2(x.db, sql, -1, &st, NULL) == SQLITE_OK;
    while (ok && sqlite3_step(st) == SQLITE_ROW) {
        int id = sqlite3_column_int(st, 0);
        const char *title = (const char *)sqlite3_column_text(st, 1);
        const char *slug = (const char *)sqlite3_column_text(st, 2);
        const char *content = (const char *)sqlite3_column_text(st, 3);
        const char *summary = (const char *)sqlite3_column_text(st, 4);
        const char *sig = (const char *)sqlite3_column_text(st, 5);
        const char *created = (const char *)sqlite3_column_text(st, 6);
        const char *updated = (const char *)sqlite3_column_text(st, 7);
        const char *status = (const char *)sqlite3_column_text(st, 8);
        const char *author = (const char *)sqlite3_column_text(st, 9);
        const char *board_slug = (const char *)sqlite3_column_text(st, 10);
        const char *board_name = (const char *)sqlite3_column_text(st, 11);
        const char *tags = (const char *)sqlite3_column_text(st, 12);
        content = content ? content : "";
        char sname[256];
        safe_name(slug, sname, sizeof(sname));

        /* Every attachment of the post is exported, linked from the body or not. */
        sqlite3_stmt *fs = NULL;
        if (sqlite3_prepare_v2(x.db, "SELECT id FROM files WHERE post_id=?", -1, &fs, NULL) == SQLITE_OK) {
            sqlite3_bind_int(fs, 1, id);
            char rel[PATH_MAX];
            while (sqlite3_step(fs) == SQLITE_ROW) export_attachment(&x, sqlite3_column_int(fs, 0), rel, sizeof(rel));
            sqlite3_finalize(fs);
        }
        char *body = rewrite ? rewrite_links(&x, content) : strdup(content);
        if (!body) { ok = false; break; }

        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/posts/%s.md", out_dir, sname);
        if (!mkdirs_for(path)) { free(body); ok = false; break; }
        FILE *f = fopen(path, "wb");
        if (!f) { free(body); ok = false; break; }
        char ts[32];
        fputs("---\n", f);
        yaml_str(f, "title", title);
        yaml_str(f, "slug", slug);
        iso_time(created, ts);
        fprintf(f, "date: %s\n", ts);
        iso_time(updated, ts);
        fprintf(f, "lastmod: %s\n", ts);
        fprintf(f, "draft: %s\n", status && !strcmp(status, "draft") ? "true" : "false");
        if (author) yaml_str(f, "author", author);
        if (board_slug) yaml_str(f, "board", board_slug);
        if (board_name) {
            fputs("categories: [", f);
            cJSON *v = cJSON_CreateString(board_name);
            char *js = cJSON_PrintUnformatted(v);
            fputs(js ? js : "\"\"", f);
            free(js);
            cJSON_Delete(v);
            fputs("]\n", f);
        }
        fputs("tags: [", f);
        if (tags) {
            char *copy = strdup(tags);
            int i = 0;
            for (char *save = NULL, *t = strtok_r(copy, "\x1f", &save); t; t = strtok_r(NULL, "\x1f", &save)) {
                cJSON *v = cJSON_CreateString(t);
                char *js = cJSON_PrintUnformatted(v);
                fprintf(f, "%s%s", i++ ? ", " : "", js ? js : "\"\"");
                free(js);
                cJSON_Delete(v);
            }
            free(copy);
        }
        fputs("]\n", f);
        if (summary && summary[0]) yaml_str(f, "summary", summary);
        if (sig && sig[0]) {
            yaml_str(f, "pqc_signature", sig);
            signed_posts++;
            if (rewrite) {
                char orig_rel[PATH_MAX];
                snprintf(orig_rel, sizeof(orig_rel), "originals/%s.md", sname);
                yaml_str(f, "pqc_signed_body", orig_rel);
                char orig[PATH_MAX + 512];
                snprintf(orig, sizeof(orig), "%s/%s", out_dir, orig_rel);
                if (!write_text(orig, content, strlen(content))) ok = false;
            }
        }
        fputs("---\n", f);
        fputs(body, f);
        if (fclose(f) != 0) ok = false;
        free(body);
        posts++;
    }
    if (st) sqlite3_finalize(st);
    sqlite3_close(x.db);

    char path[PATH_MAX];
    char *js = cJSON_Print(keys);
    snprintf(path, sizeof(path), "%s/pqc-keys.json", out_dir);
    ok = ok && js && write_text(path, js, strlen(js));
    free(js);
    js = cJSON_Print(x.links);
    snprintf(path, sizeof(path), "%s/links.json", out_dir);
    ok = ok && js && write_text(path, js, strlen(js));
    free(js);
    static const char readme[] =
        "# fly.board export\n\n"
        "- `posts/<slug>.md`: one post each, YAML front matter (Hugo/Jekyll style) and the Markdown body.\n"
        "- `files/`: attachments; `links.json` maps each original site URL to its file here.\n"
        "- `pqc-keys.json`: every public key that signed posts on the site.\n\n"
        "## Signatures\n\n"
        "`pqc_signature` is `<key id>:<base64 signature>`: ML-DSA-65 (FIPS 204) with the context string\n"
        "`fly.board/post/v1`, over the bytes of the title, a newline, and the body exactly as stored.\n"
        "The body is everything after the closing `---` line, or the file named by `pqc_signed_body`\n"
        "when links were rewritten. The key id is the first 16 hex digits of SHA-256 over the public key.\n\n"
        "`fly_board --verify-markdown <this directory>` checks every post.\n";
    snprintf(path, sizeof(path), "%s/README.md", out_dir);
    ok = ok && write_text(path, readme, sizeof(readme) - 1);
    cJSON_Delete(keys);
    cJSON_Delete(x.links);
    if (!ok) {
        say("export FAILED");
        return 1;
    }
    say("exported %d posts (%d signed) and %d files to %s%s", posts, signed_posts, x.files_copied, out_dir,
        rewrite ? " with rewritten links" : "");
    return 0;
}

static int cmd_verify_markdown(const char *dir) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/pqc-keys.json", dir);
    uint8_t *raw = NULL;
    size_t len = 0;
    cJSON *keys = read_file(path, &raw, &len, MAX_MANIFEST) ? cJSON_ParseWithLength((char *)raw, len) : NULL;
    free(raw);
    int nkeys = 0;
    cJSON *k = NULL;
    cJSON_ArrayForEach(k, keys) {
        cJSON *pk = cJSON_GetObjectItem(k, "public_key");
        if (cJSON_IsString(pk) && fly_crypto_add_public_key(pk->valuestring)) nkeys++;
    }
    if (keys) cJSON_Delete(keys);
    if (!nkeys) {
        say("no usable keys in %s", path);
        return 1;
    }
    snprintf(path, sizeof(path), "%s/posts", dir);
    struct dirent **list = NULL;
    int n = scandir(path, &list, NULL, alphasort);
    if (n < 0) {
        say("cannot read %s", path);
        return 1;
    }
    int total = 0, valid = 0, unsigned_posts = 0, bad = 0;
    for (int i = 0; i < n; i++) {
        const char *name = list[i]->d_name;
        size_t nl = strlen(name);
        if (nl < 4 || strcmp(name + nl - 3, ".md") != 0) { free(list[i]); continue; }
        char file[PATH_MAX + 512];
        snprintf(file, sizeof(file), "%s/%s", path, name);
        uint8_t *doc = NULL;
        size_t doc_len = 0;
        total++;
        if (!read_file(file, &doc, &doc_len, 256u * 1024 * 1024) || strncmp((char *)doc, "---\n", 4) != 0) {
            say("  %s: no front matter", name);
            bad++;
            free(doc);
            free(list[i]);
            continue;
        }
        char *fm_end = strstr((char *)doc + 4, "\n---\n");
        char *title = NULL, *sig = NULL, *signed_body = NULL;
        if (fm_end) {
            *fm_end = '\0';
            for (char *save = NULL, *line = strtok_r((char *)doc + 4, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
                char *colon = strstr(line, ": ");
                if (!colon) continue;
                *colon = '\0';
                cJSON *v = cJSON_Parse(colon + 2);
                if (cJSON_IsString(v)) {
                    if (!strcmp(line, "title")) title = strdup(v->valuestring);
                    else if (!strcmp(line, "pqc_signature")) sig = strdup(v->valuestring);
                    else if (!strcmp(line, "pqc_signed_body")) signed_body = strdup(v->valuestring);
                }
                cJSON_Delete(v);
            }
        }
        const char *body = fm_end ? fm_end + 5 : NULL;
        uint8_t *orig = NULL;
        size_t orig_len = 0;
        if (signed_body) {
            char opath[PATH_MAX + 512];
            snprintf(opath, sizeof(opath), "%s/%s", dir, signed_body);
            body = (!strstr(signed_body, "..") && read_file(opath, &orig, &orig_len, 256u * 1024 * 1024)) ? (char *)orig : NULL;
        }
        if (!sig) {
            unsigned_posts++;
        } else if (!title || !body) {
            say("  %s: unreadable", name);
            bad++;
        } else {
            size_t mlen = strlen(title) + 1 + strlen(body);
            char *msg = malloc(mlen + 1);
            snprintf(msg, mlen + 1, "%s\n%s", title, body);
            if (fly_crypto_verify((uint8_t *)msg, mlen, sig)) valid++;
            else { say("  %s: signature does NOT verify", name); bad++; }
            free(msg);
        }
        free(title); free(sig); free(signed_body); free(orig); free(doc);
        free(list[i]);
    }
    free(list);
    say("%d posts: %d verified, %d unsigned, %d failed (%d keys)", total, valid, unsigned_posts, bad, nkeys);
    return bad ? 1 : 0;
}

int fly_backup_cli(int argc, char **argv) {
    if (argc < 2) return -1;
    const char *cmd = argv[1];
    if (!strcmp(cmd, "--export-markdown")) {
        if (argc < 3) { say("usage: fly_board --export-markdown <dir> [--rewrite-links]"); return 2; }
        bool rewrite = argc > 3 && !strcmp(argv[3], "--rewrite-links");
        if (argc > (rewrite ? 4 : 3)) { say("unknown option %s", argv[rewrite ? 4 : 3]); return 2; }
        return cmd_export_markdown(argv[2], rewrite);
    }
    if (!strcmp(cmd, "--verify-markdown")) {
        if (argc != 3) { say("usage: fly_board --verify-markdown <dir>"); return 2; }
        return cmd_verify_markdown(argv[2]);
    }
    bool is_backup = !strcmp(cmd, "--backup"), is_verify = !strcmp(cmd, "--verify"), is_restore = !strcmp(cmd, "--restore");
    if (!is_backup && !is_verify && !is_restore) return -1;
    if (argc < 3 || argv[2][0] == '-') {
        say("usage: fly_board %s <archive> [options]", cmd);
        return 2;
    }
    bool with_secrets = false, force = false;
    const char *pass_file = NULL;
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--with-secrets") && is_backup) with_secrets = true;
        else if (!strcmp(argv[i], "--force") && is_restore) force = true;
        else if (!strcmp(argv[i], "--passphrase-file") && i + 1 < argc) pass_file = argv[++i];
        else {
            say("unknown option %s", argv[i]);
            return 2;
        }
    }
    if (is_backup) return cmd_backup(argv[2], with_secrets, pass_file);
    if (is_verify) return cmd_verify(argv[2], pass_file);
    return cmd_restore(argv[2], force, pass_file);
}

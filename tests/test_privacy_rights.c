#define _POSIX_C_SOURCE 200809L
#include "auth/auth.h"
#include "db/db.h"
#include "db/db_internal.h"
#include "utils/privacy.h"
#include <cwist/net/http/http.h>
#include <cwist/core/mem/alloc.h>
#include <zstd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;
#define CHECK(c, name) do { if (c) printf("PASS: %s\n", name); \
    else { printf("FAIL: %s (line %d)\n", name, __LINE__); failures++; } } while (0)

static cwist_db g_db;
static char root[] = "/tmp/flyboard-privacy-XXXXXX";

static int user_id(const char *name) {
    cJSON *user = db_user_get_by_username(&g_db, name);
    int id = user ? cJSON_GetObjectItem(user, "id")->valueint : 0;
    cJSON_Delete(user);
    return id;
}

static int scalar(sqlite3 *db, const char *sql, int uid) {
    sqlite3_stmt *stmt = NULL;
    int value = -1;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, uid);
        if (sqlite3_step(stmt) == SQLITE_ROW) value = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return value;
}

static unsigned char *decompress(const char *path, size_t *out_len) {
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    ZSTD_DStream *stream = ZSTD_createDStream();
    unsigned char input[16384], *output = NULL;
    size_t capacity = 0, used = 0;
    bool ok = stream && !ZSTD_isError(ZSTD_initDStream(stream));
    while (ok) {
        size_t got = fread(input, 1, sizeof(input), file);
        ZSTD_inBuffer in = {.src = input, .size = got, .pos = 0};
        while (in.pos < in.size) {
            if (used + 32768 > capacity) {
                capacity = capacity ? capacity * 2 : 65536;
                unsigned char *larger = realloc(output, capacity);
                if (!larger) { ok = false; break; }
                output = larger;
            }
            ZSTD_outBuffer out = {.dst = output + used, .size = capacity - used, .pos = 0};
            size_t rc = ZSTD_decompressStream(stream, &out, &in);
            if (ZSTD_isError(rc)) { ok = false; break; }
            used += out.pos;
        }
        if (got == 0) break;
    }
    fclose(file);
    ZSTD_freeDStream(stream);
    if (!ok) { free(output); return NULL; }
    *out_len = used;
    return output;
}

static size_t tar_entry_size(const unsigned char *header) {
    char text[13];
    memcpy(text, header + 124, 12);
    text[12] = '\0';
    return (size_t)strtoull(text, NULL, 8);
}

static bool archive_has(const unsigned char *tar, size_t len, const char *entry, const char *needle) {
    size_t offset = 0;
    while (offset + 512 <= len) {
        const unsigned char *header = tar + offset;
        if (header[0] == 0) return false;
        size_t size = tar_entry_size(header);
        if (offset + 512 + size > len) return false;
        if (strcmp((const char *)header, entry) == 0) {
            if (!needle) return true;
            if (strlen(needle) > size) return false;
            for (size_t i = 0; i + strlen(needle) <= size; i++)
                if (memcmp(header + 512 + i, needle, strlen(needle)) == 0) return true;
            return false;
        }
        offset += 512 + ((size + 511) / 512) * 512;
    }
    return false;
}

static bool verify_session(cwist_http_request *request) {
    int uid = 0;
    char role[32];
    return auth_jwt_verify_from_request(request, &uid, role, sizeof(role));
}

int main(void) {
    if (!mkdtemp(root)) return 1;
    char old_cwd[4096], path[4096];
    if (!getcwd(old_cwd, sizeof(old_cwd)) || chdir(root) != 0) return 1;
    mkdir("data", 0700);
    mkdir("public", 0700);
    mkdir("public/uploads", 0700);
    snprintf(path, sizeof(path), "%s/main.db", root);
    setenv("FLY_DB_LOCATION", path, 1);
    snprintf(path, sizeof(path), "%s/data/exports", root);
    setenv("FLY_EXPORT_DIR", path, 1);
    sqlite3 *conn = NULL;
    CHECK(sqlite3_open("main.db", &conn) == SQLITE_OK, "open main database");
    g_db.conn = conn;
    CHECK(db_init(&g_db), "migrate privacy schema");
    CHECK(db_comment_init("data/comments.db"), "open comments database");
    CHECK(db_user_create(&g_db, "alice", "alice@example.com", "alice-hash"), "create alice");
    CHECK(db_user_create(&g_db, "bob", "bob@example.com", "bob-hash"), "create bob");
    int alice = user_id("alice"), bob = user_id("bob");
    CHECK(db_board_create(&g_db, "General", "general", "", false, 0, 0, 0), "create board");
    int alice_post = db_post_create(&g_db, 1, alice, "Alice secret", "alice-secret", "ALICE_POST_MARKER", "", NULL, 0, 1, "");
    int bob_post = db_post_create(&g_db, 1, bob, "Bob private", "bob-private", "BOB_POST_MARKER", "", NULL, 0, 1, "");
    CHECK(alice_post > 0 && bob_post > 0, "create owned posts");
    int parent = db_comment_create(&g_db, "post", alice_post, alice, "", 0, "ALICE_COMMENT_MARKER");
    int child = db_comment_create(&g_db, "post", alice_post, bob, "", parent, "BOB_COMMENT_MARKER");
    CHECK(parent > 0 && child > 0, "create threaded comments");
    FILE *upload = fopen("public/uploads/alice.txt", "wb");
    fputs("ALICE_UPLOAD_MARKER", upload);
    fclose(upload);
    upload = fopen("public/uploads/bob.txt", "wb");
    fputs("BOB_UPLOAD_MARKER", upload);
    fclose(upload);
    CHECK(db_file_create_volume(&g_db, alice_post, alice, "alice.txt", "text/plain", "public/uploads/alice.txt", 19), "create alice upload");
    CHECK(db_file_create_volume(&g_db, bob_post, bob, "bob.txt", "text/plain", "public/uploads/bob.txt", 17), "create bob upload");
    CHECK(db_user_update_totp(&g_db, alice, "ALICE_TOTP_SECRET", true), "set alice TOTP");
    CHECK(db_user_update_totp_recovery(&g_db, alice, "ALICE_RECOVERY_SECRET"), "set alice recovery");
    CHECK(db_email_token_create(&g_db, alice, "ALICE_EMAIL_TOKEN", 4102444800L), "set alice email token");
    sqlite3_exec(conn, "INSERT INTO emails(owner_id,folder,body_text) VALUES (1,'INBOX','ALICE_MAIL_MARKER')", NULL, NULL, NULL);

    CHECK(privacy_export_build(conn, db_comment_conn(), alice, "direct.tar.zst"), "build direct archive");
    struct stat archive_stat;
    CHECK(stat("direct.tar.zst", &archive_stat) == 0 && (archive_stat.st_mode & 0777) == 0600,
          "archive is owner-only");
    size_t tar_len = 0;
    unsigned char *tar = decompress("direct.tar.zst", &tar_len);
    CHECK(tar != NULL && tar_len > 1024, "decompress streaming tar.zst");
    CHECK(archive_has(tar, tar_len, "profile.json", "alice@example.com"), "archive contains own profile");
    CHECK(archive_has(tar, tar_len, "posts/00000001.json", "ALICE_POST_MARKER"), "archive contains own secret post");
    CHECK(archive_has(tar, tar_len, "comments/00000001.json", "ALICE_COMMENT_MARKER"), "archive contains own comment");
    CHECK(archive_has(tar, tar_len, "uploads/00000001-alice.txt", "ALICE_UPLOAD_MARKER"), "archive contains owned upload bytes");
    CHECK(!archive_has(tar, tar_len, "posts/00000001.json", "BOB_POST_MARKER"), "archive excludes another user's post");
    CHECK(!archive_has(tar, tar_len, "comments/00000001.json", "BOB_COMMENT_MARKER"), "archive excludes another user's comment");
    bool leaked = false;
    static const char *const secrets[] = {"ALICE_TOTP_SECRET", "ALICE_RECOVERY_SECRET", "ALICE_EMAIL_TOKEN", "BOB_UPLOAD_MARKER", "alice-hash"};
    for (size_t i = 0; i < sizeof(secrets) / sizeof(secrets[0]); i++) {
        for (size_t p = 0; p + strlen(secrets[i]) <= tar_len; p++)
            if (memcmp(tar + p, secrets[i], strlen(secrets[i])) == 0) leaked = true;
    }
    CHECK(!leaked, "archive excludes credentials and other users' upload bytes");
    free(tar);

    CHECK(auth_jwt_init("data/jwt-secret"), "initialize JWT secret");
    char *token = auth_jwt_issue(alice, "alice", "user");
    cwist_http_request request = {0};
    request.db = &g_db;
    char authorization[8192];
    snprintf(authorization, sizeof(authorization), "Bearer %s", token);
    cwist_http_header_add(&request.headers, "Authorization", authorization);
    CHECK(verify_session(&request), "active account session accepted");

    char id[PRIVACY_EXPORT_ID_LEN + 1];
    CHECK(privacy_export_start(alice, id), "queue asynchronous export");
    cJSON *status = NULL;
    for (int i = 0; i < 100; i++) {
        status = privacy_export_status(alice, id);
        const char *state = status ? cJSON_GetStringValue(cJSON_GetObjectItem(status, "state")) : NULL;
        if (state && strcmp(state, "ready") == 0) break;
        cJSON_Delete(status);
        status = NULL;
        usleep(50000);
    }
    const char *state = status ? cJSON_GetStringValue(cJSON_GetObjectItem(status, "state")) : NULL;
    CHECK(state && strcmp(state, "ready") == 0, "asynchronous export becomes ready");
    cJSON *other_status = privacy_export_status(bob, id);
    CHECK(other_status == NULL, "other account cannot read export status");
    cJSON_Delete(other_status);
    CHECK(!privacy_export_download_path(bob, id, path, sizeof(path)), "other account cannot resolve download");
    CHECK(privacy_export_download_path(alice, id, path, sizeof(path)), "owner can resolve download");
    cJSON_Delete(status);

    CHECK(db_user_ghost(&g_db, alice), "ghost account");
    cJSON *ghost = db_user_get_by_id(&g_db, alice);
    CHECK(ghost && cJSON_GetObjectItem(ghost, "ghosted")->valueint == 1 &&
          cJSON_GetObjectItem(ghost, "active")->valueint == 0, "ghost is inactive");
    CHECK(ghost && strcmp(cJSON_GetObjectItem(ghost, "password_hash")->valuestring, "!") == 0 &&
          strcmp(cJSON_GetObjectItem(ghost, "totp_secret")->valuestring, "") == 0 &&
          strcmp(cJSON_GetObjectItem(ghost, "totp_recovery")->valuestring, "") == 0, "credentials and TOTP removed");
    CHECK(ghost && strcmp(cJSON_GetObjectItem(ghost, "email")->valuestring, "alice@example.com") != 0 &&
          strcmp(cJSON_GetObjectItem(ghost, "bio")->valuestring, "") == 0, "private profile removed");
    cJSON_Delete(ghost);
    CHECK(!verify_session(&request), "ghosted account JWT rejected immediately");
    CHECK(db_post_get_by_id(&g_db, alice_post) != NULL, "ghosted post preserved");
    cJSON *comment = db_comment_get_by_id(&g_db, parent);
    CHECK(comment && strcmp(cJSON_GetObjectItem(comment, "content")->valuestring, "ALICE_COMMENT_MARKER") == 0 &&
          strcmp(cJSON_GetObjectItem(comment, "author_name")->valuestring, "Deleted user") == 0, "comment content preserved with ghost label");
    cJSON_Delete(comment);
    comment = db_comment_get_by_id(&g_db, child);
    CHECK(comment && cJSON_GetObjectItem(comment, "parent_id")->valueint == parent, "comment thread parent preserved");
    cJSON_Delete(comment);
    CHECK(scalar(conn, "SELECT COUNT(*) FROM files WHERE user_id=?", alice) == 0, "owned upload rows removed");
    CHECK(scalar(conn, "SELECT COUNT(*) FROM files WHERE user_id=?", bob) == 1, "other user's uploads preserved");
    CHECK(scalar(conn, "SELECT COUNT(*) FROM email_tokens WHERE user_id=?", alice) == 0, "email tokens removed");
    CHECK(scalar(conn, "SELECT COUNT(*) FROM emails WHERE owner_id=?", alice) == 0, "private webmail removed");
    cJSON *other_post = db_post_get_by_id(&g_db, bob_post);
    CHECK(other_post != NULL, "other user's post preserved");
    cJSON_Delete(other_post);
    privacy_export_revoke_user(alice);
    status = privacy_export_status(alice, id);
    CHECK(status == NULL, "account deletion revokes completed exports");
    cJSON_Delete(status);
    CHECK(!privacy_export_download_path(alice, id, path, sizeof(path)), "revoked archive cannot be downloaded");

    cwist_free(token);
    db_comment_close_thread();
    db_comment_close();
    sqlite3_close(conn);
    g_db.conn = NULL;
    chdir(old_cwd);
    printf("%s\n", failures ? "SOME FAILED" : "ALL PASS");
    return failures != 0;
}

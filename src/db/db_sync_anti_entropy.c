#define _POSIX_C_SOURCE 200809L
#include "db_sync_anti_entropy.h"
#include <limits.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static pthread_mutex_t g_init_mtx = PTHREAD_MUTEX_INITIALIZER;
static bool g_initialized = false;

void db_sync_payload_hash(const char *entity, int entity_id, const char *op,
                          const char *payload, long long changed_at, char out[65]) {
    unsigned char digest[SHA256_DIGEST_LENGTH];
    unsigned int digest_len = 0;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx || EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) != 1) {
        EVP_MD_CTX_free(ctx);
        memset(out, '0', 64);
        out[64] = '\0';
        return;
    }
    EVP_DigestUpdate(ctx, entity ? entity : "", entity ? strlen(entity) : 0);
    EVP_DigestUpdate(ctx, "\0", 1);
    char ordering[64];
    int n = snprintf(ordering, sizeof(ordering), "%d", entity_id);
    EVP_DigestUpdate(ctx, ordering, (size_t)n);
    EVP_DigestUpdate(ctx, "\0", 1);
    n = snprintf(ordering, sizeof(ordering), "%lld", changed_at);
    EVP_DigestUpdate(ctx, ordering, (size_t)n);
    EVP_DigestUpdate(ctx, "\0", 1);
    EVP_DigestUpdate(ctx, op ? op : "", op ? strlen(op) : 0);
    EVP_DigestUpdate(ctx, "\0", 1);
    EVP_DigestUpdate(ctx, payload ? payload : "", payload ? strlen(payload) : 0);
    EVP_DigestFinal_ex(ctx, digest, &digest_len);
    EVP_MD_CTX_free(ctx);
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) sprintf(out + i * 2, "%02x", digest[i]);
    out[64] = '\0';
}

static bool mutation_valid(const db_sync_mutation *m) {
    if (!m || !m->mutation_id || !m->entity || !m->op || !m->payload_hash ||
        m->entity_id <= 0 || m->changed_at < 0) return false;
    size_t id_len = strlen(m->mutation_id);
    if (id_len == 0 || id_len > 128 || strlen(m->entity) > 64 || strlen(m->payload_hash) != 64) return false;
    if (strcmp(m->op, "upsert") != 0 && strcmp(m->op, "delete") != 0) return false;
    for (size_t i = 0; i < id_len; i++) {
        char c = m->mutation_id[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '-' || c == '_' || c == '.' || c == ':')) return false;
    }
    char expected[65];
    db_sync_payload_hash(m->entity, m->entity_id, m->op, m->payload, m->changed_at, expected);
    return strcmp(expected, m->payload_hash) == 0;
}

static bool insert_mutation(sqlite3 *conn, const db_sync_mutation *m, bool *inserted) {
    if (inserted) *inserted = false;
    if (!mutation_valid(m)) return false;
    sqlite3_stmt *st = NULL;
    const char *sql = "INSERT OR IGNORE INTO sync_mutation_log "
                      "(mutation_id,entity,entity_id,op,payload,changed_at,payload_hash) VALUES (?,?,?,?,?,?,?)";
    if (sqlite3_prepare_v2(conn, sql, -1, &st, NULL) != SQLITE_OK) return false;
    sqlite3_bind_text(st, 1, m->mutation_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, m->entity, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 3, m->entity_id);
    sqlite3_bind_text(st, 4, m->op, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 5, m->payload ? m->payload : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 6, m->changed_at);
    sqlite3_bind_text(st, 7, m->payload_hash, -1, SQLITE_TRANSIENT);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    if (ok && inserted) *inserted = sqlite3_changes(conn) > 0;
    sqlite3_finalize(st);
    return ok;
}

static bool backfill_journal(sqlite3 *conn) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(conn, "SELECT seq,entity,entity_id,op,payload FROM sync_journal ORDER BY seq",
                           -1, &st, NULL) != SQLITE_OK) return false;
    bool ok = true;
    while (ok) {
        int rc = sqlite3_step(st);
        if (rc == SQLITE_DONE) break;
        if (rc != SQLITE_ROW) { ok = false; break; }
        long long seq = sqlite3_column_int64(st, 0);
        const char *entity = (const char *)sqlite3_column_text(st, 1);
        int entity_id = sqlite3_column_int(st, 2);
        const char *op = (const char *)sqlite3_column_text(st, 3);
        const char *payload = (const char *)sqlite3_column_text(st, 4);
        char mutation_id[40];
        char hash[65];
        snprintf(mutation_id, sizeof(mutation_id), "legacy-%016llx", (unsigned long long)seq);
        db_sync_payload_hash(entity, entity_id, op, payload, seq, hash);
        db_sync_mutation m = { mutation_id, entity, entity_id, op, payload, seq, hash };
        ok = insert_mutation(conn, &m, NULL);
    }
    sqlite3_finalize(st);
    return ok;
}

static bool ensure_changed_at_column(sqlite3 *conn) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(conn, "PRAGMA table_info(sync_mutation_log)", -1, &st, NULL) != SQLITE_OK) return false;
    bool found = false;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *name = (const char *)sqlite3_column_text(st, 1);
        if (name && strcmp(name, "changed_at") == 0) found = true;
    }
    sqlite3_finalize(st);
    return found || sqlite3_exec(conn, "ALTER TABLE sync_mutation_log ADD COLUMN changed_at INTEGER NOT NULL DEFAULT 0",
                                 NULL, NULL, NULL) == SQLITE_OK;
}

static bool refresh_payload_hashes(sqlite3 *conn) {
    sqlite3_stmt *rows = NULL;
    sqlite3_stmt *update = NULL;
    if (sqlite3_prepare_v2(conn, "SELECT mutation_id,entity,entity_id,op,payload,changed_at FROM sync_mutation_log",
                           -1, &rows, NULL) != SQLITE_OK) return false;
    if (sqlite3_prepare_v2(conn, "UPDATE sync_mutation_log SET payload_hash=? WHERE mutation_id=?",
                           -1, &update, NULL) != SQLITE_OK) {
        sqlite3_finalize(rows);
        return false;
    }
    bool ok = true;
    while (ok) {
        int rc = sqlite3_step(rows);
        if (rc == SQLITE_DONE) break;
        if (rc != SQLITE_ROW) { ok = false; break; }
        const char *id = (const char *)sqlite3_column_text(rows, 0);
        const char *entity = (const char *)sqlite3_column_text(rows, 1);
        int entity_id = sqlite3_column_int(rows, 2);
        const char *op = (const char *)sqlite3_column_text(rows, 3);
        const char *payload = (const char *)sqlite3_column_text(rows, 4);
        long long changed_at = sqlite3_column_int64(rows, 5);
        char hash[65];
        db_sync_payload_hash(entity, entity_id, op, payload, changed_at, hash);
        sqlite3_bind_text(update, 1, hash, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(update, 2, id, -1, SQLITE_TRANSIENT);
        ok = sqlite3_step(update) == SQLITE_DONE;
        sqlite3_reset(update);
        sqlite3_clear_bindings(update);
    }
    sqlite3_finalize(update);
    sqlite3_finalize(rows);
    return ok;
}

bool db_sync_anti_entropy_init(sqlite3 *conn) {
    if (!conn) return false;
    pthread_mutex_lock(&g_init_mtx);
    if (g_initialized) {
        pthread_mutex_unlock(&g_init_mtx);
        return true;
    }
    const char *schema =
        "CREATE TABLE IF NOT EXISTS sync_mutation_log ("
        " mutation_id TEXT PRIMARY KEY,"
        " entity TEXT NOT NULL,"
        " entity_id INTEGER NOT NULL,"
        " op TEXT NOT NULL,"
        " payload TEXT NOT NULL DEFAULT '',"
        " changed_at INTEGER NOT NULL DEFAULT 0,"
        " payload_hash TEXT NOT NULL,"
        " created_at DATETIME DEFAULT CURRENT_TIMESTAMP)";
    bool ok = sqlite3_exec(conn, schema, NULL, NULL, NULL) == SQLITE_OK;
    if (ok) ok = ensure_changed_at_column(conn);
    if (ok) ok = refresh_payload_hashes(conn);
    if (ok) ok = sqlite3_exec(conn,
        "CREATE INDEX IF NOT EXISTS idx_sync_mutation_entity ON sync_mutation_log(entity,entity_id)",
        NULL, NULL, NULL) == SQLITE_OK;
    if (ok) ok = backfill_journal(conn);
    g_initialized = ok;
    pthread_mutex_unlock(&g_init_mtx);
    return ok;
}

static long long next_changed_at(sqlite3 *conn, const char *entity, int entity_id, bool *ok) {
    struct timespec ts;
    long long now = 0;
    *ok = clock_gettime(CLOCK_REALTIME, &ts) == 0;
    if (*ok) now = (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(conn, "SELECT COALESCE(MAX(changed_at),0) FROM sync_mutation_log "
                                 "WHERE entity=? AND entity_id=?", -1, &st, NULL) != SQLITE_OK) {
        *ok = false;
        return 0;
    }
    sqlite3_bind_text(st, 1, entity, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 2, entity_id);
    long long latest = 0;
    if (sqlite3_step(st) == SQLITE_ROW) latest = sqlite3_column_int64(st, 0);
    else *ok = false;
    sqlite3_finalize(st);
    if (latest == LLONG_MAX) *ok = false;
    return latest >= now ? latest + 1 : now;
}

bool db_sync_anti_entropy_record(sqlite3 *conn, const char *entity, int entity_id,
                                 const char *op, const char *payload) {
    if (!db_sync_anti_entropy_init(conn)) return false;
    bool ok = false;
    long long changed_at = next_changed_at(conn, entity, entity_id, &ok);
    if (!ok) return false;
    unsigned char random[16];
    if (RAND_bytes(random, sizeof(random)) != 1) return false;
    char mutation_id[33];
    for (size_t i = 0; i < sizeof(random); i++) sprintf(mutation_id + i * 2, "%02x", random[i]);
    mutation_id[32] = '\0';
    char hash[65];
    db_sync_payload_hash(entity, entity_id, op, payload, changed_at, hash);
    db_sync_mutation m = { mutation_id, entity, entity_id, op, payload ? payload : "", changed_at, hash };
    return insert_mutation(conn, &m, NULL);
}

bool db_sync_anti_entropy_import(sqlite3 *conn, const db_sync_mutation *mutation, bool *inserted) {
    if (!db_sync_anti_entropy_init(conn)) return false;
    return insert_mutation(conn, mutation, inserted);
}

bool db_sync_anti_entropy_winner(sqlite3 *conn, const char *entity, int entity_id,
                                 db_sync_mutation_owned *winner) {
    if (!conn || !entity || !winner || entity_id <= 0) return false;
    memset(winner, 0, sizeof(*winner));
    sqlite3_stmt *st = NULL;
    const char *sql = "SELECT mutation_id,entity,entity_id,op,payload,changed_at,payload_hash FROM sync_mutation_log "
                      "WHERE entity=? AND entity_id=? "
                      "ORDER BY changed_at DESC,CASE op WHEN 'delete' THEN 1 ELSE 0 END DESC,"
                      "payload_hash DESC,mutation_id DESC LIMIT 1";
    if (sqlite3_prepare_v2(conn, sql, -1, &st, NULL) != SQLITE_OK) return false;
    sqlite3_bind_text(st, 1, entity, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 2, entity_id);
    bool ok = sqlite3_step(st) == SQLITE_ROW;
    if (ok) {
        winner->mutation_id = strdup((const char *)sqlite3_column_text(st, 0));
        winner->entity = strdup((const char *)sqlite3_column_text(st, 1));
        winner->entity_id = sqlite3_column_int(st, 2);
        winner->op = strdup((const char *)sqlite3_column_text(st, 3));
        winner->payload = strdup((const char *)sqlite3_column_text(st, 4));
        winner->changed_at = sqlite3_column_int64(st, 5);
        winner->payload_hash = strdup((const char *)sqlite3_column_text(st, 6));
        ok = winner->mutation_id && winner->entity && winner->op && winner->payload && winner->payload_hash;
    }
    sqlite3_finalize(st);
    if (!ok) db_sync_mutation_free(winner);
    return ok;
}

void db_sync_mutation_free(db_sync_mutation_owned *mutation) {
    if (!mutation) return;
    free(mutation->mutation_id);
    free(mutation->entity);
    free(mutation->op);
    free(mutation->payload);
    free(mutation->payload_hash);
    memset(mutation, 0, sizeof(*mutation));
}

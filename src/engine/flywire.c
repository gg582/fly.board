#define _POSIX_C_SOURCE 200809L
#include "engine/flywire.h"
#include "engine/forkgate.h"
#include "auth/auth.h"
#include "db/db.h"
#include "db/db_internal.h"
#include "config/config.h"
#include <cwist/core/log.h>
#include <cwist/net/http/http.h>
#include <cjson/cJSON.h>
#include <curl/curl.h>
#include <curl/header.h>
#include <openssl/crypto.h>
#include <pthread.h>
#include <sqlite3.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <ctype.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>

/* FlyWire: primary/replica site synchronization.
 *
 * Primary: every content mutation appends a row to sync_journal
 * (src/db/db_sync.c) and serves GET /flywire/feed?since=<seq> to replicas
 * that present the shared token.
 *
 * Replica: a background loop in the supervisor process polls the feed every
 * poll_seconds, applies each row idempotently (upserts are full-row snapshots
 * keyed by the row id, so re-applying is harmless), and advances the
 * checkpoint marker data/.flywire_seq after every row. A replica is
 * read-only: global_middleware rejects mutating methods (handlers.c).
 *
 * File bytes (Phase 2): when a "file" upsert arrives whose file_path is set
 * but missing on disk, the id is queued and fetched with a plain HTTPS GET
 * of /file/download/<id> (Range-resumed) into the payload's file_path. This
 * universal mechanism always works; a TASFA server-side pull can replace it
 * later (the TASFA JS client path is src/handlers/tasfa/download.c).
 *
 * NOTE: view_count and download_count counters are not synced. */

#define FLYWIRE_SEQ_PATH "data/.flywire_seq"
#define FLYWIRE_FEED_LIMIT 5000
#define FLYWIRE_PENDING_MAX 64

static const char FLYWIRE_SEQ_TMP[] = "data/.flywire_seq.tmp";

/* ---- Checkpoint marker ---- */

long long flywire_checkpoint_read(void) {
    FILE *f = fopen(FLYWIRE_SEQ_PATH, "r");
    if (!f) return 0;
    long long v = 0;
    if (fscanf(f, "%lld", &v) != 1) v = 0;
    fclose(f);
    if (v < 0) v = 0;
    return v;
}

bool flywire_checkpoint_write(long long seq) {
    FILE *f = fopen(FLYWIRE_SEQ_TMP, "w");
    if (!f) return false;
    fprintf(f, "%lld\n", seq);
    if (fclose(f) != 0) return false;
    return rename(FLYWIRE_SEQ_TMP, FLYWIRE_SEQ_PATH) == 0;
}

/* Constant-time token comparison (never strcmp on a secret). */
static bool flywire_token_eq(const char *a, const char *b) {
    if (!a || !b) return false;
    size_t a_len = strlen(a);
    size_t b_len = strlen(b);
    if (a_len != b_len) return false;
    return CRYPTO_memcmp(a, b, a_len) == 0;
}

/* ---- Site version (advertised in the feed for auto-upgrade) ---- */

/* Resolved lazily on first use and cached in a static: data/.flywire_version
 * if present, else `git rev-parse --short HEAD` when running from a git
 * checkout, else "unknown". */
const char *flywire_site_version(void) {
    static char version[64] = {0};
    static bool resolved = false;
    if (resolved) return version;
    resolved = true;

    FILE *f = fopen("data/.flywire_version", "r");
    if (f) {
        if (fgets(version, sizeof(version), f)) {
            size_t n = strlen(version);
            while (n > 0 && isspace((unsigned char)version[n - 1])) version[--n] = '\0';
        }
        fclose(f);
        if (version[0]) return version;
    }

    struct stat st;
    if (stat(".git", &st) == 0) {
        FILE *p = popen("git rev-parse --short HEAD", "r");
        if (p) {
            if (fgets(version, sizeof(version), p)) {
                size_t n = strlen(version);
                while (n > 0 && isspace((unsigned char)version[n - 1])) version[--n] = '\0';
            }
            pclose(p);
            if (version[0]) return version;
        }
    }
    snprintf(version, sizeof(version), "%s", "unknown");
    return version;
}

/* ---- Replica auto-upgrade ---- */

static char g_upgrade_spawned_for[64] = {0};

/* Spawn ./flywire-upgrade.sh detached (double fork like backup_spawn()). The
 * script does git pull/build/restart; the C code never runs git or make
 * itself. At most one spawn per primary version: g_upgrade_spawned_for
 * remembers the version we already tried, so a failed upgrade is retried
 * only when the primary moves to yet another version. */
static void flywire_maybe_auto_upgrade(const char *primary_version) {
    if (!primary_version || !primary_version[0]) return;
    const char *local = flywire_site_version();
    if (strcmp(primary_version, local) == 0) return;
    if (!flywire_auto_upgrade()) return;
    if (strcmp(g_upgrade_spawned_for, primary_version) == 0) return;
    struct stat st;
    if (stat("./flywire-upgrade.sh", &st) != 0 || !S_ISREG(st.st_mode) || !(st.st_mode & S_IXUSR)) {
        CWIST_LOG_WARN("FlyWire: primary runs version %s, local %s, but ./flywire-upgrade.sh "
                       "is missing or not executable; not upgrading", primary_version, local);
        snprintf(g_upgrade_spawned_for, sizeof(g_upgrade_spawned_for), "%s", primary_version);
        return;
    }
    snprintf(g_upgrade_spawned_for, sizeof(g_upgrade_spawned_for), "%s", primary_version);
    CWIST_LOG_INFO("FlyWire: primary runs version %s, local %s — spawning flywire-upgrade.sh",
                   primary_version, local);
    pid_t pid = fork();
    if (pid < 0) {
        CWIST_LOG_ERROR("FlyWire: fork for flywire-upgrade.sh failed: %s", strerror(errno));
        return;
    }
    if (pid == 0) {
        if (fork() != 0) _exit(0);
        setsid();
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) { dup2(devnull, STDIN_FILENO); close(devnull); }
        if (close_range(3, ~0U, 0) != 0) {
            for (int fd = 3; fd < 65536; fd++) close(fd);
        }
        execl("/bin/sh", "sh", "./flywire-upgrade.sh", (char *)NULL);
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
}


/* ---- Feed endpoint (primary side) ---- */

static void flywire_send_json(cwist_http_response *res, cJSON *obj, int status) {
    char *body = obj ? cJSON_PrintUnformatted(obj) : NULL;
    res->status_code = status;
    cwist_http_header_add(&res->headers, "Content-Type", "application/json; charset=utf-8");
    cwist_http_header_add(&res->headers, "Cache-Control", "no-store");
    cwist_sstring_assign(res->body, body ? body : "{}");
    free(body);
}

/* GET /flywire/feed?since=<seq>&limit=<n> (token via X-FlyWire-Token header
 * or ?token= for curl debugging). Primary mode only; anything else 404s.
 * Immediate response, no long-poll: replicas poll every couple of seconds. */
void handler_flywire_feed(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    if (!flywire_is_primary()) {
        res->status_code = CWIST_HTTP_NOT_FOUND;
        cwist_sstring_assign(res->body, "Not found");
        return;
    }
    const char *token = cwist_http_header_get(req->headers, "X-FlyWire-Token");
    if (!token || !token[0]) token = cwist_query_map_get(req->query_params, "token");
    if (!flywire_token_eq(token ? token : "", flywire_token())) {
        cJSON *err = cJSON_CreateObject();
        cJSON_AddBoolToObject(err, "ok", false);
        cJSON_AddStringToObject(err, "error", "unauthorized");
        flywire_send_json(res, err, CWIST_HTTP_UNAUTHORIZED);
        cJSON_Delete(err);
        return;
    }

    long long since = 0;
    const char *since_s = cwist_query_map_get(req->query_params, "since");
    if (since_s && since_s[0]) since = atoll(since_s);
    if (since < 0) since = 0;
    int limit = 500;
    const char *limit_s = cwist_query_map_get(req->query_params, "limit");
    if (limit_s && limit_s[0]) limit = atoi(limit_s);
    if (limit < 1) limit = 500;
    if (limit > 5000) limit = 5000;

    sqlite3 *conn = fly_db_conn(req->db);

    /* Gap detection: if the requested checkpoint fell off the 7-day purge
     * window, the replica cannot catch up from the feed anymore. */
    long long min_seq = 0;
    {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(conn, "SELECT MIN(seq) FROM sync_journal", -1, &st, NULL) == SQLITE_OK) {
            if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_type(st, 0) != SQLITE_NULL) {
                min_seq = sqlite3_column_int64(st, 0);
            }
            sqlite3_finalize(st);
        }
    }
    if (since > 0 && min_seq > 0 && since < min_seq - 1) {
        cJSON *err = cJSON_CreateObject();
        cJSON_AddBoolToObject(err, "ok", false);
        cJSON_AddStringToObject(err, "error", "journal_purged");
        flywire_send_json(res, err, CWIST_HTTP_OK);
        cJSON_Delete(err);
        return;
    }

    cJSON *doc = cJSON_CreateObject();
    cJSON_AddBoolToObject(doc, "ok", true);
    cJSON *rows = cJSON_AddArrayToObject(doc, "rows");
    long long next_since = since;
    const char *sql = "SELECT seq, entity, entity_id, op, payload FROM sync_journal WHERE seq>? ORDER BY seq LIMIT ?";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(conn, sql, -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, since);
        sqlite3_bind_int(st, 2, limit);
        while (sqlite3_step(st) == SQLITE_ROW) {
            cJSON *row = cJSON_CreateObject();
            long long seq = sqlite3_column_int64(st, 0);
            cJSON_AddNumberToObject(row, "seq", (double)seq);
            cJSON_AddStringToObject(row, "entity", (const char *)sqlite3_column_text(st, 1));
            cJSON_AddNumberToObject(row, "id", sqlite3_column_int(st, 2));
            cJSON_AddStringToObject(row, "op", (const char *)sqlite3_column_text(st, 3));
            const char *payload = (const char *)sqlite3_column_text(st, 4);
            cJSON *pj = (payload && payload[0]) ? cJSON_Parse(payload) : NULL;
            if (pj) cJSON_AddItemToObject(row, "payload", pj);
            else cJSON_AddObjectToObject(row, "payload");
            cJSON_AddItemToArray(rows, row);
            next_since = seq;
        }
        sqlite3_finalize(st);
    }
    cJSON_AddNumberToObject(doc, "next_since", (double)next_since);
    cJSON_AddStringToObject(doc, "version", flywire_site_version());
    flywire_send_json(res, doc, CWIST_HTTP_OK);
    cJSON_Delete(doc);
}

/* ---- Entity -> table mapping ---- */

typedef enum {
    FLYWIRE_DB_MAIN,     /* data/blog.db */
    FLYWIRE_DB_COMMENTS, /* data/comments.db */
    FLYWIRE_DB_TREE,     /* data/board_tree.db */
} flywire_db_kind;

typedef struct {
    const char *entity;
    const char *table;
    flywire_db_kind db;
    bool special;        /* payload is not a plain row snapshot */
} flywire_entity_t;

static const flywire_entity_t k_entities[] = {
    { "post",       "posts",           FLYWIRE_DB_MAIN,     false },
    { "board",      "boards",          FLYWIRE_DB_MAIN,     false },
    { "board_perm", "board_permissions", FLYWIRE_DB_MAIN,   true  },
    { "file",       "files",           FLYWIRE_DB_MAIN,     false },
    { "series",     "series",          FLYWIRE_DB_MAIN,     false },
    { "tag",        "tags",            FLYWIRE_DB_MAIN,     false },
    { "tag_link",   "post_tags",       FLYWIRE_DB_MAIN,     true  },
    { "vote",       "post_votes",      FLYWIRE_DB_MAIN,     false },
    { "vote_anon",  "post_votes_anon", FLYWIRE_DB_MAIN,     false },
    { "i18n",       "i18n",            FLYWIRE_DB_MAIN,     true  },
    { "comment",    "comments",        FLYWIRE_DB_COMMENTS, false },
    { "board_tree", "board_tree",      FLYWIRE_DB_TREE,     false },
    { "user",       "users",           FLYWIRE_DB_MAIN,     false },
};

static const flywire_entity_t *flywire_entity(const char *name) {
    if (!name) return NULL;
    for (size_t i = 0; i < sizeof(k_entities) / sizeof(k_entities[0]); i++) {
        if (strcmp(k_entities[i].entity, name) == 0) return &k_entities[i];
    }
    return NULL;
}

static sqlite3 *flywire_conn(flywire_db_kind kind) {
    switch (kind) {
        case FLYWIRE_DB_COMMENTS: return db_comment_conn();
        case FLYWIRE_DB_TREE:     return db_board_tree_conn();
        default:                  return NULL; /* caller passes the main conn */
    }
}

/* Column whitelist cache: INSERT OR REPLACE only touches columns that exist
 * in the target table, so a payload from a newer primary cannot break an
 * older replica (unknown keys are dropped defensively). */
#define FLYWIRE_COLS_MAX 32
typedef struct {
    char name[FLYWIRE_COLS_MAX][48];
    int n;
    bool loaded;
} flywire_cols_t;

static pthread_mutex_t g_cols_mtx = PTHREAD_MUTEX_INITIALIZER;
static flywire_cols_t g_cols[16];

static flywire_cols_t *flywire_cols_slot(const char *table) {
    static const struct { const char *t; int slot; } map[] = {
        { "posts", 0 }, { "boards", 1 }, { "files", 2 }, { "series", 3 },
        { "tags", 4 }, { "post_votes", 5 }, { "post_votes_anon", 6 },
        { "comments", 7 }, { "board_tree", 8 }, { "users", 9 },
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (strcmp(map[i].t, table) == 0) return &g_cols[map[i].slot];
    }
    return NULL;
}

static bool flywire_table_cols(sqlite3 *conn, const char *table, flywire_cols_t *out) {
    if (out->loaded) return true;
    char sql[128];
    snprintf(sql, sizeof(sql), "PRAGMA table_info(%s)", table);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(conn, sql, -1, &st, NULL) != SQLITE_OK) return false;
    out->n = 0;
    while (sqlite3_step(st) == SQLITE_ROW && out->n < FLYWIRE_COLS_MAX) {
        const char *name = (const char *)sqlite3_column_text(st, 1);
        if (!name) continue;
        snprintf(out->name[out->n], sizeof(out->name[out->n]), "%s", name);
        out->n++;
    }
    sqlite3_finalize(st);
    out->loaded = out->n > 0;
    return out->loaded;
}

/* Generic idempotent upsert of a full-row snapshot. The payload object keys
 * are used as column names (filtered against the real table columns); the
 * row id inside the payload preserves the primary's id. ON CONFLICT DO
 * UPDATE is used instead of INSERT OR REPLACE: REPLACE is a DELETE+INSERT
 * and would fire ON DELETE CASCADE on child rows (post_votes, post_tags)
 * that the primary did not touch. */
static bool flywire_apply_upsert(sqlite3 *conn, const flywire_entity_t *e, cJSON *payload) {
    if (!conn || !e || !payload || !cJSON_IsObject(payload)) return false;
    flywire_cols_t *cols = flywire_cols_slot(e->table);
    if (!cols) return false;
    pthread_mutex_lock(&g_cols_mtx);
    bool have = flywire_table_cols(conn, e->table, cols);
    pthread_mutex_unlock(&g_cols_mtx);
    if (!have) {
        CWIST_LOG_ERROR("flywire: no columns for table %s", e->table);
        return false;
    }

    const char *key_col = strcmp(e->entity, "board_tree") == 0 ? "board_id" : "id";

    char sql[2048];
    size_t off = (size_t)snprintf(sql, sizeof(sql), "INSERT INTO %s (", e->table);
    int col_count = 0;
    bool has_key = false;
    for (int i = 0; i < cols->n; i++) {
        cJSON *v = cJSON_GetObjectItem(payload, cols->name[i]);
        if (!v) continue; /* column absent from snapshot: keep default */
        if (strcmp(cols->name[i], key_col) == 0) has_key = true;
        if (col_count > 0 && off < sizeof(sql) - 64) off += (size_t)snprintf(sql + off, sizeof(sql) - off, ",");
        off += (size_t)snprintf(sql + off, sizeof(sql) - off, "%s", cols->name[i]);
        col_count++;
    }
    if (col_count == 0 || !has_key) return false;
    off += (size_t)snprintf(sql + off, sizeof(sql) - off, ") VALUES (");
    for (int i = 0; i < col_count && off < sizeof(sql) - 4; i++) {
        off += (size_t)snprintf(sql + off, sizeof(sql) - off, i == col_count - 1 ? "?" : "?,");
    }
    off += (size_t)snprintf(sql + off, sizeof(sql) - off, ") ON CONFLICT(%s) DO UPDATE SET ", key_col);
    bool first_set = true;
    for (int i = 0; i < cols->n; i++) {
        cJSON *v = cJSON_GetObjectItem(payload, cols->name[i]);
        if (!v || strcmp(cols->name[i], key_col) == 0) continue;
        if (!first_set && off < sizeof(sql) - 64) off += (size_t)snprintf(sql + off, sizeof(sql) - off, ",");
        off += (size_t)snprintf(sql + off, sizeof(sql) - off, "%s=excluded.%s", cols->name[i], cols->name[i]);
        first_set = false;
    }
    if (first_set) {
        /* Only the key column was present: nothing to update. */
        off += (size_t)snprintf(sql + off, sizeof(sql) - off, "%s=%s", key_col, key_col);
    }

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(conn, sql, -1, &st, NULL) != SQLITE_OK) {
        CWIST_LOG_ERROR("flywire: upsert prepare failed on %s: %s", e->table, sqlite3_errmsg(conn));
        return false;
    }
    int idx = 1;
    for (int i = 0; i < cols->n; i++) {
        cJSON *v = cJSON_GetObjectItem(payload, cols->name[i]);
        if (!v) continue;
        if (cJSON_IsNumber(v)) {
            sqlite3_bind_double(st, idx++, v->valuedouble);
        } else if (cJSON_IsString(v) && v->valuestring) {
            sqlite3_bind_text(st, idx++, v->valuestring, -1, SQLITE_TRANSIENT);
        } else {
            sqlite3_bind_null(st, idx++);
        }
    }
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE;
}

static bool flywire_apply_delete(sqlite3 *conn, const flywire_entity_t *e, int entity_id, cJSON *payload) {
    if (!conn || !e || entity_id <= 0) return false;
    char sql[256];
    if (strcmp(e->entity, "board_perm") == 0 && payload) {
        int board_id = cJSON_GetObjectItem(payload, "board_id") ? cJSON_GetObjectItem(payload, "board_id")->valueint : entity_id;
        int user_id = cJSON_GetObjectItem(payload, "user_id") ? cJSON_GetObjectItem(payload, "user_id")->valueint : 0;
        snprintf(sql, sizeof(sql), "DELETE FROM %s WHERE board_id=? AND user_id=?", e->table);
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(conn, sql, -1, &st, NULL) != SQLITE_OK) return false;
        sqlite3_bind_int(st, 1, board_id);
        sqlite3_bind_int(st, 2, user_id);
        int rc = sqlite3_step(st);
        sqlite3_finalize(st);
        return rc == SQLITE_DONE;
    }
    if (strcmp(e->entity, "tag_link") == 0 && payload) {
        int post_id = cJSON_GetObjectItem(payload, "post_id") ? cJSON_GetObjectItem(payload, "post_id")->valueint : entity_id;
        snprintf(sql, sizeof(sql), "DELETE FROM %s WHERE post_id=?", e->table);
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(conn, sql, -1, &st, NULL) != SQLITE_OK) return false;
        sqlite3_bind_int(st, 1, post_id);
        int rc = sqlite3_step(st);
        sqlite3_finalize(st);
        return rc == SQLITE_DONE;
    }
    if (strcmp(e->entity, "i18n") == 0 && payload) {
        const char *kind = cJSON_GetObjectItem(payload, "kind") && cJSON_IsString(cJSON_GetObjectItem(payload, "kind"))
                               ? cJSON_GetObjectItem(payload, "kind")->valuestring : "post";
        snprintf(sql, sizeof(sql), "DELETE FROM %s WHERE kind=? AND item_id=?", e->table);
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(conn, sql, -1, &st, NULL) != SQLITE_OK) return false;
        sqlite3_bind_text(st, 1, kind, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st, 2, entity_id);
        int rc = sqlite3_step(st);
        sqlite3_finalize(st);
        return rc == SQLITE_DONE;
    }
    const char *key_col = strcmp(e->entity, "board_tree") == 0 ? "board_id" : "id";
    snprintf(sql, sizeof(sql), "DELETE FROM %s WHERE %s=?", e->table, key_col);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(conn, sql, -1, &st, NULL) != SQLITE_OK) return false;
    sqlite3_bind_int(st, 1, entity_id);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE;
}

/* Special non-row payloads. */
static bool flywire_apply_special(sqlite3 *conn, const flywire_entity_t *e, const char *op, int entity_id, cJSON *payload) {
    if (!conn || !e || !payload) return false;
    char sql[256];
    sqlite3_stmt *st = NULL;
    int rc = SQLITE_ERROR;
    if (strcmp(e->entity, "board_perm") == 0) {
        int board_id = cJSON_GetObjectItem(payload, "board_id") ? cJSON_GetObjectItem(payload, "board_id")->valueint : entity_id;
        int user_id = cJSON_GetObjectItem(payload, "user_id") ? cJSON_GetObjectItem(payload, "user_id")->valueint : 0;
        if (strcmp(op, "delete") == 0) return flywire_apply_delete(conn, e, entity_id, payload);
        snprintf(sql, sizeof(sql), "INSERT OR IGNORE INTO %s (board_id, user_id) VALUES (?,?)", e->table);
        if (sqlite3_prepare_v2(conn, sql, -1, &st, NULL) != SQLITE_OK) return false;
        sqlite3_bind_int(st, 1, board_id);
        sqlite3_bind_int(st, 2, user_id);
        rc = sqlite3_step(st);
        sqlite3_finalize(st);
        return rc == SQLITE_DONE;
    }
    if (strcmp(e->entity, "tag_link") == 0) {
        int post_id = cJSON_GetObjectItem(payload, "post_id") ? cJSON_GetObjectItem(payload, "post_id")->valueint : entity_id;
        int tag_id = cJSON_GetObjectItem(payload, "tag_id") ? cJSON_GetObjectItem(payload, "tag_id")->valueint : 0;
        if (strcmp(op, "delete") == 0 || tag_id <= 0) {
            return flywire_apply_delete(conn, e, entity_id, payload);
        }
        snprintf(sql, sizeof(sql), "INSERT OR IGNORE INTO %s (post_id, tag_id) VALUES (?,?)", e->table);
        if (sqlite3_prepare_v2(conn, sql, -1, &st, NULL) != SQLITE_OK) return false;
        sqlite3_bind_int(st, 1, post_id);
        sqlite3_bind_int(st, 2, tag_id);
        rc = sqlite3_step(st);
        sqlite3_finalize(st);
        return rc == SQLITE_DONE;
    }
    if (strcmp(e->entity, "i18n") == 0) {
        if (strcmp(op, "delete") == 0) return flywire_apply_delete(conn, e, entity_id, payload);
        const char *kind = cJSON_GetObjectItem(payload, "kind") && cJSON_IsString(cJSON_GetObjectItem(payload, "kind"))
                               ? cJSON_GetObjectItem(payload, "kind")->valuestring : "post";
        const char *lang = cJSON_GetObjectItem(payload, "lang") && cJSON_IsString(cJSON_GetObjectItem(payload, "lang"))
                               ? cJSON_GetObjectItem(payload, "lang")->valuestring : "";
        int grp = cJSON_GetObjectItem(payload, "grp") ? cJSON_GetObjectItem(payload, "grp")->valueint : 0;
        snprintf(sql, sizeof(sql),
                 "INSERT INTO %s (kind, item_id, lang, grp) VALUES (?,?,?,?)"
                 " ON CONFLICT(kind, item_id) DO UPDATE SET lang=excluded.lang, grp=excluded.grp", e->table);
        if (sqlite3_prepare_v2(conn, sql, -1, &st, NULL) != SQLITE_OK) return false;
        sqlite3_bind_text(st, 1, kind, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st, 2, entity_id);
        sqlite3_bind_text(st, 3, lang, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st, 4, grp);
        rc = sqlite3_step(st);
        sqlite3_finalize(st);
        return rc == SQLITE_DONE;
    }
    return false;
}

/* ---- Pending file-byte fetches (Phase 2) ---- */

static pthread_mutex_t g_pending_mtx = PTHREAD_MUTEX_INITIALIZER;
static int g_pending_files[FLYWIRE_PENDING_MAX];
static int g_pending_len = 0;

static void flywire_enqueue_file(int file_id) {
    if (file_id <= 0) return;
    pthread_mutex_lock(&g_pending_mtx);
    for (int i = 0; i < g_pending_len; i++) {
        if (g_pending_files[i] == file_id) { pthread_mutex_unlock(&g_pending_mtx); return; }
    }
    if (g_pending_len < FLYWIRE_PENDING_MAX) g_pending_files[g_pending_len++] = file_id;
    pthread_mutex_unlock(&g_pending_mtx);
}

static int flywire_dequeue_file(void) {
    pthread_mutex_lock(&g_pending_mtx);
    int id = 0;
    if (g_pending_len > 0) {
        id = g_pending_files[0];
        memmove(g_pending_files, g_pending_files + 1, (size_t)(g_pending_len - 1) * sizeof(int));
        g_pending_len--;
    }
    pthread_mutex_unlock(&g_pending_mtx);
    return id;
}

typedef struct {
    FILE *f;
} flywire_download_ctx;

static size_t flywire_download_write(void *ptr, size_t size, size_t nmemb, void *userdata) {
    flywire_download_ctx *ctx = (flywire_download_ctx *)userdata;
    return fwrite(ptr, size, nmemb, ctx->f);
}

/* Fetch one file's bytes from the primary with a resumable HTTPS GET of
 * /file/download/<id>. Always-through-HTTPS by design: it works whether or
 * not the primary serves TASFA; a TASFA server-side pull can replace this
 * later. Idempotent: resumes at the existing partial size and rewrites the
 * row's file on HTTP 200. Failure is logged; the id stays out of the queue
 * and a later "file" upsert re-queues it. */
static void flywire_fetch_file_bytes(int file_id, sqlite3 *main_conn) {
    sqlite3_stmt *st = NULL;
    char file_path[PATH_MAX] = {0};
    if (sqlite3_prepare_v2(main_conn, "SELECT file_path FROM files WHERE id=?", -1, &st, NULL) != SQLITE_OK) return;
    sqlite3_bind_int(st, 1, file_id);
    if (sqlite3_step(st) == SQLITE_ROW) {
        const char *p = (const char *)sqlite3_column_text(st, 0);
        if (p) snprintf(file_path, sizeof(file_path), "%s", p);
    }
    sqlite3_finalize(st);
    if (!file_path[0]) return;
    if (strstr(file_path, "..") || file_path[0] == '/') {
        CWIST_LOG_ERROR("flywire: refusing unsafe file_path %s", file_path);
        return;
    }
    struct stat fst;
    if (stat(file_path, &fst) == 0 && fst.st_size > 0) return; /* already have it */

    /* Create the target directories. */
    char dirs[PATH_MAX];
    snprintf(dirs, sizeof(dirs), "%s", file_path);
    char *slash = strrchr(dirs, '/');
    if (slash) {
        *slash = '\0';
        for (char *p = dirs + 1; *p; p++) {
            if (*p != '/') continue;
            *p = '\0';
            mkdir(dirs, 0755);
            *p = '/';
        }
        mkdir(dirs, 0755);
    }

    char url[600];
    const char *base = flywire_primary_url();
    size_t bl = strlen(base);
    snprintf(url, sizeof(url), "%s%s/file/download/%d", base, (bl > 0 && base[bl - 1] == '/') ? "" : "", file_id);

    FILE *f = fopen(file_path, "ab");
    if (!f) {
        CWIST_LOG_ERROR("flywire: cannot open %s for download: %s", file_path, strerror(errno));
        return;
    }
    long have = ftell(f);
    CURL *curl = curl_easy_init();
    if (!curl) { fclose(f); return; }
    char range_hdr[64];
    struct curl_slist *headers = NULL;
    if (have > 0) {
        snprintf(range_hdr, sizeof(range_hdr), "Range: bytes=%ld-", have);
        headers = curl_slist_append(headers, range_hdr);
    }
    char token_hdr[192];
    snprintf(token_hdr, sizeof(token_hdr), "X-FlyWire-Token: %s", flywire_token());
    headers = curl_slist_append(headers, token_hdr);
    flywire_download_ctx ctx = { .f = f };
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, flywire_download_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    bool ok = (rc == CURLE_OK) && (status == 200 || status == 206);
    if (fclose(f) != 0) ok = false;
    if (ok) {
        CWIST_LOG_INFO("flywire: fetched file %d bytes (%s)", file_id, file_path);
    } else {
        CWIST_LOG_WARN("flywire: file %d fetch failed (curl=%d http=%ld); will retry on next upsert", file_id, (int)rc, status);
    }
}

/* ---- Feed response buffer ---- */

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} flywire_buf;

static size_t flywire_feed_write(void *ptr, size_t size, size_t nmemb, void *userdata) {
    flywire_buf *b = (flywire_buf *)userdata;
    size_t need = b->len + size * nmemb + 1;
    if (need > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 8192;
        while (cap < need) cap *= 2;
        char *g = realloc(b->data, cap);
        if (!g) return 0;
        b->data = g;
        b->cap = cap;
    }
    memcpy(b->data + b->len, ptr, size * nmemb);
    b->len += size * nmemb;
    b->data[b->len] = '\0';
    return size * nmemb;
}

/* Apply one journal row to the local databases. Returns false on a hard
 * failure (unknown entity); individual SQL errors are logged and skipped so
 * one bad row cannot wedge the loop. */
static bool flywire_apply_row(sqlite3 *main_conn, long long seq, const char *entity, int entity_id,
                              const char *op, const char *payload_json) {
    const flywire_entity_t *e = flywire_entity(entity);
    if (!e) {
        CWIST_LOG_WARN("flywire: unknown entity '%s' at seq %lld; skipped", entity ? entity : "?", seq);
        return false;
    }
    sqlite3 *conn = e->db == FLYWIRE_DB_MAIN ? main_conn : flywire_conn(e->db);
    if (!conn) return false;

    bool ok;
    if (strcmp(e->entity, "user") == 0 && strcmp(op, "upsert") == 0) {
        /* Never clobber the replica's own admin identity: skip any user
         * upsert whose username matches the local admin.settings username,
         * and never touch the admin.settings account row itself. The primary
         * may journal the same-named row (or the same id after a reseed);
         * locally that row is how the operator signs in. */
        cJSON *payload = cJSON_Parse(payload_json && payload_json[0] ? payload_json : "{}");
        const char *uname = payload && cJSON_IsString(cJSON_GetObjectItem(payload, "username"))
                                ? cJSON_GetObjectItem(payload, "username")->valuestring : "";
        if (uname[0] && strcmp(uname, auth_admin_username()) == 0) {
            CWIST_LOG_WARN("flywire: skipped user upsert for local admin username '%s' at seq %lld",
                           uname, seq);
            if (payload) cJSON_Delete(payload);
            return true;
        }
        if (entity_id > 0 && entity_id == auth_site_admin_uid()) {
            CWIST_LOG_WARN("flywire: skipped user upsert for local admin.settings row (id %d) at seq %lld",
                           entity_id, seq);
            if (payload) cJSON_Delete(payload);
            return true;
        }
        ok = payload ? flywire_apply_upsert(conn, e, payload) : false;
        if (payload) cJSON_Delete(payload);
    } else if (e->special && strcmp(op, "upsert") == 0) {
        cJSON *payload = cJSON_Parse(payload_json && payload_json[0] ? payload_json : "{}");
        ok = payload ? flywire_apply_special(conn, e, op, entity_id, payload) : false;
        if (payload) cJSON_Delete(payload);
    } else if (strcmp(op, "delete") == 0) {
        ok = flywire_apply_delete(conn, e, entity_id, NULL);
    } else {
        cJSON *payload = cJSON_Parse(payload_json && payload_json[0] ? payload_json : "{}");
        ok = payload ? flywire_apply_upsert(conn, e, payload) : false;
        if (ok && payload && strcmp(e->entity, "post") == 0) {
            /* Keep the replica's trigram search index in step with posts. */
            cwist_db db = { .conn = main_conn };
            db_search_index_post(&db, entity_id);
        }
        if (ok && payload && strcmp(e->entity, "file") == 0) {
            /* Queue byte fetch when the file is missing on disk (Phase 2). */
            cJSON *fp = cJSON_GetObjectItem(payload, "file_path");
            if (cJSON_IsString(fp) && fp->valuestring && fp->valuestring[0]) {
                struct stat fst;
                if (stat(fp->valuestring, &fst) != 0) flywire_enqueue_file(entity_id);
            }
        }
        if (payload) cJSON_Delete(payload);
    }
    if (!ok) {
        CWIST_LOG_WARN("flywire: apply %s %d (%s) at seq %lld failed: %s",
                       entity, entity_id, op, seq, sqlite3_errmsg(conn));
    }
    return true;
}

/* ---- Replica loop ---- */

static _Atomic bool g_flywire_running = false;

/* Poll one feed batch and apply it. Returns -1 on network/parse failure
 * (retry next iteration), 0 when caught up, 1 when the journal has been
 * purged past our checkpoint (operator must reseed). */
static int flywire_poll_once(sqlite3 *main_conn, long long since) {
    char url[768];
    const char *base = flywire_primary_url();
    size_t bl = strlen(base);
    snprintf(url, sizeof(url), "%s%s/flywire/feed?since=%lld&limit=%d",
             base, (bl > 0 && base[bl - 1] == '/') ? "" : "", since, FLYWIRE_FEED_LIMIT);

    flywire_buf buf = {0};
    CURL *curl = curl_easy_init();
    if (!curl) return -1;
    char token_hdr[192];
    snprintf(token_hdr, sizeof(token_hdr), "X-FlyWire-Token: %s", flywire_token());
    struct curl_slist *headers = curl_slist_append(NULL, token_hdr);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, flywire_feed_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK || status != 200) {
        free(buf.data);
        CWIST_LOG_WARN("flywire: feed fetch failed (curl=%d http=%ld); retrying", (int)rc, status);
        return -1;
    }

    cJSON *doc = cJSON_Parse(buf.data ? buf.data : "");
    free(buf.data);
    if (!doc) {
        CWIST_LOG_WARN("flywire: feed returned unparseable JSON; retrying");
        return -1;
    }
    cJSON *okj = cJSON_GetObjectItem(doc, "ok");
    if (!cJSON_IsTrue(okj)) {        const char *err = cJSON_GetObjectItem(doc, "error") && cJSON_IsString(cJSON_GetObjectItem(doc, "error"))
                              ? cJSON_GetObjectItem(doc, "error")->valuestring : "?";
        if (strcmp(err, "journal_purged") == 0) {
            CWIST_LOG_ERROR("flywire: the primary no longer has journal rows for our checkpoint "
                            "(seq %lld). RESEED this replica: fly_board --restore <archive from the primary>.", since);
            cJSON_Delete(doc);
            return 1;
        }
        CWIST_LOG_WARN("flywire: feed error '%s'; retrying", err);
        cJSON_Delete(doc);
        return -1;
    }

    cJSON *ver = cJSON_GetObjectItem(doc, "version");
    if (cJSON_IsString(ver) && ver->valuestring) flywire_maybe_auto_upgrade(ver->valuestring);

    cJSON *rows = cJSON_GetObjectItem(doc, "rows");
    int applied = 0;
    cJSON *row = NULL;
    cJSON_ArrayForEach(row, rows) {
        cJSON *seqj = cJSON_GetObjectItem(row, "seq");
        cJSON *ent = cJSON_GetObjectItem(row, "entity");
        cJSON *idj = cJSON_GetObjectItem(row, "id");
        cJSON *op = cJSON_GetObjectItem(row, "op");
        cJSON *payload = cJSON_GetObjectItem(row, "payload");
        if (!cJSON_IsNumber(seqj) || !cJSON_IsString(ent) || !cJSON_IsNumber(idj) || !cJSON_IsString(op)) continue;
        char *payload_str = payload && cJSON_IsObject(payload) ? cJSON_PrintUnformatted(payload) : strdup("");
        flywire_apply_row(main_conn, (long long)seqj->valuedouble, ent->valuestring, idj->valueint,
                          op->valuestring, payload_str ? payload_str : "");
        free(payload_str);
        flywire_checkpoint_write((long long)seqj->valuedouble);
        applied++;
    }
    if (applied > 0) {
        CWIST_LOG_INFO("flywire: applied %d journal rows (now at seq %lld)", applied, flywire_checkpoint_read());
    }
    cJSON_Delete(doc);
    return 0;
}

static void *flywire_replica_loop(void *arg) {
    (void)arg;
    /* A dedicated short-lived connection per iteration: nothing is held
     * across cwist_app_listen()'s worker fork, and a crash never loses more
     * than the rows already checkpointed. sqlite sections are bracketed by
     * the fork gate exactly like the cleanup worker. */
    while (atomic_load_explicit(&g_flywire_running, memory_order_acquire)) {
        sleep((unsigned)flywire_poll_seconds());
        if (!atomic_load_explicit(&g_flywire_running, memory_order_acquire)) break;

        fly_forkgate_enter();
        sqlite3 *conn = NULL;
        bool open_ok = sqlite3_open(FLY_DB_MAIN_PATH, &conn) == SQLITE_OK;
        if (open_ok) open_ok = db_configure_connection(conn);
        fly_forkgate_leave();
        if (!open_ok) {
            CWIST_LOG_ERROR("flywire: cannot open %s", FLY_DB_MAIN_PATH);
            if (conn) sqlite3_close(conn);
            continue;
        }

        long long since = flywire_checkpoint_read();
        int rc = flywire_poll_once(conn, since);
        if (rc == 0) {
            /* At most one queued file-byte fetch per iteration. */
            int file_id = flywire_dequeue_file();
            if (file_id > 0) {
                fly_forkgate_enter();
                flywire_fetch_file_bytes(file_id, conn);
                fly_forkgate_leave();
            }
        } else if (rc == 1) {
            /* Journal purged past our checkpoint: back off hard and keep
             * telling the operator to reseed. Never auto-restore. */
            sleep(300);
        }

        fly_forkgate_enter();
        sqlite3_close(conn);
        fly_forkgate_leave();
    }
    return NULL;
}

bool flywire_start(void) {
    if (!flywire_is_replica()) return true; /* off or primary: nothing to spawn */
    atomic_store_explicit(&g_flywire_running, true, memory_order_release);
    pthread_t thread;
    if (pthread_create(&thread, NULL, flywire_replica_loop, NULL) != 0) {
        atomic_store_explicit(&g_flywire_running, false, memory_order_release);
        CWIST_LOG_ERROR("flywire: failed to start the replica apply loop");
        return false;
    }
    pthread_detach(thread);
    CWIST_LOG_INFO("flywire: replica mode, syncing from %s every %d s",
                   flywire_primary_url(), flywire_poll_seconds());
    return true;
}

void flywire_stop(void) {
    atomic_store_explicit(&g_flywire_running, false, memory_order_release);
}

/* ---- Admin promotion request flow ----
 *
 * A user registered on a replica asks for admin; the replica forwards the
 * request to the primary's /flywire/promote-request (token-authenticated),
 * where a webmail message is created for the primary's first admin. Approval
 * itself happens through the existing admin dashboard role change, which is
 * journaled as a normal "user" upsert and therefore flows back to replicas. */

static void flywire_redirect(cwist_http_response *res, const char *url) {
    res->status_code = (cwist_http_status_t)302;
    cwist_http_header_add(&res->headers, "Location", url);
    cwist_sstring_assign(res->body, "");
    cwist_http_header_add(&res->headers, "Content-Length", "0");
}

/* POST /flywire/request-admin — replica only (404 otherwise). Forwards the
 * current user's username/email/password_hash to the primary so the account
 * can be created (or matched) there and an admin notified. */
void handler_flywire_request_admin(cwist_http_request *req, cwist_http_response *res) {
    if (!flywire_is_replica()) {
        res->status_code = CWIST_HTTP_NOT_FOUND;
        cwist_sstring_assign(res->body, "Not found");
        return;
    }
    int uid = 0;
    char role[32] = {0};
    if (!auth_require_login(req, res, &uid, role, sizeof(role))) return;
    if (strcmp(role, "admin") == 0) {
        flywire_redirect(res, "/account/settings");
        return;
    }
    cJSON *user = db_user_get_by_id(req->db, uid);
    if (!user) {
        flywire_redirect(res, "/account/settings?msg=admin_request_failed");
        return;
    }
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "username",
        cJSON_GetObjectItem(user, "username") && cJSON_GetObjectItem(user, "username")->valuestring
            ? cJSON_GetObjectItem(user, "username")->valuestring : "");
    cJSON_AddStringToObject(body, "email",
        cJSON_GetObjectItem(user, "email") && cJSON_GetObjectItem(user, "email")->valuestring
            ? cJSON_GetObjectItem(user, "email")->valuestring : "");
    cJSON_AddStringToObject(body, "password_hash",
        cJSON_GetObjectItem(user, "password_hash") && cJSON_GetObjectItem(user, "password_hash")->valuestring
            ? cJSON_GetObjectItem(user, "password_hash")->valuestring : "");
    char *payload = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    cJSON_Delete(user);
    if (!payload) {
        flywire_redirect(res, "/account/settings?msg=admin_request_failed");
        return;
    }

    char url[600];
    const char *base = flywire_primary_url();
    size_t bl = strlen(base);
    snprintf(url, sizeof(url), "%s%s/flywire/promote-request",
             base, (bl > 0 && base[bl - 1] == '/') ? "" : "");

    flywire_buf buf = {0};
    CURL *curl = curl_easy_init();
    bool ok = false;
    if (curl) {
        char token_hdr[192];
        snprintf(token_hdr, sizeof(token_hdr), "X-FlyWire-Token: %s", flywire_token());
        struct curl_slist *headers = NULL;
        headers = curl_slist_append(headers, token_hdr);
        headers = curl_slist_append(headers, "Content-Type: application/json");
        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, flywire_feed_write);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
        CURLcode rc = curl_easy_perform(curl);
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        ok = (rc == CURLE_OK) && status >= 200 && status < 300;
    }
    free(buf.data);
    free(payload);
    if (!ok) CWIST_LOG_WARN("flywire: admin promotion request to primary failed");
    flywire_redirect(res, ok ? "/account/settings?msg=admin_requested"
                             : "/account/settings?msg=admin_request_failed");
}

/* POST /flywire/promote-request — primary only (404 otherwise), token
 * REQUIRED (constant-time compare, same as the feed). Creates the requesting
 * user locally when unknown, then notifies the first admin via webmail. */
void handler_flywire_promote_request(cwist_http_request *req, cwist_http_response *res) {
    if (!flywire_is_primary()) {
        res->status_code = CWIST_HTTP_NOT_FOUND;
        cwist_sstring_assign(res->body, "Not found");
        return;
    }
    const char *token = cwist_http_header_get(req->headers, "X-FlyWire-Token");
    if (!token || !token[0]) token = cwist_query_map_get(req->query_params, "token");
    if (!flywire_token_eq(token ? token : "", flywire_token())) {
        cJSON *err = cJSON_CreateObject();
        cJSON_AddBoolToObject(err, "ok", false);
        cJSON_AddStringToObject(err, "error", "unauthorized");
        flywire_send_json(res, err, CWIST_HTTP_UNAUTHORIZED);
        cJSON_Delete(err);
        return;
    }
    cJSON *doc = (req->body && req->body->data) ? cJSON_Parse(req->body->data) : NULL;
    if (!doc) {
        cJSON *err = cJSON_CreateObject();
        cJSON_AddBoolToObject(err, "ok", false);
        cJSON_AddStringToObject(err, "error", "bad_json");
        flywire_send_json(res, err, CWIST_HTTP_BAD_REQUEST);
        cJSON_Delete(err);
        return;
    }
    const char *username = cJSON_GetObjectItem(doc, "username") && cJSON_IsString(cJSON_GetObjectItem(doc, "username"))
                               ? cJSON_GetObjectItem(doc, "username")->valuestring : NULL;
    const char *email = cJSON_GetObjectItem(doc, "email") && cJSON_IsString(cJSON_GetObjectItem(doc, "email"))
                            ? cJSON_GetObjectItem(doc, "email")->valuestring : "";
    const char *password_hash = cJSON_GetObjectItem(doc, "password_hash") && cJSON_IsString(cJSON_GetObjectItem(doc, "password_hash"))
                                    ? cJSON_GetObjectItem(doc, "password_hash")->valuestring : "";
    if (!username || !username[0] || strlen(username) >= 128) {
        cJSON *err = cJSON_CreateObject();
        cJSON_AddBoolToObject(err, "ok", false);
        cJSON_AddStringToObject(err, "error", "missing_fields");
        flywire_send_json(res, err, CWIST_HTTP_BAD_REQUEST);
        cJSON_Delete(err);
        cJSON_Delete(doc);
        return;
    }

    cJSON *existing = db_user_get_by_username(req->db, username);
    if (!existing) {
        /* Unknown on the primary: create with role "user", verified, storing
         * the hash as given. db_user_create / db_user_set_email_verified
         * journal the row normally, so the replica that asked gets the
         * account back through the regular sync. */
        if (db_user_create(req->db, username, email, password_hash)) {
            cJSON *created = db_user_get_by_username(req->db, username);
            if (created) {
                int new_id = cJSON_GetObjectItem(created, "id") ? cJSON_GetObjectItem(created, "id")->valueint : 0;
                if (new_id > 0) db_user_set_email_verified(req->db, new_id, true);
                cJSON_Delete(created);
            }
            CWIST_LOG_INFO("flywire: promote-request created user '%s' on the primary", username);
        } else {
            CWIST_LOG_ERROR("flywire: promote-request could not create user '%s'", username);
        }
    }
    if (existing) cJSON_Delete(existing);

    /* Notify the primary's first admin by webmail. */
    int admin_id = 0;
    char admin_email[256] = {0};
    {
        sqlite3 *conn = fly_db_conn(req->db);
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(conn, "SELECT id, email FROM users WHERE role='admin' ORDER BY id LIMIT 1", -1, &st, NULL) == SQLITE_OK) {
            if (sqlite3_step(st) == SQLITE_ROW) {
                admin_id = sqlite3_column_int(st, 0);
                const char *e = (const char *)sqlite3_column_text(st, 1);
                if (e) snprintf(admin_email, sizeof(admin_email), "%s", e);
            }
            sqlite3_finalize(st);
        }
    }
    if (admin_id > 0) {
        char from[320];
        snprintf(from, sizeof(from), "postmaster@%s", fly_mail_domain());
        char subject[192];
        snprintf(subject, sizeof(subject), "Admin promotion request: %s", username);
        char body[1024];
        snprintf(body, sizeof(body),
                 "User '%s' (email: %s) registered on a FlyWire replica and requests the admin role.\n\n"
                 "Approve by promoting this user in the admin dashboard.\n",
                 username, email && email[0] ? email : "(none given)");
        if (db_email_create(req->db, admin_id, MAIL_FOLDER_INBOX, from, admin_email,
                            subject, body, NULL, NULL) > 0) {
            CWIST_LOG_INFO("flywire: admin promotion request for '%s' delivered to admin uid=%d",
                           username, admin_id);
        }
    } else {
        CWIST_LOG_WARN("flywire: promote-request for '%s' but no admin user exists on the primary",
                       username);
    }

    cJSON *out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "ok", true);
    flywire_send_json(res, out, CWIST_HTTP_OK);
    cJSON_Delete(out);
    cJSON_Delete(doc);
}

/* ---- Replica write-through proxy ---- */

typedef struct {
    cwist_sstring *body;
} flywire_proxy_ctx;

static size_t flywire_proxy_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    flywire_proxy_ctx *ctx = (flywire_proxy_ctx *)userdata;
    cwist_sstring_append_len(ctx->body, (const char *)ptr, size * nmemb);
    return size * nmemb;
}

/* Header names we forward to the primary (session, request typing, auth). */
static bool flywire_proxy_fwd_header(const char *key) {
    return strcasecmp(key, "Cookie") == 0 ||
           strcasecmp(key, "Content-Type") == 0 ||
           strcasecmp(key, "Authorization") == 0;
}

/* Header names we relay back from the primary's response. */
static bool flywire_proxy_relay_header(const char *key) {
    return strcasecmp(key, "Location") == 0 ||
           strcasecmp(key, "Set-Cookie") == 0 ||
           strcasecmp(key, "Content-Type") == 0;
}

bool flywire_proxy_write(cwist_http_request *req, cwist_http_response *res) {
    const char *base = flywire_primary_url();
    if (!base || !base[0]) return false;

    char url[2048];
    size_t bl = strlen(base);
    const char *path = (req->path && req->path->data) ? req->path->data : "/";
    const char *query = (req->query && req->query->data && req->query->size) ? req->query->data : NULL;
    snprintf(url, sizeof(url), "%s%s%s%s%s",
             base, (bl > 0 && base[bl - 1] == '/') ? "" : "/",
             path[0] == '/' ? path + 1 : path,
             query ? "?" : "", query ? query : "");

    const char *method = cwist_http_method_to_string(req->method);
    if (!method || !method[0]) method = "POST";

    struct curl_slist *headers = NULL;
    for (cwist_http_header_node *h = req->headers; h; h = h->next) {
        if (!h->key || !h->key->data || !h->value || !h->value->data) continue;
        if (!flywire_proxy_fwd_header(h->key->data)) continue;
        char line[1400];
        snprintf(line, sizeof(line), "%s: %s", h->key->data, h->value->data);
        headers = curl_slist_append(headers, line);
    }

    flywire_proxy_ctx ctx = { .body = res->body };
    cwist_sstring_assign(res->body, "");

    CURL *curl = curl_easy_init();
    if (!curl) {
        curl_slist_free_all(headers);
        return false;
    }
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    if (req->body && req->body->data && req->body->size) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req->body->data);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)req->body->size);
    }
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, flywire_proxy_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

    CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    if (rc == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    if (rc != CURLE_OK || status <= 0) {
        CWIST_LOG_WARN("flywire: write proxy to %s %s failed: %s", method, url, curl_easy_strerror(rc));
        curl_easy_cleanup(curl);
        curl_slist_free_all(headers);
        return false;
    }

    res->status_code = (cwist_http_status_t)status;
    /* Relay selected response headers (multiple Set-Cookie supported). */
    struct curl_header *prev = NULL;
    struct curl_header *h = NULL;
    while ((h = curl_easy_nextheader(curl, CURLH_HEADER, 0, prev)) != NULL) {
        if (flywire_proxy_relay_header(h->name))
            cwist_http_header_add(&res->headers, h->name, h->value);
        prev = h;
    }
    CWIST_LOG_INFO("flywire: proxied %s %s -> %ld (%zu bytes)", method, path, status, res->body->size);
    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);
    return true;
}

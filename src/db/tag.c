#define _POSIX_C_SOURCE 200809L
#include "db.h"
#include "db_internal.h"
#include <cwist/core/mem/alloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int db_tag_get_or_create(cwist_db *db, const char *name) {
    const char *sql_sel = "SELECT id FROM tags WHERE name=? LIMIT 1";
    sqlite3_stmt *stmt = NULL;
    int tag_id = 0;
    if (sqlite3_prepare_v2(fly_db_conn(db), sql_sel, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            tag_id = sqlite3_column_int(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }
    if (tag_id > 0) return tag_id;
    const char *sql_ins = "INSERT INTO tags (name) VALUES (?)";
    bool created = false;
    if (sqlite3_prepare_v2(fly_db_conn(db), sql_ins, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_DONE) {
            tag_id = (int)sqlite3_last_insert_rowid(fly_db_conn(db));
            created = true;
        }
        sqlite3_finalize(stmt);
    }
    if (created) db_sync_journal_row(db, "tags", "tag", tag_id);
    return tag_id;
}

bool db_tag_link(cwist_db *db, int post_id, int tag_id) {
    const char *sql = "INSERT OR IGNORE INTO post_tags (post_id, tag_id) VALUES (?,?)";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(fly_db_conn(db), sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_int(stmt, 1, post_id);
    sqlite3_bind_int(stmt, 2, tag_id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc == SQLITE_DONE && sqlite3_changes(fly_db_conn(db)) > 0) {
        char payload[96];
        snprintf(payload, sizeof(payload), "{\"post_id\":%d,\"tag_id\":%d}", post_id, tag_id);
        db_sync_journal(db, "tag_link", post_id, "upsert", payload);
    }
    return rc == SQLITE_DONE;
}

cJSON *db_tag_list_by_post(cwist_db *db, int post_id) {
    const char *sql = "SELECT t.name FROM tags t JOIN post_tags pt ON t.id=pt.tag_id WHERE pt.post_id=? ORDER BY t.name";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(fly_db_conn(db), sql, -1, &stmt, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_int(stmt, 1, post_id);
    return db_sqlite3_rows_to_json(stmt);
}

bool db_tag_clear_by_post(cwist_db *db, int post_id) {
    const char *sql = "DELETE FROM post_tags WHERE post_id=?";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(fly_db_conn(db), sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_int(stmt, 1, post_id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc == SQLITE_DONE && sqlite3_changes(fly_db_conn(db)) > 0) {
        char payload[48];
        snprintf(payload, sizeof(payload), "{\"post_id\":%d}", post_id);
        db_sync_journal(db, "tag_link", post_id, "delete", payload);
    }
    return rc == SQLITE_DONE;
}

/* ---- Tag normalization and listing ---- */

/* Tags travel in URL paths (/tag/<name>) and feed titles, so characters with
 * meaning there are dropped; ASCII letters fold to lower case so "C" and "c"
 * are one tag. Whitespace runs collapse to a single space. */
static size_t tag_normalize(const char *in, size_t in_len, char *out, size_t out_size) {
    size_t o = 0;
    bool pending_space = false;
    size_t i = 0;
    while (i < in_len && (in[i] == '#' || in[i] == ' ' || in[i] == '\t')) i++;
    for (; i < in_len && o + 1 < out_size; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            if (o > 0) pending_space = true;
            continue;
        }
        if (c < 0x20 || strchr("/?#%&<>\"'\\`", c)) continue;
        if (pending_space) {
            if (o + 2 >= out_size) break;
            out[o++] = ' ';
            pending_space = false;
        }
        out[o++] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : (char)c;
    }
    /* Never end inside a multi-byte sequence after truncation. */
    size_t end = o;
    while (end > 0 && ((unsigned char)out[end - 1] & 0xC0) == 0x80) end--;
    if (end > 0) {
        unsigned char lead = (unsigned char)out[end - 1];
        size_t need = (lead & 0xE0) == 0xC0 ? 2 : (lead & 0xF0) == 0xE0 ? 3 : (lead & 0xF8) == 0xF0 ? 4 : 1;
        if (o - (end - 1) < need) o = end - 1;
    }
    out[o] = '\0';
    return o;
}

int db_tag_set_for_post(cwist_db *db, int post_id, const char *csv) {
    if (post_id <= 0) return 0;
    if (!db_tag_clear_by_post(db, post_id)) return -1;
    if (!csv) return 0;
    char seen[DB_TAG_MAX_PER_POST][DB_TAG_MAX_BYTES];
    int n = 0;
    const char *p = csv;
    while (*p && n < DB_TAG_MAX_PER_POST) {
        const char *end = p;
        while (*end && *end != ',') end++;
        char name[DB_TAG_MAX_BYTES];
        if (tag_normalize(p, (size_t)(end - p), name, sizeof(name)) > 0) {
            bool dup = false;
            for (int i = 0; i < n; i++) {
                if (strcmp(seen[i], name) == 0) { dup = true; break; }
            }
            if (!dup) {
                int tag_id = db_tag_get_or_create(db, name);
                if (tag_id > 0 && db_tag_link(db, post_id, tag_id)) {
                    snprintf(seen[n++], DB_TAG_MAX_BYTES, "%s", name);
                }
            }
        }
        p = *end ? end + 1 : end;
    }
    return n;
}

bool db_tag_name_valid(const char *name) {
    if (!name || !name[0]) return false;
    char norm[DB_TAG_MAX_BYTES];
    size_t len = strlen(name);
    if (len >= sizeof(norm)) return false;
    return tag_normalize(name, len, norm, sizeof(norm)) == len && strcmp(norm, name) == 0;
}

cJSON *db_tag_list_public(cwist_db *db) {
    const char *sql =
        "SELECT t.name AS name, COUNT(*) AS n FROM tags t"
        " JOIN post_tags pt ON pt.tag_id=t.id"
        " JOIN posts p ON p.id=pt.post_id"
        " WHERE " POST_PUBLIC_SQL
        " GROUP BY t.id ORDER BY n DESC, t.name";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(fly_db_conn(db), sql, -1, &stmt, NULL) != SQLITE_OK) return NULL;
    return db_sqlite3_rows_to_json(stmt);
}

cJSON *db_post_list_by_tag(cwist_db *db, const char *tag, int limit, int offset) {
    const char *sql =
        "SELECT p.*, u.username as author_name, b.name as board_name FROM posts p"
        " JOIN post_tags pt ON pt.post_id=p.id JOIN tags t ON t.id=pt.tag_id"
        " LEFT JOIN users u ON p.user_id=u.id LEFT JOIN boards b ON p.board_id=b.id"
        " WHERE t.name=? AND " POST_PUBLIC_SQL
        " ORDER BY p.created_at DESC LIMIT ? OFFSET ?";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(fly_db_conn(db), sql, -1, &stmt, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_text(stmt, 1, tag ? tag : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, limit);
    sqlite3_bind_int(stmt, 3, offset);
    return db_sqlite3_rows_to_json(stmt);
}

int db_post_count_by_tag(cwist_db *db, const char *tag) {
    const char *sql =
        "SELECT COUNT(*) FROM posts p JOIN post_tags pt ON pt.post_id=p.id JOIN tags t ON t.id=pt.tag_id"
        " WHERE t.name=? AND " POST_PUBLIC_SQL;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(fly_db_conn(db), sql, -1, &stmt, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_text(stmt, 1, tag ? tag : "", -1, SQLITE_TRANSIENT);
    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return count;
}

cJSON *db_post_related_by_tags(cwist_db *db, int post_id, int limit) {
    const char *sql =
        "SELECT p.slug AS slug, p.title AS title, p.created_at AS created_at, COUNT(*) AS shared FROM post_tags mine"
        " JOIN post_tags other ON other.tag_id=mine.tag_id AND other.post_id<>mine.post_id"
        " JOIN posts p ON p.id=other.post_id"
        " WHERE mine.post_id=? AND " POST_PUBLIC_SQL
        " GROUP BY p.id ORDER BY shared DESC, p.created_at DESC LIMIT ?";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(fly_db_conn(db), sql, -1, &stmt, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_int(stmt, 1, post_id);
    sqlite3_bind_int(stmt, 2, limit);
    return db_sqlite3_rows_to_json(stmt);
}

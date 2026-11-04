#define _POSIX_C_SOURCE 200809L
#include "moderation.h"
#include "db_internal.h"
#include "engine/community.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MODERATION_NEW_ACCOUNT_HOURS 24
#define MODERATION_NEW_POST_LIMIT 1
#define MODERATION_NEW_COMMENT_LIMIT 3
#define MODERATION_TRUST_ACCOUNT_DAYS 7
#define MODERATION_TRUST_APPROVED_ITEMS 3
#define MODERATION_MAX_LINKS 5

static moderation_decision_t decision(moderation_action_t action, const char *reason) {
    moderation_decision_t d = {action, reason};
    return d;
}

bool moderation_can_review(const char *role) {
    return community_role_can_moderate(role);
}

bool db_moderation_migrate(cwist_db *db) {
    return db_exec_sql(db,
        "CREATE TABLE IF NOT EXISTS moderation_items ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "target_type TEXT NOT NULL,"
        "target_id INTEGER NOT NULL,"
        "user_id INTEGER NOT NULL DEFAULT 0,"
        "content TEXT NOT NULL DEFAULT '',"
        "author_name TEXT NOT NULL DEFAULT '',"
        "target_kind TEXT NOT NULL DEFAULT '',"
        "target_ref_id INTEGER NOT NULL DEFAULT 0,"
        "parent_id INTEGER NOT NULL DEFAULT 0,"
        "reason TEXT NOT NULL DEFAULT '',"
        "status TEXT NOT NULL DEFAULT 'pending',"
        "reviewer_uid INTEGER,"
        "created_at DATETIME DEFAULT CURRENT_TIMESTAMP,"
        "reviewed_at DATETIME,"
        "UNIQUE(target_type,target_id))") &&
        db_exec_sql(db, "CREATE INDEX IF NOT EXISTS idx_moderation_pending ON moderation_items(status,created_at)") &&
        db_exec_sql(db, "CREATE INDEX IF NOT EXISTS idx_moderation_user ON moderation_items(user_id,status)");
}

static char *normalized(const char *content) {
    if (!content) return NULL;
    size_t len = strlen(content);
    char *out = malloc(len + 1);
    if (!out) return NULL;
    size_t n = 0;
    bool space = false;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)content[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v') {
            space = n > 0;
            continue;
        }
        if (space) {
            out[n++] = ' ';
            space = false;
        }
        out[n++] = (char)(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    }
    out[n] = '\0';
    return out;
}

static int link_count(const char *content) {
    int count = 0;
    if (!content) return 0;
    for (const char *p = content; *p; p++) {
        if (strncasecmp(p, "http://", 7) == 0) {
            count++;
            p += 6;
        } else if (strncasecmp(p, "https://", 8) == 0) {
            count++;
            p += 7;
        }
    }
    return count;
}

static bool duplicate_rows(sqlite3_stmt *stmt, const char *want) {
    bool found = false;
    while (!found && sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char *text = sqlite3_column_text(stmt, 0);
        char *candidate = normalized((const char *)(text ? text : (const unsigned char *)""));
        if (candidate) {
            found = strcmp(candidate, want) == 0;
            free(candidate);
        }
    }
    sqlite3_finalize(stmt);
    return found;
}

static bool duplicate_content(cwist_db *db, int uid, const char *target_type, const char *want) {
    sqlite3_stmt *stmt = NULL;
    if (strcmp(target_type, MODERATION_POST) == 0) {
        const char *sql = "SELECT content FROM posts WHERE COALESCE(user_id,0)=?";
        if (sqlite3_prepare_v2(fly_db_conn(db), sql, -1, &stmt, NULL) != SQLITE_OK) return false;
        sqlite3_bind_int(stmt, 1, uid);
        return duplicate_rows(stmt, want);
    }
    const char *sql = "SELECT content FROM comments WHERE user_id=? AND deleted=0";
    if (sqlite3_prepare_v2(db_comment_conn(), sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, uid);
        if (duplicate_rows(stmt, want)) return true;
    }
    sql = "SELECT content FROM moderation_items WHERE target_type='comment' AND user_id=? AND status='pending'";
    if (sqlite3_prepare_v2(fly_db_conn(db), sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_int(stmt, 1, uid);
    return duplicate_rows(stmt, want);
}

static bool account_is_new(cwist_db *db, int uid) {
    if (uid <= 0) return false;
    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT created_at>CURRENT_TIMESTAMP OR created_at>datetime('now','-24 hours') FROM users WHERE id=?";
    if (sqlite3_prepare_v2(fly_db_conn(db), sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_int(stmt, 1, uid);
    bool is_new = sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int(stmt, 0) == 1;
    sqlite3_finalize(stmt);
    return is_new;
}

static int recent_count(cwist_db *db, int uid, const char *target_type) {
    bool post = strcmp(target_type, MODERATION_POST) == 0;
    sqlite3_stmt *stmt = NULL;
    const char *sql = post
        ? "SELECT COUNT(*) FROM posts WHERE COALESCE(user_id,0)=? AND created_at>=datetime('now','-24 hours')"
        : "SELECT COUNT(*) FROM comments WHERE user_id=? AND created_at>=datetime('now','-24 hours')";
    sqlite3 *conn = post ? fly_db_conn(db) : db_comment_conn();
    if (sqlite3_prepare_v2(conn, sql, -1, &stmt, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int(stmt, 1, uid);
    int count = sqlite3_step(stmt) == SQLITE_ROW ? sqlite3_column_int(stmt, 0) : 0;
    sqlite3_finalize(stmt);
    if (!post) {
        sql = "SELECT COUNT(*) FROM moderation_items WHERE target_type='comment' AND user_id=? AND status='pending' AND created_at>=datetime('now','-24 hours')";
        if (sqlite3_prepare_v2(fly_db_conn(db), sql, -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, uid);
            if (sqlite3_step(stmt) == SQLITE_ROW) count += sqlite3_column_int(stmt, 0);
            sqlite3_finalize(stmt);
        }
    }
    return count;
}

static bool trusted(cwist_db *db, int uid) {
    if (uid <= 0) return false;
    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT created_at<=datetime('now','-7 days') FROM users WHERE id=?";
    if (sqlite3_prepare_v2(fly_db_conn(db), sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, uid);
        bool old_enough = sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int(stmt, 0) == 1;
        sqlite3_finalize(stmt);
        if (old_enough) return true;
    }
    sql = "SELECT COUNT(*)>=3 FROM moderation_items WHERE user_id=? AND status='approved'";
    if (sqlite3_prepare_v2(fly_db_conn(db), sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_int(stmt, 1, uid);
    bool enough = sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int(stmt, 0) == 1;
    sqlite3_finalize(stmt);
    return enough;
}

moderation_decision_t moderation_check_submission(cwist_db *db, int uid, const char *role,
                                                  const char *target_type, const char *content) {
    if (!db || !target_type || (strcmp(target_type, MODERATION_POST) != 0 &&
                               strcmp(target_type, MODERATION_COMMENT) != 0)) {
        return decision(MODERATION_REJECT, "invalid");
    }
    if (moderation_can_review(role) || (!community.new_user_limits &&
        !community.trust && !community.spam_detection && !community.approval_queue)) {
        return decision(MODERATION_ALLOW, "");
    }
    bool is_trusted = community.trust && trusted(db, uid);
    if (community.new_user_limits && !is_trusted && account_is_new(db, uid)) {
        int limit = strcmp(target_type, MODERATION_POST) == 0 ? MODERATION_NEW_POST_LIMIT : MODERATION_NEW_COMMENT_LIMIT;
        if (recent_count(db, uid, target_type) >= limit) {
            return decision(MODERATION_REJECT, strcmp(target_type, MODERATION_POST) == 0
                ? "new_user_post_limit" : "new_user_comment_limit");
        }
    }
    if (community.spam_detection) {
        if (link_count(content) > MODERATION_MAX_LINKS) return decision(MODERATION_PENDING, "excessive_links");
        char *normal = normalized(content);
        bool duplicate = normal && duplicate_content(db, uid, target_type, normal);
        free(normal);
        if (duplicate) return decision(MODERATION_PENDING, "duplicate_content");
    }
    if (community.approval_queue && !is_trusted) return decision(MODERATION_PENDING, "approval_queue");
    return decision(MODERATION_ALLOW, "");
}

static bool queue_row(cwist_db *db, const char *target_type, int target_id, int uid,
                      const char *content, const char *author_name, const char *target_kind,
                      int target_ref_id, int parent_id, const char *reason) {
    const char *sql = "INSERT INTO moderation_items (target_type,target_id,user_id,content,author_name,target_kind,target_ref_id,parent_id,reason) VALUES (?,?,?,?,?,?,?,?,?)";
    sqlite3_stmt *stmt = NULL;
    sqlite3 *conn = fly_db_conn(db);
    if (sqlite3_prepare_v2(conn, sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, target_type, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, target_id);
    sqlite3_bind_int(stmt, 3, uid);
    sqlite3_bind_text(stmt, 4, content ? content : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, author_name ? author_name : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, target_kind ? target_kind : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 7, target_ref_id);
    sqlite3_bind_int(stmt, 8, parent_id);
    sqlite3_bind_text(stmt, 9, reason ? reason : "", -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool db_moderation_queue_post(cwist_db *db, int post_id, int uid, const char *reason) {
    return post_id > 0 && queue_row(db, MODERATION_POST, post_id, uid, "", "", "", 0, 0, reason);
}

int db_moderation_create_pending_comment(cwist_db *db, const char *target_type, int target_id,
                                         int uid, const char *author_name, int parent_id,
                                         const char *content, const char *reason) {
    int id = db_comment_create(db, target_type, target_id, uid, author_name, parent_id, content);
    if (id <= 0) return -1;
    if (!db_comment_delete_admin(db, id) ||
        !queue_row(db, MODERATION_COMMENT, id, uid, content, author_name, target_type, target_id, parent_id, reason)) {
        sqlite3_stmt *stmt = NULL;
        sqlite3 *conn = db_comment_conn();
        if (sqlite3_prepare_v2(conn, "DELETE FROM comments WHERE id=?", -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, id);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
        return -1;
    }
    return id;
}

cJSON *db_moderation_list_pending(cwist_db *db) {
    const char *sql = "SELECT m.*,p.title AS post_title,p.slug AS post_slug,p.content AS post_content FROM moderation_items m "
                      "LEFT JOIN posts p ON m.target_type='post' AND p.id=m.target_id "
                      "WHERE m.status='pending' ORDER BY m.created_at,m.id LIMIT 500";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(fly_db_conn(db), sql, -1, &stmt, NULL) != SQLITE_OK) return NULL;
    return db_sqlite3_rows_to_json(stmt);
}

static cJSON *pending_item(cwist_db *db, const char *target_type, int target_id) {
    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT * FROM moderation_items WHERE target_type=? AND target_id=? AND status='pending'";
    if (sqlite3_prepare_v2(fly_db_conn(db), sql, -1, &stmt, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_text(stmt, 1, target_type, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, target_id);
    return db_sqlite3_row_to_json(stmt);
}

static bool finish_review(cwist_db *db, const char *target_type, int target_id, int reviewer_uid, const char *status) {
    sqlite3_stmt *stmt = NULL;
    const char *sql = "UPDATE moderation_items SET status=?,reviewer_uid=?,reviewed_at=CURRENT_TIMESTAMP WHERE target_type=? AND target_id=? AND status='pending'";
    sqlite3 *conn = fly_db_conn(db);
    if (sqlite3_prepare_v2(conn, sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, status, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, reviewer_uid);
    sqlite3_bind_text(stmt, 3, target_type, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 4, target_id);
    int rc = sqlite3_step(stmt);
    bool changed = rc == SQLITE_DONE && sqlite3_changes(conn) == 1;
    sqlite3_finalize(stmt);
    return changed;
}

bool db_moderation_approve(cwist_db *db, const char *target_type, int target_id, int reviewer_uid) {
    cJSON *item = pending_item(db, target_type, target_id);
    if (!item) return false;
    bool ok = false;
    if (strcmp(target_type, MODERATION_POST) == 0) {
        sqlite3_stmt *stmt = NULL;
        sqlite3 *conn = fly_db_conn(db);
        const char *sql = "UPDATE posts SET status='published',announced=0,updated_at=CURRENT_TIMESTAMP WHERE id=? AND status='pending'";
        if (sqlite3_prepare_v2(conn, sql, -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, target_id);
            int rc = sqlite3_step(stmt);
            ok = rc == SQLITE_DONE && sqlite3_changes(conn) == 1;
            sqlite3_finalize(stmt);
        }
        if (ok) {
            db_search_index_post(db, target_id);
            db_sync_journal_row(db, "posts", "post", target_id);
        }
    } else if (strcmp(target_type, MODERATION_COMMENT) == 0) {
        cJSON *content = cJSON_GetObjectItem(item, "content");
        sqlite3_stmt *stmt = NULL;
        sqlite3 *conn = db_comment_conn();
        const char *sql = "UPDATE comments SET content=?,deleted=0,updated_at=CURRENT_TIMESTAMP WHERE id=? AND deleted=1";
        if (cJSON_IsString(content) && sqlite3_prepare_v2(conn, sql, -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, content->valuestring, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(stmt, 2, target_id);
            int rc = sqlite3_step(stmt);
            ok = rc == SQLITE_DONE && sqlite3_changes(conn) == 1;
            sqlite3_finalize(stmt);
        }
        if (ok) db_sync_journal_external_row(conn, "comments", "id", "comment", target_id);
    }
    cJSON_Delete(item);
    return ok && finish_review(db, target_type, target_id, reviewer_uid, "approved");
}

bool db_moderation_reject(cwist_db *db, const char *target_type, int target_id, int reviewer_uid) {
    cJSON *item = pending_item(db, target_type, target_id);
    if (!item) return false;
    cJSON_Delete(item);
    bool ok = true;
    if (strcmp(target_type, MODERATION_POST) == 0) ok = db_post_delete(db, target_id);
    else if (strcmp(target_type, MODERATION_COMMENT) != 0) ok = false;
    return ok && finish_review(db, target_type, target_id, reviewer_uid, "rejected");
}

void db_moderation_filter_comments(cwist_db *db, cJSON *comments) {
    if (!db || !comments || cJSON_GetArraySize(comments) == 0) return;
    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT target_id FROM moderation_items WHERE target_type='comment' AND status='pending'";
    if (sqlite3_prepare_v2(fly_db_conn(db), sql, -1, &stmt, NULL) != SQLITE_OK) return;
    cJSON *pending = db_sqlite3_rows_to_json(stmt);
    for (int i = cJSON_GetArraySize(comments) - 1; i >= 0; i--) {
        cJSON *id = cJSON_GetObjectItem(cJSON_GetArrayItem(comments, i), "id");
        if (!cJSON_IsNumber(id)) continue;
        cJSON *row = NULL;
        cJSON_ArrayForEach(row, pending) {
            cJSON *pending_id = cJSON_GetObjectItem(row, "target_id");
            if (cJSON_IsNumber(pending_id) && pending_id->valueint == id->valueint) {
                cJSON_DeleteItemFromArray(comments, i);
                break;
            }
        }
    }
    if (pending) cJSON_Delete(pending);
}

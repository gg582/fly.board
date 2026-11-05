#define _POSIX_C_SOURCE 200809L
#include "moderation.h"
#include "db_internal.h"
#include "engine/community.h"
#include "utils/moderation_filter.h"
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
        db_exec_sql(db, "CREATE INDEX IF NOT EXISTS idx_moderation_user ON moderation_items(user_id,status)") &&
        db_exec_sql(db,
        "CREATE TABLE IF NOT EXISTS moderation_external_requests ("
        "correlation_id TEXT PRIMARY KEY,"
        "target_type TEXT NOT NULL,"
        "target_id INTEGER NOT NULL,"
        "issued_at INTEGER NOT NULL,"
        "expires_at INTEGER NOT NULL,"
        "UNIQUE(target_type,target_id))") &&
        db_exec_sql(db,
        "CREATE TABLE IF NOT EXISTS moderation_decision_receipts ("
        "decision_id TEXT PRIMARY KEY,"
        "correlation_id TEXT NOT NULL,"
        "action TEXT NOT NULL,"
        "issued_at INTEGER NOT NULL,"
        "reason TEXT NOT NULL DEFAULT '',"
        "created_at DATETIME DEFAULT CURRENT_TIMESTAMP)");
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
    if (moderation_can_review(role)) return decision(MODERATION_ALLOW, "");
    moderation_filter_policy_t filter = moderation_filter_get();
    if (!filter.enabled && !community.new_user_limits &&
        !community.trust && !community.spam_detection && !community.approval_queue) {
        return decision(MODERATION_ALLOW, "");
    }
    moderation_filter_action_t filter_action;
    if (filter.enabled && moderation_filter_check(content, &filter_action)) {
        return decision(filter_action == MODERATION_FILTER_REJECT ? MODERATION_REJECT : MODERATION_PENDING,
                        "banned_content");
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

int db_moderation_target_board_id(cwist_db *db, const char *target_type, int target_id) {
    if (!db || !target_type || target_id <= 0) return 0;
    int post_id = target_id;
    if (strcmp(target_type, MODERATION_COMMENT) == 0) {
        cJSON *comment = db_comment_get_by_id(db, target_id);
        post_id = 0;
        cJSON *kind = comment ? cJSON_GetObjectItem(comment, "target_type") : NULL;
        cJSON *ref = comment ? cJSON_GetObjectItem(comment, "target_id") : NULL;
        if (cJSON_IsString(kind) && strcmp(kind->valuestring, "post") == 0 && cJSON_IsNumber(ref))
            post_id = ref->valueint;
        if (comment) cJSON_Delete(comment);
    } else if (strcmp(target_type, MODERATION_POST) != 0) {
        return 0;
    }
    if (post_id <= 0) return 0;
    cJSON *post = db_post_get_by_id(db, post_id);
    cJSON *board = post ? cJSON_GetObjectItem(post, "board_id") : NULL;
    int board_id = cJSON_IsNumber(board) ? board->valueint : 0;
    if (post) cJSON_Delete(post);
    return board_id;
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

cJSON *db_moderation_external_create(cwist_db *db, const char *target_type, int target_id,
                                     const char *correlation_id, long long issued_at, long long expires_at) {
    if (!db || !correlation_id || target_id <= 0 || expires_at <= issued_at) return NULL;
    sqlite3 *conn = fly_db_conn(db);
    sqlite3_stmt *stmt = NULL;
    const char *insert = "INSERT INTO moderation_external_requests "
                         "(correlation_id,target_type,target_id,issued_at,expires_at) VALUES (?,?,?,?,?)";
    if (sqlite3_prepare_v2(conn, insert, -1, &stmt, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_text(stmt, 1, correlation_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, target_type, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, target_id);
    sqlite3_bind_int64(stmt, 4, issued_at);
    sqlite3_bind_int64(stmt, 5, expires_at);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return NULL;

    const char *select = strcmp(target_type, MODERATION_POST) == 0
        ? "SELECT p.content,m.reason FROM moderation_items m JOIN posts p ON p.id=m.target_id "
          "WHERE m.target_type='post' AND m.target_id=? AND m.status='pending'"
        : "SELECT m.content,m.reason FROM moderation_items m "
          "WHERE m.target_type='comment' AND m.target_id=? AND m.status='pending'";
    stmt = NULL;
    if (sqlite3_prepare_v2(conn, select, -1, &stmt, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_int(stmt, 1, target_id);
    cJSON *item = db_sqlite3_row_to_json(stmt);
    return item;
}

static const char *external_action_name(moderation_action_t action) {
    if (action == MODERATION_ALLOW) return "allow";
    if (action == MODERATION_PENDING) return "pending";
    return "reject";
}

moderation_external_result_t db_moderation_external_apply(cwist_db *db, const char *correlation_id,
                                                         const char *decision_id, moderation_action_t action,
                                                         long long issued_at, const char *reason, long long now) {
    if (!db || !correlation_id || !decision_id || !reason) return MODERATION_EXTERNAL_ERROR;
    sqlite3 *conn = fly_db_conn(db);
    sqlite3_stmt *stmt = NULL;
    const char *receipt_sql = "SELECT correlation_id,action,issued_at,reason FROM moderation_decision_receipts WHERE decision_id=?";
    if (sqlite3_prepare_v2(conn, receipt_sql, -1, &stmt, NULL) != SQLITE_OK) return MODERATION_EXTERNAL_ERROR;
    sqlite3_bind_text(stmt, 1, decision_id, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char *receipt_correlation = sqlite3_column_text(stmt, 0);
        const unsigned char *receipt_action = sqlite3_column_text(stmt, 1);
        const unsigned char *receipt_reason = sqlite3_column_text(stmt, 3);
        bool same = receipt_correlation && receipt_action && receipt_reason &&
                    strcmp((const char *)receipt_correlation, correlation_id) == 0 &&
                    strcmp((const char *)receipt_action, external_action_name(action)) == 0 &&
                    sqlite3_column_int64(stmt, 2) == issued_at &&
                    strcmp((const char *)receipt_reason, reason) == 0;
        sqlite3_finalize(stmt);
        return same ? MODERATION_EXTERNAL_DUPLICATE : MODERATION_EXTERNAL_REJECTED;
    }
    sqlite3_finalize(stmt);

    const char *request_sql = "SELECT r.target_type,r.target_id,r.issued_at,r.expires_at "
                              "FROM moderation_external_requests r JOIN moderation_items m "
                              "ON m.target_type=r.target_type AND m.target_id=r.target_id "
                              "WHERE r.correlation_id=? AND m.status='pending'";
    stmt = NULL;
    if (sqlite3_prepare_v2(conn, request_sql, -1, &stmt, NULL) != SQLITE_OK) return MODERATION_EXTERNAL_ERROR;
    sqlite3_bind_text(stmt, 1, correlation_id, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return MODERATION_EXTERNAL_REJECTED;
    }
    char target_type[16];
    snprintf(target_type, sizeof(target_type), "%s", sqlite3_column_text(stmt, 0));
    int target_id = sqlite3_column_int(stmt, 1);
    long long request_issued = sqlite3_column_int64(stmt, 2);
    long long expires_at = sqlite3_column_int64(stmt, 3);
    sqlite3_finalize(stmt);
    if (now > expires_at || issued_at > now + 300 || issued_at < request_issued - 300) {
        return MODERATION_EXTERNAL_REJECTED;
    }

    const char *insert = "INSERT INTO moderation_decision_receipts "
                         "(decision_id,correlation_id,action,issued_at,reason) VALUES (?,?,?,?,?)";
    stmt = NULL;
    if (sqlite3_prepare_v2(conn, insert, -1, &stmt, NULL) != SQLITE_OK) return MODERATION_EXTERNAL_ERROR;
    sqlite3_bind_text(stmt, 1, decision_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, correlation_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, external_action_name(action), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 4, issued_at);
    sqlite3_bind_text(stmt, 5, reason, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return MODERATION_EXTERNAL_DUPLICATE;

    bool ok = true;
    if (action == MODERATION_ALLOW) ok = db_moderation_approve(db, target_type, target_id, 0);
    else if (action == MODERATION_REJECT) ok = db_moderation_reject(db, target_type, target_id, 0);
    if (!ok) {
        stmt = NULL;
        if (sqlite3_prepare_v2(conn, "DELETE FROM moderation_decision_receipts WHERE decision_id=?", -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, decision_id, -1, SQLITE_TRANSIENT);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
        return MODERATION_EXTERNAL_ERROR;
    }
    return MODERATION_EXTERNAL_APPLIED;
}

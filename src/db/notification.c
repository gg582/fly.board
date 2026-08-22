#define _POSIX_C_SOURCE 200809L
#include "db.h"
#include "db_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Notifications live in the main database (see db_migrate). Request handlers
 * pass their cwist_db so queries run on the request connection. The NATS
 * worker thread has no cwist_db, so a lazily opened dedicated connection is
 * kept for that path; it is re-opened if the process forks (SQLite handles
 * must not be shared across fork()). */
static sqlite3 *g_notif_aux = NULL;
static pid_t g_notif_aux_pid = 0;

static sqlite3 *notif_conn(cwist_db *db) {
    if (db && fly_db_conn(db)) return fly_db_conn(db);
    pid_t pid = getpid();
    if (g_notif_aux && g_notif_aux_pid != pid) {
        sqlite3_close(g_notif_aux);
        g_notif_aux = NULL;
    }
    if (!g_notif_aux) {
        if (sqlite3_open("data/blog.db", &g_notif_aux) != SQLITE_OK) {
            sqlite3_close(g_notif_aux);
            g_notif_aux = NULL;
            return NULL;
        }
        if (!db_configure_connection(g_notif_aux)) {
            sqlite3_close(g_notif_aux);
            g_notif_aux = NULL;
            return NULL;
        }
        g_notif_aux_pid = pid;
    }
    return g_notif_aux;
}

bool db_notification_create(cwist_db *db, int user_id, const char *actor_name, const char *kind, int post_id, const char *post_slug, int comment_id, const char *excerpt) {
    sqlite3 *conn = notif_conn(db);
    if (!conn || user_id <= 0 || !kind || !kind[0]) return false;
    const char *sql = "INSERT INTO notifications (user_id, actor_name, kind, post_id, post_slug, comment_id, excerpt) VALUES (?,?,?,?,?,?,?)";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(conn, sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_int(stmt, 1, user_id);
    sqlite3_bind_text(stmt, 2, actor_name ? actor_name : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, kind, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 4, post_id);
    sqlite3_bind_text(stmt, 5, post_slug ? post_slug : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 6, comment_id);
    sqlite3_bind_text(stmt, 7, excerpt ? excerpt : "", -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

int db_notification_unread_count(cwist_db *db, int user_id) {
    sqlite3 *conn = notif_conn(db);
    if (!conn || user_id <= 0) return 0;
    const char *sql = "SELECT COUNT(*) FROM notifications WHERE user_id=? AND is_read=0";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(conn, sql, -1, &stmt, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int(stmt, 1, user_id);
    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return count;
}

cJSON *db_notification_list(cwist_db *db, int user_id, int limit) {
    sqlite3 *conn = notif_conn(db);
    if (!conn || user_id <= 0) return NULL;
    if (limit <= 0) limit = 100;
    const char *sql = "SELECT id, user_id, actor_name, kind, post_id, post_slug, comment_id, excerpt, is_read, created_at FROM notifications WHERE user_id=? ORDER BY created_at DESC, id DESC LIMIT ?";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(conn, sql, -1, &stmt, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_int(stmt, 1, user_id);
    sqlite3_bind_int(stmt, 2, limit);
    return db_sqlite3_rows_to_json(stmt);
}

bool db_notification_mark_all_read(cwist_db *db, int user_id) {
    sqlite3 *conn = notif_conn(db);
    if (!conn || user_id <= 0) return false;
    const char *sql = "UPDATE notifications SET is_read=1 WHERE user_id=? AND is_read=0";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(conn, sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_int(stmt, 1, user_id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool db_notification_deliver_federated(long user_id, const char *actor_name, const char *kind, const char *post_slug, const char *excerpt) {
    sqlite3 *conn = notif_conn(NULL);
    if (!conn || user_id <= 0) return false;
    /* Only deliver when the recipient exists in the local users table. */
    const char *check = "SELECT 1 FROM users WHERE id=? LIMIT 1";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(conn, check, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)user_id);
    bool exists = sqlite3_step(stmt) == SQLITE_ROW;
    sqlite3_finalize(stmt);
    if (!exists) return false;
    return db_notification_create(NULL, (int)user_id, actor_name, kind, 0, post_slug, 0, excerpt);
}

#define _POSIX_C_SOURCE 200809L
#include "db.h"
#include "db_internal.h"
#include "db_portability.h"
#include "config/config.h"
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
        if (fly_db_open_main(&g_notif_aux) != SQLITE_OK) {
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

static char *actor_history_append(const char *history, const char *actor_name) {
    cJSON *actors = cJSON_Parse(history ? history : "");
    if (!cJSON_IsArray(actors)) {
        cJSON_Delete(actors);
        actors = cJSON_CreateArray();
    }
    if (!actors) return NULL;
    const char *actor = actor_name ? actor_name : "";
    bool found = false;
    cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, actors) {
        if (cJSON_IsString(entry) && entry->valuestring && strcmp(entry->valuestring, actor) == 0) {
            found = true;
            break;
        }
    }
    if (!found) cJSON_AddItemToArray(actors, cJSON_CreateString(actor));
    char *json = cJSON_PrintUnformatted(actors);
    cJSON_Delete(actors);
    return json;
}

static char *actor_history_new(const char *actor_name) {
    return actor_history_append(NULL, actor_name);
}

static bool notification_event_exists(sqlite3 *conn, const char *event_id) {
    if (!event_id || !event_id[0]) return false;
    const char *sql = "SELECT 1 FROM notifications WHERE event_id=? LIMIT 1";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(conn, sql, -1, &stmt, NULL) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, event_id, -1, SQLITE_TRANSIENT);
    bool exists = sqlite3_step(stmt) == SQLITE_ROW;
    sqlite3_finalize(stmt);
    return exists;
}

static bool notification_store(cwist_db *db, int user_id, const char *actor_name,
                               const char *kind, int post_id, const char *post_slug,
                               int comment_id, const char *excerpt, const char *event_id) {
    sqlite3 *conn = notif_conn(db);
    if (!conn || user_id <= 0 || !kind || !kind[0]) return false;
    const char *actor = actor_name ? actor_name : "";
    const char *slug = post_slug ? post_slug : "";
    const char *body = excerpt ? excerpt : "";
    bool aggregate = g_config.notification_aggregation;
    bool transactional = aggregate || (event_id && event_id[0]);

    if (transactional && sqlite3_exec(conn, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK) return false;
    if (notification_event_exists(conn, event_id)) {
        if (transactional) sqlite3_exec(conn, "COMMIT", NULL, NULL, NULL);
        return true;
    }

    int notification_id = 0;
    char *history = NULL;
    if (aggregate) {
        const char *find =
            "SELECT id, actor_history FROM notifications "
            "WHERE user_id=? AND kind=? AND post_id=? AND post_slug=? "
            "AND created_at>=datetime('now', printf('-%d seconds', ?)) "
            "ORDER BY created_at DESC, id DESC LIMIT 1";
        sqlite3_stmt *find_stmt = NULL;
        if (sqlite3_prepare_v2(conn, find, -1, &find_stmt, NULL) != SQLITE_OK) goto rollback;
        sqlite3_bind_int(find_stmt, 1, user_id);
        sqlite3_bind_text(find_stmt, 2, kind, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(find_stmt, 3, post_id);
        sqlite3_bind_text(find_stmt, 4, slug, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(find_stmt, 5, g_config.notification_window_seconds);
        if (sqlite3_step(find_stmt) == SQLITE_ROW) {
            notification_id = sqlite3_column_int(find_stmt, 0);
            history = actor_history_append((const char *)sqlite3_column_text(find_stmt, 1), actor);
        }
        sqlite3_finalize(find_stmt);
        if (notification_id > 0 && !history) goto rollback;
    }

    int distinct_actors = 1;
    if (history) {
        cJSON *actors = cJSON_Parse(history);
        if (cJSON_IsArray(actors)) distinct_actors = cJSON_GetArraySize(actors);
        cJSON_Delete(actors);
    }
    int rc;
    if (notification_id > 0) {
        const char *update =
            "UPDATE notifications SET actor_name=?, comment_id=?, excerpt=?, is_read=0, "
            "created_at=CURRENT_TIMESTAMP, actor_count=?, actor_history=?, event_id=? "
            "WHERE id=?";
        sqlite3_stmt *stmt = NULL;
        if (sqlite3_prepare_v2(conn, update, -1, &stmt, NULL) != SQLITE_OK) goto rollback;
        sqlite3_bind_text(stmt, 1, actor, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 2, comment_id);
        sqlite3_bind_text(stmt, 3, body, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 4, distinct_actors);
        sqlite3_bind_text(stmt, 5, history, -1, SQLITE_TRANSIENT);
        if (event_id && event_id[0]) sqlite3_bind_text(stmt, 6, event_id, -1, SQLITE_TRANSIENT);
        else sqlite3_bind_null(stmt, 6);
        sqlite3_bind_int(stmt, 7, notification_id);
        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    } else {
        history = actor_history_new(actor);
        if (!history) goto rollback;
        const char *insert =
            "INSERT INTO notifications (user_id, actor_name, kind, post_id, post_slug, comment_id, "
            "excerpt, actor_history, event_id) VALUES (?,?,?,?,?,?,?,?,?)";
        sqlite3_stmt *stmt = NULL;
        if (sqlite3_prepare_v2(conn, insert, -1, &stmt, NULL) != SQLITE_OK) goto rollback;
        sqlite3_bind_int(stmt, 1, user_id);
        sqlite3_bind_text(stmt, 2, actor, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, kind, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 4, post_id);
        sqlite3_bind_text(stmt, 5, slug, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 6, comment_id);
        sqlite3_bind_text(stmt, 7, body, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 8, history, -1, SQLITE_TRANSIENT);
        if (event_id && event_id[0]) sqlite3_bind_text(stmt, 9, event_id, -1, SQLITE_TRANSIENT);
        else sqlite3_bind_null(stmt, 9);
        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
    free(history);
    history = NULL;
    if (rc != SQLITE_DONE && rc != SQLITE_CONSTRAINT) goto rollback;
    if (transactional && sqlite3_exec(conn, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) goto rollback;
    return true;

rollback:
    free(history);
    if (transactional) sqlite3_exec(conn, "ROLLBACK", NULL, NULL, NULL);
    return false;
}

bool db_notification_create(cwist_db *db, int user_id, const char *actor_name, const char *kind, int post_id, const char *post_slug, int comment_id, const char *excerpt) {
    return notification_store(db, user_id, actor_name, kind, post_id, post_slug, comment_id, excerpt, NULL);
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
    const char *sql = "SELECT id, user_id, actor_name, kind, post_id, post_slug, comment_id, excerpt, is_read, created_at, actor_count, actor_history FROM notifications WHERE user_id=? ORDER BY created_at DESC, id DESC LIMIT ?";
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

bool db_notification_deliver_federated(long user_id, const char *actor_name, const char *kind, const char *post_slug, const char *excerpt, const char *event_id) {
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
    return notification_store(NULL, (int)user_id, actor_name, kind, 0, post_slug, 0, excerpt, event_id);
}

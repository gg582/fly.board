#include "config/config.h"
#include "db/db.h"
#include "db/db_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;
#define CHECK(condition, name) do { \
    if (condition) printf("PASS: %s\n", name); \
    else { printf("FAIL: %s (line %d)\n", name, __LINE__); failures++; } \
} while (0)

static cJSON *only_notification(cwist_db *db, int uid) {
    cJSON *items = db_notification_list(db, uid, 100);
    if (!items || cJSON_GetArraySize(items) != 1) {
        cJSON_Delete(items);
        return NULL;
    }
    cJSON *item = cJSON_DetachItemFromArray(items, 0);
    cJSON_Delete(items);
    return item;
}

int main(void) {
    const char *path = "/tmp/flytest_notification.db";
    remove(path);
    setenv("FLY_DB_LOCATION", path, 1);
    sqlite3 *conn = NULL;
    cwist_db db = {0};
    CHECK(sqlite3_open(path, &conn) == SQLITE_OK, "open database");
    db.conn = conn;
    CHECK(sqlite3_exec(conn, "CREATE TABLE notifications (id INTEGER PRIMARY KEY AUTOINCREMENT, user_id INTEGER NOT NULL, actor_name TEXT, kind TEXT NOT NULL, post_id INTEGER, post_slug TEXT, comment_id INTEGER, excerpt TEXT, is_read INTEGER DEFAULT 0, created_at DATETIME DEFAULT CURRENT_TIMESTAMP)", NULL, NULL, NULL) == SQLITE_OK,
          "create legacy notification schema");
    CHECK(sqlite3_exec(conn, "INSERT INTO notifications (user_id,actor_name,kind) VALUES (1,'Legacy','comment')", NULL, NULL, NULL) == SQLITE_OK,
          "insert legacy notification");
    CHECK(db_init(&db), "initialize schema");
    CHECK(db_init(&db), "migration is idempotent");
    sqlite3_stmt *legacy = NULL;
    CHECK(sqlite3_prepare_v2(conn, "SELECT actor_count,actor_history FROM notifications", -1, &legacy, NULL) == SQLITE_OK &&
          sqlite3_step(legacy) == SQLITE_ROW && sqlite3_column_int(legacy, 0) == 1 &&
          strcmp((const char *)sqlite3_column_text(legacy, 1), "[\"Legacy\"]") == 0,
          "legacy actor history backfilled");
    sqlite3_finalize(legacy);
    CHECK(db_exec_sql(&db, "DELETE FROM notifications"), "remove migration fixture");
    CHECK(db_user_create(&db, "recipient", "recipient@example.com", "hash"), "create recipient");
    cJSON *user = db_user_get_by_username(&db, "recipient");
    int uid = user ? cJSON_GetObjectItem(user, "id")->valueint : 0;
    cJSON_Delete(user);

    memset(&g_config, 0, sizeof(g_config));
    g_config.notification_window_seconds = 60;
    CHECK(db_notification_create(&db, uid, "Alice", "comment", 10, "first", 101, "one"), "create with aggregation disabled");
    CHECK(db_notification_create(&db, uid, "Bob", "comment", 10, "first", 102, "two"), "second event stays separate by default");
    cJSON *items = db_notification_list(&db, uid, 100);
    CHECK(items && cJSON_GetArraySize(items) == 2, "default off preserves one row per event");
    cJSON_Delete(items);

    g_config.notification_aggregation = true;
    CHECK(db_notification_create(&db, uid, "Carol", "reply", 20, "target", 201, "old"), "create aggregate root");
    CHECK(db_notification_create(&db, uid, "Dan", "reply", 20, "target", 202, "new"), "aggregate matching event");
    items = db_notification_list(&db, uid, 100);
    CHECK(items && cJSON_GetArraySize(items) == 3, "matching recipient kind and target coalesce");
    cJSON_Delete(items);
    cJSON *item = NULL;
    items = db_notification_list(&db, uid, 100);
    if (items) {
        for (int i = 0; i < cJSON_GetArraySize(items); i++) {
            cJSON *candidate = cJSON_GetArrayItem(items, i);
            cJSON *slug = cJSON_GetObjectItem(candidate, "post_slug");
            if (slug && cJSON_IsString(slug) && strcmp(slug->valuestring, "target") == 0) {
                item = cJSON_Duplicate(candidate, true);
            }
        }
    }
    cJSON_Delete(items);
    CHECK(item && cJSON_GetObjectItem(item, "actor_count")->valueint == 2, "actor count includes distinct actors");
    CHECK(item && strcmp(cJSON_GetObjectItem(item, "actor_name")->valuestring, "Dan") == 0, "latest actor leads group");
    CHECK(item && strcmp(cJSON_GetObjectItem(item, "actor_history")->valuestring, "[\"Carol\",\"Dan\"]") == 0, "actor history retained in order");
    CHECK(item && cJSON_GetObjectItem(item, "comment_id")->valueint == 202 && strcmp(cJSON_GetObjectItem(item, "excerpt")->valuestring, "new") == 0,
          "latest direct-link metadata and excerpt retained");
    cJSON_Delete(item);
    CHECK(db_notification_create(&db, uid, "Dan", "reply", 20, "target", 207, "repeat"), "repeated actor event");
    items = db_notification_list(&db, uid, 100);
    item = NULL;
    if (items) {
        for (int i = 0; i < cJSON_GetArraySize(items); i++) {
            cJSON *candidate = cJSON_GetArrayItem(items, i);
            cJSON *comment = cJSON_GetObjectItem(candidate, "comment_id");
            if (comment && cJSON_IsNumber(comment) && comment->valueint == 207) item = cJSON_Duplicate(candidate, true);
        }
    }
    cJSON_Delete(items);
    CHECK(item && cJSON_GetObjectItem(item, "actor_count")->valueint == 2 &&
          strcmp(cJSON_GetObjectItem(item, "actor_history")->valuestring, "[\"Carol\",\"Dan\"]") == 0,
          "repeated actor does not inflate distinct actor history or count");
    CHECK(db_notification_unread_count(&db, uid) == 3, "unread count counts grouped rows");
    CHECK(db_notification_mark_all_read(&db, uid), "mark all read");
    CHECK(db_notification_unread_count(&db, uid) == 0, "all groups read");
    CHECK(db_notification_create(&db, uid, "Eve", "reply", 20, "target", 203, "latest"), "new event after read");
    CHECK(db_notification_unread_count(&db, uid) == 1, "new event makes existing group unread again");
    cJSON_Delete(item);

    CHECK(db_notification_create(&db, uid, "Eve", "comment", 20, "target", 204, "other kind"), "different kind remains separate");
    CHECK(db_notification_create(&db, uid, "Eve", "reply", 21, "target", 205, "other target"), "different target remains separate");
    items = db_notification_list(&db, uid, 100);
    CHECK(items && cJSON_GetArraySize(items) == 5, "kind and target define aggregate boundary");
    cJSON_Delete(items);

    CHECK(db_exec_sql(&db, "UPDATE notifications SET created_at=datetime('now','-2 minutes') WHERE comment_id=203"), "age aggregate beyond window");
    CHECK(db_notification_create(&db, uid, "Fran", "reply", 20, "target", 206, "outside"), "event outside window starts new group");
    items = db_notification_list(&db, uid, 100);
    CHECK(items && cJSON_GetArraySize(items) == 6, "expired time window starts a new row");
    cJSON_Delete(items);

    int before = db_notification_unread_count(&db, uid);
    CHECK(db_notification_deliver_federated(uid, "Remote", "comment", "remote", "remote", "origin#comment-1"), "deliver federated event");
    CHECK(db_notification_deliver_federated(uid, "Remote", "comment", "remote", "remote", "origin#comment-1"), "duplicate delivery acknowledged");
    items = db_notification_list(&db, uid, 100);
    CHECK(items && cJSON_GetArraySize(items) == 7, "duplicate event id does not create another row");
    cJSON_Delete(items);
    CHECK(db_notification_unread_count(&db, uid) == before + 1, "duplicate event does not increment unread count");
    CHECK(!db_notification_deliver_federated(uid + 1000, "Remote", "comment", "remote", "remote", "origin#comment-2"),
          "unknown federated recipient rejected");

    item = only_notification(&db, uid + 1000);
    CHECK(item == NULL, "no notification for unknown recipient");
    cJSON_Delete(item);
    sqlite3_close(conn);
    remove(path);
    unsetenv("FLY_DB_LOCATION");
    printf("%s\n", failures ? "SOME TESTS FAILED" : "ALL TESTS PASSED");
    return failures ? 1 : 0;
}

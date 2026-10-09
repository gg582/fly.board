/* Unit test: src/db webmail store (db_email_*): create, folder listing,
 * folder move, read/unread counts, delete, empty trash.
 *
 * Build:
 *   gcc -O1 -Iinclude -Isrc -I/home/yjlee/cwist/include -I/home/yjlee/cwist/lib \
 *       -I/home/yjlee/cwist/lib/cjson -I/home/yjlee/cwist/lib/sqlite3 \
 *       tests/test_db_email.c tests/stubs/db_test_stubs.c \
 *       src/db/db.o src/db/user.o src/db/board.o src/db/post.o src/db/tag.o \
 *       src/db/series.o src/db/db_email.o src/db/db_sync.o src/db/sql_escape.o \
 *       src/db/orm.o src/db/fts5_mecab_tokenizer.o \
 *       /home/yjlee/cwist/libcwist.a /home/yjlee/cwist/lib/libttak/lib/libttak.a \
 *       /home/yjlee/cwist/lib/cjson/libcjson.a \
 *       -lpthread -lm -ldl -lmecab -o /tmp/test_db_email
 * Run: /tmp/test_db_email
 */
#include "db/db.h"
#include "db/db_internal.h"
#include <cjson/cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures = 0;
#define CHECK(cond, name) do { \
    if (cond) printf("PASS: %s\n", name); \
    else { printf("FAIL: %s (line %d)\n", name, __LINE__); failures++; } \
} while (0)

static cwist_db g_db;

static cwist_db *open_test_db(const char *path) {
    remove(path);
    sqlite3 *conn = NULL;
    if (sqlite3_open(path, &conn) != SQLITE_OK) return NULL;
    g_db.conn = conn;
    g_db.pool_slot = 0;
    if (!db_init(&g_db)) { sqlite3_close(conn); g_db.conn = NULL; }
    return g_db.conn ? &g_db : NULL;
}

int main(void) {
    cwist_db *db = open_test_db("/tmp/flytest_email.db");
    if (!db) { printf("FAIL: db_init\n"); return 1; }

    CHECK(db_user_create(db, "mailuser", "m@example.com", "h"), "user create");
    cJSON *u = db_user_get_by_username(db, "mailuser");
    int uid = cJSON_GetObjectItem(u, "id")->valueint;
    cJSON_Delete(u);

    int id1 = db_email_create(db, uid, "INBOX", "a@x.com", "m@example.com",
                              "Hello", "body one", "msg-1", NULL);
    int id2 = db_email_create(db, uid, "INBOX", "b@x.com", "m@example.com",
                              "World", "body two", "msg-2", NULL);
    int id3 = db_email_create(db, uid, "Sent", "m@example.com", "c@x.com",
                              "Re: Hi", "body three", "msg-3", "msg-1");
    CHECK(id1 > 0 && id2 > 0 && id3 > 0, "emails created");

    CHECK(db_email_count(db, uid, "INBOX") == 2, "INBOX count is 2");
    CHECK(db_email_count(db, uid, "Sent") == 1, "Sent count is 1");
    CHECK(db_email_count(db, uid, "Trash") == 0, "Trash count is 0");
    CHECK(db_email_unread_count(db, uid) == 2, "INBOX unread is 2 (Sent not counted)");

    cJSON *list = db_email_list(db, uid, "INBOX", 0, 10);
    CHECK(list && cJSON_GetArraySize(list) == 2, "INBOX list has 2 rows");
    if (list) {
        cJSON *first = list->child;
        cJSON *subj = first ? cJSON_GetObjectItem(first, "subject") : NULL;
        CHECK(cJSON_IsString(subj), "list rows carry subject");
    }
    cJSON_Delete(list);

    CHECK(db_email_set_read(db, uid, id1, true), "mark read");
    CHECK(db_email_unread_count(db, uid) == 1, "unread drops to 1");

    CHECK(db_email_set_folder(db, uid, id2, "Trash"), "move to Trash");
    CHECK(db_email_count(db, uid, "INBOX") == 1, "INBOX count drops to 1");
    CHECK(db_email_count(db, uid, "Trash") == 1, "Trash count rises to 1");

    cJSON *got = db_email_get(db, uid, id1);
    CHECK(got != NULL, "email get by id");
    if (got) {
        cJSON *folder = cJSON_GetObjectItem(got, "folder");
        CHECK(cJSON_IsString(folder) && strcmp(folder->valuestring, "INBOX") == 0, "folder field stored");
    }
    cJSON_Delete(got);

    CHECK(db_email_delete(db, uid, id3), "email delete");
    CHECK(db_email_get(db, uid, id3) == NULL, "deleted email gone");

    /* another user's mailbox is isolated */
    CHECK(db_user_create(db, "other", "o@example.com", "h"), "second user create");
    cJSON *u2 = db_user_get_by_username(db, "other");
    int uid2 = cJSON_GetObjectItem(u2, "id")->valueint;
    cJSON_Delete(u2);
    CHECK(db_email_count(db, uid2, "INBOX") == 0, "other user INBOX empty");
    CHECK(db_email_get(db, uid2, id1) == NULL, "other user cannot read email");

    CHECK(db_email_empty_trash(db, uid) == 1, "empty trash returns deleted count");
    CHECK(db_email_count(db, uid, "Trash") == 0, "trash empty after purge");

    sqlite3_close(g_db.conn);
    g_db.conn = NULL;
    remove("/tmp/flytest_email.db");
    remove("/tmp/flytest_email.db-wal");
    remove("/tmp/flytest_email.db-shm");

    printf("%s\n", failures == 0 ? "ALL PASS" : "SOME FAILED");
    return failures == 0 ? 0 : 1;
}

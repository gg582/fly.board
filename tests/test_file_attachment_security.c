#include "db/db.h"
#include "db/db_internal.h"
#include "handlers/handlers_internal.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int failures;
#define CHECK(condition, name) do { \
    if (condition) printf("PASS: %s\n", name); \
    else { printf("FAIL: %s (line %d)\n", name, __LINE__); failures++; } \
} while (0)

static int user_id(cwist_db *db, const char *name) {
    cJSON *user = db_user_get_by_username(db, name);
    int id = user ? json_int(user, "id", 0) : 0;
    if (user) cJSON_Delete(user);
    return id;
}

static int file_post_id(cwist_db *db, int fid) {
    cJSON *file = db_file_get(db, fid);
    int post_id = file ? json_int(file, "post_id", 0) : -1;
    if (file) cJSON_Delete(file);
    return post_id;
}

int main(void) {
    const char *path = "/tmp/flytest_file_attachment_security.db";
    remove(path);
    cwist_db db = {0};
    CHECK(sqlite3_open(path, &db.conn) == SQLITE_OK, "open database");
    CHECK(db_init(&db), "initialize schema");
    CHECK(db_user_create(&db, "owner", "owner@example.com", "hash"), "create owner");
    CHECK(db_user_create(&db, "other", "other@example.com", "hash"), "create other user");
    int owner = user_id(&db, "owner");
    int other = user_id(&db, "other");
    int first = db_post_create(&db, 0, owner, "First", "first", "body", "", NULL, 0, 0, "");
    int second = db_post_create(&db, 0, other, "Second", "second", "body", "", NULL, 0, 0, "");
    CHECK(first > 0 && second > 0, "create posts");

    int foreign = db_file_create_volume_get_id(&db, 0, owner, "foreign.png", "image/png",
                                               "public/uploads/test-foreign.png", 10);
    CHECK(foreign > 0, "create foreign orphan");
    CHECK(db_file_set_delete_pin_hash(&db, foreign, "delete-capability-must-not-authorize-attach"),
          "give foreign orphan a delete capability");
    CHECK(!db_file_attach_to_post(&db, foreign, second, 0, other, false),
          "foreign user cannot attach orphan even when it has a delete PIN");
    CHECK(file_post_id(&db, foreign) == 0, "failed foreign attach leaves file orphaned");

    CHECK(db_file_attach_to_post(&db, foreign, first, 0, owner, false), "owner attaches own orphan");
    CHECK(file_post_id(&db, foreign) == first, "owner attach stores post relation");
    CHECK(!db_file_attach_to_post(&db, foreign, second, 0, owner, false),
          "owner cannot move a file already attached to another post");
    CHECK(file_post_id(&db, foreign) == first, "cross-post attach preserves original relation");

    int admin_file = db_file_create_volume_get_id(&db, 0, owner, "admin.png", "image/png",
                                                  "public/uploads/test-admin.png", 10);
    CHECK(db_file_attach_to_post(&db, admin_file, second, 1, other, true), "admin may attach an orphan");
    cJSON *file = db_file_get(&db, admin_file);
    CHECK(file && json_int(file, "post_id", 0) == second && json_int(file, "is_inline", 0) == 1,
          "admin attach preserves inline mode");
    if (file) cJSON_Delete(file);

    CHECK(!db_file_attach_to_post(&db, admin_file, first, 0, 0, false),
          "anonymous actor cannot attach a file");
    CHECK(!db_file_attach_to_post(&db, 999999, first, 0, owner, false),
          "missing file is not reported as attached");

    sqlite3_close(db.conn);
    remove(path);
    remove("/tmp/flytest_file_attachment_security.db-wal");
    remove("/tmp/flytest_file_attachment_security.db-shm");
    printf("%s\n", failures == 0 ? "ALL PASS" : "SOME FAILED");
    return failures == 0 ? 0 : 1;
}

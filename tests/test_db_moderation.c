#include "db/db.h"
#include "db/db_internal.h"
#include "db/moderation.h"
#include "engine/community.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;
#define CHECK(condition, name) do { \
    if (condition) printf("PASS: %s\n", name); \
    else { printf("FAIL: %s (line %d)\n", name, __LINE__); failures++; } \
} while (0)

static void reset_settings(void) {
    memset(&community, 0, sizeof(community));
}

static int make_post(cwist_db *db, int uid, const char *title, const char *body, const char *status) {
    char *slug = NULL;
    int id = db_post_create_with_auto_slug(db, 0, uid, title, title, body, "", "", 0, 0, "", status, NULL, &slug);
    if (slug) free(slug);
    return id;
}

int main(void) {
    const char *main_path = "/tmp/flytest_moderation.db";
    const char *comments_path = "/tmp/flytest_moderation_comments.db";
    remove(main_path);
    remove(comments_path);
    sqlite3 *conn = NULL;
    cwist_db db = {0};
    CHECK(sqlite3_open(main_path, &conn) == SQLITE_OK, "open main database");
    db.conn = conn;
    CHECK(db_init(&db), "initialize main schema");
    CHECK(db_comment_init(comments_path), "initialize comments schema");
    CHECK(db_moderation_migrate(&db), "initialize moderation schema");
    CHECK(db_user_create(&db, "newbie", "new@example.com", "hash"), "create new account");
    CHECK(db_user_create(&db, "established", "old@example.com", "hash"), "create old account");
    cJSON *user = db_user_get_by_username(&db, "newbie");
    int uid = user ? cJSON_GetObjectItem(user, "id")->valueint : 0;
    cJSON_Delete(user);
    user = db_user_get_by_username(&db, "established");
    int old_uid = user ? cJSON_GetObjectItem(user, "id")->valueint : 0;
    cJSON_Delete(user);
    db_exec_sql(&db, "UPDATE users SET created_at=datetime('now','-8 days') WHERE username='established'");

    reset_settings();
    CHECK(moderation_check_submission(&db, uid, "user", MODERATION_POST, "https://a https://b https://c https://d https://e https://f").action == MODERATION_ALLOW,
          "all controls disabled preserves submission");
    CHECK(moderation_check_submission(&db, 0, "", MODERATION_COMMENT, "anonymous").action == MODERATION_ALLOW,
          "all controls disabled preserves anonymous comments");

    community.spam_detection = true;
    CHECK(moderation_check_submission(&db, old_uid, "user", MODERATION_POST, "https://a https://b https://c https://d https://e").action == MODERATION_ALLOW,
          "five links allowed");
    moderation_decision_t d = moderation_check_submission(&db, old_uid, "user", MODERATION_POST, "https://a https://b https://c https://d https://e HTTPS://f");
    CHECK(d.action == MODERATION_PENDING && strcmp(d.reason, "excessive_links") == 0, "six links pending");
    int original = make_post(&db, uid, "original", "Same   BODY", POST_STATUS_PUBLISHED);
    CHECK(original > 0, "create duplicate source");
    d = moderation_check_submission(&db, uid, "user", MODERATION_POST, "  same body\n");
    CHECK(d.action == MODERATION_PENDING && strcmp(d.reason, "duplicate_content") == 0, "normalized duplicate pending");
    CHECK(moderation_check_submission(&db, old_uid, "user", MODERATION_POST, "same body").action == MODERATION_ALLOW,
          "duplicate scope is same account");
    reset_settings();

    community.new_user_limits = true;
    d = moderation_check_submission(&db, uid, "user", MODERATION_POST, "second");
    CHECK(d.action == MODERATION_REJECT && strcmp(d.reason, "new_user_post_limit") == 0, "new account post limit");
    CHECK(moderation_check_submission(&db, old_uid, "user", MODERATION_POST, "second").action == MODERATION_ALLOW,
          "old account exempt from age limit");
    for (int i = 0; i < 3; i++) {
        char body[16];
        snprintf(body, sizeof(body), "comment %d", i);
        CHECK(db_comment_create(&db, "post", original, uid, "newbie", 0, body) > 0, "create limited comment");
    }
    CHECK(moderation_check_submission(&db, uid, "user", MODERATION_COMMENT, "fourth").action == MODERATION_REJECT,
          "new account comment limit");
    reset_settings();

    int pending_post = make_post(&db, uid, "queued", "queued body", "pending");
    CHECK(db_moderation_queue_post(&db, pending_post, uid, "approval_queue"), "queue post");
    cJSON *queue = db_moderation_list_pending(&db);
    CHECK(queue && cJSON_GetArraySize(queue) == 1, "queue lists pending post");
    if (queue && cJSON_GetArraySize(queue) == 1) {
        cJSON *row = cJSON_GetArrayItem(queue, 0);
        CHECK(strcmp(cJSON_GetObjectItem(row, "post_content")->valuestring, "queued body") == 0, "queue contains post body");
    }
    cJSON_Delete(queue);
    CHECK(db_moderation_approve(&db, MODERATION_POST, pending_post, old_uid), "approve post");
    cJSON *post = db_post_get_by_id(&db, pending_post);
    CHECK(post && strcmp(cJSON_GetObjectItem(post, "status")->valuestring, POST_STATUS_PUBLISHED) == 0, "approved post published");
    cJSON_Delete(post);

    int pending_comment = db_moderation_create_pending_comment(&db, "post", pending_post, uid, "newbie", 0,
                                                               "secret until approval", "approval_queue");
    CHECK(pending_comment > 0, "queue comment");
    cJSON *comment = db_comment_get_by_id(&db, pending_comment);
    CHECK(comment && cJSON_GetObjectItem(comment, "deleted")->valueint == 1 && strcmp(cJSON_GetObjectItem(comment, "content")->valuestring, "") == 0,
          "pending comment hides body");
    cJSON *comments = cJSON_CreateArray();
    cJSON_AddItemToArray(comments, cJSON_Duplicate(comment, true));
    db_moderation_filter_comments(&db, comments);
    CHECK(cJSON_GetArraySize(comments) == 0, "pending comment filtered from public list");
    cJSON_Delete(comments);
    cJSON_Delete(comment);
    CHECK(db_moderation_approve(&db, MODERATION_COMMENT, pending_comment, old_uid), "approve comment");
    comment = db_comment_get_by_id(&db, pending_comment);
    CHECK(comment && cJSON_GetObjectItem(comment, "deleted")->valueint == 0 && strcmp(cJSON_GetObjectItem(comment, "content")->valuestring, "secret until approval") == 0,
          "approved comment restored");
    cJSON_Delete(comment);

    int rejected_post = make_post(&db, uid, "rejected", "reject body", "pending");
    CHECK(db_moderation_queue_post(&db, rejected_post, uid, "duplicate_content"), "queue rejection candidate");
    CHECK(db_moderation_reject(&db, MODERATION_POST, rejected_post, old_uid), "reject post");
    CHECK(db_post_get_by_id(&db, rejected_post) == NULL, "rejected post removed");
    CHECK(!db_moderation_approve(&db, MODERATION_POST, rejected_post, old_uid), "rejected item cannot be approved");

    community.approval_queue = true;
    community.trust = true;
    CHECK(moderation_check_submission(&db, old_uid, "user", MODERATION_POST, "trusted").action == MODERATION_ALLOW,
          "account age confers trust");
    CHECK(moderation_check_submission(&db, uid, "user", MODERATION_POST, "not yet").action == MODERATION_PENDING,
          "new account requires approval");
    int third_approval = db_moderation_create_pending_comment(&db, "post", pending_post, uid, "newbie", 0,
                                                              "third approval", "approval_queue");
    CHECK(third_approval > 0 && db_moderation_approve(&db, MODERATION_COMMENT, third_approval, old_uid),
          "store third approval");
    CHECK(moderation_check_submission(&db, uid, "user", MODERATION_POST, "trusted now").action == MODERATION_ALLOW,
          "three approvals confer trust");
    CHECK(moderation_check_submission(&db, uid, "moderator", MODERATION_POST, "reviewer").action == MODERATION_ALLOW,
          "reviewer bypasses queue");
    reset_settings();

    db_comment_close_thread();
    db_comment_close();
    sqlite3_close(conn);
    remove(main_path);
    remove(comments_path);
    printf("%s\n", failures ? "SOME TESTS FAILED" : "ALL TESTS PASSED");
    return failures ? 1 : 0;
}

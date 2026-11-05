#include "db/db.h"
#include "db/db_internal.h"
#include "db/moderation.h"
#include "engine/community.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(condition, name) do { \
    if (condition) printf("PASS: %s\n", name); \
    else { printf("FAIL: %s (line %d)\n", name, __LINE__); failures++; } \
} while (0)

static int user_id(cwist_db *db, const char *username) {
    cJSON *user = db_user_get_by_username(db, username);
    cJSON *id = user ? cJSON_GetObjectItem(user, "id") : NULL;
    int result = cJSON_IsNumber(id) ? id->valueint : 0;
    if (user) cJSON_Delete(user);
    return result;
}

static int board_id(cwist_db *db, const char *slug) {
    cJSON *board = db_board_get_by_slug(db, slug);
    cJSON *id = board ? cJSON_GetObjectItem(board, "id") : NULL;
    int result = cJSON_IsNumber(id) ? id->valueint : 0;
    if (board) cJSON_Delete(board);
    return result;
}

static int journal_count(cwist_db *db, const char *entity) {
    sqlite3_stmt *stmt = NULL;
    int count = -1;
    if (sqlite3_prepare_v2(db->conn, "SELECT COUNT(*) FROM sync_journal WHERE entity=?", -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, entity, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW) count = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return count;
}

int main(void) {
    const char *main_path = "/tmp/flytest_board_delegations.db";
    const char *comments_path = "/tmp/flytest_board_delegations_comments.db";
    remove(main_path);
    remove(comments_path);
    sqlite3 *conn = NULL;
    cwist_db db = {0};
    CHECK(sqlite3_open(main_path, &conn) == SQLITE_OK, "open main database");
    db.conn = conn;
    CHECK(db_init(&db), "initialize schema");
    CHECK(db_comment_init(comments_path), "initialize comments");
    CHECK(db_user_create(&db, "delegate", "delegate@example.com", "hash"), "create delegate");
    CHECK(db_board_create(&db, "One", "one", "", false, 0, 0, 0), "create first board");
    CHECK(db_board_create(&db, "Two", "two", "", false, 0, 0, 0), "create second board");
    int uid = user_id(&db, "delegate");
    int one = board_id(&db, "one");
    int two = board_id(&db, "two");

    memset(&community, 0, sizeof(community));
    CHECK(!db_board_delegation_grant(&db, one, uid, true, true, NULL), "grant rejected while feature disabled");
    CHECK(!db_board_user_can_moderate(&db, one, uid, "user"), "moderation disabled by default");
    CHECK(!db_board_user_can_notice(&db, one, uid, "user"), "notice disabled by default");
    CHECK(db_board_user_can_moderate(&db, one, uid, "admin"), "admin retains moderation");

    community.board_delegations = true;
    CHECK(db_board_delegation_grant(&db, one, uid, true, false, NULL), "grant permanent moderator");
    CHECK(db_board_user_can_moderate(&db, one, uid, "user"), "delegate moderates granted board");
    CHECK(!db_board_user_can_moderate(&db, two, uid, "user"), "moderation is board scoped");
    CHECK(!db_board_user_can_notice(&db, one, uid, "user"), "notice privilege is independent");
    CHECK(db_board_delegation_grant(&db, one, uid, false, true, NULL), "upsert notice-only delegation");
    CHECK(!db_board_user_can_moderate(&db, one, uid, "user"), "upsert replaces old privileges");
    CHECK(db_board_user_can_notice(&db, one, uid, "user"), "notice-only grant works");
    CHECK(!db_board_user_can_notice(&db, two, uid, "user"), "notice privilege is board scoped");

    CHECK(db_board_delegation_grant(&db, one, uid, true, true, "2000-01-01 00:00:00"), "grant expired delegation");
    CHECK(!db_board_user_can_moderate(&db, one, uid, "user"), "expired moderation denied");
    CHECK(!db_board_user_can_notice(&db, one, uid, "user"), "expired notice denied");
    CHECK(db_board_delegation_grant(&db, one, uid, true, false, "2999-01-01 00:00:00"), "renew delegation");
    CHECK(db_board_user_can_moderate(&db, one, uid, "user"), "future expiration accepted");
    CHECK(journal_count(&db, "board_delegation") > 0, "grants journaled for FlyWire");

    char *slug = NULL;
    int post = db_post_create_with_auto_slug(&db, one, uid, "Pending", "pending", "body", "", "", 0, 0, "", "pending", NULL, &slug);
    free(slug);
    CHECK(post > 0 && db_moderation_queue_post(&db, post, uid, "approval_queue"), "queue board post");
    CHECK(db_moderation_target_board_id(&db, MODERATION_POST, post) == one, "post target resolves board");
    int comment = db_moderation_create_pending_comment(&db, "post", post, uid, "delegate", 0, "pending", "approval_queue");
    CHECK(comment > 0, "queue board comment");
    CHECK(db_moderation_target_board_id(&db, MODERATION_COMMENT, comment) == one, "comment target resolves post board");

    CHECK(db_board_delegation_revoke(&db, one, uid), "revoke delegation");
    CHECK(!db_board_user_can_moderate(&db, one, uid, "user"), "revocation is immediate");
    CHECK(!db_board_delegation_revoke(&db, one, uid), "second revocation reports missing grant");
    cJSON *grants = db_board_delegation_list(&db, one);
    CHECK(grants && cJSON_GetArraySize(grants) == 0, "revoked grant removed from admin list");
    if (grants) cJSON_Delete(grants);

    db_comment_close_thread();
    db_comment_close();
    sqlite3_close(conn);
    remove(main_path);
    remove(comments_path);
    printf("%s\n", failures ? "SOME TESTS FAILED" : "ALL TESTS PASSED");
    return failures ? 1 : 0;
}

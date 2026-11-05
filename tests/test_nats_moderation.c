#include "crypto/fly_crypto.h"
#include "db/db.h"
#include "db/db_internal.h"
#include "db/moderation.h"
#include "nats/fly_nats.h"
#include "nats/moderation.h"
#include <cwist/core/mem/alloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;
#define CHECK(condition, name) do { \
    if (condition) printf("PASS: %s\n", name); \
    else { printf("FAIL: %s (line %d)\n", name, __LINE__); failures++; } \
} while (0)

static char *reviewer_pubkey;

static char *decision(const char *correlation_id, const char *decision_id, const char *action,
                      long long issued_at, const char *reason) {
    cJSON *body = cJSON_CreateObject();
    cJSON_AddNumberToObject(body, "version", 1);
    cJSON_AddStringToObject(body, "type", "moderation.decision");
    cJSON_AddStringToObject(body, "correlation_id", correlation_id);
    cJSON_AddStringToObject(body, "decision_id", decision_id);
    cJSON_AddStringToObject(body, "action", action);
    cJSON_AddNumberToObject(body, "issued_at", (double)issued_at);
    cJSON_AddStringToObject(body, "reason", reason);
    char *canonical = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    char *signature = NULL;
    if (!canonical || !fly_crypto_sign_moderation_decision((const uint8_t *)canonical, strlen(canonical), &signature)) {
        if (canonical) cwist_free(canonical);
        return NULL;
    }
    cJSON *envelope = cJSON_Parse(canonical);
    cwist_free(canonical);
    cJSON_AddStringToObject(envelope, "sig_alg", "ML-DSA-65");
    cJSON_AddStringToObject(envelope, "sig_b64", signature);
    cJSON_AddStringToObject(envelope, "pubkey_b64", reviewer_pubkey);
    cwist_free(signature);
    char *json = cJSON_PrintUnformatted(envelope);
    cJSON_Delete(envelope);
    return json;
}

static moderation_external_result_t deliver(cwist_db *db, char *json, long long now) {
    moderation_external_result_t result = fly_moderation_process_decision(db, json, strlen(json), reviewer_pubkey, now);
    cwist_free(json);
    return result;
}

static int external_request_count(cwist_db *db) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(fly_db_conn(db), "SELECT COUNT(*) FROM moderation_external_requests", -1, &stmt, NULL) != SQLITE_OK) return -1;
    int count = sqlite3_step(stmt) == SQLITE_ROW ? sqlite3_column_int(stmt, 0) : -1;
    sqlite3_finalize(stmt);
    return count;
}

static int pending_post(cwist_db *db, const char *title, const char *content) {
    char *slug = NULL;
    int id = db_post_create_with_auto_slug(db, 0, 0, title, title, content, "", "", 0, 0, "", "pending", NULL, &slug);
    if (slug) free(slug);
    if (id > 0 && !db_moderation_queue_post(db, id, 0, "approval_queue")) return -1;
    return id;
}

int main(void) {
    const char *main_path = "/tmp/flytest_nats_moderation.db";
    const char *comments_path = "/tmp/flytest_nats_moderation_comments.db";
    const char *seed_path = "/tmp/flytest_nats_moderation.seed";
    remove(main_path);
    remove(comments_path);
    remove(seed_path);
    CHECK(fly_crypto_init(seed_path), "initialize real ML-DSA loopback signer");
    CHECK(fly_crypto_pubkey_export(&reviewer_pubkey), "export pinned reviewer key");
    sqlite3 *conn = NULL;
    cwist_db db = {0};
    CHECK(sqlite3_open(main_path, &conn) == SQLITE_OK, "open main database");
    db.conn = conn;
    CHECK(db_init(&db), "initialize schemas");
    CHECK(db_comment_init(comments_path), "initialize comments");

    const long long now = 1700000000;
    const char *correlation = "0123456789abcdef0123456789abcdef";
    int post_id = pending_post(&db, "private title", "body requiring review");
    CHECK(fly_nats_publish_moderation(&db, MODERATION_POST, post_id) && external_request_count(&db) == 0,
          "external review is disabled by default");
    cJSON *request = db_moderation_external_create(&db, MODERATION_POST, post_id, correlation, now, now + 60);
    CHECK(request != NULL, "persist external review correlation");
    char *review = request ? fly_moderation_build_request(correlation, MODERATION_POST,
                                                         cJSON_GetObjectItem(request, "content")->valuestring,
                                                         cJSON_GetObjectItem(request, "reason")->valuestring,
                                                         now, now + 60) : NULL;
    CHECK(review != NULL, "build signed review request");
    if (review) {
        cJSON *envelope = cJSON_Parse(review);
        CHECK(cJSON_GetObjectItem(envelope, "user_id") == NULL &&
              cJSON_GetObjectItem(envelope, "author_name") == NULL &&
              cJSON_GetObjectItem(envelope, "target_id") == NULL &&
              cJSON_GetObjectItem(envelope, "title") == NULL, "request minimizes private metadata");
        CHECK(strcmp(cJSON_GetObjectItem(envelope, "content")->valuestring, "body requiring review") == 0,
              "request includes only necessary review content");
        cJSON_Delete(envelope);
        cwist_free(review);
    }
    cJSON_Delete(request);

    CHECK(deliver(&db, decision(correlation, "decision-pending", "pending", now + 1, "review started"), now + 2) ==
          MODERATION_EXTERNAL_APPLIED, "authenticated pending acknowledgement accepted");
    CHECK(deliver(&db, decision(correlation, "decision-pending", "pending", now + 1, "review started"), now + 2) ==
          MODERATION_EXTERNAL_DUPLICATE, "identical pending replay is idempotent");
    CHECK(deliver(&db, decision(correlation, "decision-pending", "allow", now + 1, "review started"), now + 2) ==
          MODERATION_EXTERNAL_REJECTED, "conflicting decision id rejected");
    cJSON *post = db_post_get_by_id(&db, post_id);
    CHECK(post && strcmp(cJSON_GetObjectItem(post, "status")->valuestring, "pending") == 0,
          "pending decision leaves local queue authoritative");
    cJSON_Delete(post);

    char *tampered = decision(correlation, "decision-forged", "allow", now + 2, "forged");
    if (tampered) tampered[strlen(tampered) - 2] = tampered[strlen(tampered) - 2] == 'x' ? 'y' : 'x';
    CHECK(tampered && deliver(&db, tampered, now + 2) == MODERATION_EXTERNAL_REJECTED,
          "tampered signature rejected");
    CHECK(deliver(&db, decision(correlation, "decision-late", "allow", now + 2, "late"), now + 61) ==
          MODERATION_EXTERNAL_REJECTED, "timed out request rejected");
    post = db_post_get_by_id(&db, post_id);
    CHECK(post && strcmp(cJSON_GetObjectItem(post, "status")->valuestring, "pending") == 0,
          "timeout and invalid messages fall back to local review");
    cJSON_Delete(post);

    CHECK(deliver(&db, decision(correlation, "decision-allow", "allow", now + 3, "acceptable"), now + 4) ==
          MODERATION_EXTERNAL_APPLIED, "valid allow decision applied");
    CHECK(deliver(&db, decision(correlation, "decision-allow", "allow", now + 3, "acceptable"), now + 4) ==
          MODERATION_EXTERNAL_DUPLICATE, "allow delivery replay is idempotent");
    post = db_post_get_by_id(&db, post_id);
    CHECK(post && strcmp(cJSON_GetObjectItem(post, "status")->valuestring, "published") == 0,
          "allowed post published exactly once");
    cJSON_Delete(post);

    int comment_id = db_moderation_create_pending_comment(&db, "post", post_id, 0, "private author", 0,
                                                          "comment requiring review", "approval_queue");
    const char *comment_correlation = "fedcba9806543210fedcba9806543210";
    request = db_moderation_external_create(&db, MODERATION_COMMENT, comment_id, comment_correlation, now, now + 60);
    CHECK(request != NULL, "create comment review request");
    cJSON_Delete(request);
    CHECK(deliver(&db, decision(comment_correlation, "decision-reject", "reject", now + 3, "spam"), now + 4) ==
          MODERATION_EXTERNAL_APPLIED, "valid reject decision applied");
    cJSON *queue = db_moderation_list_pending(&db);
    CHECK(queue && cJSON_GetArraySize(queue) == 0, "all externally resolved items leave local queue");
    cJSON_Delete(queue);
    CHECK(deliver(&db, decision("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "decision-unknown", "allow", now, "unknown"), now) ==
          MODERATION_EXTERNAL_REJECTED, "unknown correlation rejected");

    db_comment_close_thread();
    db_comment_close();
    sqlite3_close(conn);
    if (reviewer_pubkey) cwist_free(reviewer_pubkey);
    fly_crypto_cleanup();
    remove(main_path);
    remove(comments_path);
    remove(seed_path);
    printf("%s\n", failures ? "SOME TESTS FAILED" : "ALL TESTS PASSED");
    return failures ? 1 : 0;
}

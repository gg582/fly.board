#include "db/db_sync_anti_entropy.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;
#define CHECK(c, name) do { if (c) printf("PASS: %s\n", name); \
    else { printf("FAIL: %s (line %d)\n", name, __LINE__); failures++; } } while (0)

static db_sync_mutation make_mutation(const char *id, const char *op, const char *payload,
                                      long long changed_at, char hash[65]) {
    db_sync_payload_hash("post", 7, op, payload, changed_at, hash);
    db_sync_mutation m = { id, "post", 7, op, payload, changed_at, hash };
    return m;
}

static int winner_is(sqlite3 *conn, db_sync_mutation_owned *winner, const char *id) {
    int ok = db_sync_anti_entropy_winner(conn, "post", 7, winner) && strcmp(winner->mutation_id, id) == 0;
    db_sync_mutation_free(winner);
    return ok;
}

int main(void) {
    const char *path = "/tmp/flytest_anti_entropy.db";
    unlink(path);
    sqlite3 *conn = NULL;
    CHECK(sqlite3_open(path, &conn) == SQLITE_OK, "open test database");
    int rc = sqlite3_exec(conn,
        "CREATE TABLE sync_journal (seq INTEGER PRIMARY KEY AUTOINCREMENT,entity TEXT,entity_id INTEGER,"
        "op TEXT,payload TEXT,created_at DATETIME DEFAULT CURRENT_TIMESTAMP);"
        "INSERT INTO sync_journal(entity,entity_id,op,payload) VALUES('post',7,'upsert','{\"title\":\"legacy\"}');"
        "CREATE TABLE sync_mutation_log (mutation_id TEXT PRIMARY KEY,entity TEXT,entity_id INTEGER,"
        "op TEXT,payload TEXT,payload_hash TEXT,created_at DATETIME DEFAULT CURRENT_TIMESTAMP);"
        "INSERT INTO sync_mutation_log VALUES('old-format','post',8,'upsert','{}','0000000000000000000000000000000000000000000000000000000000000000',CURRENT_TIMESTAMP);",
        NULL, NULL, NULL);
    CHECK(rc == SQLITE_OK, "legacy journal fixture");
    CHECK(db_sync_anti_entropy_init(conn), "initialize and backfill mutation log");
    db_sync_mutation_owned winner;
    CHECK(db_sync_anti_entropy_winner(conn, "post", 7, &winner) &&
          strcmp(winner.mutation_id, "legacy-0000000000000001") == 0 && winner.changed_at == 1,
          "legacy row uses journal seq ordering");
    db_sync_mutation_free(&winner);
    CHECK(db_sync_anti_entropy_winner(conn, "post", 8, &winner) && winner.changed_at == 0, "read old-format mutation");
    char migrated_hash[65];
    db_sync_payload_hash("post", 8, "upsert", "{}", 0, migrated_hash);
    CHECK(strcmp(winner.payload_hash, migrated_hash) == 0, "migration refreshes hash with changed_at");
    db_sync_mutation_free(&winner);

    CHECK(db_sync_anti_entropy_record(conn, "post", 7, "upsert", "{\"title\":\"new\"}"), "record first upsert");
    CHECK(db_sync_anti_entropy_record(conn, "post", 7, "delete", "{}"), "record delete");
    CHECK(db_sync_anti_entropy_record(conn, "post", 7, "upsert", "{\"title\":\"recreated\"}"), "record recreation");
    CHECK(db_sync_anti_entropy_winner(conn, "post", 7, &winner) && strcmp(winner.op, "upsert") == 0 &&
          strstr(winner.payload, "recreated") != NULL, "sequential latest mutation wins after deletion");
    db_sync_mutation_free(&winner);

    CHECK(sqlite3_exec(conn, "DELETE FROM sync_mutation_log", NULL, NULL, NULL) == SQLITE_OK, "reset union");
    char hash_new[65], hash_old[65], hash_delete[65], hash_recreate[65];
    db_sync_mutation newer = make_mutation("newer", "upsert", "{\"title\":\"newer\"}", 200, hash_new);
    db_sync_mutation older = make_mutation("older", "upsert", "{\"title\":\"older\"}", 100, hash_old);
    db_sync_mutation old_delete = make_mutation("old-delete", "delete", "{}", 150, hash_delete);
    bool inserted = false;
    CHECK(db_sync_anti_entropy_import(conn, &newer, &inserted) && inserted, "import newer mutation");
    CHECK(db_sync_anti_entropy_import(conn, &newer, &inserted) && !inserted, "duplicate import idempotent");
    CHECK(db_sync_anti_entropy_import(conn, &older, NULL), "older upsert arrives late");
    CHECK(winner_is(conn, &winner, "newer"), "older upsert cannot overwrite newer upsert");
    CHECK(db_sync_anti_entropy_import(conn, &old_delete, NULL), "older delete arrives late");
    CHECK(winner_is(conn, &winner, "newer"), "older delete cannot remove newer upsert");

    db_sync_mutation delete = make_mutation("delete-300", "delete", "{}", 300, hash_delete);
    db_sync_mutation recreate = make_mutation("recreate-400", "upsert", "{\"title\":\"alive\"}", 400, hash_recreate);
    CHECK(db_sync_anti_entropy_import(conn, &delete, NULL), "import newer delete");
    CHECK(winner_is(conn, &winner, "delete-300"), "newer delete beats older upsert");
    CHECK(db_sync_anti_entropy_import(conn, &recreate, NULL), "import post-delete upsert");
    CHECK(winner_is(conn, &winner, "recreate-400"), "newer upsert after deletion is allowed");

    db_sync_mutation same_delete = make_mutation("same-delete", "delete", "{}", 400, hash_delete);
    CHECK(db_sync_anti_entropy_import(conn, &same_delete, NULL), "import equal-time delete");
    CHECK(winner_is(conn, &winner, "same-delete"), "delete breaks equal-time operation tie");

    char bad_hash[65];
    db_sync_mutation bad = make_mutation("tampered", "upsert", "{\"title\":\"bad\"}", 500, bad_hash);
    bad.changed_at = 600;
    CHECK(!db_sync_anti_entropy_import(conn, &bad, &inserted), "changed_at tampering fails hash integrity");

    CHECK(sqlite3_exec(conn, "DELETE FROM sync_mutation_log", NULL, NULL, NULL) == SQLITE_OK, "reset tie union");
    char hash_a[65], hash_b[65];
    db_sync_mutation a = make_mutation("peer-a", "upsert", "{\"title\":\"a\"}", 700, hash_a);
    db_sync_mutation b = make_mutation("peer-b", "upsert", "{\"title\":\"b\"}", 700, hash_b);
    CHECK(db_sync_anti_entropy_import(conn, &b, NULL) && db_sync_anti_entropy_import(conn, &a, NULL),
          "import equal-time upserts in reverse order");
    const char *expected = strcmp(a.payload_hash, b.payload_hash) > 0 ||
                           (strcmp(a.payload_hash, b.payload_hash) == 0 && strcmp(a.mutation_id, b.mutation_id) > 0)
                               ? a.mutation_id : b.mutation_id;
    CHECK(winner_is(conn, &winner, expected), "payload hash and mutation id break remaining ties");

    sqlite3_close(conn);
    unlink(path);
    printf("%s\n", failures ? "SOME FAILED" : "ALL PASS");
    return failures != 0;
}

#ifndef DB_SYNC_ANTI_ENTROPY_H
#define DB_SYNC_ANTI_ENTROPY_H

#include <stdbool.h>
#include <sqlite3.h>

typedef struct {
    const char *mutation_id;
    const char *entity;
    int entity_id;
    const char *op;
    const char *payload;
    long long changed_at;
    const char *payload_hash;
} db_sync_mutation;

typedef struct {
    char *mutation_id;
    char *entity;
    int entity_id;
    char *op;
    char *payload;
    long long changed_at;
    char *payload_hash;
} db_sync_mutation_owned;

bool db_sync_anti_entropy_init(sqlite3 *conn);
bool db_sync_anti_entropy_record(sqlite3 *conn, const char *entity, int entity_id,
                                 const char *op, const char *payload);
bool db_sync_anti_entropy_import(sqlite3 *conn, const db_sync_mutation *mutation, bool *inserted);
bool db_sync_anti_entropy_winner(sqlite3 *conn, const char *entity, int entity_id,
                                 db_sync_mutation_owned *winner);
void db_sync_mutation_free(db_sync_mutation_owned *mutation);
void db_sync_payload_hash(const char *entity, int entity_id, const char *op,
                          const char *payload, long long changed_at, char out[65]);

#endif

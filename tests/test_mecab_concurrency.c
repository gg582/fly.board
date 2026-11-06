#include "db/fts5_mecab.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#define THREADS 8
#define ITERATIONS 100

static atomic_int g_failures;

static void *indexer(void *arg) {
    (void)arg;
    for (int i = 0; i < ITERATIONS; i++) {
        sqlite3 *db = NULL;
        char out[8][64];
        int count = fts5_mecab_split_term("개발일지", out, 8);
        if (count <= 0 || sqlite3_open(":memory:", &db) != SQLITE_OK ||
            !fts5_search_register_conn(db) ||
            sqlite3_exec(db, "CREATE VIRTUAL TABLE posts USING fts5(body, tokenize='mecab')", NULL, NULL, NULL) != SQLITE_OK ||
            sqlite3_exec(db, "INSERT INTO posts(body) VALUES ('개발일지를 작성했다')", NULL, NULL, NULL) != SQLITE_OK) {
            atomic_fetch_add(&g_failures, 1);
        }
        if (db) sqlite3_close(db);
    }
    return NULL;
}

int main(void) {
    if (strcmp(fts5_search_preferred_tokenizer(), "mecab") != 0) {
        puts("MeCab concurrency: SKIP (dictionary unavailable)");
        return 0;
    }

    pthread_t threads[THREADS];
    for (int i = 0; i < THREADS; i++) {
        if (pthread_create(&threads[i], NULL, indexer, NULL) != 0) return 1;
    }
    for (int i = 0; i < THREADS; i++) pthread_join(threads[i], NULL);
    if (atomic_load(&g_failures) != 0) return 1;

    puts("MeCab concurrency: independent tokenizers and FTS5 writes OK");
    return 0;
}

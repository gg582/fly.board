#define _POSIX_C_SOURCE 200809L
#include "db.h"
#include "db_internal.h"
#include "crypto/fly_crypto.h"
#include <cwist/core/mem/alloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Public keys that have signed posts on this site. The current key is
 * added on every start; keys from earlier hosts or rotations stay, so their
 * signatures keep verifying without the private keys. */

static bool exec(sqlite3 *conn, const char *sql) {
    return sqlite3_exec(conn, sql, NULL, NULL, NULL) == SQLITE_OK;
}

/* Rewrite bare signatures from before key ids as "<id>:<sig>", but only
 * those that verify under the current key: a signature over altered content
 * stays as it is and keeps failing. */
static int prefix_legacy_signatures(sqlite3 *conn, const char *key_id) {
    sqlite3_stmt *sel = NULL;
    if (sqlite3_prepare_v2(conn, "SELECT id, title, content, pqc_signature FROM posts"
                                 " WHERE pqc_signature<>'' AND instr(pqc_signature, ':')=0", -1, &sel, NULL) != SQLITE_OK) {
        return -1;
    }
    sqlite3_stmt *upd = NULL;
    if (sqlite3_prepare_v2(conn, "UPDATE posts SET pqc_signature=? WHERE id=?", -1, &upd, NULL) != SQLITE_OK) {
        sqlite3_finalize(sel);
        return -1;
    }
    int n = 0;
    while (sqlite3_step(sel) == SQLITE_ROW) {
        const char *t = (const char *)sqlite3_column_text(sel, 1);
        const char *c = (const char *)sqlite3_column_text(sel, 2);
        const char *sig = (const char *)sqlite3_column_text(sel, 3);
        t = t ? t : "";
        c = c ? c : "";
        size_t mlen = strlen(t) + 1 + strlen(c);
        char *msg = (char *)malloc(mlen + 1);
        if (!msg) continue;
        snprintf(msg, mlen + 1, "%s\n%s", t, c);
        if (fly_crypto_verify((const uint8_t *)msg, mlen, sig)) {
            size_t slen = strlen(key_id) + 1 + strlen(sig) + 1;
            char *prefixed = (char *)malloc(slen);
            if (prefixed) {
                snprintf(prefixed, slen, "%s:%s", key_id, sig);
                sqlite3_bind_text(upd, 1, prefixed, -1, SQLITE_TRANSIENT);
                sqlite3_bind_int(upd, 2, sqlite3_column_int(sel, 0));
                if (sqlite3_step(upd) == SQLITE_DONE) n++;
                sqlite3_reset(upd);
                free(prefixed);
            }
        }
        free(msg);
    }
    sqlite3_finalize(sel);
    sqlite3_finalize(upd);
    return n;
}

bool pqc_keys_sync_conn(sqlite3 *conn, bool register_current) {
    if (!conn) return false;
    if (!exec(conn, "CREATE TABLE IF NOT EXISTS pqc_keys (key_id TEXT PRIMARY KEY, public_key TEXT NOT NULL,"
                    " created_at DATETIME DEFAULT CURRENT_TIMESTAMP)")) {
        return false;
    }
    const char *cur = fly_crypto_key_id();
    if (register_current && cur) {
        char *pk = NULL;
        if (!fly_crypto_pubkey_export(&pk)) return false;
        sqlite3_stmt *ins = NULL;
        bool ok = sqlite3_prepare_v2(conn, "INSERT OR IGNORE INTO pqc_keys (key_id, public_key) VALUES (?, ?)", -1, &ins, NULL) == SQLITE_OK;
        if (ok) {
            sqlite3_bind_text(ins, 1, cur, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(ins, 2, pk, -1, SQLITE_TRANSIENT);
            ok = sqlite3_step(ins) == SQLITE_DONE;
            sqlite3_finalize(ins);
        }
        cwist_free(pk);
        if (!ok) return false;
    }

    /* Load every known key for verification. */
    sqlite3_stmt *sel = NULL;
    if (sqlite3_prepare_v2(conn, "SELECT key_id, public_key FROM pqc_keys", -1, &sel, NULL) != SQLITE_OK) return false;
    while (sqlite3_step(sel) == SQLITE_ROW) {
        const char *id = (const char *)sqlite3_column_text(sel, 0);
        const char *pk = (const char *)sqlite3_column_text(sel, 1);
        char check[FLY_PQC_KEY_ID_LEN + 1];
        /* The id is derived from the key; a row whose id does not match was
         * edited by hand and is ignored. */
        if (!pk || !fly_crypto_key_id_of(pk, check) || !id || strcmp(check, id) != 0) {
            fprintf(stderr, "[pqc] ignoring pqc_keys row %s: id does not match its key\n", id ? id : "?");
            continue;
        }
        fly_crypto_add_public_key(pk);
    }
    sqlite3_finalize(sel);

    if (register_current && cur) {
        int n = prefix_legacy_signatures(conn, cur);
        if (n > 0) fprintf(stderr, "[pqc] tagged %d existing signatures with key id %s\n", n, cur);
    }
    return true;
}

bool db_pqc_keys_sync(cwist_db *db) {
    return pqc_keys_sync_conn(fly_db_conn(db), true);
}

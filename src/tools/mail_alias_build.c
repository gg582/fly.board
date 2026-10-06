#define _POSIX_C_SOURCE 200809L
/**
 * @file mail_alias_build.c
 * @brief Emit the Postfix virtual alias map for oborona.zip on stdout.
 *
 * Every account gets a identity mapping (user@oborona.zip -> user@oborona.zip)
 * and the reserved role addresses (postmaster, abuse, ...) point at the site
 * admin account. The output is plain text; the deployment script feeds it to
 * postmap(1) to build /etc/postfix/fly_aliases.db.
 */
#include "db/db_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MAIL_DOMAIN "oborona.zip"

static const char *const k_reserved[] = {
    "postmaster", "abuse", "admin", "administrator", "support", "help",
    "noreply", "mailer-daemon", "root", "info", "webmaster", "hostmaster",
    "noc", "security", NULL
};

static const char *db_path(void) {
    const char *p = getenv("FLY_DB_PATH");
    return (p && p[0]) ? p : FLY_DB_MAIN_PATH;
}

int main(void) {
    sqlite3 *conn = NULL;
    if (sqlite3_open_v2(db_path(), &conn, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK) {
        fprintf(stderr, "mail-alias-build: cannot open %s\n", db_path());
        if (conn) sqlite3_close(conn);
        return 1;
    }
    db_configure_connection(conn);

    /* Site admin target for the reserved addresses. */
    char admin_addr[256] = {0};
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(conn,
            "SELECT username FROM users WHERE role='admin' ORDER BY id LIMIT 1", -1, &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const unsigned char *un = sqlite3_column_text(stmt, 0);
            if (un) snprintf(admin_addr, sizeof(admin_addr), "%s@" MAIL_DOMAIN, (const char *)un);
        }
    }
    sqlite3_finalize(stmt);
    if (!admin_addr[0]) {
        fprintf(stderr, "mail-alias-build: no admin account found in %s\n", db_path());
        sqlite3_close(conn);
        return 1;
    }

    for (int i = 0; k_reserved[i]; i++)
        printf("%s@%s %s\n", k_reserved[i], MAIL_DOMAIN, admin_addr);

    stmt = NULL;
    if (sqlite3_prepare_v2(conn,
            "SELECT username FROM users WHERE active=1 ORDER BY username", -1, &stmt, NULL) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const unsigned char *un = sqlite3_column_text(stmt, 0);
            if (!un) continue;
            const char *name = (const char *)un;
            /* A reserved localpart already has a mapping above; emitting an
             * identity line for it too would be a duplicate key. */
            bool reserved = false;
            for (int i = 0; k_reserved[i]; i++)
                if (strcasecmp(name, k_reserved[i]) == 0) { reserved = true; break; }
            if (reserved) continue;
            printf("%s@%s %s@%s\n", name, MAIL_DOMAIN, name, MAIL_DOMAIN);
        }
    }
    sqlite3_finalize(stmt);
    sqlite3_close(conn);
    return 0;
}

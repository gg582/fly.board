#define _POSIX_C_SOURCE 200809L
/**
 * @file mail_alias_build.c
 * @brief Emit the Postfix virtual maps for oborona.zip.
 *
 * Stdout: virtual alias map — the reserved role addresses (postmaster,
 * abuse, ...) point at the site admin account. Identity mappings are NOT
 * emitted: oborona.zip is a virtual_mailbox_domain, so plain users resolve
 * through the mailbox map (an identity alias would look like a self-referral
 * loop to Postfix and be rejected).
 *
 * argv[1] (optional): path of the virtual mailbox map, one "user@domain OK"
 * line per active user. The deployment script feeds both outputs to
 * postmap(1).
 */
#include "db/db_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const char *mail_domain(void) {
    const char *d = getenv("FLY_MAIL_DOMAIN");
    return (d && d[0]) ? d : "localhost";
}

static const char *const k_reserved[] = {
    "postmaster", "abuse", "admin", "administrator", "support", "help",
    "noreply", "mailer-daemon", "root", "info", "webmaster", "hostmaster",
    "noc", "security", NULL
};

static const char *db_path(void) {
    const char *p = getenv("FLY_DB_PATH");
    return (p && p[0]) ? p : FLY_DB_MAIN_PATH;
}

int main(int argc, char **argv) {
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
            if (un) snprintf(admin_addr, sizeof(admin_addr), "%s@%s", (const char *)un, mail_domain());
        }
    }
    sqlite3_finalize(stmt);
    if (!admin_addr[0]) {
        fprintf(stderr, "mail-alias-build: no admin account found in %s\n", db_path());
        sqlite3_close(conn);
        return 1;
    }

    for (int i = 0; k_reserved[i]; i++)
        printf("%s@%s %s\n", k_reserved[i], mail_domain(), admin_addr);

    /* Virtual mailbox map: the user list Postfix validates recipients
     * against. Identity alias lines are NOT emitted — with
     * virtual_alias_domains a self-mapping is treated as an alias loop and
     * the recipient is rejected with "User unknown in virtual alias table".
     * oborona.zip is instead a virtual_mailbox_domain: users resolve via
     * this map and only the reserved role addresses use virtual aliases. */
    FILE *mailboxes = NULL;
    if (argc > 1 && argv[1][0]) {
        mailboxes = fopen(argv[1], "w");
        if (!mailboxes) {
            fprintf(stderr, "mail-alias-build: cannot write %s\n", argv[1]);
            sqlite3_close(conn);
            return 1;
        }
    }
    stmt = NULL;
    if (sqlite3_prepare_v2(conn,
            "SELECT username FROM users WHERE active=1 ORDER BY username", -1, &stmt, NULL) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const unsigned char *un = sqlite3_column_text(stmt, 0);
            if (!un) continue;
            const char *name = (const char *)un;
            bool reserved = false;
            for (int i = 0; k_reserved[i]; i++)
                if (strcasecmp(name, k_reserved[i]) == 0) { reserved = true; break; }
            if (reserved) continue;
            if (mailboxes) fprintf(mailboxes, "%s@%s OK\n", name, mail_domain());
        }
    }
    if (mailboxes) fclose(mailboxes);
    sqlite3_finalize(stmt);
    sqlite3_close(conn);
    return 0;
}

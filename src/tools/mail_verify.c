#define _POSIX_C_SOURCE 200809L
/**
 * @file mail_verify.c
 * @brief Dovecot checkpassword helper for the fly.board mail stack.
 *
 * Two modes, selected by Dovecot's invocation environment:
 *
 *   passdb  (default): USER carries the login name and the password arrives
 *                      either in the PASSWORD environment variable or, per the
 *                      classic checkpassword protocol, on file descriptor 3.
 *                      The password is checked against users.password_hash
 *                      with auth_verify_password(); accounts with
 *                      users.email_verified=0 are rejected so unverified
 *                      signups cannot reach IMAP/webmail.
 *
 *   userdb  (DOVECOT_USERDB_LOOKUP=1 or argv[1] == "userdb"): USER alone is
 *                      given; the account must simply exist (and be verified).
 *
 *   pam     (argv[1] == "pam"): invoked by pam_exec(8). PAM_USER carries the
 *                      login name and expose_authtok delivers the password on
 *                      stdin. Used with Dovecot 2.4, whose checkpassword
 *                      driver was removed: passdb pam { } -> pam service
 *                      "fly-mail" -> pam_exec.so -> this binary.
 *
 * On success the userdb tuple (USER/HOME/UID/GID) is written as key=value
 * lines terminated by an empty line, to stdout and, when it is writable, fd 3
 * (the classic protocol channel). Exit status: 0 ok, 1 rejected, 111 tempfail.
 */
#include "db/db_internal.h"
#include "auth/auth.h"
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAIL_HOME_ROOT "/var/mail/fly"

static const char *db_path(void) {
    static char exe_relative[512];
    const char *p = getenv("FLY_DB_PATH");
    if (p && p[0]) return p;
    if (access(FLY_DB_MAIN_PATH, R_OK) == 0) return FLY_DB_MAIN_PATH;
    /* Invoked by Dovecot/PAM/Postfix with an unrelated cwd: fall back to the
     * database next to the binary (/proc/self/exe). */
    ssize_t n = readlink("/proc/self/exe", exe_relative, sizeof(exe_relative) - 1);
    if (n > 0) {
        exe_relative[n] = '\0';
        char *slash = strrchr(exe_relative, '/');
        if (slash) {
            snprintf(slash + 1, sizeof(exe_relative) - (size_t)(slash + 1 - exe_relative),
                     "data/blog.db");
            if (access(exe_relative, R_OK) == 0) return exe_relative;
        }
    }
    return FLY_DB_MAIN_PATH;
}

static bool fd_writable(int fd) {
    int fl = fcntl(fd, F_GETFL);
    return fl >= 0 && ((fl & O_ACCMODE) == O_WRONLY || (fl & O_ACCMODE) == O_RDWR);
}

static void emit_userdb(int fd, const char *username, uid_t uid, gid_t gid) {
    dprintf(fd, "USER=%s\nHOME=%s/%s\nUID=%u\nGID=%u\n\n",
            username, MAIL_HOME_ROOT, username, (unsigned)uid, (unsigned)gid);
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);

    bool userdb_mode = getenv("DOVECOT_USERDB_LOOKUP") != NULL ||
                       (argc > 1 && strcmp(argv[1], "userdb") == 0);
    bool pam_mode = argc > 1 && strcmp(argv[1], "pam") == 0;
    const char *user_env = pam_mode ? getenv("PAM_USER") : getenv("USER");
    if (!user_env || !user_env[0]) return 1;
    /* Dovecot passes the full login name; we only serve the bare local part. */
    char username[128];
    snprintf(username, sizeof(username), "%s", user_env);
    char *at = strrchr(username, '@');
    if (at) *at = '\0';
    if (!username[0] || strlen(username) >= sizeof(username)) return 1;

    char password[512] = {0};
    if (!userdb_mode) {
        const char *pw_env = pam_mode ? NULL : getenv("PASSWORD");
        if (pw_env && pw_env[0]) {
            snprintf(password, sizeof(password), "%s", pw_env);
        } else {
            /* Classic checkpassword: password bytes on fd 3.
             * pam_exec with expose_authtok: password bytes on stdin. */
            int fd = pam_mode ? STDIN_FILENO : 3;
            size_t off = 0;
            char tmp[256];
            ssize_t n;
            while (off + 1 < sizeof(password) && (n = read(fd, tmp, sizeof(tmp))) > 0) {
                for (ssize_t i = 0; i < n && off + 1 < sizeof(password); i++) {
                    if (tmp[i] == '\0' || tmp[i] == '\n') continue;
                    password[off++] = tmp[i];
                }
            }
            password[off] = '\0';
        }
        if (!password[0]) return 1;
    }

    sqlite3 *conn = NULL;
    if (sqlite3_open_v2(db_path(), &conn, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK) {
        if (conn) sqlite3_close(conn);
        fprintf(stderr, "mail-verify: cannot open %s\n", db_path());
        return 111;
    }
    db_configure_connection(conn);

    char sql[] = "SELECT password_hash, email_verified FROM users WHERE username = ?1 "
                 "OR username = ?2 LIMIT 1";
    char lower[128];
    size_t ln = strlen(username);
    for (size_t i = 0; i <= ln && i < sizeof(lower) - 1; i++)
        lower[i] = (char)((username[i] >= 'A' && username[i] <= 'Z') ? username[i] + 32 : username[i]);
    sqlite3_stmt *stmt = NULL;
    bool found = false, verified = false;
    char hash[512] = {0};
    if (sqlite3_prepare_v2(conn, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, lower, -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            found = true;
            const unsigned char *h = sqlite3_column_text(stmt, 0);
            if (h) snprintf(hash, sizeof(hash), "%s", (const char *)h);
            verified = sqlite3_column_int(stmt, 1) == 1;
        }
    }
    sqlite3_finalize(stmt);
    sqlite3_close(conn);

    if (!found || !verified) return 1;
    if (!userdb_mode && !auth_verify_password(password, hash)) {
        if (getenv("MV_DEBUG"))
            fprintf(stderr, "mail-verify: verify failed user=%s hash=%.16s pwlen=%zu\n",
                    username, hash, strlen(password));
        return 1;
    }

    uid_t uid = getuid();
    gid_t gid = getgid();
    emit_userdb(STDOUT_FILENO, username, uid, gid);
    if (fd_writable(3)) emit_userdb(3, username, uid, gid);
    return 0;
}

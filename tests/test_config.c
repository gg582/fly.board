/* Unit test: src/config/config.c blog.settings parsing: defaults, key/value
 * parsing, FLY_MAIL_DOMAIN env override, and root_url host fallback for the
 * resolved mail domain.
 *
 * Build:
 *   gcc -O1 -Iinclude -Isrc -I/home/yjlee/cwist/include -I/home/yjlee/cwist/lib \
 *       tests/test_config.c src/config/config.o \
 *       /home/yjlee/cwist/libcwist.a /home/yjlee/cwist/lib/libttak/lib/libttak.a \
 *       -lpthread -lm -ldl -o /tmp/test_config
 * Run: /tmp/test_config
 */
#include "config/config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures = 0;
#define CHECK(cond, name) do { \
    if (cond) printf("PASS: %s\n", name); \
    else { printf("FAIL: %s (line %d)\n", name, __LINE__); failures++; } \
} while (0)

#define PATH "/tmp/flytest_blog.settings"

static void write_settings(const char *content) {
    FILE *f = fopen(PATH, "w");
    fputs(content, f);
    fclose(f);
}

int main(void) {
    unsetenv("FLY_MAIL_DOMAIN");
    remove(PATH);

    /* missing file -> defaults written and loaded */
    CHECK(blog_config_load(PATH), "load with missing file succeeds");
    CHECK(access(PATH, F_OK) == 0, "missing file created with defaults");
    CHECK(g_config.title[0] != '\0', "default title set");
    CHECK(g_config.port > 0, "default port set");
    CHECK(strcmp(g_config.language, "ko") == 0, "default language is ko");
    /* a just-created file is written with defaults but not parsed until the
     * next load; mail_domain resolves on the reload */
    CHECK(g_config.mail_domain[0] == '\0', "fresh defaults leave mail_domain unresolved");
    CHECK(blog_config_load(PATH), "reload freshly created defaults");
    CHECK(strcmp(g_config.mail_domain, "localhost") == 0,
          "mail domain falls back to localhost");

    /* key/value parsing */
    write_settings(
        "title=My Blog\n"
        "subtitle=Sub here\n"
        "port=9999\n"
        "language=en\n"
        "invert_logo=true\n"
        "use_tasfa=true\n"
        "roundness=0.75\n"
        "max_upload_size=5M\n"
        "vote_only=authorized\n"
        "root_url=https://blog.example.com:8443/path\nmail_domain=\n"
    );
    CHECK(blog_config_load(PATH), "load custom settings");
    CHECK(strcmp(g_config.title, "My Blog") == 0, "title parsed");
    CHECK(strcmp(g_config.subtitle, "Sub here") == 0, "subtitle parsed");
    CHECK(g_config.port == 9999, "port parsed");
    CHECK(strcmp(g_config.language, "en") == 0, "language parsed");
    CHECK(g_config.invert_logo == true, "invert_logo bool parsed");
    CHECK(g_config.use_tasfa == true, "use_tasfa bool parsed");
    CHECK(g_config.roundness > 0.7f && g_config.roundness < 0.8f, "roundness parsed");
    CHECK(g_config.max_upload_size == 5LL * 1024 * 1024, "max_upload_size parsed");
    CHECK(strcmp(g_config.vote_only, "authorized") == 0, "vote_only parsed");
    CHECK(strcmp(g_config.mail_domain, "blog.example.com") == 0,
          "mail domain derived from root_url host (port/path stripped)");

    /* explicit mail_domain setting wins over root_url host */
    write_settings("root_url=https://blog.example.com\nmail_domain=mail.example.org\n");
    CHECK(blog_config_load(PATH), "load with explicit mail_domain");
    CHECK(strcmp(g_config.mail_domain, "mail.example.org") == 0,
          "explicit mail_domain setting wins");

    /* FLY_MAIL_DOMAIN env override wins over everything */
    setenv("FLY_MAIL_DOMAIN", "env.example.net", 1);
    write_settings("root_url=https://blog.example.com\nmail_domain=mail.example.org\n");
    CHECK(blog_config_load(PATH), "load with env override present");
    CHECK(strcmp(g_config.mail_domain, "env.example.net") == 0,
          "FLY_MAIL_DOMAIN env overrides setting");
    unsetenv("FLY_MAIL_DOMAIN");

    /* vote_only normalization */
    write_settings("vote_only=bogus\n");
    CHECK(blog_config_load(PATH), "load with bogus vote_only");
    CHECK(g_config.vote_only[0] == '\0', "bogus vote_only reset to default");

    /* accessor helpers */
    CHECK(config_vote_allowed(false, NULL), "vote allowed for all by default");
    write_settings("vote_only=authorized\n");
    CHECK(blog_config_load(PATH), "reload vote_only=authorized");
    CHECK(!config_vote_allowed(false, NULL), "anonymous vote denied when authorized");
    CHECK(config_vote_allowed(true, "user"), "logged-in vote allowed when authorized");

    remove(PATH);
    printf("%s\n", failures == 0 ? "ALL PASS" : "SOME FAILED");
    return failures == 0 ? 0 : 1;
}

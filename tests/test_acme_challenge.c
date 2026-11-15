#define _POSIX_C_SOURCE 200809L
#include "utils/acme_challenge.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;
#define CHECK(cond, name) do { \
    if (cond) printf("PASS: %s\n", name); \
    else { printf("FAIL: %s (line %d)\n", name, __LINE__); failures++; } \
} while (0)

static cwist_http_response *call_handler(const char *json, bool authorized) {
    cwist_http_request *req = cwist_http_request_create();
    cwist_http_response *res = cwist_http_response_create();
    req->method = CWIST_HTTP_POST;
    req->client_fd = -1;
    if (authorized) cwist_http_header_add(&req->headers, "X-FlyWire-Token", "test-token");
    cwist_sstring_assign(req->body, json);
    handler_flywire_acme_challenge(req, res);
    cwist_http_request_destroy(req);
    return res;
}

static bool file_equals(const char *path, const char *expected) {
    char buf[256] = {0};
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    return n >= 0 && strcmp(buf, expected) == 0;
}

static int connect_client(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(19091);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static bool http_get(const char *path, char *out, size_t out_size) {
    int fd = connect_client();
    if (fd < 0) return false;
    char request[768];
    int n = snprintf(request, sizeof(request), "GET %s HTTP/1.1\r\nHost: test.local\r\nConnection: close\r\n\r\n", path);
    ssize_t sent = write(fd, request, (size_t)n);
    size_t used = 0;
    while (sent == n && used + 1 < out_size) {
        ssize_t got = read(fd, out + used, out_size - used - 1);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) break;
        used += (size_t)got;
    }
    close(fd);
    out[used] = '\0';
    return sent == n;
}

int main(void) {
    char root[] = "/tmp/flyboard-acme-test.XXXXXX";
    char cwd[4096];
    if (!mkdtemp(root) || !getcwd(cwd, sizeof(cwd))) return 2;
    if (chdir(root) != 0) return 2;

    const char *json = "{\"token\":\"abc_DEF-123\",\"response\":\"abc_DEF-123.key-auth_hash\"}";
    cwist_http_response *res = call_handler(json, false);
    CHECK(res->status_code == 401, "unauthenticated replication rejected");
    cwist_http_response_destroy(res);
    CHECK(access(".lego/webroot/.well-known/acme-challenge/abc_DEF-123", F_OK) != 0,
          "unauthenticated token not written");

    res = call_handler(json, true);
    CHECK(res->status_code == 200, "authenticated replication accepted");
    cwist_http_response_destroy(res);
    CHECK(file_equals(".lego/webroot/.well-known/acme-challenge/abc_DEF-123", "abc_DEF-123.key-auth_hash"),
          "challenge persisted byte-for-byte");

    res = call_handler("{\"token\":\"../escape\",\"response\":\"bad\"}", true);
    CHECK(res->status_code == 400, "unsafe token rejected");
    cwist_http_response_destroy(res);

    setenv("FLY_CERT_HTTP_PORT", "19091", 1);
    acme_challenge_start();
    int client = -1;
    for (int i = 0; i < 30; i++) {
        client = connect_client();
        if (client >= 0) break;
        usleep(100000);
    }
    if (client >= 0) close(client);
    CHECK(client >= 0, "ACME listener ready");

    char response[8192];
    bool ok = http_get("/.well-known/acme-challenge/abc_DEF-123", response, sizeof(response));
    CHECK(ok && strstr(response, "200 OK") && strstr(response, "abc_DEF-123.key-auth_hash"),
          "HTTP-01 serves the replicated token");
    ok = http_get("/.well-known/acme-challenge/missing", response, sizeof(response));
    CHECK(ok && strstr(response, "404 Not Found"), "missing token returns 404");
    ok = http_get("/etc/passwd", response, sizeof(response));
    CHECK(ok && strstr(response, "404 Not Found"), "non-ACME path returns 404");

    acme_challenge_stop();
    acme_challenge_join();
    if (chdir(cwd) == 0) {
        char command[4200];
        snprintf(command, sizeof(command), "rm -rf %s", root);
        (void)system(command);
    }
    return failures ? 1 : 0;
}

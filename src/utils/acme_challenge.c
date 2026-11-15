#define _POSIX_C_SOURCE 200809L
#include "utils/acme_challenge.h"
#include "config/config.h"
#include "engine/flywire.h"
#include "engine/flywire_cluster.h"
#include <cjson/cJSON.h>
#include <curl/curl.h>
#include <cwist/core/log.h>
#include <cwist/core/mem/alloc.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define ACME_TOKEN_DIR ACME_LEGO_WEBROOT "/.well-known/acme-challenge"
#define ACME_MAX_TOKEN 128
#define ACME_MAX_RESPONSE 4096
#define ACME_SEEN_CAPACITY 64

static _Atomic bool g_acme_running = false;
static _Atomic int g_acme_listen_fd = -1;
static pthread_t g_acme_thread;
static bool g_acme_thread_started = false;
static pid_t g_acme_owner = 0;

static pthread_mutex_t g_seen_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_seen[ACME_SEEN_CAPACITY][ACME_MAX_TOKEN + 1];
static size_t g_seen_next;

static bool acme_token_valid(const char *token) {
    if (!token || !token[0] || strlen(token) > ACME_MAX_TOKEN) return false;
    for (const unsigned char *p = (const unsigned char *)token; *p; p++) {
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
            (*p >= '0' && *p <= '9') || *p == '-' || *p == '_') continue;
        return false;
    }
    return true;
}

static bool acme_response_valid(const char *response) {
    if (!response || !response[0] || strlen(response) > ACME_MAX_RESPONSE) return false;
    for (const unsigned char *p = (const unsigned char *)response; *p; p++) {
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
            (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' || *p == '.') continue;
        return false;
    }
    return true;
}

static bool acme_seen_mark(const char *token) {
    bool found = false;
    pthread_mutex_lock(&g_seen_lock);
    for (size_t i = 0; i < ACME_SEEN_CAPACITY; i++) {
        if (g_seen[i][0] && strcmp(g_seen[i], token) == 0) {
            found = true;
            break;
        }
    }
    if (!found) {
        snprintf(g_seen[g_seen_next], sizeof(g_seen[g_seen_next]), "%s", token);
        g_seen_next = (g_seen_next + 1) % ACME_SEEN_CAPACITY;
    }
    pthread_mutex_unlock(&g_seen_lock);
    return found;
}

static bool acme_make_dirs(void) {
    const char *dirs[] = {".lego", ACME_LEGO_WEBROOT,
                          ACME_LEGO_WEBROOT "/.well-known", ACME_TOKEN_DIR};
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        if (mkdir(dirs[i], 0755) != 0 && errno != EEXIST) return false;
    }
    return true;
}

static bool acme_read_file(const char *token, char *response, size_t capacity) {
    if (!acme_token_valid(token) || !response || capacity < 2) return false;
    char path[512];
    int n = snprintf(path, sizeof(path), "%s/%s", ACME_TOKEN_DIR, token);
    if (n < 0 || (size_t)n >= sizeof(path)) return false;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    struct stat st;
    bool ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0 &&
              st.st_size <= ACME_MAX_RESPONSE;
    ssize_t used = 0;
    if (ok) {
        while ((size_t)used < (size_t)st.st_size) {
            ssize_t got = read(fd, response + used, (size_t)st.st_size - (size_t)used);
            if (got < 0 && errno == EINTR) continue;
            if (got <= 0) { ok = false; break; }
            used += got;
        }
    }
    close(fd);
    if (!ok) return false;
    response[used] = '\0';
    return acme_response_valid(response);
}

static bool acme_write_file(const char *token, const char *response) {
    if (!acme_token_valid(token) || !acme_response_valid(response) || !acme_make_dirs()) return false;
    char path[512], tmp[512];
    int n = snprintf(path, sizeof(path), "%s/%s", ACME_TOKEN_DIR, token);
    int tn = snprintf(tmp, sizeof(tmp), "%s/.%s.tmp-%ld", ACME_TOKEN_DIR, token, (long)getpid());
    if (n < 0 || (size_t)n >= sizeof(path) || tn < 0 || (size_t)tn >= sizeof(tmp)) return false;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return false;
    size_t len = strlen(response);
    size_t done = 0;
    bool ok = true;
    while (done < len) {
        ssize_t written = write(fd, response + done, len - done);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) { ok = false; break; }
        done += (size_t)written;
    }
    if (close(fd) != 0) ok = false;
    if (ok && rename(tmp, path) == 0) return true;
    unlink(tmp);
    return false;
}

static size_t acme_curl_discard(void *ptr, size_t size, size_t nmemb, void *userdata) {
    (void)ptr;
    (void)userdata;
    return size * nmemb;
}

static bool acme_share_peer(int node_idx, const char *json, size_t json_len) {
    CURL *curl = curl_easy_init();
    if (!curl) return false;
    char token_header[192];
    snprintf(token_header, sizeof(token_header), "X-FlyWire-Token: %s", flywire_token());
    struct curl_slist *headers = curl_slist_append(NULL, token_header);
    headers = curl_slist_append(headers, "Content-Type: application/json");
    struct curl_slist *target = flywire_curl_target(curl, node_idx, "/flywire/acme-challenge");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)json_len);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, acme_curl_discard);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 2L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_slist_free_all(target);
    curl_easy_cleanup(curl);
    return rc == CURLE_OK && status == 200;
}

static void acme_share(const char *token, const char *response) {
    if (!flywire_cluster_configured()) return;
    cJSON *doc = cJSON_CreateObject();
    if (!doc) return;
    cJSON_AddStringToObject(doc, "token", token);
    cJSON_AddStringToObject(doc, "response", response);
    char *json = cJSON_PrintUnformatted(doc);
    cJSON_Delete(doc);
    if (!json) return;
    size_t len = strlen(json);
    for (int i = 0; i < g_flywire.n_nodes; i++) {
        if (i == g_flywire.self_idx) continue;
        if (!acme_share_peer(i, json, len)) {
            CWIST_LOG_WARN("ACME: could not replicate a challenge to %s", g_flywire.nodes[i]);
        }
    }
    cwist_free(json);
}

void handler_flywire_acme_challenge(cwist_http_request *req, cwist_http_response *res) {
    if (!flywire_request_authorized(req)) {
        res->status_code = CWIST_HTTP_UNAUTHORIZED;
        cwist_sstring_assign(res->body, "{\"ok\":false}");
        return;
    }
    cJSON *doc = req->body && req->body->data ? cJSON_Parse(req->body->data) : NULL;
    cJSON *token = cJSON_GetObjectItem(doc, "token");
    cJSON *response = cJSON_GetObjectItem(doc, "response");
    bool ok = cJSON_IsString(token) && cJSON_IsString(response) &&
              acme_write_file(token->valuestring, response->valuestring);
    if (ok) acme_seen_mark(token->valuestring);
    cJSON_Delete(doc);
    res->status_code = ok ? CWIST_HTTP_OK : CWIST_HTTP_BAD_REQUEST;
    cwist_http_header_add(&res->headers, "Content-Type", "application/json");
    cwist_http_header_add(&res->headers, "Cache-Control", "no-store");
    cwist_sstring_assign(res->body, ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

static void acme_send_all(int fd, const char *data, size_t len) {
    size_t done = 0;
    while (done < len) {
        ssize_t n = send(fd, data + done, len - done, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return;
        done += (size_t)n;
    }
}

static void acme_send_response(int fd, int status, const char *body, size_t body_len) {
    const char *label = status == 200 ? "OK" : status == 404 ? "Not Found" : "Bad Request";
    char header[512];
    int n = snprintf(header, sizeof(header),
                     "HTTP/1.1 %d %s\r\nContent-Type: text/plain\r\nContent-Length: %zu\r\n"
                     "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
                     status, label, body_len);
    if (n > 0) acme_send_all(fd, header, (size_t)n);
    if (body && body_len) acme_send_all(fd, body, body_len);
}

static void acme_handle_client(int fd) {
    struct timeval timeout = {.tv_sec = 2, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    char request[2049];
    size_t used = 0;
    while (used < sizeof(request) - 1) {
        ssize_t n = recv(fd, request + used, sizeof(request) - 1 - used, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        used += (size_t)n;
        request[used] = '\0';
        if (strstr(request, "\r\n\r\n")) break;
    }
    request[used] = '\0';
    char method[8], path[512], version[16];
    if (sscanf(request, "%7s %511s %15s", method, path, version) != 3 || strcmp(method, "GET") != 0) {
        acme_send_response(fd, 400, NULL, 0);
        return;
    }
    const char prefix[] = "/.well-known/acme-challenge/";
    if (strncmp(path, prefix, sizeof(prefix) - 1) != 0) {
        acme_send_response(fd, 404, NULL, 0);
        return;
    }
    const char *token = path + sizeof(prefix) - 1;
    const char *query = strchr(token, '?');
    size_t token_len = query ? (size_t)(query - token) : strlen(token);
    char token_buf[ACME_MAX_TOKEN + 1];
    if (token_len == 0 || token_len > ACME_MAX_TOKEN) {
        acme_send_response(fd, 404, NULL, 0);
        return;
    }
    memcpy(token_buf, token, token_len);
    token_buf[token_len] = '\0';
    char response[ACME_MAX_RESPONSE + 1];
    if (!acme_read_file(token_buf, response, sizeof(response))) {
        acme_send_response(fd, 404, NULL, 0);
        return;
    }
    acme_send_response(fd, 200, response, strlen(response));
}

static int acme_listener(void) {
    const char *value = getenv("FLY_CERT_HTTP_PORT");
    int port = value && value[0] ? atoi(value) : 80;
    if (port <= 0 || port > 65535) port = 80;
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(fd, 32) != 0) {
        close(fd);
        return -1;
    }
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    return fd;
}

static void acme_scan_challenges(void) {
    DIR *dir = opendir(ACME_TOKEN_DIR);
    if (!dir) return;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (!acme_token_valid(entry->d_name)) continue;
        char response[ACME_MAX_RESPONSE + 1];
        if (!acme_read_file(entry->d_name, response, sizeof(response)) || acme_seen_mark(entry->d_name)) continue;
        acme_share(entry->d_name, response);
    }
    closedir(dir);
}

static void *acme_worker(void *arg) {
    (void)arg;
    if (!acme_make_dirs()) {
        CWIST_LOG_ERROR("ACME: cannot create the challenge webroot");
        return NULL;
    }
    int fd = acme_listener();
    if (fd < 0) {
        CWIST_LOG_ERROR("ACME: cannot listen on FLY_CERT_HTTP_PORT (default 80)");
        return NULL;
    }
    atomic_store_explicit(&g_acme_listen_fd, fd, memory_order_release);
    while (atomic_load_explicit(&g_acme_running, memory_order_acquire)) {
        struct pollfd pfd = {.fd = fd, .events = POLLIN, .revents = 0};
        int ready = poll(&pfd, 1, 100);
        if (ready < 0 && errno != EINTR) break;
        if (ready > 0 && (pfd.revents & POLLIN)) {
            for (;;) {
                int client = accept4(fd, NULL, NULL, SOCK_CLOEXEC);
                if (client < 0) break;
                acme_handle_client(client);
                close(client);
            }
        }
        acme_scan_challenges();
    }
    atomic_store_explicit(&g_acme_listen_fd, -1, memory_order_release);
    close(fd);
    return NULL;
}

void acme_challenge_start(void) {
    if (g_acme_thread_started || atomic_load_explicit(&g_acme_running, memory_order_relaxed)) return;
    atomic_store_explicit(&g_acme_running, true, memory_order_release);
    if (pthread_create(&g_acme_thread, NULL, acme_worker, NULL) != 0) {
        atomic_store_explicit(&g_acme_running, false, memory_order_release);
        CWIST_LOG_ERROR("ACME: failed to start the challenge worker");
        return;
    }
    g_acme_thread_started = true;
    g_acme_owner = getpid();
}

void acme_challenge_stop(void) {
    if (!g_acme_thread_started) return;
    atomic_store_explicit(&g_acme_running, false, memory_order_release);
    int fd = atomic_load_explicit(&g_acme_listen_fd, memory_order_acquire);
    if (fd >= 0) shutdown(fd, SHUT_RDWR);
}

void acme_challenge_join(void) {
    if (g_acme_thread_started && g_acme_owner == getpid()) pthread_join(g_acme_thread, NULL);
    g_acme_thread_started = false;
}

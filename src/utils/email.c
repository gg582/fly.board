/**
 * @file email.c
 * @brief Minimal blocking SMTP client used for signup email verification.
 *
 * Supports plain SMTP, STARTTLS, and implicit TLS (wrapper) plus optional
 * AUTH LOGIN.  Kept deliberately small: no MIME beyond basic headers.
 */

#define _POSIX_C_SOURCE 200809L
#include "utils/email.h"
#include "config/config.h"
#include <cwist/core/log.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <netdb.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#define SMTP_TIMEOUT_SEC 15
#define SMTP_BUF 2048

bool email_cert_enabled(void) {
    const char *value = getenv("FLY_EMAIL_CERT");
    return value && (strcmp(value, "1") == 0 ||
                     strcasecmp(value, "true") == 0 ||
                     strcasecmp(value, "on") == 0);
}

typedef struct {
    int fd;
    SSL *ssl;
    SSL_CTX *ctx;
} smtp_conn;

static ssize_t smtp_write(smtp_conn *c, const char *buf, size_t len) {
    if (c->ssl) return SSL_write(c->ssl, buf, (int)len);
    return send(c->fd, buf, len, 0);
}

/* Read one reply (handles multi-line "250-..." continuations) and return
 * the 3-digit status code, or -1 on error/timeout. */
static int smtp_reply(smtp_conn *c) {
    char line[SMTP_BUF];
    char code[4] = {0};
    for (;;) {
        size_t off = 0;
        while (off + 1 < sizeof(line)) {
            char ch;
            ssize_t n = c->ssl ? SSL_read(c->ssl, &ch, 1) : recv(c->fd, &ch, 1, 0);
            if (n <= 0) return -1;
            if (ch == '\n') break;
            if (ch != '\r') line[off++] = ch;
        }
        line[off] = '\0';
        if (off < 4) return -1;
        memcpy(code, line, 3);
        if (line[3] != '-') break; /* last line of the reply */
    }
    return atoi(code);
}

static int smtp_cmd(smtp_conn *c, const char *fmt, ...) {
    char buf[SMTP_BUF];
    va_list ap;
    va_start(ap, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (len < 0 || len >= (int)(sizeof(buf) - 2)) return -1;
    buf[len++] = '\r';
    buf[len++] = '\n';
    if (smtp_write(c, buf, (size_t)len) != len) return -1;
    return smtp_reply(c);
}

static void b64_encode(const char *in, char *out, size_t out_size) {
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t len = strlen(in), o = 0;
    for (size_t i = 0; i < len && o + 4 < out_size; i += 3) {
        unsigned v = (unsigned char)in[i] << 16;
        if (i + 1 < len) v |= (unsigned char)in[i + 1] << 8;
        if (i + 2 < len) v |= (unsigned char)in[i + 2];
        out[o++] = tbl[(v >> 18) & 63];
        out[o++] = tbl[(v >> 12) & 63];
        out[o++] = (i + 1 < len) ? tbl[(v >> 6) & 63] : '=';
        out[o++] = (i + 2 < len) ? tbl[v & 63] : '=';
    }
    out[o] = '\0';
}

static bool smtp_start_tls(smtp_conn *c) {
    c->ctx = SSL_CTX_new(TLS_client_method());
    if (!c->ctx) return false;
    c->ssl = SSL_new(c->ctx);
    if (!c->ssl) return false;
    SSL_set_fd(c->ssl, c->fd);
    if (SSL_connect(c->ssl) != 1) {
        SSL_free(c->ssl);
        c->ssl = NULL;
        return false;
    }
    return true;
}

static void smtp_close(smtp_conn *c) {
    if (c->ssl) SSL_free(c->ssl);
    if (c->ctx) SSL_CTX_free(c->ctx);
    if (c->fd >= 0) close(c->fd);
}

static int smtp_connect(const char *host, const char *port) {
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &res) != 0) return -1;
    int fd = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        struct timeval tv = { .tv_sec = SMTP_TIMEOUT_SEC, .tv_usec = 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

bool email_send_from(const char *from_arg, const char *to, const char *subject, const char *body) {
    /* With no FLY_SMTP_* configuration, fall back to a local Postfix on
     * 127.0.0.1:25 so outbound mail (verification, webmail, broadcasts)
     * flows through the site's own MX without extra environment. */
    const char *host = getenv("FLY_SMTP_HOST");
    bool local_fallback = !host || !host[0];
    if (local_fallback) host = "127.0.0.1";
    const char *tls_mode = getenv("FLY_SMTP_TLS");
    bool implicit = !local_fallback && tls_mode && strcasecmp(tls_mode, "implicit") == 0;
    bool starttls = !local_fallback && tls_mode && strcasecmp(tls_mode, "starttls") == 0;
    const char *port = getenv("FLY_SMTP_PORT");
    char port_buf[8];
    if (!port || !port[0]) {
        snprintf(port_buf, sizeof(port_buf), "%d", implicit ? 465 : 25);
        port = port_buf;
    }
    const char *from = from_arg;
    const char *user = getenv("FLY_SMTP_USER");
    const char *pass = getenv("FLY_SMTP_PASS");
    if (!from || !from[0]) from = getenv("FLY_SMTP_FROM");
    if (!from || !from[0]) from = user;
    char from_buf[384] = {0};
    if (!from || !from[0]) {
        /* Default identity: noreply@<site mail domain> (config.c). */
        snprintf(from_buf, sizeof(from_buf), "noreply@%s", fly_mail_domain());
        from = from_buf;
    }

    smtp_conn c = { .fd = smtp_connect(host, port), .ssl = NULL, .ctx = NULL };
    if (c.fd < 0) {
        FLY_LOG_ERROR("Email: cannot connect to SMTP %s:%s", host, port);
        return false;
    }

    bool ok = smtp_reply(&c) == 220;
    if (ok && implicit) {
        ok = smtp_start_tls(&c);
    }
    if (ok) ok = smtp_cmd(&c, "EHLO fly.board") == 250;
    if (ok && starttls) {
        ok = smtp_cmd(&c, "STARTTLS") == 220 && smtp_start_tls(&c);
        if (ok) ok = smtp_cmd(&c, "EHLO fly.board") == 250;
    }
    if (ok && user && user[0] && pass) {
        char b64[512];
        ok = smtp_cmd(&c, "AUTH LOGIN") == 334;
        if (ok) {
            b64_encode(user, b64, sizeof(b64));
            ok = smtp_cmd(&c, "%s", b64) == 334;
        }
        if (ok) {
            b64_encode(pass, b64, sizeof(b64));
            ok = smtp_cmd(&c, "%s", b64) == 235;
        }
    }
    if (ok) ok = smtp_cmd(&c, "MAIL FROM:<%s>", from) == 250;
    if (ok) ok = smtp_cmd(&c, "RCPT TO:<%s>", to) == 250;
    if (ok) ok = smtp_cmd(&c, "DATA") == 354;
    if (ok) {
        /* dot-stuffing is skipped deliberately: verification bodies contain
         * no lines starting with '.'; keep the client minimal. */
        const char *eom = "\r\n.\r\n";
        size_t hdr_len = (size_t)snprintf(NULL, 0,
            "From: %s\r\nTo: %s\r\nSubject: %s\r\nContent-Type: text/plain; charset=utf-8\r\n\r\n",
            from, to, subject);
        char *msg = malloc(hdr_len + strlen(body) + strlen(eom) + 1);
        if (!msg) {
            ok = false;
        } else {
            sprintf(msg,
                "From: %s\r\nTo: %s\r\nSubject: %s\r\nContent-Type: text/plain; charset=utf-8\r\n\r\n",
                from, to, subject);
            strcat(msg, body);
            strcat(msg, eom);
            ok = smtp_write(&c, msg, strlen(msg)) == (ssize_t)strlen(msg) &&
                 smtp_reply(&c) == 250;
            free(msg);
        }
    }
    smtp_cmd(&c, "QUIT");
    smtp_close(&c);

    if (!ok) {
        FLY_LOG_ERROR("Email: SMTP transaction with %s:%s failed for recipient %s", host, port, to);
    }
    return ok;
}

bool email_send(const char *to, const char *subject, const char *body) {
    return email_send_from(NULL, to, subject, body);
}

/* ---- Mailjet sender auto-registration ------------------------------------
 *
 * Outbound webmail is rejected by the relay unless the sender address is
 * allowed on the account. When the site's domain is authenticated with the
 * relay (SPF/DKIM), a sender created UNDER that domain is active immediately
 * - no per-address validation email, no admin involvement. Registration runs
 * detached and best-effort: signup must never fail because of the relay.
 * Requires FLY_SMTP_USER/FLY_SMTP_PASS to hold the relay API key pair.
 */

#include <curl/curl.h>
#include <pthread.h>

#define MJ_API_BASE "https://api.mailjet.com/v3/REST"
#define MJ_TIMEOUT_SEC 10

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} mj_buf;

static size_t mj_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    size_t n = size * nmemb;
    mj_buf *b = (mj_buf *)userdata;
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 1024;
        while (cap < b->len + n + 1) cap *= 2;
        char *nd = realloc(b->data, cap);
        if (!nd) return 0;
        b->data = nd;
        b->cap = cap;
    }
    memcpy(b->data + b->len, ptr, n);
    b->len += n;
    b->data[b->len] = '\0';
    return n;
}

/* One Mailjet REST call. Returns malloc'd response body (may be "") or NULL
 * on transport failure, with the HTTP status in *code. */
static char *mj_api(const char *key, const char *secret,
                    const char *method, const char *path,
                    const char *payload, long *code) {
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;
    char url[512];
    snprintf(url, sizeof(url), "%s%s", MJ_API_BASE, path);
    mj_buf buf = {0};
    struct curl_slist *hdrs = NULL;
    curl_easy_setopt(curl, CURLOPT_URL, url);
    /* Basic auth from the raw key pair (avoid %-encoding surprises). */
    char userpwd[256];
    snprintf(userpwd, sizeof(userpwd), "%s:%s", key, secret);
    curl_easy_setopt(curl, CURLOPT_USERPWD, userpwd);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)MJ_TIMEOUT_SEC);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, mj_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    if (payload) {
        hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload);
    }
    if (strcmp(method, "POST") == 0) curl_easy_setopt(curl, CURLOPT_POST, 1L);
    else if (strcmp(method, "PUT") == 0) curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
    else if (strcmp(method, "DELETE") == 0) curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
    CURLcode rc = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, code);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK) {
        free(buf.data);
        return NULL;
    }
    if (!buf.data) buf.data = strdup("");
    return buf.data;
}

/* Extract the integer field "name" from the first object of a Mailjet
 * collection response; returns def when absent. */
static long mj_first_id(const char *body, const char *name, long def) {
    if (!body) return def;
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\":", name);
    const char *p = strstr(body, pattern);
    if (!p) return def;
    p += strlen(pattern);
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '"') {
        const char *e = strchr(p + 1, '"');
        return e ? atol(p + 1) : def;
    }
    return strtol(p, NULL, 10);
}

static void *mailjet_register_sender_worker(void *arg) {
    char *address = (char *)arg;
    const char *key = getenv("FLY_SMTP_USER");
    const char *secret = getenv("FLY_SMTP_PASS");
    const char *domain = fly_mail_domain();
    if (!key || !secret || !domain) { free(address); return NULL; }

    /* 1. Find the authenticated domain's relay-side ID. */
    char path[512];
    snprintf(path, sizeof(path), "/dns?Domain=%s", domain);
    long code = 0;
    char *body = mj_api(key, secret, "GET", path, NULL, &code);
    long dns_id = (code == 200) ? mj_first_id(body, "ID", -1) : -1;
    free(body);
    if (dns_id < 0) {
        CWIST_LOG_WARN("mailjet: no authenticated relay domain for %s; skipping sender registration for %s",
                       domain, address);
        free(address);
        return NULL;
    }

    /* 2. Look up the sender; create under the domain or relink+revalidate. */
    snprintf(path, sizeof(path), "/sender?Email=%s", address);
    body = mj_api(key, secret, "GET", path, NULL, &code);
    long sender_id = (code == 200) ? mj_first_id(body, "ID", -1) : -1;
    free(body);

    if (sender_id < 0) {
        char payload[512];
        snprintf(payload, sizeof(payload), "{\"Email\":\"%s\",\"DNSID\":%ld}", address, dns_id);
        body = mj_api(key, secret, "POST", "/sender", payload, &code);
        if (body && code >= 200 && code < 300) {
            CWIST_LOG_INFO("mailjet: registered sender %s under authenticated domain", address);
        } else if (body && strstr(body, "already existing")) {
            snprintf(path, sizeof(path), "/sender?Email=%s", address);
            char *b2 = mj_api(key, secret, "GET", path, NULL, &code);
            sender_id = (code == 200) ? mj_first_id(b2, "ID", -1) : -1;
            free(b2);
        } else {
            CWIST_LOG_WARN("mailjet: sender registration for %s failed (http %ld)", address, code);
        }
        free(body);
    }

    if (sender_id >= 0) {
        char payload[128];
        snprintf(payload, sizeof(payload), "{\"DNSID\":%ld}", dns_id);
        snprintf(path, sizeof(path), "/sender/%ld", sender_id);
        body = mj_api(key, secret, "PUT", path, payload, &code);
        free(body);
        snprintf(path, sizeof(path), "/sender/%ld/validate", sender_id);
        body = mj_api(key, secret, "POST", path, NULL, &code);
        if (body && (code == 200 || strstr(body, "already active")))
            CWIST_LOG_INFO("mailjet: sender %s active", address);
        free(body);
    }
    free(address);
    return NULL;
}

void email_register_local_sender(const char *username) {
    const char *key = getenv("FLY_SMTP_USER");
    const char *secret = getenv("FLY_SMTP_PASS");
    if (!key || !key[0] || !secret || !secret[0]) return;
    if (!username || !username[0] || strlen(username) > 64) return;
    const char *domain = fly_mail_domain();
    if (!domain || !domain[0] || strchr(domain, '@')) return;
    size_t len = strlen(username) + strlen(domain) + 2;
    char *address = malloc(len);
    if (!address) return;
    snprintf(address, len, "%s@%s", username, domain);
    pthread_t thread;
    if (pthread_create(&thread, NULL, mailjet_register_sender_worker, address) != 0) {
        free(address);
        return;
    }
    pthread_detach(thread);
}

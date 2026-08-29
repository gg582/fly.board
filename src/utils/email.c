/**
 * @file email.c
 * @brief Minimal blocking SMTP client used for signup email verification.
 *
 * Supports plain SMTP, STARTTLS, and implicit TLS (wrapper) plus optional
 * AUTH LOGIN.  Kept deliberately small: no MIME beyond basic headers.
 */

#define _POSIX_C_SOURCE 200809L
#include "utils/email.h"
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

bool email_send(const char *to, const char *subject, const char *body) {
    const char *host = getenv("FLY_SMTP_HOST");
    if (!host || !host[0]) {
        FLY_LOG_ERROR("Email: FLY_SMTP_HOST not set; cannot send verification mail");
        return false;
    }
    const char *tls_mode = getenv("FLY_SMTP_TLS");
    bool implicit = tls_mode && strcasecmp(tls_mode, "implicit") == 0;
    bool starttls = tls_mode && strcasecmp(tls_mode, "starttls") == 0;
    const char *port = getenv("FLY_SMTP_PORT");
    char port_buf[8];
    if (!port || !port[0]) {
        snprintf(port_buf, sizeof(port_buf), "%d", implicit ? 465 : 25);
        port = port_buf;
    }
    const char *from = getenv("FLY_SMTP_FROM");
    const char *user = getenv("FLY_SMTP_USER");
    const char *pass = getenv("FLY_SMTP_PASS");
    if (!from || !from[0]) from = user;
    if (!from || !from[0]) {
        FLY_LOG_ERROR("Email: FLY_SMTP_FROM (or FLY_SMTP_USER) not set");
        return false;
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

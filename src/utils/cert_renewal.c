/**
 * @file cert_renewal.c
 * @brief Daily TLS certificate expiry watchdog with lego-based renewal.
 *
 * Enabled by FLY_CERT_RENEWAL=true in the environment.  The worker inspects
 * server.crt once a day (plus once at startup) and renews it through a local
 * ACME client (lego by default) when it is close to expiry, then hot-reloads
 * the certificate into the running SSL_CTX so no restart is required.
 *
 * Temporary self-signed certificates produced by keygen.sh (CN=localhost)
 * are detected and never touched.
 */

#define _POSIX_C_SOURCE 200809L
#include "utils/cert_renewal.h"
#include "config/config.h"
#include <cwist/core/log.h>
#include <openssl/x509.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <cwist/net/http/http3.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <errno.h>

#define CERT_FILE "server.crt"
#define KEY_FILE  "server.key"
#define LEGO_STATE_DIR ".lego"

static _Atomic bool g_renew_running = false;
static int g_renew_wake_fd = -1;
static pthread_t g_renew_thread;
static bool g_renew_thread_started = false;
/* cwist_app_listen() forks worker processes; the thread exists only in the
 * process that created it, so children must not join the inherited handle. */
static pid_t g_renew_thread_owner = 0;
static cwist_app *g_renew_app = NULL;
static pthread_mutex_t g_renew_lock = PTHREAD_MUTEX_INITIALIZER;

static bool cert_renewal_enabled(void) {
    const char *value = getenv("FLY_CERT_RENEWAL");
    return value && (strcmp(value, "1") == 0 ||
                     strcasecmp(value, "true") == 0 ||
                     strcasecmp(value, "on") == 0);
}

static X509 *cert_load(const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) return NULL;
    X509 *cert = PEM_read_X509(fp, NULL, NULL, NULL);
    fclose(fp);
    return cert;
}

/* keygen.sh issues a self-signed cert with CN=localhost.  Treat any
 * self-signed cert, or any cert whose subject CN is "localhost", as a
 * temporary development certificate that must not be fed to an ACME client. */
static bool cert_is_temporary(const X509 *cert) {
    X509_NAME *subject = X509_get_subject_name(cert);
    X509_NAME *issuer = X509_get_issuer_name(cert);
    if (subject && issuer && X509_NAME_cmp(subject, issuer) == 0) return true;

    if (subject) {
        char cn[256];
        if (X509_NAME_get_text_by_NID(subject, NID_commonName, cn, sizeof(cn)) > 0 &&
            strcmp(cn, "localhost") == 0) {
            return true;
        }
    }
    return false;
}

static int cert_days_remaining(const X509 *cert) {
    const ASN1_TIME *not_after = X509_get0_notAfter(cert);
    if (!not_after) return -1;
    int days = 0, secs = 0;
    if (!ASN1_TIME_diff(&days, &secs, NULL, not_after)) return -1;
    if (secs < 0) days--;
    return days;
}

/* Derive the bare hostname from the configured root_url
 * (e.g. "https://blog.example.com:8888/" -> "blog.example.com"). */
static bool renewal_host(char *out, size_t out_size) {
    const char *url = g_config.root_url;
    const char *start = strstr(url, "://");
    start = start ? start + 3 : url;
    const char *end = start;
    while (*end && *end != ':' && *end != '/' && *end != '?') end++;
    size_t len = (size_t)(end - start);
    if (len == 0 || len >= out_size) return false;
    memcpy(out, start, len);
    out[len] = '\0';
    if (strcmp(out, "localhost") == 0) return false;
    return true;
}

static int lego_run(const char *host, const char *email, const char *days,
                    const char *command) {
    const char *lego = getenv("FLY_CERT_LEGO_BIN");
    if (!lego || !lego[0]) lego = "lego";

    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execlp(lego, lego,
               "--accept-tos",
               "--email", email,
               "--domains", host,
               "--http",
               "--path", LEGO_STATE_DIR,
               command,
               "--days", days,
               (char *)NULL);
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
}

/* lego stores issued material at <path>/certificates/<host>.{crt,key}.
 * Install via tmp+rename so a crash mid-copy cannot truncate the live cert. */
static bool cert_install(const char *host) {
    char src_crt[512], src_key[512];
    char tmp_crt[520], tmp_key[520];
    snprintf(src_crt, sizeof(src_crt), LEGO_STATE_DIR "/certificates/%s.crt", host);
    snprintf(src_key, sizeof(src_key), LEGO_STATE_DIR "/certificates/%s.key", host);
    snprintf(tmp_crt, sizeof(tmp_crt), "%s.tmp", CERT_FILE);
    snprintf(tmp_key, sizeof(tmp_key), "%s.tmp", KEY_FILE);

    char buf[8192];
    const char *pairs[][2] = {
        { src_crt, tmp_crt },
        { src_key, tmp_key },
    };
    for (size_t i = 0; i < sizeof(pairs) / sizeof(pairs[0]); i++) {
        FILE *in = fopen(pairs[i][0], "r");
        if (!in) {
            FLY_LOG_ERROR("Cert renewal: cannot open %s", pairs[i][0]);
            return false;
        }
        FILE *out = fopen(pairs[i][1], "w");
        if (!out) {
            fclose(in);
            FLY_LOG_ERROR("Cert renewal: cannot write %s", pairs[i][1]);
            return false;
        }
        size_t n;
        bool ok = true;
        while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
            if (fwrite(buf, 1, n, out) != n) { ok = false; break; }
        }
        if (ferror(in)) ok = false;
        fclose(in);
        if (fclose(out) != 0) ok = false;
        if (!ok) {
            FLY_LOG_ERROR("Cert renewal: copy of %s failed", pairs[i][0]);
            return false;
        }
    }
    if (rename(tmp_crt, CERT_FILE) != 0 || rename(tmp_key, KEY_FILE) != 0) {
        FLY_LOG_ERROR("Cert renewal: failed to install renewed certificate");
        return false;
    }
    return true;
}

/* Hot-reload cert/key into the live contexts; new handshakes pick up the
 * new certificate without dropping existing connections. */
static bool cert_reload_ctx(SSL_CTX *ctx) {
    if (!ctx) return false;
    if (SSL_CTX_use_certificate_chain_file(ctx, CERT_FILE) != 1 ||
        SSL_CTX_use_PrivateKey_file(ctx, KEY_FILE, SSL_FILETYPE_PEM) != 1 ||
        SSL_CTX_check_private_key(ctx) != 1) {
        return false;
    }
    return true;
}

static void cert_reload_running_server(void) {
    cwist_app *app = g_renew_app;
    if (!app) return;
    bool ok = true;
    if (app->ssl_ctx && app->ssl_ctx->ctx) {
        if (!cert_reload_ctx(app->ssl_ctx->ctx)) {
            FLY_LOG_ERROR("Cert renewal: HTTPS context reload failed");
            ok = false;
        }
    }
    if (app->h3_ctx && app->h3_ctx->ssl_ctx) {
        if (!cert_reload_ctx(app->h3_ctx->ssl_ctx)) {
            FLY_LOG_ERROR("Cert renewal: HTTP/3 context reload failed");
            ok = false;
        }
    }
    if (ok) CWIST_LOG_INFO("Cert renewal: running server now uses the renewed certificate");
}

static void cert_renewal_check(void) {
    pthread_mutex_lock(&g_renew_lock);

    X509 *cert = cert_load(CERT_FILE);
    if (!cert) {
        FLY_LOG_ERROR("Cert renewal: cannot read %s; skipping check", CERT_FILE);
        pthread_mutex_unlock(&g_renew_lock);
        return;
    }
    if (cert_is_temporary(cert)) {
        CWIST_LOG_INFO("Cert renewal: temporary self-signed certificate detected; skipping");
        X509_free(cert);
        pthread_mutex_unlock(&g_renew_lock);
        return;
    }

    char host[256];
    if (!renewal_host(host, sizeof(host))) {
        FLY_LOG_ERROR("Cert renewal: cannot derive hostname from root_url='%s'", g_config.root_url);
        X509_free(cert);
        pthread_mutex_unlock(&g_renew_lock);
        return;
    }

    const char *days_env = getenv("FLY_CERT_DAYS");
    const char *days = (days_env && days_env[0]) ? days_env : "30";
    int remaining = cert_days_remaining(cert);
    X509_free(cert);
    int threshold = atoi(days);
    if (threshold <= 0) threshold = 30;
    if (remaining < 0 || remaining > threshold) {
        CWIST_LOG_INFO("Cert renewal: certificate valid for %d more day(s); no action", remaining);
        pthread_mutex_unlock(&g_renew_lock);
        return;
    }

    CWIST_LOG_INFO("Cert renewal: %d day(s) left; running ACME renewal for %s", remaining, host);
    char email[320];
    const char *email_env = getenv("FLY_CERT_EMAIL");
    if (email_env && email_env[0]) {
        snprintf(email, sizeof(email), "%s", email_env);
    } else {
        snprintf(email, sizeof(email), "admin@%s", host);
    }

    int rc = lego_run(host, email, days, "renew");
    if (rc != 0) {
        /* First run on this machine: no account/lineage yet, so issue fresh. */
        CWIST_LOG_INFO("Cert renewal: lego renew exited %d; attempting initial issuance", rc);
        rc = lego_run(host, email, days, "run");
    }
    if (rc != 0) {
        FLY_LOG_ERROR("Cert renewal: ACME client failed (exit %d); keeping current certificate", rc);
        pthread_mutex_unlock(&g_renew_lock);
        return;
    }

    if (!cert_install(host)) {
        pthread_mutex_unlock(&g_renew_lock);
        return;
    }
    cert_reload_running_server();
    pthread_mutex_unlock(&g_renew_lock);
}

static int create_daily_timer(void) {
    int fd = timerfd_create(CLOCK_REALTIME, 0);
    if (fd < 0) return -1;
    struct itimerspec its;
    its.it_value.tv_sec = 24 * 3600;
    its.it_value.tv_nsec = 0;
    its.it_interval.tv_sec = 24 * 3600;
    its.it_interval.tv_nsec = 0;
    if (timerfd_settime(fd, 0, &its, NULL) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void *cert_renewal_worker(void *arg) {
    (void)arg;
    int tfd = create_daily_timer();
    if (tfd < 0) {
        FLY_LOG_ERROR("Cert renewal: failed to create timerfd");
        return NULL;
    }
    int epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0) {
        FLY_LOG_ERROR("Cert renewal: failed to create epoll instance");
        close(tfd);
        return NULL;
    }
    struct epoll_event event = { .events = EPOLLIN };
    event.data.fd = tfd;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, tfd, &event) < 0) {
        close(epfd);
        close(tfd);
        return NULL;
    }
    event.data.fd = g_renew_wake_fd;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, g_renew_wake_fd, &event) < 0) {
        close(epfd);
        close(tfd);
        return NULL;
    }

    /* Check right away so a cert already near expiry is handled at boot. */
    cert_renewal_check();

    uint64_t exp;
    while (atomic_load_explicit(&g_renew_running, memory_order_acquire)) {
        struct epoll_event ready_event;
        int ready = epoll_wait(epfd, &ready_event, 1, -1);
        if (ready < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (ready_event.data.fd == g_renew_wake_fd) break;
        if (ready_event.data.fd != tfd) continue;
        ssize_t s = read(tfd, &exp, sizeof(exp));
        if (s != sizeof(exp)) continue;
        cert_renewal_check();
    }
    close(epfd);
    close(tfd);
    return NULL;
}

void cert_renewal_start(cwist_app *app) {
    if (!cert_renewal_enabled()) {
        CWIST_LOG_INFO("Cert renewal disabled; set FLY_CERT_RENEWAL=true to enable");
        return;
    }
    g_renew_app = app;
    g_renew_wake_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (g_renew_wake_fd < 0) {
        FLY_LOG_ERROR("Cert renewal: failed to create wake event");
        return;
    }
    atomic_store_explicit(&g_renew_running, true, memory_order_release);
    if (pthread_create(&g_renew_thread, NULL, cert_renewal_worker, NULL) != 0) {
        atomic_store_explicit(&g_renew_running, false, memory_order_release);
        close(g_renew_wake_fd);
        g_renew_wake_fd = -1;
        FLY_LOG_ERROR("Cert renewal: failed to start worker");
        return;
    }
    g_renew_thread_started = true;
    g_renew_thread_owner = getpid();
    CWIST_LOG_INFO("Cert renewal watchdog started");
}

void cert_renewal_stop(void) {
    if (g_renew_wake_fd < 0) return;
    atomic_store_explicit(&g_renew_running, false, memory_order_release);
    uint64_t one = 1;
    (void)write(g_renew_wake_fd, &one, sizeof(one));
}

void cert_renewal_join(void) {
    if (g_renew_thread_started && g_renew_thread_owner == getpid()) {
        pthread_join(g_renew_thread, NULL);
    }
    g_renew_thread_started = false;
    if (g_renew_wake_fd >= 0) {
        close(g_renew_wake_fd);
        g_renew_wake_fd = -1;
    }
}

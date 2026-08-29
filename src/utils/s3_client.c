/**
 * @file s3_client.c
 * @brief Minimal S3-compatible object storage client (Signature V4).
 *
 * Supports exactly what the file storage layer needs: PUT an object from a
 * local file, DELETE an object, and presign a GET URL.  Uses libcurl for
 * transport (already linked for the translation API) and OpenSSL HMAC for
 * SigV4 signing.  Payload hashing is "UNSIGNED-PAYLOAD", valid over HTTPS.
 */

#define _POSIX_C_SOURCE 200809L
#include "utils/s3_client.h"
#include "utils/utils.h"
#include "config/config.h"
#include <cwist/core/log.h>
#include <curl/curl.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define S3_SIGN_BUF 4096

bool s3_path_is(const char *path) {
    return path && strncmp(path, S3_PATH_PREFIX, sizeof(S3_PATH_PREFIX) - 1) == 0;
}

const char *s3_path_key(const char *path) {
    return s3_path_is(path) ? path + sizeof(S3_PATH_PREFIX) - 1 : NULL;
}

/* RFC 3986 unreserved set is left as-is; '/' survives when encoding keys. */
static void uri_encode(const char *in, char *out, size_t out_size, bool keep_slash) {
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 4 < out_size; p++) {
        unsigned char c = *p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~' || (keep_slash && c == '/')) {
            out[o++] = (char)c;
        } else {
            o += (size_t)snprintf(out + o, out_size - o, "%%%02X", c);
        }
    }
    out[o] = '\0';
}

static void hex_encode(const unsigned char *data, size_t len, char *out) {
    for (size_t i = 0; i < len; i++) snprintf(out + (i * 2), 3, "%02x", data[i]);
    out[len * 2] = '\0';
}

static void hmac_sha256(const void *key, size_t key_len, const char *data, unsigned char out[32]) {
    unsigned int out_len = 32;
    HMAC(EVP_sha256(), key, (int)key_len, (const unsigned char *)data, strlen(data), out, &out_len);
}

/* endpoint "https://host[:port]" -> scheme/host split; returns false on junk. */
static bool endpoint_split(const char *endpoint, char *scheme, size_t scheme_size,
                           char *host, size_t host_size) {
    const char *rest = strstr(endpoint, "://");
    if (!rest) return false;
    size_t scheme_len = (size_t)(rest - endpoint);
    if (scheme_len == 0 || scheme_len >= scheme_size) return false;
    memcpy(scheme, endpoint, scheme_len);
    scheme[scheme_len] = '\0';
    rest += 3;
    size_t host_len = strcspn(rest, "/");
    if (host_len == 0 || host_len >= host_size) return false;
    memcpy(host, rest, host_len);
    host[host_len] = '\0';
    return true;
}

/* Full object URL path component: "/bucket/<key>" (path style) or "/<key>"
 * (virtual-host style, bucket moves into the host).  The key already
 * carries the configured prefix. */
static void build_host_and_uri(const char *key, char *host, size_t host_size,
                               char *uri, size_t uri_size) {
    const s3_config_t *c = &g_s3_config;
    char ep_scheme[16], ep_host[256];
    if (!endpoint_split(c->endpoint, ep_scheme, sizeof(ep_scheme), ep_host, sizeof(ep_host))) {
        ep_host[0] = '\0';
    }
    char enc_key[1024];
    uri_encode(key, enc_key, sizeof(enc_key), true);
    if (c->use_path_style) {
        snprintf(host, host_size, "%s", ep_host);
        snprintf(uri, uri_size, "/%s/%s", c->bucket, enc_key);
    } else {
        snprintf(host, host_size, "%s.%s", c->bucket, ep_host);
        snprintf(uri, uri_size, "/%s", enc_key);
    }
}

static void build_url(const char *key, char *url, size_t url_size) {
    char scheme[16], ep_host[256], host[320], uri[1280];
    if (!endpoint_split(g_s3_config.endpoint, scheme, sizeof(scheme), ep_host, sizeof(ep_host))) return;
    build_host_and_uri(key, host, sizeof(host), uri, sizeof(uri));
    snprintf(url, url_size, "%s://%s%s", scheme, host, uri);
}

/* Derive the SigV4 signing key: HMAC chain date -> region -> service -> "aws4_request". */
static void signing_key(const char *date, unsigned char out[32]) {
    char secret[512];
    snprintf(secret, sizeof(secret), "AWS4%s", g_s3_config.secret_key);
    const char *region = g_s3_config.region[0] ? g_s3_config.region : "us-east-1";
    unsigned char k[32];
    hmac_sha256(secret, strlen(secret), date, k);
    hmac_sha256(k, 32, region, k);
    hmac_sha256(k, 32, "s3", k);
    hmac_sha256(k, 32, "aws4_request", out);
}

static void current_amz_dates(char *amz_date, char *date) {
    time_t now = time(NULL);
    struct tm tm_utc;
    gmtime_r(&now, &tm_utc);
    strftime(amz_date, 17, "%Y%m%dT%H%M%SZ", &tm_utc);
    strftime(date, 9, "%Y%m%d", &tm_utc);
}

static void credential_scope(const char *date, char *out, size_t out_size) {
    snprintf(out, out_size, "%s/%s/s3/aws4_request", date,
             g_s3_config.region[0] ? g_s3_config.region : "us-east-1");
}

static bool sign_and_finish(const char *method, const char *uri, const char *query,
                            const char *signed_headers, const char *canonical_headers,
                            const char *payload_hash, const char *amz_date, const char *date,
                            char *out_signature, size_t out_sig_size) {
    char canonical[S3_SIGN_BUF];
    snprintf(canonical, sizeof(canonical), "%s\n%s\n%s\n%s\n%s\n%s",
             method, uri, query, canonical_headers, signed_headers, payload_hash);
    unsigned char canonical_hash[32];
    SHA256((const unsigned char *)canonical, strlen(canonical), canonical_hash);
    char canonical_hex[65];
    hex_encode(canonical_hash, 32, canonical_hex);

    char scope[128];
    credential_scope(date, scope, sizeof(scope));
    char to_sign[512];
    snprintf(to_sign, sizeof(to_sign), "AWS4-HMAC-SHA256\n%s\n%s\n%s", amz_date, scope, canonical_hex);

    unsigned char key[32], sig[32];
    signing_key(date, key);
    hmac_sha256(key, 32, to_sign, sig);
    hex_encode(sig, 32, out_signature);
    (void)out_sig_size;
    return true;
}

struct s3_put_ctx {
    FILE *fp;
};

static size_t s3_put_read(char *buf, size_t size, size_t nitems, void *userdata) {
    struct s3_put_ctx *ctx = userdata;
    return fread(buf, size, nitems, ctx->fp);
}

bool s3_upload_file(const char *local_path, const char *key, const char *content_type) {
    if (!s3_config_enabled()) return false;
    FILE *fp = fopen(local_path, "rb");
    if (!fp) {
        FLY_LOG_ERROR("S3 upload: cannot open %s", local_path);
        return false;
    }
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (size < 0) {
        fclose(fp);
        return false;
    }

    char amz_date[17], date[9], host[320], uri[1280], url[2048];
    current_amz_dates(amz_date, date);
    build_host_and_uri(key, host, sizeof(host), uri, sizeof(uri));
    build_url(key, url, sizeof(url));

    char canonical_headers[640];
    snprintf(canonical_headers, sizeof(canonical_headers),
             "host:%s\nx-amz-content-sha256:UNSIGNED-PAYLOAD\nx-amz-date:%s\n", host, amz_date);
    char signature[65];
    sign_and_finish("PUT", uri, "", "host;x-amz-content-sha256;x-amz-date",
                    canonical_headers, "UNSIGNED-PAYLOAD", amz_date, date, signature, sizeof(signature));

    char scope[128], auth[640];
    credential_scope(date, scope, sizeof(scope));
    snprintf(auth, sizeof(auth),
             "AWS4-HMAC-SHA256 Credential=%s/%s, SignedHeaders=host;x-amz-content-sha256;x-amz-date, Signature=%s",
             g_s3_config.access_key, scope, signature);

    char hdr_date[64], hdr_hash[96], hdr_type[320];
    snprintf(hdr_date, sizeof(hdr_date), "x-amz-date: %s", amz_date);
    snprintf(hdr_hash, sizeof(hdr_hash), "x-amz-content-sha256: UNSIGNED-PAYLOAD");
    snprintf(hdr_type, sizeof(hdr_type), "Content-Type: %s",
             (content_type && content_type[0]) ? content_type : "application/octet-stream");

    CURL *curl = curl_easy_init();
    if (!curl) {
        fclose(fp);
        return false;
    }
    struct s3_put_ctx ctx = { .fp = fp };
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, hdr_date);
    headers = curl_slist_append(headers, hdr_hash);
    headers = curl_slist_append(headers, hdr_type);
    char hdr_auth[768];
    snprintf(hdr_auth, sizeof(hdr_auth), "Authorization: %s", auth);
    headers = curl_slist_append(headers, hdr_auth);

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(curl, CURLOPT_READFUNCTION, s3_put_read);
    curl_easy_setopt(curl, CURLOPT_READDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, (curl_off_t)size);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);

    CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    fclose(fp);

    if (rc != CURLE_OK || status < 200 || status >= 300) {
        FLY_LOG_ERROR("S3 upload failed: key=%s curl=%d status=%ld", key, (int)rc, status);
        return false;
    }
    CWIST_LOG_INFO("S3 upload: key=%s size=%ld", key, size);
    return true;
}

bool s3_delete_object(const char *key) {
    if (!s3_config_enabled()) return false;

    char amz_date[17], date[9], host[320], uri[1280], url[2048];
    current_amz_dates(amz_date, date);
    build_host_and_uri(key, host, sizeof(host), uri, sizeof(uri));
    build_url(key, url, sizeof(url));

    char canonical_headers[640];
    snprintf(canonical_headers, sizeof(canonical_headers),
             "host:%s\nx-amz-content-sha256:UNSIGNED-PAYLOAD\nx-amz-date:%s\n", host, amz_date);
    char signature[65];
    sign_and_finish("DELETE", uri, "", "host;x-amz-content-sha256;x-amz-date",
                    canonical_headers, "UNSIGNED-PAYLOAD", amz_date, date, signature, sizeof(signature));

    char scope[128], hdr_auth[768], hdr_date[64];
    credential_scope(date, scope, sizeof(scope));
    snprintf(hdr_auth, sizeof(hdr_auth),
             "Authorization: AWS4-HMAC-SHA256 Credential=%s/%s, SignedHeaders=host;x-amz-content-sha256;x-amz-date, Signature=%s",
             g_s3_config.access_key, scope, signature);
    snprintf(hdr_date, sizeof(hdr_date), "x-amz-date: %s", amz_date);

    CURL *curl = curl_easy_init();
    if (!curl) return false;
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, hdr_date);
    headers = curl_slist_append(headers, "x-amz-content-sha256: UNSIGNED-PAYLOAD");
    headers = curl_slist_append(headers, hdr_auth);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    /* 204 No Content on success; 404 means it is already gone. */
    if (rc != CURLE_OK || (status != 204 && status != 200 && status != 404)) {
        FLY_LOG_ERROR("S3 delete failed: key=%s curl=%d status=%ld", key, (int)rc, status);
        return false;
    }
    CWIST_LOG_INFO("S3 delete: key=%s", key);
    return true;
}

bool s3_presign_get(const char *key, char *out, size_t out_size, int expires_sec) {
    if (!s3_config_enabled() || expires_sec <= 0) return false;

    char amz_date[17], date[9], host[320], uri[1280];
    current_amz_dates(amz_date, date);
    build_host_and_uri(key, host, sizeof(host), uri, sizeof(uri));

    char scope[128], enc_cred[512];
    credential_scope(date, scope, sizeof(scope));
    char cred[384];
    snprintf(cred, sizeof(cred), "%s/%s", g_s3_config.access_key, scope);
    uri_encode(cred, enc_cred, sizeof(enc_cred), false);

    char query[1024];
    snprintf(query, sizeof(query),
             "X-Amz-Algorithm=AWS4-HMAC-SHA256&X-Amz-Credential=%s&X-Amz-Date=%s&X-Amz-Expires=%d&X-Amz-SignedHeaders=host",
             enc_cred, amz_date, expires_sec);

    char canonical_headers[384];
    snprintf(canonical_headers, sizeof(canonical_headers), "host:%s\n", host);
    char signature[65];
    sign_and_finish("GET", uri, query, "host", canonical_headers,
                    "UNSIGNED-PAYLOAD", amz_date, date, signature, sizeof(signature));

    char scheme[16], ep_host[256];
    if (!endpoint_split(g_s3_config.endpoint, scheme, sizeof(scheme), ep_host, sizeof(ep_host))) return false;
    int n = snprintf(out, out_size, "%s://%s%s?%s&X-Amz-Signature=%s",
                     scheme, host, uri, query, signature);
    return n > 0 && (size_t)n < out_size;
}

/* Push a freshly uploaded local file to the bucket.  The object key is
 * "<prefix><basename>".  On success writes the "s3://<key>" marker into
 * out_marker (when provided) and returns true. */
bool s3_store_upload(const char *local_path, const char *content_type,
                     char *out_marker, size_t out_marker_size) {
    if (!s3_config_enabled() || !local_path) return false;
    const char *base = strrchr(local_path, '/');
    base = base ? base + 1 : local_path;
    if (!base[0]) return false;
    char key[768];
    snprintf(key, sizeof(key), "%s%s", g_s3_config.prefix, base);
    if (!s3_upload_file(local_path, key, content_type)) return false;
    if (out_marker && out_marker_size) {
        snprintf(out_marker, out_marker_size, S3_PATH_PREFIX "%s", key);
    }
    return true;
}

void storage_delete_file(const char *path) {
    if (!path || !path[0]) return;
    const char *key = s3_path_key(path);
    if (key) {
        s3_delete_object(key);
        return;
    }
    if (is_safe_public_path(path)) unlink(path);
    else CWIST_LOG_WARN("Refusing to delete unsafe file path: %s", path);
    /* Mirror mode leaves the DB path local, so the mirrored object has to be
     * removed too.  DELETE is idempotent, so a missing object is harmless. */
    if (s3_config_enabled()) {
        const char *base = strrchr(path, '/');
        base = base ? base + 1 : path;
        if (base[0]) {
            char mkey[768];
            snprintf(mkey, sizeof(mkey), "%s%s", g_s3_config.prefix, base);
            s3_delete_object(mkey);
        }
    }
}

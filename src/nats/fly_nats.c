#define _GNU_SOURCE
#include "fly_nats.h"
#include "../crypto/fly_crypto.h"
#if defined __has_include
#  if __has_include (<cwist/net/nats/cwist_nats.h>)
#    define HAVE_NATS 1
#    include <cwist/net/nats/cwist_nats.h>
#  endif
#endif
#include <cwist/core/mem/alloc.h>
#include <cwist/core/log.h>
#include <cjson/cJSON.h>
#include <cwist/core/mem/alloc.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef HAVE_NATS
static cwist_nats_t *g_nats = NULL;

static void on_post_msg(const char *subject, const char *data, size_t len, void *ctx) {
    (void)subject; (void)ctx;
    cJSON *obj = cJSON_ParseWithLength(data, len);
    const char *slug = NULL;
    bool has_sig = false;
    if (obj) {
        cJSON *slug_item = cJSON_GetObjectItemCaseSensitive(obj, "slug");
        cJSON *sig_item = cJSON_GetObjectItemCaseSensitive(obj, "sig_b64");
        if (cJSON_IsString(slug_item) && slug_item->valuestring) slug = slug_item->valuestring;
        has_sig = cJSON_IsString(sig_item) && sig_item->valuestring && sig_item->valuestring[0] != '\0';
    }
    FLY_LOG_DEBUG("[fly_nats] received post broadcast slug=%s sig=%s", slug ? slug : "(unknown)", has_sig ? "present" : "missing");
    if (obj) cJSON_Delete(obj);
}

static char *build_signed_post_payload(const char *title, const char *slug, const char *summary) {
    char *sig_b64 = NULL;
    char *pubkey_b64 = NULL;
    char *signed_json = NULL;
    cJSON *canonical = cJSON_CreateObject();
    if (!canonical) return NULL;

    cJSON_AddStringToObject(canonical, "title", title ? title : "");
    cJSON_AddStringToObject(canonical, "slug", slug ? slug : "");
    cJSON_AddStringToObject(canonical, "summary", summary ? summary : "");

    char *canonical_json = cJSON_PrintUnformatted(canonical);
    cJSON_Delete(canonical);
    if (!canonical_json) return NULL;

    if (!fly_crypto_sign((const uint8_t *)canonical_json, strlen(canonical_json), &sig_b64)) {
        free(canonical_json);
        return NULL;
    }
    if (!fly_crypto_pubkey_export(&pubkey_b64)) {
        free(canonical_json);
        cwist_free(sig_b64);
        return NULL;
    }

    cJSON *envelope = cJSON_Parse(canonical_json);
    free(canonical_json);
    if (!envelope) {
        cwist_free(sig_b64);
        cwist_free(pubkey_b64);
        return NULL;
    }
    cJSON_AddStringToObject(envelope, "sig_alg", "ML-DSA-65");
    cJSON_AddStringToObject(envelope, "sig_b64", sig_b64);
    cJSON_AddStringToObject(envelope, "pubkey_b64", pubkey_b64);
    signed_json = cJSON_PrintUnformatted(envelope);
    cJSON_Delete(envelope);
    cwist_free(sig_b64);
    cwist_free(pubkey_b64);
    return signed_json;
}
#endif

bool fly_nats_init(const char *url) {
#ifdef HAVE_NATS
    if (g_nats) return true;
    if (!url) url = "nats://localhost:4222";
    cwist_error_t err = cwist_nats_connect(&g_nats, url);
    if (err.errtype != CWIST_ERR_INT16 || err.error.err_i16 != 0) {
        FLY_LOG_ERROR("[fly_nats] connect failed");
        return false;
    }
    err = cwist_nats_subscribe(g_nats, "flyboard.posts", on_post_msg, NULL);
    if (err.errtype != CWIST_ERR_INT16 || err.error.err_i16 != 0) {
        FLY_LOG_ERROR("[fly_nats] subscribe failed");
        cwist_nats_destroy(g_nats);
        g_nats = NULL;
        return false;
    }
    FLY_LOG_DEBUG("[fly_nats] connected to %s", url);
    return true;
#else
    (void)url;
    return true;
#endif
}

bool fly_nats_publish_post(const char *title, const char *slug, const char *summary) {
#ifdef HAVE_NATS
    if (!g_nats) return false;
    char *json = build_signed_post_payload(title, slug, summary);
    if (!json) return false;
    cwist_error_t err = cwist_nats_publish_string(g_nats, "flyboard.posts", json);
    free(json);
    return err.errtype == CWIST_ERR_INT16 && err.error.err_i16 == 0;
#else
    (void)title; (void)slug; (void)summary;
    return true;
#endif
}

void fly_nats_dispatch(void) {
#ifdef HAVE_NATS
    if (g_nats) cwist_nats_dispatch(g_nats);
#endif
}

void fly_nats_close(void) {
#ifdef HAVE_NATS
    if (g_nats) {
        cwist_nats_destroy(g_nats);
        g_nats = NULL;
    }
#endif
}

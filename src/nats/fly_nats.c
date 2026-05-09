#define _GNU_SOURCE
#include "fly_nats.h"
#if defined __has_include
#  if __has_include (<cwist/net/nats/cwist_nats.h>)
#    define HAVE_NATS 1
#    include <cwist/net/nats/cwist_nats.h>
#  endif
#endif
#include <cwist/core/mem/alloc.h>
#include <cwist/core/log.h>
#include <cjson/cJSON.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef HAVE_NATS
static cwist_nats_t *g_nats = NULL;

static void on_post_msg(const char *subject, const char *data, size_t len, void *ctx) {
    (void)subject; (void)ctx;
    (void)len;
    FLY_LOG_DEBUG("[fly_nats] received post broadcast: %s", data);
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
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "title", title ? title : "");
    cJSON_AddStringToObject(obj, "slug", slug ? slug : "");
    cJSON_AddStringToObject(obj, "summary", summary ? summary : "");
    char *json = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
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

#include "config/config.h"
#include "engine/flywire.h"
#include "engine/flywire_cluster.h"
#include <string.h>

flywire_config_t g_flywire = {.self_idx = 0};

const char *flywire_token(void) {
    return "test-token";
}

bool flywire_request_authorized(cwist_http_request *req) {
    const char *token = req ? cwist_http_header_get(req->headers, "X-FlyWire-Token") : NULL;
    return token && strcmp(token, "test-token") == 0;
}

bool flywire_cluster_configured(void) {
    return false;
}

struct curl_slist *flywire_curl_target(CURL *curl, int node_idx, const char *path_and_query) {
    (void)curl;
    (void)node_idx;
    (void)path_and_query;
    return NULL;
}

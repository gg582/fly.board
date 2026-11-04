#ifndef ENGINE_FLYWIRE_ANTI_ENTROPY_H
#define ENGINE_FLYWIRE_ANTI_ENTROPY_H

#include <stdbool.h>
#include <cwist/net/http/http.h>

bool flywire_anti_entropy_enabled(void);
void handler_flywire_anti_entropy(cwist_http_request *req, cwist_http_response *res);
int flywire_anti_entropy_peer_iteration(int node_idx);

#endif

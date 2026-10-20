#ifndef ENGINE_FLYWIRE_CLUSTER_H
#define ENGINE_FLYWIRE_CLUSTER_H

#include <stdbool.h>
#include <curl/curl.h>
#include <sqlite3.h>

/* Point @p curl at <path_and_query> on nodes= entry @p node_idx. With
 * site_host set and an IP/host entry, the URL keeps the public host (TLS SNI,
 * certificate, Host header) and CURLOPT_CONNECT_TO routes it to the node.
 * Returns the connect-to list to free after the transfer (may be NULL). */
struct curl_slist *flywire_curl_target(CURL *curl, int node_idx, const char *path_and_query);

/* Cluster bookkeeping for the shared replicated journal, called from the
 * replica apply path in flywire.c. */
bool flywire_cluster_active(void);
/* One replica round against the current primary (flywire.c): 0 caught up,
 * -1 transient failure, 1 journal purged past our checkpoint (reseed). */
int flywire_replica_iteration(void);
/* Start the cluster thread (supervisor only, before the worker fork). */
bool flywire_cluster_start(void);
void flywire_cluster_stop(void);
/* Mirror an applied primary journal row into the local journal under the
 * same seq, so every node holds the same log and any of them can serve it
 * after a takeover. */
bool flywire_cluster_mirror_row(sqlite3 *conn, long long seq, const char *entity, int entity_id,
                                const char *op, const char *payload);

#endif

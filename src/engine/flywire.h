#ifndef ENGINE_FLYWIRE_H
#define ENGINE_FLYWIRE_H

#include <stdbool.h>
#include <cwist/sys/app/app.h>

/* Read the replica checkpoint (data/.flywire_seq); 0 when missing/corrupt. */
long long flywire_checkpoint_read(void);
/* Atomically persist the replica checkpoint (temp file + rename). */
bool flywire_checkpoint_write(long long seq);

/* This site's version string (cached): data/.flywire_version, else the short
 * git HEAD when running from a checkout, else "unknown". Advertised by the
 * primary in the feed so replicas can auto-upgrade. */
const char *flywire_site_version(void);

/* Spawn the replica apply loop when flywire.settings says mode=replica.
 * Called once from main() before cwist_app_listen(): the thread then lives
 * only in the parent/supervisor process, never in forked workers (same
 * single-runner approach as the cleanup worker). No-op on a primary. */
bool flywire_start(void);

/* Stop the replica loop (called at shutdown). */
void flywire_stop(void);

/* Replica write-through proxy: forward a mutating request (POST/PUT/PATCH/
 * DELETE) to the primary and copy the response back, so a replica behind the
 * same public hostname accepts writes transparently.  Forwards method, path,
 * query, body and the Cookie/Content-Type/Authorization headers; relays back
 * status, body, and Location/Set-Cookie/Content-Type.  Returns false when the
 * upstream call could not be made at all (caller should fall back to an
 * error).  Only meaningful on a replica. */
bool flywire_proxy_write(cwist_http_request *req, cwist_http_response *res);

#endif

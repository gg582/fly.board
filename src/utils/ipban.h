#ifndef FLY_IPBAN_H
#define FLY_IPBAN_H

#include <cwist/net/http/http.h>
#include <cwist/sys/app/app.h>
#include <stdbool.h>
#include <stddef.h>

/* Minimal in-process IP ban list (app-level fail2ban replacement).
 *
 * Login failures are counted per client address in a fixed-size hash table
 * in shared memory (survives worker forks), protected by a process-shared
 * robust mutex. After FLY_BAN_THRESHOLD (default 5) consecutive failures
 * within FLY_BAN_WINDOW_SEC (default 600) seconds the address is banned
 * for FLY_BAN_DURATION_SEC (default 3600) seconds; a successful login
 * clears the counter and any ban. Expired bans are evicted lazily on
 * lookup. Addresses that cannot be determined (HTTP/3 without a connected
 * socket) are ignored everywhere. */

bool ipban_init(void);

/* Record a failed login attempt from the request's peer address. */
void ipban_note_failure(cwist_http_request *req);

/* Clear the failure counter (and ban) for the request's peer address. */
void ipban_note_success(cwist_http_request *req);

/* Request middleware: 403 with a short plain-text body when the peer is
 * banned, otherwise passes the request down the chain. */
void ipban_middleware(cwist_http_request *req, cwist_http_response *res, cwist_handler_func next);

/* Number of addresses currently banned (for diagnostics/tests). */
size_t ipban_banned_count(void);

#endif

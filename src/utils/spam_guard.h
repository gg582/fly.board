#ifndef FLY_SPAM_GUARD_H
#define FLY_SPAM_GUARD_H

#include <cwist/core/sstring/sstring.h>
#include <cwist/net/http/http.h>
#include <stdbool.h>

/* Optional spam defenses for posts, comments, reports and sign-ups. Both are
 * switched on from the admin dashboard (write_policy_t) and are off by
 * default, because anonymous writing is a feature of this site.
 *
 * Honeypot: forms carry a field humans never see and a signed render time.
 * A filled trap field, or a submission faster than SPAM_MIN_FILL_SECONDS,
 * is rejected. Nothing about the writer is recorded.
 *
 * Rate limit: at most N submissions per writer per 10 minutes. A writer is
 * the account when logged in, otherwise the client address (IPv6 by /64).
 * Writers are keyed by an HMAC under a key drawn fresh at every start and
 * kept only in shared memory, so the table cannot be mapped back to
 * addresses offline and vanishes on restart. Requests whose address is
 * unknown (HTTP/3 has no connected socket) are not limited. Admins are
 * never limited. */

#define SPAM_MIN_FILL_SECONDS 3
/* Not a name browsers autofill (e.g. "website"), or real users would trip it. */
#define SPAM_FIELD_TRAP "fb_extra"
#define SPAM_FIELD_TOKEN "form_ts"

typedef enum {
    SPAM_OK = 0,
    SPAM_REJECTED,     /* honeypot tripped or token missing/forged/too fresh */
    SPAM_RATE_LIMITED,
} spam_verdict_t;

/* Allocate the shared counter table. Call before cwist forks the workers. */
bool spam_guard_init(void);

/* Append the hidden trap field and render-time token to a form; nothing
 * when the honeypot is off. */
void spam_guard_append_fields(cwist_sstring *b);

/* Check one submission. trap and token are the submitted SPAM_FIELD_TRAP and
 * SPAM_FIELD_TOKEN values (NULL when absent). A passing check counts toward
 * the writer's rate limit. */
spam_verdict_t spam_guard_check(cwist_http_request *req, int uid, const char *role,
                                const char *trap, const char *token);

/* Fill res with the matching 400/429 page for a failed verdict. */
void spam_guard_reject(cwist_http_response *res, spam_verdict_t verdict);

#endif

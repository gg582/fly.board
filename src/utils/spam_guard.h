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

/* Password-locked posts: always on, independent of the write policy, since
 * every attempt costs an Argon2id verification. Counts one attempt for this
 * client on this post and returns false once the client went over
 * SPAM_UNLOCK_PER_POST on the post or SPAM_UNLOCK_PER_CLIENT in total, or the
 * post itself went over SPAM_UNLOCK_PER_TARGET from every client, within the
 * 10-minute window. */
#define SPAM_UNLOCK_PER_POST 10
#define SPAM_UNLOCK_PER_CLIENT 40
#define SPAM_UNLOCK_PER_TARGET 300
bool spam_guard_unlock_allow(cwist_http_request *req, int uid, int post_id);

/* Fill res with the matching 400/429 page for a failed verdict. */
void spam_guard_reject(cwist_http_response *res, spam_verdict_t verdict);

#endif

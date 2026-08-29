#ifndef FLY_EMAIL_H
#define FLY_EMAIL_H

#include <stdbool.h>

/* True when signup requires email verification (FLY_EMAIL_CERT=true). */
bool email_cert_enabled(void);

/* Send a plain-text mail through the configured SMTP relay.
 * Configuration (environment):
 *   FLY_SMTP_HOST      relay host (required; send fails without it)
 *   FLY_SMTP_PORT      port (default 25, or 465 with FLY_SMTP_TLS=implicit)
 *   FLY_SMTP_TLS       "starttls" | "implicit" | off otherwise
 *   FLY_SMTP_USER/PASS AUTH LOGIN credentials (optional)
 *   FLY_SMTP_FROM      envelope/header sender (default FLY_SMTP_USER) */
bool email_send(const char *to, const char *subject, const char *body);

#endif

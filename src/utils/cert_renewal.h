#ifndef CERT_RENEWAL_H
#define CERT_RENEWAL_H

#include <cwist/sys/app/app.h>

/* Start the daily certificate-renewal worker.  No-op unless TLS is on and
 * the environment sets FLY_CERT_RENEWAL=true.  Safe to call unconditionally. */
void cert_renewal_start(cwist_app *app);
void cert_renewal_stop(void);
void cert_renewal_join(void);

#endif

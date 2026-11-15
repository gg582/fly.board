#ifndef ACME_CHALLENGE_H
#define ACME_CHALLENGE_H

#include <cwist/sys/app/app.h>

#define ACME_LEGO_WEBROOT ".lego/webroot"

void acme_challenge_start(void);
void acme_challenge_stop(void);
void acme_challenge_join(void);
void handler_flywire_acme_challenge(cwist_http_request *req, cwist_http_response *res);

#endif

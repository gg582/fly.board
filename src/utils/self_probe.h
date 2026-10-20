#ifndef SELF_PROBE_H
#define SELF_PROBE_H

#include <stdbool.h>

/* Start the loopback liveness probe (GET /metrics every FLY_SELF_PROBE_SEC
 * seconds, default 10; 0 disables).  Call before cwist_app_listen() so the
 * thread lives only in the supervisor.  Safe to call unconditionally. */
void self_probe_start(int port, bool use_tls);
void self_probe_stop(void);
void self_probe_join(void);

#endif

#ifndef ENGINE_FLYWIRE_H
#define ENGINE_FLYWIRE_H

#include <stdbool.h>
#include <cwist/sys/app/app.h>

/* Read the replica checkpoint (data/.flywire_seq); 0 when missing/corrupt. */
long long flywire_checkpoint_read(void);
/* Atomically persist the replica checkpoint (temp file + rename). */
bool flywire_checkpoint_write(long long seq);

/* Spawn the replica apply loop when flywire.settings says mode=replica.
 * Called once from main() before cwist_app_listen(): the thread then lives
 * only in the parent/supervisor process, never in forked workers (same
 * single-runner approach as the cleanup worker). No-op on a primary. */
bool flywire_start(void);

/* Stop the replica loop (called at shutdown). */
void flywire_stop(void);

#endif

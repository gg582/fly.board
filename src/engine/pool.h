#ifndef ENGINE_POOL_H
#define ENGINE_POOL_H

#include <ttak/async/task.h>
#include <ttak/thread/pool.h>
#include <stdbool.h>
#include <stdint.h>

bool engine_pool_init(void);
void engine_pool_shutdown(void);
/* Schedule a bounded, finite background task (transcoding, finalization,
 * warmup).  These tasks never share workers with service loops. */
bool engine_pool_schedule(ttak_task_func_t func,
                          void *arg,
                          uint64_t hash,
                          ttak_task_domain_t domain,
                          uint8_t urgency);
/* Schedule a long-lived service loop (for example NATS or housekeeping).
 * Keep these separate so a service loop cannot starve user work. */
bool engine_pool_schedule_service(ttak_task_func_t func,
                                  void *arg,
                                  uint64_t hash,
                                  ttak_task_domain_t domain,
                                  uint8_t urgency);

#endif

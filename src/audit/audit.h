#ifndef FLY_AUDIT_H
#define FLY_AUDIT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool audit_init(const char *settings_path);
bool audit_enabled(void);
bool audit_record(int actor_uid, const char *action, const char *target,
                  const char *outcome, const char *detail);
bool audit_verify(const char *path, uint64_t *entries, char *error, size_t error_size);

#endif

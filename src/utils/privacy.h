#ifndef FLYBOARD_PRIVACY_H
#define FLYBOARD_PRIVACY_H

#include <cjson/cJSON.h>
#include <sqlite3.h>
#include <stdbool.h>
#include <stddef.h>

#define PRIVACY_EXPORT_ID_LEN 64

bool privacy_export_start(int user_id, char out_id[PRIVACY_EXPORT_ID_LEN + 1]);
cJSON *privacy_export_status(int user_id, const char *id);
bool privacy_export_download_path(int user_id, const char *id, char *out, size_t out_len);
void privacy_export_revoke_user(int user_id);

bool privacy_export_build(sqlite3 *main_db, sqlite3 *comments_db, int user_id,
                          const char *archive_path);

#endif

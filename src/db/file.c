#define _POSIX_C_SOURCE 200809L
#include "db.h"
#include <cwist/core/mem/alloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool db_file_create_volume(cwist_db *db, int post_id, int user_id, const char *filename, const char *mime_type, const char *file_path, size_t len) {
    char sql[2048];
    snprintf(sql, sizeof(sql),
        "INSERT INTO files (post_id, user_id, filename, mime_type, file_path, size) VALUES (%d,%d,'%s','%s','%s',%lu)",
        post_id, user_id, filename, mime_type ? mime_type : "application/octet-stream", file_path, (unsigned long)len);
    return db_exec_sql(db, sql);
}

cJSON *db_file_get(cwist_db *db, int id) {
    char sql[256];
    snprintf(sql, sizeof(sql), "SELECT * FROM files WHERE id=%d LIMIT 1", id);
    cJSON *res = NULL;
    cwist_db_query(db, sql, &res);
    if (res && cJSON_GetArraySize(res) > 0) {
        cJSON *row = cJSON_GetArrayItem(res, 0);
        cJSON *cpy = cJSON_Duplicate(row, 1);
        cJSON_Delete(res);
        return cpy;
    }
    if (res) cJSON_Delete(res);
    return NULL;
}

cJSON *db_file_list_by_post(cwist_db *db, int post_id) {
    char sql[512];
    snprintf(sql, sizeof(sql), "SELECT id, filename, mime_type, file_path, size, created_at FROM files WHERE post_id=%d", post_id);
    cJSON *res = NULL;
    cwist_db_query(db, sql, &res);
    return res;
}

bool db_file_delete(cwist_db *db, int id) {
    char sql[256];
    snprintf(sql, sizeof(sql), "DELETE FROM files WHERE id=%d", id);
    return db_exec_sql(db, sql);
}

bool db_file_increment_download(cwist_db *db, int id) {
    char sql[256];
    snprintf(sql, sizeof(sql), "UPDATE files SET download_count = download_count + 1 WHERE id=%d", id);
    return db_exec_sql(db, sql);
}

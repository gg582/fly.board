#define _POSIX_C_SOURCE 200809L
#include "db.h"
#include <cwist/core/mem/alloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

cJSON *db_user_get_by_username(cwist_db *db, const char *username) {
    char sql[512];
    snprintf(sql, sizeof(sql), "SELECT * FROM users WHERE username='%s' LIMIT 1", username);
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

cJSON *db_user_get_by_id(cwist_db *db, int id) {
    char sql[256];
    snprintf(sql, sizeof(sql), "SELECT * FROM users WHERE id=%d LIMIT 1", id);
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

bool db_user_create(cwist_db *db, const char *username, const char *email, const char *password_hash) {
    char sql[1024];
    snprintf(sql, sizeof(sql),
        "INSERT INTO users (username, email, password_hash) VALUES ('%s','%s','%s')",
        username, email, password_hash);
    return db_exec_sql(db, sql);
}

bool db_user_delete(cwist_db *db, int id) {
    char sql[256];
    snprintf(sql, sizeof(sql), "DELETE FROM users WHERE id=%d", id);
    return db_exec_sql(db, sql);
}

bool db_user_update_role(cwist_db *db, int id, const char *role) {
    char sql[256];
    snprintf(sql, sizeof(sql), "UPDATE users SET role='%s' WHERE id=%d", role, id);
    return db_exec_sql(db, sql);
}

bool db_user_update_profile_pic(cwist_db *db, int id, const char *profile_pic) {
    char sql[512];
    snprintf(sql, sizeof(sql), "UPDATE users SET profile_pic='%s' WHERE id=%d", profile_pic, id);
    return db_exec_sql(db, sql);
}

bool db_user_update_profile(cwist_db *db, int id, const char *nickname, const char *bio, const char *profile_pic) {
    char sql[2048];
    snprintf(sql, sizeof(sql),
        "UPDATE users SET nickname='%s', bio='%s', profile_pic='%s' WHERE id=%d",
        nickname ? nickname : "", bio ? bio : "", profile_pic ? profile_pic : "", id);
    return db_exec_sql(db, sql);
}

bool db_user_update_password(cwist_db *db, int id, const char *password_hash) {
    char sql[1024];
    snprintf(sql, sizeof(sql), "UPDATE users SET password_hash='%s' WHERE id=%d", password_hash, id);
    return db_exec_sql(db, sql);
}

cJSON *db_user_list(cwist_db *db) {
    const char *sql = "SELECT id, username, email, role, profile_pic, created_at, active FROM users ORDER BY id";
    cJSON *res = NULL;
    cwist_db_query(db, sql, &res);
    return res;
}
bool db_user_delete_with_cascade(cwist_db *db, int id, bool delete_replies) {
    if (delete_replies) {
        char sql[256];
        snprintf(sql, sizeof(sql), "DELETE FROM comments WHERE user_id=%d OR parent_id IN (SELECT id FROM comments WHERE user_id=%d)", id, id);
        db_exec_sql(db, sql);
    } else {
        char sql[256];
        snprintf(sql, sizeof(sql), "UPDATE comments SET content='', deleted=1 WHERE user_id=%d", id);
        db_exec_sql(db, sql);
    }
    char sql_posts[256];
    snprintf(sql_posts, sizeof(sql_posts), "DELETE FROM posts WHERE user_id=%d", id);
    db_exec_sql(db, sql_posts);
    char sql_files[256];
    snprintf(sql_files, sizeof(sql_files), "DELETE FROM files WHERE user_id=%d", id);
    db_exec_sql(db, sql_files);
    char sql_user[256];
    snprintf(sql_user, sizeof(sql_user), "DELETE FROM users WHERE id=%d", id);
    return db_exec_sql(db, sql_user);
}


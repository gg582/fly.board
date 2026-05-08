#ifndef DOCKER_BLOG_DB_H
#define DOCKER_BLOG_DB_H

#include <cwist/core/db/sql.h>
#include <cjson/cJSON.h>
#include <stdbool.h>

bool db_init(cwist_db *db);
bool db_migrate(cwist_db *db);

/* Users */
cJSON *db_user_get_by_username(cwist_db *db, const char *username);
cJSON *db_user_get_by_id(cwist_db *db, int id);
bool db_user_create(cwist_db *db, const char *username, const char *email, const char *password_hash);
bool db_user_delete(cwist_db *db, int id);
bool db_user_update_role(cwist_db *db, int id, const char *role);
bool db_user_update_profile_pic(cwist_db *db, int id, const char *profile_pic);
bool db_user_update_profile(cwist_db *db, int id, const char *nickname, const char *bio, const char *profile_pic);
cJSON *db_user_list(cwist_db *db);

/* Boards */
bool db_board_create(cwist_db *db, const char *name, const char *slug, const char *description, bool admin_only);
bool db_board_delete(cwist_db *db, int id);
bool db_board_update(cwist_db *db, int id, const char *name, const char *slug, const char *description, bool admin_only);
cJSON *db_board_list(cwist_db *db);
cJSON *db_board_get_by_slug(cwist_db *db, const char *slug);
cJSON *db_board_get_by_id(cwist_db *db, int id);
bool db_board_can_user_access(cwist_db *db, int board_id, int user_id, bool is_admin);

/* Board permissions */
bool db_board_perm_grant(cwist_db *db, int board_id, int user_id);
bool db_board_perm_revoke(cwist_db *db, int board_id, int user_id);
cJSON *db_board_perm_list(cwist_db *db, int board_id);

/* Posts */
bool db_post_create(cwist_db *db, int board_id, int user_id, const char *title, const char *slug, const char *content, const char *summary, const char *pqc_signature);
bool db_post_update(cwist_db *db, int id, const char *title, const char *content, const char *summary, const char *pqc_signature);
bool db_post_delete(cwist_db *db, int id);
cJSON *db_post_get_by_slug(cwist_db *db, const char *slug);
cJSON *db_post_get_by_id(cwist_db *db, int id);
cJSON *db_post_list(cwist_db *db, int board_id, int limit, int offset);
cJSON *db_post_recent(cwist_db *db, int limit);
int db_post_count(cwist_db *db, int board_id);

/* Files */
bool db_file_create_volume(cwist_db *db, int post_id, int user_id, const char *filename, const char *mime_type, const char *file_path, size_t len);
cJSON *db_file_get(cwist_db *db, int id);
cJSON *db_file_list_by_post(cwist_db *db, int post_id);
bool db_file_delete(cwist_db *db, int id);

/* Comments */
bool db_comment_create(cwist_db *db, const char *target_type, int target_id, int user_id, int parent_id, const char *content);
bool db_comment_update(cwist_db *db, int id, int user_id, const char *content);
bool db_comment_delete(cwist_db *db, int id, int user_id);
cJSON *db_comment_get_by_id(cwist_db *db, int id);
cJSON *db_comment_list_by_target(cwist_db *db, const char *target_type, int target_id);

/* User delete with cascade */
bool db_user_delete_with_cascade(cwist_db *db, int id, bool delete_replies);

#endif

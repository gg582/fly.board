#ifndef DOCKER_BLOG_DB_H
#define DOCKER_BLOG_DB_H

#include <cwist/core/db/sql.h>
#include <cjson/cJSON.h>
#include <stdbool.h>

bool db_init(cwist_db *db);
bool db_exec_sql(cwist_db *db, const char *sql);
bool db_migrate(cwist_db *db);

/* Users */
cJSON *db_user_get_by_username(cwist_db *db, const char *username);
cJSON *db_user_get_by_id(cwist_db *db, int id);
bool db_user_create(cwist_db *db, const char *username, const char *email, const char *password_hash);
bool db_user_delete(cwist_db *db, int id);
bool db_user_update_role(cwist_db *db, int id, const char *role);
bool db_user_update_profile_pic(cwist_db *db, int id, const char *profile_pic);
bool db_user_update_profile(cwist_db *db, int id, const char *nickname, const char *bio, const char *profile_pic);
bool db_user_update_password(cwist_db *db, int id, const char *password_hash);
cJSON *db_user_list(cwist_db *db);

/* Boards */
bool db_board_create(cwist_db *db, const char *name, const char *slug, const char *description, bool admin_only, int read_perm, int write_perm, int comment_perm);
bool db_board_delete(cwist_db *db, int id);
bool db_board_update(cwist_db *db, int id, const char *name, const char *slug, const char *description, bool admin_only, int read_perm, int write_perm, int comment_perm);
cJSON *db_board_list(cwist_db *db);
cJSON *cwist_orm_board_list_popular(cwist_db *db);
cJSON *db_board_get_by_slug(cwist_db *db, const char *slug);
cJSON *db_board_get_by_id(cwist_db *db, int id);
bool db_board_can_user_access(cwist_db *db, int board_id, int user_id, bool is_admin);

/* Board permissions */
bool db_board_perm_grant(cwist_db *db, int board_id, int user_id);
bool db_board_perm_revoke(cwist_db *db, int board_id, int user_id);
cJSON *db_board_perm_list(cwist_db *db, int board_id);

/* Posts */
int db_post_create(cwist_db *db, int board_id, int user_id, const char *title, const char *slug, const char *content, const char *summary, const char *pqc_signature, int is_notice, int is_secret, const char *category);
int db_post_create_with_auto_slug(cwist_db *db, int board_id, int user_id, const char *title, const char *slug_base, const char *content, const char *summary, const char *pqc_signature, int is_notice, int is_secret, const char *category, char **out_slug);
bool db_post_update(cwist_db *db, int id, int board_id, const char *title, const char *content, const char *summary, const char *pqc_signature, int is_notice, int is_secret, const char *category);
bool db_post_delete(cwist_db *db, int id);
bool db_post_set_delete_pin_hash(cwist_db *db, int id, const char *delete_pin_hash);
cJSON *db_post_get_by_slug(cwist_db *db, const char *slug);
cJSON *db_post_get_by_id(cwist_db *db, int id);
cJSON *db_post_list(cwist_db *db, int board_id, int limit, int offset);
cJSON *db_post_recent(cwist_db *db, int limit);
cJSON *db_post_recent_by_board(cwist_db *db, int board_id, int limit);
int db_post_count(cwist_db *db, int board_id);

/* Post extended features */
bool db_post_increment_view(cwist_db *db, int id);
cJSON *db_post_list_search(cwist_db *db, int board_id, const char *search, const char *search_type, int limit, int offset);
int db_post_count_search(cwist_db *db, int board_id, const char *search, const char *search_type);

/* Post votes */
bool db_post_vote(cwist_db *db, int post_id, int user_id, int vote_type);
bool db_post_vote_remove(cwist_db *db, int post_id, int user_id);
bool db_post_vote_anon(cwist_db *db, int post_id, int vote_type);
cJSON *db_post_vote_counts(cwist_db *db, int post_id);
int db_post_user_vote(cwist_db *db, int post_id, int user_id);

/* Tags */
int db_tag_get_or_create(cwist_db *db, const char *name);
bool db_tag_link(cwist_db *db, int post_id, int tag_id);
cJSON *db_tag_list_by_post(cwist_db *db, int post_id);
bool db_tag_clear_by_post(cwist_db *db, int post_id);

/* Files */
bool db_file_create_volume(cwist_db *db, int post_id, int user_id, const char *filename, const char *mime_type, const char *file_path, size_t len);
int db_file_create_volume_get_id(cwist_db *db, int post_id, int user_id, const char *filename, const char *mime_type, const char *file_path, size_t len);
char *db_file_unique_filename(cwist_db *db, int post_id, const char *filename);
cJSON *db_file_get(cwist_db *db, int id);
cJSON *db_file_list_all(cwist_db *db);
cJSON *db_file_list_by_post(cwist_db *db, int post_id);
cJSON *db_file_list_by_user(cwist_db *db, int user_id, int limit);
bool db_file_delete(cwist_db *db, int id);
int db_file_drop_all(cwist_db *db);
bool db_file_increment_download(cwist_db *db, int id);
bool db_file_set_delete_pin_hash(cwist_db *db, int id, const char *delete_pin_hash);
bool db_file_set_preview_paths(cwist_db *db, int id, const char *thumb_path, const char *preview_path);
bool db_file_attach_to_post(cwist_db *db, int id, int post_id, int is_inline);
void db_file_replace_for_post(cwist_db *db, int post_id, const char *filename);
void db_file_cleanup_duplicates(cwist_db *db);
void db_cleanup_orphaned_files(cwist_db *db);
void db_file_delete_by_post(cwist_db *db, int post_id);

/* Comments (separate DB) */
bool db_comment_init(const char *path);
void db_comment_close(void);
bool db_comment_create(cwist_db *db, const char *target_type, int target_id, int user_id, const char *author_name, int parent_id, const char *content);
bool db_comment_update(cwist_db *db, int id, int user_id, const char *content);
bool db_comment_delete(cwist_db *db, int id, int user_id);
cJSON *db_comment_get_by_id(cwist_db *db, int id);
cJSON *db_comment_list_by_target(cwist_db *db, const char *target_type, int target_id);
bool db_comment_delete_by_target(const char *target_type, int target_id);

/* User delete with cascade */
bool db_user_delete_with_cascade(cwist_db *db, int id, bool delete_replies);

#endif

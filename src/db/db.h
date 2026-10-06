#ifndef DOCKER_BLOG_DB_H
#define DOCKER_BLOG_DB_H

#include <cwist/core/db/sql.h>
#include <cjson/cJSON.h>
#include <stdbool.h>

bool db_init(cwist_db *db);
bool db_exec_sql(cwist_db *db, const char *sql);
bool db_migrate(cwist_db *db);
bool db_transaction_begin(cwist_db *db);
bool db_transaction_commit(cwist_db *db);
bool db_transaction_rollback(cwist_db *db);

/* Site settings: admin-editable key/value pairs (see config/write_policy.h) */
bool db_site_setting_get(cwist_db *db, const char *key, char *out, size_t out_len);
bool db_site_setting_set(cwist_db *db, const char *key, const char *value);

/* Users */
cJSON *db_user_get_by_username(cwist_db *db, const char *username);
cJSON *db_user_get_by_id(cwist_db *db, int id);
bool db_user_create(cwist_db *db, const char *username, const char *email, const char *password_hash);
/* users.id of the admin.settings account, creating its row on first run and
 * keeping its name in step with admin.settings. 0 on failure. */
int db_user_ensure_site_admin(cwist_db *db, const char *username);
bool db_user_delete(cwist_db *db, int id);
bool db_user_update_role(cwist_db *db, int id, const char *role);
bool db_user_update_profile_pic(cwist_db *db, int id, const char *profile_pic);
bool db_user_update_profile(cwist_db *db, int id, const char *nickname, const char *bio, const char *profile_pic);
bool db_user_update_password(cwist_db *db, int id, const char *password_hash);
cJSON *db_user_list(cwist_db *db);
bool db_user_set_email_verified(cwist_db *db, int id, bool verified);
bool db_email_token_create(cwist_db *db, int user_id, const char *token, long expires_at);
int db_email_token_consume(cwist_db *db, const char *token);

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
/* Publication state. A post's created_at doubles as its publish time: a
 * 'published' post whose created_at is still in the future is scheduled.
 * Timestamps are UTC "YYYY-MM-DD HH:MM:SS", the CURRENT_TIMESTAMP format. */
#define POST_STATUS_DRAFT "draft"
#define POST_STATUS_PUBLISHED "published"
#define POST_TIME_LEN 20
/* WHERE fragment selecting posts anyone may see (table alias p). */
#define POST_PUBLIC_SQL "p.status='published' AND p.created_at<=CURRENT_TIMESTAMP"

/* status NULL keeps the column default (published); publish_at NULL means
 * now on create and "keep the current value" on update. */
int db_post_create_with_auto_slug(cwist_db *db, int board_id, int user_id, const char *title, const char *slug_base, const char *content, const char *summary, const char *pqc_signature, int is_notice, int is_secret, const char *category, const char *status, const char *publish_at, char **out_slug);
bool db_post_update(cwist_db *db, int id, int board_id, const char *title, const char *content, const char *summary, const char *pqc_signature, int is_notice, int is_secret, const char *category, const char *status, const char *publish_at);
void post_utc_now(char out[POST_TIME_LEN]);
bool post_is_public(cJSON *post);
bool post_is_scheduled(cJSON *post);
/* Drafts and scheduled posts, newest edit first; user_id 0 lists everyone's. */
cJSON *db_post_list_unpublished(cwist_db *db, int user_id);
/* Earliest future publish time as a UTC epoch, or 0 when nothing is queued. */
long long db_post_next_scheduled(cwist_db *db);
/* Public posts whose "published" event has not gone out yet, each claimed
 * (announced=1) by this call so no other process announces it again. */
cJSON *db_post_claim_unannounced(cwist_db *db);
int db_post_count_drafts(cwist_db *db, int user_id);
bool db_post_delete(cwist_db *db, int id);
/* [{id, title, content, pqc_signature}] for every post, for re-signing. */
cJSON *db_post_list_for_signing(cwist_db *db);
/* Store a signature without touching updated_at (the content did not change). */
bool db_post_set_signature(cwist_db *db, int id, const char *pqc_signature);
bool db_post_set_delete_pin_hash(cwist_db *db, int id, const char *delete_pin_hash);
cJSON *db_post_get_by_slug(cwist_db *db, const char *slug);
cJSON *db_post_get_by_id(cwist_db *db, int id);
cJSON *db_post_list(cwist_db *db, int board_id, int limit, int offset);
cJSON *db_post_recent(cwist_db *db, int limit);
cJSON *db_post_recent_by_board(cwist_db *db, int board_id, int limit);
cJSON *db_post_recent_by_boards_batch(cwist_db *db, int limit_per_board);
int db_post_count(cwist_db *db, int board_id);
/* Archive: [{ym:"YYYY-MM", n}] over public posts, newest month first. */
cJSON *db_post_archive_months(cwist_db *db);
/* Public posts published in month @p ym ("YYYY-MM"); NULL/0 on a bad ym. */
cJSON *db_post_list_by_month(cwist_db *db, const char *ym, int limit, int offset);
int db_post_count_by_month(cwist_db *db, const char *ym);
/* Public neighbours of @p post in publish order: {"prev":{slug,title}?,
 * "next":{slug,title}?}; prev is the older post. */
cJSON *db_post_adjacent(cwist_db *db, int post_id, const char *created_at);
/* Newest public posts, notices not pinned (feeds); board_id 0 = all. */
cJSON *db_post_feed(cwist_db *db, int board_id, int limit);

/* Post extended features */
bool db_post_increment_view(cwist_db *db, int id);
cJSON *db_post_list_search(cwist_db *db, int board_id, const char *search, const char *search_type, int limit, int offset);
int db_post_count_search(cwist_db *db, int board_id, const char *search, const char *search_type);

/* Search index (src/db/search.c). Creates/rebuilds the trigram index; call
 * from db_migrate. db_search_index_post re-reads one post and reindexes it. */
bool db_search_migrate(cwist_db *db);
bool db_search_index_post(cwist_db *db, int post_id);

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
#define DB_TAG_MAX_PER_POST 10
#define DB_TAG_MAX_BYTES 48
/* Replace a post's tags with the comma-separated @p csv (normalized, deduped,
 * at most DB_TAG_MAX_PER_POST). Returns the number linked, -1 on error. */
int db_tag_set_for_post(cwist_db *db, int post_id, const char *csv);
/* True when @p name is already in normalized form (safe for a URL path). */
bool db_tag_name_valid(const char *name);
/* [{name, n}] for tags on at least one public post, most used first. */
cJSON *db_tag_list_public(cwist_db *db);
cJSON *db_post_list_by_tag(cwist_db *db, const char *tag, int limit, int offset);
int db_post_count_by_tag(cwist_db *db, const char *tag);
/* [{slug, title, created_at, shared}] public posts sharing tags with post_id. */
cJSON *db_post_related_by_tags(cwist_db *db, int post_id, int limit);

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
bool db_file_update_file_path(cwist_db *db, int id, const char *file_path);
bool db_file_attach_to_post(cwist_db *db, int id, int post_id, int is_inline);
void db_file_replace_for_post(cwist_db *db, int post_id, const char *filename);
void db_file_cleanup_duplicates(cwist_db *db);
void db_cleanup_orphaned_files(cwist_db *db);
void db_file_delete_by_post(cwist_db *db, int post_id);

/* Comments (separate DB) */
bool db_comment_init(const char *path);
void db_comment_close(void);
int db_comment_create(cwist_db *db, const char *target_type, int target_id, int user_id, const char *author_name, int parent_id, const char *content);
bool db_comment_update(cwist_db *db, int id, int user_id, const char *content);
bool db_comment_delete(cwist_db *db, int id, int user_id);
cJSON *db_comment_get_by_id(cwist_db *db, int id);
cJSON *db_comment_list_by_target(cwist_db *db, const char *target_type, int target_id);
bool db_comment_delete_by_target(const char *target_type, int target_id);

/* Content reports (src/db/report.c) */
#define REPORT_STATUS_OPEN "open"
#define REPORT_STATUS_RESOLVED "resolved"
#define REPORT_STATUS_DISMISSED "dismissed"
bool db_report_migrate(cwist_db *db);
/* 1 when stored, 0 when this account already has an open report on the
 * target, -1 on error. reporter_user_id 0 = anonymous (nothing stored). */
int db_report_create(cwist_db *db, const char *target_type, int target_id, int post_id,
                     const char *reason, const char *detail, int reporter_user_id);
/* Newest first, at most 500; status NULL/"all" for every report. */
cJSON *db_report_list(cwist_db *db, const char *status);
int db_report_count_open(cwist_db *db);
/* Close every open report on one target; returns how many were closed. */
int db_report_close_target(cwist_db *db, const char *target_type, int target_id, const char *status,
                           const char *resolution, int resolved_by);
/* Admin removal of any comment (db_comment_delete only removes one's own). */
bool db_comment_delete_admin(cwist_db *db, int id);

/* User delete with cascade */
bool db_user_delete_with_cascade(cwist_db *db, int id, bool delete_replies);

/* Notifications (main DB; db may be NULL from the NATS worker thread) */
bool db_notification_create(cwist_db *db, int user_id, const char *actor_name, const char *kind, int post_id, const char *post_slug, int comment_id, const char *excerpt);
int db_notification_unread_count(cwist_db *db, int user_id);
cJSON *db_notification_list(cwist_db *db, int user_id, int limit);
bool db_notification_mark_all_read(cwist_db *db, int user_id);
/* Federated delivery: inserts only when the recipient exists locally. */
bool db_notification_deliver_federated(long user_id, const char *actor_name, const char *kind, const char *post_slug, const char *excerpt);

#endif

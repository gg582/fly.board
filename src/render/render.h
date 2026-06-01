#ifndef FLYBOARD_RENDER_H
#define FLYBOARD_RENDER_H

#include <stdbool.h>
#include <cwist/core/sstring/sstring.h>
#include <cjson/cJSON.h>

void render_set_nav_profile(const char *display_name, const char *account_name);
cwist_sstring *render_page(const char *title, const char *body_html, bool dark, const char *user_role, const char *profile_pic, bool is_mobile);
cwist_sstring *render_profile(cJSON *user, bool dark, const char *user_role, const char *profile_pic, bool is_own_profile, bool is_mobile);
cwist_sstring *render_account_settings(cJSON *user, bool dark, const char *viewer_role, const char *profile_pic, const char *error, bool is_mobile);
cwist_sstring *render_password_change(bool dark, const char *user_role, const char *profile_pic, const char *error, bool is_mobile);
cwist_sstring *render_login(bool dark, const char *error, bool is_mobile);
cwist_sstring *render_register(bool dark, const char *error, bool is_mobile, cJSON *legal_docs);
cwist_sstring *render_post_list(cJSON *posts, cJSON *boards, bool dark, const char *user_role, int page, int total_pages, const char *board_slug, const char *search, const char *search_type, const char *profile_pic, int user_id, bool is_mobile);
cwist_sstring *render_post_detail(cJSON *post, cJSON *files, cJSON *comments, bool dark, const char *user_role, bool pqc_verified, int vote_up, int vote_down, int user_vote, const char *profile_pic, const char *author_profile_pic, int user_id, const char *ephemeral_delete_pin, bool is_mobile);
cwist_sstring *render_file_detail(cJSON *file, cJSON *comments, bool dark, const char *user_role, const char *profile_pic, int user_id, bool is_mobile);
cwist_sstring *render_post_editor(cJSON *boards, cJSON *post, cJSON *files, bool dark, const char *user_role, const char *error, const char *profile_pic, bool is_mobile);
cwist_sstring *render_board_list(cJSON *boards, bool dark, const char *user_role, const char *profile_pic, bool is_mobile);
cwist_sstring *render_board_form(cJSON *board, cJSON *all_boards, bool dark, const char *error, const char *profile_pic, bool is_mobile, const char *user_role);
cwist_sstring *render_board_perms(cJSON *board, cJSON *perms, cJSON *users, bool dark, const char *msg, const char *profile_pic, bool is_mobile);
cwist_sstring *render_admin_dashboard(bool dark, const char *profile_pic, bool is_mobile);
cwist_sstring *render_user_admin(cJSON *users, bool dark, const char *profile_pic, bool is_mobile);
cwist_sstring *render_admin_boards(cJSON *boards, cJSON *tree, bool dark, const char *profile_pic, bool is_mobile);
cwist_sstring *render_file_repo(cJSON *files, bool dark, const char *user_role, int user_id, const char *profile_pic, bool is_mobile);
cwist_sstring *render_markdown_to_html(const char *md);

#endif

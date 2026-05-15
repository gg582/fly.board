#ifndef FLYBOARD_HANDLERS_H
#define FLYBOARD_HANDLERS_H

#include <cwist/net/http/http.h>
#include <cwist/sys/app/app.h>

void handler_home(cwist_http_request *req, cwist_http_response *res);
void handler_theme_json(cwist_http_request *req, cwist_http_response *res);
void handler_themes_json(cwist_http_request *req, cwist_http_response *res);
void handler_rss_xml(cwist_http_request *req, cwist_http_response *res);
void handler_post_vote(cwist_http_request *req, cwist_http_response *res);

void handler_login_get(cwist_http_request *req, cwist_http_response *res);
void handler_login_post(cwist_http_request *req, cwist_http_response *res);
void handler_logout(cwist_http_request *req, cwist_http_response *res);
void handler_register_get(cwist_http_request *req, cwist_http_response *res);
void handler_register_post(cwist_http_request *req, cwist_http_response *res);
void handler_unregister_post(cwist_http_request *req, cwist_http_response *res);

void handler_profile_get(cwist_http_request *req, cwist_http_response *res);
void handler_profile_post(cwist_http_request *req, cwist_http_response *res);
void handler_account_settings_get(cwist_http_request *req, cwist_http_response *res);
void handler_account_settings_post(cwist_http_request *req, cwist_http_response *res);
void handler_password_change_get(cwist_http_request *req, cwist_http_response *res);
void handler_password_change_post(cwist_http_request *req, cwist_http_response *res);
void handler_user_profile_get(cwist_http_request *req, cwist_http_response *res);

void handler_board_list(cwist_http_request *req, cwist_http_response *res);
void handler_board_new_get(cwist_http_request *req, cwist_http_response *res);
void handler_board_new_post(cwist_http_request *req, cwist_http_response *res);
void handler_board_edit_get(cwist_http_request *req, cwist_http_response *res);
void handler_board_edit_post(cwist_http_request *req, cwist_http_response *res);
void handler_board_delete(cwist_http_request *req, cwist_http_response *res);
void handler_board_perms_get(cwist_http_request *req, cwist_http_response *res);
void handler_board_perms_post(cwist_http_request *req, cwist_http_response *res);
void handler_board_perms_revoke_post(cwist_http_request *req, cwist_http_response *res);

void handler_post_list(cwist_http_request *req, cwist_http_response *res);
void handler_post_get(cwist_http_request *req, cwist_http_response *res);
void handler_post_new_get(cwist_http_request *req, cwist_http_response *res);
void handler_post_new_post(cwist_http_request *req, cwist_http_response *res);
void handler_post_edit_get(cwist_http_request *req, cwist_http_response *res);
void handler_post_edit_post(cwist_http_request *req, cwist_http_response *res);
void handler_post_delete(cwist_http_request *req, cwist_http_response *res);

void handler_asset_img(cwist_http_request *req, cwist_http_response *res);
void handler_asset_upload(cwist_http_request *req, cwist_http_response *res);
void handler_file_repo(cwist_http_request *req, cwist_http_response *res);
void handler_file_upload(cwist_http_request *req, cwist_http_response *res);
void handler_file_detail_get(cwist_http_request *req, cwist_http_response *res);
void handler_file_download(cwist_http_request *req, cwist_http_response *res);
void handler_file_delete(cwist_http_request *req, cwist_http_response *res);

void handler_comment_new_post(cwist_http_request *req, cwist_http_response *res);
void handler_comment_edit_post(cwist_http_request *req, cwist_http_response *res);
void handler_comment_delete_get(cwist_http_request *req, cwist_http_response *res);

void handler_admin_users(cwist_http_request *req, cwist_http_response *res);
void handler_admin_user_role(cwist_http_request *req, cwist_http_response *res);

void handler_api_preview(cwist_http_request *req, cwist_http_response *res);
void handler_api_upload(cwist_http_request *req, cwist_http_response *res);
void handler_api_boards_json(cwist_http_request *req, cwist_http_response *res);

void global_middleware(cwist_http_request *req, cwist_http_response *res, cwist_handler_func next);

#endif

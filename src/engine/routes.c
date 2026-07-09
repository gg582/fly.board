#include "engine/routes.h"
#include "handlers/handlers.h"

void engine_routes_register(cwist_app *app) {
    cwist_app_use(app, global_middleware);
    cwist_app_register_error_handler(app, CWIST_HTTP_NOT_FOUND, handler_not_found);

    cwist_app_get(app, "/assets/img/:filename", handler_asset_img);
    cwist_app_get(app, "/assets/uploads/:filename", handler_asset_upload);
    cwist_app_get(app, "/assets/profile/:filename", handler_asset_profile_upload);
    cwist_app_get(app, "/assets/tasfa/:scope/:filename/handshake", handler_asset_tasfa_handshake);
    cwist_app_get(app, "/assets/tasfa/:scope/:filename/chunk/:chunk_index", handler_asset_tasfa_chunk);
    cwist_app_static_with_cache(app, "/assets/images", "public/images", "public, max-age=31536000, immutable");
    cwist_app_get(app, "/assets/js/:filename", handler_static_js);
    cwist_app_get(app, "/js/:filename", handler_static_js);
    cwist_app_get(app, "/assets/css/:filename", handler_static_css);
    cwist_app_static_with_cache(app, "/assets/media", "public/media", "public, max-age=31536000, immutable");
    cwist_app_get(app, "/sw.js", handler_sw_js);
    cwist_app_get(app, "/__tasfa_stream__/:stream_id", handler_tasfa_stream_placeholder);

    /* Routes */
    cwist_app_get(app, "/", handler_home);
    cwist_app_get(app, "/theme.json", handler_theme_json);
    cwist_app_get(app, "/themes.json", handler_themes_json);
    cwist_app_get(app, "/rss.xml", handler_rss_xml);

    cwist_app_get(app, "/login", handler_login_get);
    cwist_app_post(app, "/login", handler_login_post);
    cwist_app_get(app, "/logout", handler_logout);
    cwist_app_get(app, "/register", handler_register_get);
    cwist_app_post(app, "/register", handler_register_post);
    cwist_app_post(app, "/unregister", handler_unregister_post);

    cwist_app_get(app, "/profile", handler_profile_get);
    cwist_app_post(app, "/profile", handler_profile_post);
    cwist_app_get(app, "/account/settings", handler_account_settings_get);
    cwist_app_post(app, "/account/settings", handler_account_settings_post);
    cwist_app_get(app, "/account/password", handler_password_change_get);
    cwist_app_post(app, "/account/password", handler_password_change_post);
    cwist_app_get(app, "/user/:id", handler_user_profile_get);

    cwist_app_get(app, "/boards", handler_board_list);
    cwist_app_get(app, "/board/new", handler_board_new_get);
    cwist_app_post(app, "/board/new", handler_board_new_post);
    cwist_app_get(app, "/board/:id/edit", handler_board_edit_get);
    cwist_app_post(app, "/board/edit", handler_board_edit_post);
    cwist_app_get(app, "/board/:id/delete", handler_board_delete);
    cwist_app_get(app, "/board/:id/perms", handler_board_perms_get);
    cwist_app_post(app, "/board/perms", handler_board_perms_post);
    cwist_app_post(app, "/board/perms/revoke", handler_board_perms_revoke_post);

    cwist_app_get(app, "/search", handler_post_list);
    cwist_app_get(app, "/board/:slug", handler_post_list);
    cwist_app_get(app, "/post/:slug", handler_post_get);
    cwist_app_get(app, "/post/new", handler_post_new_get);
    cwist_app_post(app, "/post/new", handler_post_new_post);
    cwist_app_get(app, "/post/:id/edit", handler_post_edit_get);
    cwist_app_post(app, "/post/:id/edit", handler_post_edit_post);
    cwist_app_get(app, "/post/delete/:id", handler_post_delete);

    cwist_app_get(app, "/files", handler_file_repo);
    cwist_app_get(app, "/file/preview/:id", handler_file_preview);
    cwist_app_get(app, "/file/:id", handler_file_detail_get);
    cwist_app_get(app, "/file/download/:id", handler_file_download);
    cwist_app_get(app, "/file/download/:id/handshake", handler_file_download_handshake);
    cwist_app_get(app, "/file/download/:id/chunk/:chunk_index", handler_file_download_chunk);
    cwist_app_post(app, "/file/download/complete", handler_file_download_complete);
    cwist_app_post(app, "/file/upload/init", handler_file_upload_init);
    cwist_app_post(app, "/file/upload/status", handler_file_upload_status);
    cwist_app_post(app, "/file/upload/renegotiate", handler_file_upload_renegotiate);
    cwist_app_post(app, "/file/upload", handler_file_upload);
    cwist_app_post(app, "/file/upload/complete", handler_file_upload_complete);
    cwist_app_post(app, "/file/upload/cancel", handler_file_upload_cancel);
    cwist_app_post(app, "/file/delete", handler_file_delete);

    cwist_app_post(app, "/comment/new", handler_comment_new_post);
    cwist_app_post(app, "/comment/edit", handler_comment_edit_post);
    cwist_app_get(app, "/comment/:id/delete", handler_comment_delete_get);

    cwist_app_get(app, "/admin", handler_admin_dashboard);
    cwist_app_get(app, "/admin/users", handler_admin_users);
    cwist_app_post(app, "/admin/user/role", handler_admin_user_role);
    cwist_app_post(app, "/admin/files/drop", handler_admin_files_drop);
    cwist_app_get(app, "/admin/boards", handler_admin_boards_get);

    cwist_app_post(app, "/api/preview", handler_api_preview);
    cwist_app_post(app, "/api/upload", handler_api_upload);
    cwist_app_get(app, "/api/boards", handler_api_boards_json);
    cwist_app_get(app, "/api/my-files", handler_api_my_files);
    cwist_app_post(app, "/post/vote", handler_post_vote);
}

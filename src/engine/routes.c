#include "engine/routes.h"
#include "engine/async_route.h"
#include "handlers/handlers.h"

void engine_routes_register(cwist_app *app) {
    cwist_app_use(app, global_middleware);
    engine_async_set_hit_hook(post_bdr_hit);
    cwist_app_register_error_handler(app, CWIST_HTTP_NOT_FOUND, handler_not_found);

    engine_async_get(app, "/assets/img/:filename", handler_asset_img);
    engine_async_get(app, "/assets/uploads/:filename", handler_asset_upload);
    engine_async_get(app, "/assets/profile/:filename", handler_asset_profile_upload);
    engine_async_get(app, "/assets/tasfa/:scope/:filename/handshake", handler_asset_tasfa_handshake);
    engine_async_get(app, "/assets/tasfa/:scope/:filename/chunk/:chunk_index", handler_asset_tasfa_chunk);
    cwist_app_static_with_cache(app, "/assets/images", "public/images", "public, max-age=31536000, immutable");
    engine_async_get(app, "/assets/js/:filename", handler_static_js);
    engine_async_get(app, "/js/:filename", handler_static_js);
    engine_async_get(app, "/assets/css/:filename", handler_static_css);
    engine_async_get(app, "/assets/fonts/:filename", handler_static_font);
    cwist_app_static_with_cache(app, "/assets/media", "public/media", "public, max-age=31536000, immutable");
    engine_async_get(app, "/sw.js", handler_sw_js);
    engine_async_get(app, "/__tasfa_stream__/:stream_id", handler_tasfa_stream_placeholder);

    /* Routes */
    engine_async_get_cached(app, "/", handler_home);
    engine_async_get_cached(app, "/theme.json", handler_theme_json);
    engine_async_get_cached(app, "/themes.json", handler_themes_json);
    engine_async_get_cached(app, "/rss.xml", handler_rss_xml);
    engine_async_get_cached(app, "/sitemap.xml", handler_sitemap_xml);
    engine_async_get_cached(app, "/archive", handler_archive_get);
    engine_async_get_cached(app, "/archive/:ym", handler_archive_month_get);
    engine_async_get(app, "/tags", handler_tags_get);
    engine_async_get_cached(app, "/series", handler_series_index);
    engine_async_get_cached(app, "/series/:id", handler_series_get);
    engine_async_get_cached(app, "/series/:id/rss.xml", handler_series_rss_xml);
    engine_async_get(app, "/series/:id/edit", handler_series_edit_get);
    engine_async_post(app, "/series/:id/edit", handler_series_edit_post);
    engine_async_post(app, "/series/:id/delete", handler_series_delete_post);
    engine_async_get_cached(app, "/tag/:name", handler_tag_get);
    engine_async_get_cached(app, "/tag/:name/rss.xml", handler_tag_rss_xml);
    engine_async_get_cached(app, "/robots.txt", handler_robots_txt);
    engine_async_get_cached(app, "/llms.txt", handler_llms_txt);

    engine_async_get(app, "/login", handler_login_get);
    engine_async_post(app, "/login", handler_login_post);
    engine_async_get(app, "/logout", handler_logout);
    engine_async_get(app, "/register", handler_register_get);
    engine_async_post(app, "/register", handler_register_post);
    engine_async_get(app, "/verify-email", handler_verify_email_get);
    engine_async_post(app, "/resend-verification", handler_resend_verification_post);
    engine_async_post(app, "/unregister", handler_unregister_post);

    engine_async_get(app, "/profile", handler_profile_get);
    engine_async_post(app, "/profile", handler_profile_post);
    engine_async_get(app, "/account/settings", handler_account_settings_get);
    engine_async_post(app, "/account/settings", handler_account_settings_post);
    engine_async_get(app, "/account/password", handler_password_change_get);
    engine_async_post(app, "/account/password", handler_password_change_post);
    engine_async_get(app, "/user", handler_profile_get);
    engine_async_get_cached(app, "/user/:id", handler_user_profile_get);

    engine_async_get_cached(app, "/boards", handler_board_list);
    engine_async_get_cached(app, "/board", handler_board_list);
    engine_async_get(app, "/board/new", handler_board_new_get);
    engine_async_post(app, "/board/new", handler_board_new_post);
    engine_async_get(app, "/board/:id/edit", handler_board_edit_get);
    engine_async_get(app, "/board/edit", handler_board_list);
    engine_async_post(app, "/board/edit", handler_board_edit_post);
    engine_async_get(app, "/board/:id/delete", handler_board_delete);
    engine_async_get(app, "/board/:id/perms", handler_board_perms_get);
    engine_async_get(app, "/board/perms", handler_board_list);
    engine_async_post(app, "/board/perms", handler_board_perms_post);
    engine_async_get(app, "/board/perms/revoke", handler_board_list);
    engine_async_post(app, "/board/perms/revoke", handler_board_perms_revoke_post);

    engine_async_get_cached(app, "/posts", handler_home);
    engine_async_get_cached(app, "/search", handler_post_list);
    engine_async_get(app, "/drafts", handler_post_drafts);
    engine_async_get(app, "/account/drafts", handler_post_drafts);
    engine_async_get(app, "/post/new", handler_post_new_get);
    engine_async_post(app, "/post/new", handler_post_new_post);
    engine_async_get(app, "/post/delete/:id", handler_post_delete);
    engine_async_get(app, "/post/:id/edit", handler_post_edit_get);
    engine_async_post(app, "/post/:id/edit", handler_post_edit_post);
    engine_async_get_cached(app, "/post/:slug", handler_post_get);
    engine_async_get_cached(app, "/board/:slug/rss.xml", handler_board_rss_xml);
    engine_async_get_cached(app, "/board/:slug", handler_post_list);

    engine_async_get_cached(app, "/files", handler_file_repo);
    engine_async_get_cached(app, "/file", handler_file_repo);
    engine_async_get(app, "/file/preview/:id", handler_file_preview);
    engine_async_get(app, "/file/preview/:id/handshake", handler_file_preview_handshake);
    engine_async_get(app, "/file/preview/:id/chunk/:chunk_index", handler_file_preview_chunk);
    engine_async_get_cached(app, "/file/:id", handler_file_detail_get);
    engine_async_get(app, "/file/download/:id", handler_file_download);
    engine_async_get(app, "/file/download/:id/handshake", handler_file_download_handshake);
    engine_async_get(app, "/file/download/:id/chunk/:chunk_index", handler_file_download_chunk);
    engine_async_post(app, "/file/download/complete", handler_file_download_complete);
    engine_async_post(app, "/file/upload/init", handler_file_upload_init);
    engine_async_post(app, "/file/upload/status", handler_file_upload_status);
    engine_async_post(app, "/file/upload/renegotiate", handler_file_upload_renegotiate);
    engine_async_post(app, "/file/upload", handler_file_upload);
    engine_async_post(app, "/file/upload/complete", handler_file_upload_complete);
    engine_async_post(app, "/file/upload/cancel", handler_file_upload_cancel);
    engine_async_post(app, "/file/delete", handler_file_delete);

    engine_async_post(app, "/comment/new", handler_comment_new_post);
    engine_async_post(app, "/comment/edit", handler_comment_edit_post);
    engine_async_get(app, "/comment/:id/delete", handler_comment_delete_get);

    engine_async_get(app, "/notifications", handler_notifications_get);
    engine_async_get(app, "/notification", handler_notifications_get);

    /* Webmail */
    engine_async_get(app, "/mail", handler_mail_get);
    engine_async_get(app, "/mail/view", handler_mail_view_get);
    engine_async_post(app, "/mail/send", handler_mail_send_post);
    engine_async_post(app, "/mail/delete", handler_mail_delete_post);
    engine_async_post(app, "/mail/read", handler_mail_read_post);
    engine_async_post(app, "/mail/empty-trash", handler_mail_empty_trash_post);
    engine_async_get(app, "/mail/users", handler_mail_users_get);
    engine_async_post(app, "/admin/broadcast", handler_admin_broadcast_post);

    engine_async_get(app, "/dashboard", handler_dashboard);
    engine_async_get(app, "/admin", handler_admin_dashboard);
    engine_async_get(app, "/admin/dashboard", handler_admin_dashboard);
    engine_async_get(app, "/admin/users", handler_admin_users);
    engine_async_get(app, "/admin/user/role", handler_admin_users);
    engine_async_post(app, "/admin/user/role", handler_admin_user_role);
    engine_async_get(app, "/admin/files/drop", handler_file_repo);
    engine_async_post(app, "/admin/files/drop", handler_admin_files_drop);
    engine_async_get(app, "/admin/boards", handler_admin_boards_get);
    engine_async_get(app, "/admin/write-policy", handler_admin_dashboard);
    engine_async_post(app, "/admin/write-policy", handler_admin_write_policy_post);
    engine_async_post(app, "/admin/backup", handler_admin_backup_post);
    engine_async_post(app, "/admin/test-email", handler_admin_test_email_post);

    engine_async_post(app, "/api/preview", handler_api_preview);
    engine_async_post(app, "/api/upload", handler_api_upload);
    engine_async_get(app, "/api/boards", handler_api_boards_json);
    engine_async_get(app, "/api/tags", handler_api_tags);
    engine_async_get(app, "/api/my-files", handler_api_my_files);
    engine_async_post(app, "/api/translate", handler_api_translate);
    engine_async_get(app, "/metrics", handler_api_metrics);
    engine_async_post(app, "/api/reports", handler_api_reports);
    engine_async_post(app, "/post/vote", handler_post_vote);
    engine_async_post(app, "/report", handler_report_post);
    engine_async_get(app, "/admin/reports", handler_admin_reports_get);
    engine_async_post(app, "/admin/reports/action", handler_admin_reports_action);
}

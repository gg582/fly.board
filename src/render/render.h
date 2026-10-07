#ifndef FLYBOARD_RENDER_H
#define FLYBOARD_RENDER_H

#include <stdbool.h>
#include <cwist/core/sstring/sstring.h>
#include <cjson/cJSON.h>

/* Per-page <head> metadata for the next render_page() call on this thread.
 * Pointers are borrowed and must stay valid until that call, which consumes
 * and clears them. Unset fields fall back to site-wide values. */
typedef struct {
    const char *description;    /* plain text */
    const char *canonical_path; /* "/post/slug"; also og:url */
    const char *og_type;        /* "article" for posts; default "website" */
    const char *image;          /* root-relative or absolute */
    const char *published;      /* UTC "YYYY-MM-DD HH:MM:SS" */
    const char *modified;
    const char *author;
    cJSON *tags;                /* array of strings */
    const char *feed_path;      /* page-specific feed, e.g. "/tag/x/rss.xml" */
    const char *feed_title;
    bool noindex;               /* drafts and other private views */
    const char *lang;           /* page language; default g_config.language */
    cJSON *alternates;          /* [{lang, path}] translations incl. this page, for hreflang */
} render_page_meta;
void render_set_page_meta(const render_page_meta *meta);

/* Data for the next render_post_editor() call: series the writer may add to
 * ([{id,title}]) and posts they may name as the original of a translation
 * ([{id,title}]). Borrowed until that call. */
void render_set_editor_options(cJSON *series, cJSON *posts);
/* Translations of the board the next render_post_list() call shows
 * ([{lang, slug, title}]), plus that board's own language. */
void render_set_board_translations(const char *lang, cJSON *siblings);

void render_set_nav_profile(const char *display_name, const char *account_name);
void render_set_nav_notifications(int unread_count);
cwist_sstring *render_page(const char *title, const char *body_html, bool dark, const char *user_role, const char *profile_pic, bool is_mobile);
cwist_sstring *render_profile(cJSON *user, bool dark, const char *user_role, const char *profile_pic, bool is_own_profile, bool is_mobile);
cwist_sstring *render_account_settings(cJSON *user, bool dark, const char *viewer_role, const char *profile_pic, const char *error, bool is_mobile, int draft_count);
cwist_sstring *render_password_change(bool dark, const char *user_role, const char *profile_pic, const char *error, bool is_mobile);
cwist_sstring *render_login(bool dark, const char *error, bool is_mobile, const char *redirect_target);
cwist_sstring *render_register(bool dark, const char *error, bool is_mobile, cJSON *legal_docs);
cwist_sstring *render_post_list(cJSON *posts, cJSON *boards, bool dark, const char *user_role, int page, int total_pages, const char *board_slug, const char *search, const char *search_type, const char *profile_pic, int user_id, bool is_mobile, cJSON *children);
cwist_sstring *render_post_detail(cJSON *post, cJSON *files, cJSON *comments, bool dark, const char *user_role, bool pqc_verified, int vote_up, int vote_down, int user_vote, const char *profile_pic, const char *author_profile_pic, int user_id, const char *ephemeral_delete_pin, bool is_mobile);
cwist_sstring *render_file_detail(cJSON *file, cJSON *comments, bool dark, const char *user_role, const char *profile_pic, int user_id, bool is_mobile);
cwist_sstring *render_post_drafts(cJSON *posts, bool show_author, bool dark, const char *user_role, const char *profile_pic, bool is_mobile);
cwist_sstring *render_post_editor(cJSON *boards, cJSON *post, cJSON *files, int initial_board_id, bool dark, const char *user_role, const char *error, const char *profile_pic, bool is_mobile, int draft_count);
cwist_sstring *render_board_list(cJSON *boards, bool dark, const char *user_role, const char *profile_pic, bool is_mobile);
cwist_sstring *render_board_form(cJSON *board, cJSON *all_boards, bool dark, const char *error, const char *profile_pic, bool is_mobile, const char *user_role);
cwist_sstring *render_board_perms(cJSON *board, cJSON *perms, cJSON *users, bool dark, const char *msg, const char *profile_pic, bool is_mobile);
cwist_sstring *render_admin_dashboard(bool dark, const char *profile_pic, bool is_mobile, const char *msg, int open_reports, cJSON *backup, cJSON *flywire);
/* /admin/reports: groups from handlers/report.c, status is the active tab. */
cwist_sstring *render_admin_reports(cJSON *groups, const char *status, const char *msg, bool dark,
                                    const char *profile_pic, bool is_mobile);
cwist_sstring *render_user_admin(cJSON *users, bool dark, const char *profile_pic, bool is_mobile);
cwist_sstring *render_admin_boards(cJSON *boards, cJSON *tree, bool dark, const char *profile_pic, bool is_mobile);
cwist_sstring *render_file_repo(cJSON *files, bool dark, const char *user_role, int user_id, const char *profile_pic, bool is_mobile);
cwist_sstring *render_notifications_page(cJSON *notifs, bool dark, const char *user_role, const char *profile_pic, bool is_mobile);
cwist_sstring *render_markdown_to_html(const char *md);
/* Post index pages (tag, month): heading, optional lead line, rows, pager
 * links to base_path?page=N. feed_path adds a subscribe link. */
cwist_sstring *render_post_index(const char *heading, const char *lead, cJSON *posts, int page, int total_pages,
                                 const char *base_path, const char *feed_path, bool dark, const char *user_role,
                                 const char *profile_pic, int user_id, bool is_mobile);
/* Series pages. */
cwist_sstring *render_series_index(cJSON *series, bool dark, const char *user_role, const char *profile_pic, bool is_mobile);
cwist_sstring *render_series_detail(cJSON *series, cJSON *posts, cJSON *siblings, const char *lang, bool can_edit,
                                    bool dark, const char *user_role, const char *profile_pic, bool is_mobile);
cwist_sstring *render_series_edit(cJSON *series, cJSON *posts, cJSON *pair_choices, const char *lang, int paired_with,
                                  const char *error, bool dark, const char *user_role, const char *profile_pic, bool is_mobile);
/* /archive: tag cloud plus posts per month. */
cwist_sstring *render_archive(cJSON *months, cJSON *tags, cJSON *series, bool dark, const char *user_role,
                              const char *profile_pic, bool is_mobile);

/* Webmail (src/render/render_mail.c). */
cwist_sstring *render_mail_list(cJSON *emails, const char *folder, int page, int total_pages,
                                int unread, const char *msg, bool dark, const char *user_role,
                                const char *profile_pic, bool is_mobile);
cwist_sstring *render_mail_view(cJSON *email, bool dark, const char *user_role,
                                const char *profile_pic, bool is_mobile);

#endif

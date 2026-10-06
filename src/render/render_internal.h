#ifndef FLYBOARD_RENDER_INTERNAL_H
#define FLYBOARD_RENDER_INTERNAL_H

#include <cwist/core/html/builder.h>
#include <cwist/core/sstring/sstring.h>
#include <cjson/cJSON.h>
#include <stdbool.h>
#include <stddef.h>

cwist_html_element_t *nav_link(const char *href, const char *label);
cwist_sstring *build_form(const char *title, const char *action, const char *method,
                          const char *fields_html, const char *btn_text, const char *error, bool dark);
char *format_join_date(const char *iso_date);

extern const char *login_register_script;
extern const char *code_copy_script;
int json_int(cJSON *obj, const char *key, int def);
/* Shown in place of the comment form when the write policy excludes the viewer. */
void render_comment_closed_note(cwist_sstring *b, const char *user_role);
void render_comment_node(cwist_sstring *b, cJSON *comment, cJSON *all_comments, int depth, int current_user_id, const char *user_role, int target_id);

/* Percent-encode @p s as one URL path segment (e.g. a tag name). */
void render_append_url_segment(cwist_sstring *b, const char *s);
/* Plain-text excerpt of markdown: markup, code blocks and images dropped,
 * whitespace collapsed, cut at @p max_cp code points with an ellipsis. */
void render_text_excerpt(const char *md, size_t max_cp, char *out, size_t out_size);
/* URL of the first usable image in markdown, for og:image. */
bool render_find_lead_image(const char *md, char *out, size_t out_size);
/* Reading time at 220 words a minute; 0 for empty text. */
int render_reading_minutes(const char *md);

/* Display name of a language code ("en" -> "English"); the code itself
 * when unknown. */
const char *render_lang_name(const char *code);
/* <option>s for a language <select>, with an empty "(site default)" entry
 * first when include_unset. */
void render_append_lang_options(cwist_sstring *b, const char *selected, bool include_unset);
/* "Also in: English · 日本語" links for translations [{lang, path, title}]. */
void render_append_translation_links(cwist_sstring *b, const char *label, cJSON *links);

#endif

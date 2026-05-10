#ifndef FLYBOARD_RENDER_INTERNAL_H
#define FLYBOARD_RENDER_INTERNAL_H

#include <cwist/core/html/builder.h>
#include <cwist/core/sstring/sstring.h>

cwist_html_element_t *nav_link(const char *href, const char *label);
cwist_sstring *build_form(const char *title, const char *action, const char *method,
                          const char *fields_html, const char *btn_text, const char *error, bool dark);
char *format_join_date(const char *iso_date);

extern const char *login_register_script;
int json_int(cJSON *obj, const char *key, int def);
void render_comment_node(cwist_sstring *b, cJSON *comment, cJSON *all_comments, int depth, int current_user_id, const char *user_role, int target_id);

#endif

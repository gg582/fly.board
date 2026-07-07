#ifndef DOCKER_BLOG_AUTH_H
#define DOCKER_BLOG_AUTH_H

#include <cwist/net/http/http.h>
#include <stdbool.h>

#define SESSION_COOKIE_NAME "docker_blog_session"

/* Session lifetime in seconds. Used for both JWT claims and cookie Max-Age.
 * 12 hours gives users a generous workday window without constant re-auth. */
#define AUTH_SESSION_LIFETIME 43200

bool auth_jwt_init(const char *secret_path);
const char *auth_jwt_secret(void);

bool auth_hash_password(const char *password, char *out_hash, size_t out_len);
bool auth_verify_password(const char *password, const char *hash);
char *auth_jwt_issue(int user_id, const char *username, const char *role);
bool auth_jwt_verify_from_request(cwist_http_request *req, int *out_user_id, char *out_role, size_t role_len);
bool auth_is_logged_in(cwist_http_request *req, int *out_user_id, char *out_role, size_t role_len);
bool auth_has_session_cookie(cwist_http_request *req);
bool auth_require_login(cwist_http_request *req, cwist_http_response *res, int *out_user_id, char *out_role, size_t role_len);
bool auth_require_admin(cwist_http_request *req, cwist_http_response *res);

bool auth_admin_load(const char *path);
bool auth_admin_check(const char *username, const char *password);

void auth_session_hint_update(cwist_http_request *req, int user_id, const char *role);
void auth_session_hint_remove(cwist_http_request *req);

#endif

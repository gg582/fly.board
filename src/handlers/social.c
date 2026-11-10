/* Password recovery, bookmarks and personal blocks.
 *
 * Every POST here runs on the primary: a replica proxies all non-GET
 * requests (handlers.c global_middleware). The GET pages only read rows
 * that FlyWire replicates (bookmarks, user_blocks), except /reset-password,
 * which does not touch the database at all: the token is checked by the
 * POST, so the form also works when it is served by a replica. */
#include "handlers_internal.h"
#include "../engine/bdr.h"
#include "../utils/email.h"
#include <openssl/rand.h>
#include <ctype.h>

#define RESET_TTL_SELF   3600L       /* emailed by the user's own request */
#define RESET_TTL_ADMIN  (24 * 3600L) /* issued by an administrator */
#define RESET_RESEND_GAP 120L        /* one mail per account per 2 minutes */

static bool reset_token(char out[65]) {
    unsigned char raw[32];
    if (RAND_bytes(raw, sizeof(raw)) != 1) return false;
    for (size_t i = 0; i < sizeof(raw); i++) snprintf(out + i * 2, 3, "%02x", raw[i]);
    return true;
}

static bool token_shape_ok(const char *token) {
    if (!token || strlen(token) != 64) return false;
    for (const char *p = token; *p; p++)
        if (!isxdigit((unsigned char)*p) || isupper((unsigned char)*p)) return false;
    return true;
}

static void reset_link(const char *token, char *out, size_t out_size) {
    const char *root = g_config.root_url[0] ? g_config.root_url : "/";
    size_t n = strlen(root);
    snprintf(out, out_size, "%s%sreset-password?token=%s", root, (n && root[n - 1] == '/') ? "" : "/", token);
}

/* Accounts a reset may touch: live, not ghosted, not the admin.settings
 * account (its password lives in admin.settings, not in users). */
static bool resettable(cJSON *user) {
    if (!user) return false;
    int id = json_int(user, "id", 0);
    return id > 0 && !auth_is_site_admin(id) && json_int(user, "ghosted", 0) == 0 &&
           json_int(user, "active", 1) != 0;
}

static const char *usable_email(cJSON *user) {
    const char *email = json_string_or_empty(user, "email");
    if (!email[0] || !strchr(email, '@') || strstr(email, "@deleted.invalid")) return NULL;
    return email;
}

static bool mail_reset_link(cJSON *user, const char *link, long ttl) {
    const char *email = usable_email(user);
    if (!email) return false;
    char body[1400];
    snprintf(body, sizeof(body),
             "Hello %s,\n\nA password reset was requested for your account. Open this link to choose a new password:\n\n%s\n\n"
             "The link works once and expires in %ld hour%s. Every device signed in to the account is signed out.\n"
             "If you did not ask for this, ignore this mail; your password stays the same.\n",
             json_string_or_empty(user, "username"), link, ttl / 3600, ttl / 3600 == 1 ? "" : "s");
    return email_send(email, "Reset your password", body);
}

void handler_forgot_password_get(cwist_http_request *req, cwist_http_response *res) {
    send_html_res(res, render_forgot_password(is_dark(req), NULL, false, is_mobile_request(req)));
}

void handler_forgot_password_post(cwist_http_request *req, cwist_http_response *res) {
    bool dark = is_dark(req);
    bool mobile = is_mobile_request(req);
    cwist_query_map *kv = cwist_query_map_create();
    cwist_query_map_parse(kv, req->body && req->body->data ? req->body->data : "");
    spam_verdict_t verdict = spam_guard_check(req, 0, "", cwist_query_map_get(kv, SPAM_FIELD_TRAP),
                                              cwist_query_map_get(kv, SPAM_FIELD_TOKEN));
    if (verdict != SPAM_OK) {
        cwist_query_map_destroy(kv);
        spam_guard_reject(res, verdict);
        return;
    }
    char login[256];
    const char *raw = cwist_query_map_get(kv, "login");
    snprintf(login, sizeof(login), "%s", raw ? raw : "");
    cwist_query_map_destroy(kv);
    char *s = login;
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = '\0';

    /* One answer for every outcome, so the form cannot be used to find out
     * which usernames or addresses have accounts. */
    const char *generic = "If that matches an account with an email address, a reset link is on its way. Check your inbox.";
    if (!s[0]) {
        send_html_res(res, render_forgot_password(dark, "Enter your username or email.", false, mobile));
        return;
    }
    db_password_reset_sweep(req->db);
    cJSON *user = strchr(s, '@') ? db_user_get_by_email(req->db, s) : db_user_get_by_username(req->db, s);
    if (resettable(user) && usable_email(user)) {
        int uid = json_int(user, "id", 0);
        long age = db_password_reset_age(req->db, uid);
        char token[65], link[768];
        if (age >= 0 && age < RESET_RESEND_GAP) {
            CWIST_LOG_INFO("Password reset: resend suppressed uid=%d age=%lds", uid, age);
        } else if (reset_token(token) &&
                   db_password_reset_create(req->db, uid, token, (long)time(NULL) + RESET_TTL_SELF)) {
            reset_link(token, link, sizeof(link));
            bool sent = mail_reset_link(user, link, RESET_TTL_SELF);
            audit_record(uid, "auth.password.reset_request", "self", sent ? "success" : "failure", "emailed link");
            CWIST_LOG_INFO("Password reset requested: uid=%d mailed=%d", uid, sent);
        }
    } else {
        CWIST_LOG_INFO("Password reset requested for an unknown or non-resettable account");
    }
    if (user) cJSON_Delete(user);
    send_html_res(res, render_forgot_password(dark, generic, true, mobile));
}

void handler_reset_password_get(cwist_http_request *req, cwist_http_response *res) {
    const char *token = cwist_query_map_get(req->query_params, "token");
    /* The site-wide Referrer-Policy (strict-origin-when-cross-origin)
     * already keeps the token path out of cross-origin requests. */
    cwist_http_header_add(&res->headers, "Cache-Control", "no-store");
    if (!token_shape_ok(token)) {
        send_html_res(res, render_forgot_password(is_dark(req), "That reset link is not valid. Request a new one.", false,
                                                  is_mobile_request(req)));
        return;
    }
    send_html_res(res, render_reset_password(is_dark(req), token, NULL, is_mobile_request(req)));
}

void handler_reset_password_post(cwist_http_request *req, cwist_http_response *res) {
    bool dark = is_dark(req);
    bool mobile = is_mobile_request(req);
    cwist_http_header_add(&res->headers, "Cache-Control", "no-store");
    cwist_query_map *kv = cwist_query_map_create();
    cwist_query_map_parse(kv, req->body && req->body->data ? req->body->data : "");
    const char *token = cwist_query_map_get(kv, "token");
    const char *pw = cwist_query_map_get(kv, "new_password");
    const char *confirm = cwist_query_map_get(kv, "confirm_password");
    char token_copy[65] = "";
    if (token_shape_ok(token)) snprintf(token_copy, sizeof(token_copy), "%s", token);

    const char *error = NULL;
    if (!token_copy[0]) error = "That reset link is not valid. Request a new one.";
    else if (!pw || !confirm || strlen(pw) < 6) error = "Use at least 6 characters.";
    else if (strlen(pw) > 1024) error = "That password is too long.";
    else if (strcmp(pw, confirm) != 0) error = "The passwords do not match.";
    if (error) {
        cwist_query_map_destroy(kv);
        if (!token_copy[0])
            send_html_res(res, render_forgot_password(dark, error, false, mobile));
        else
            send_html_res(res, render_reset_password(dark, token_copy, error, mobile));
        return;
    }
    char hash[256];
    bool hashed = auth_hash_password(pw, hash, sizeof(hash));
    cwist_query_map_destroy(kv);
    if (!hashed) {
        send_html_res(res, render_reset_password(dark, token_copy, "Server error; try again.", mobile));
        return;
    }
    /* Only consume once the new password is ready, so a server error does
     * not burn the link. */
    int uid = db_password_reset_peek(req->db, token_copy);
    cJSON *user = uid > 0 ? db_user_get_by_id(req->db, uid) : NULL;
    bool ok = resettable(user) && db_password_reset_consume(req->db, token_copy) == uid &&
              db_user_update_password(req->db, uid, hash);
    if (ok) {
        /* Following an emailed or admin-issued link proves the address. */
        if (json_int(user, "email_verified", 1) == 0) db_user_set_email_verified(req->db, uid, true);
        db_user_revoke_sessions(req->db, uid);
        audit_record(uid, "auth.password.reset", "self", "success", "reset link used");
        CWIST_LOG_INFO("Password reset completed: uid=%d", uid);
    } else {
        CWIST_LOG_WARN("Password reset refused: invalid, expired or used token");
    }
    if (user) cJSON_Delete(user);
    if (!ok) {
        send_html_res(res, render_forgot_password(dark, "That reset link is invalid, expired or already used. Request a new one.",
                                                  false, mobile));
        return;
    }
    send_html_res(res, render_login(dark, "Password changed. Sign in with your new password.", mobile, NULL));
}

/* POST /admin/user/reset-link (id): an administrator issues a one-time link
 * for a member, e.g. one without an email address. It is shown to the
 * admin and, when the account has an address, mailed as well. */
void handler_admin_user_reset_link_post(cwist_http_request *req, cwist_http_response *res) {
    if (!auth_require_admin(req, res)) return;
    int admin_uid = 0;
    char role[32] = {0};
    auth_is_logged_in(req, &admin_uid, role, sizeof(role));
    cwist_query_map *kv = cwist_query_map_create();
    cwist_query_map_parse(kv, req->body && req->body->data ? req->body->data : "");
    int target = atoi(cwist_query_map_get(kv, "id") ? cwist_query_map_get(kv, "id") : "0");
    cwist_query_map_destroy(kv);
    cJSON *user = target > 0 ? db_user_get_by_id(req->db, target) : NULL;
    char token[65], link[768];
    if (!resettable(user) || !reset_token(token) ||
        !db_password_reset_create(req->db, target, token, (long)time(NULL) + RESET_TTL_ADMIN)) {
        CWIST_LOG_WARN("Admin reset link refused: uid=%d", target);
        if (user) cJSON_Delete(user);
        redirect(res, "/admin/users");
        return;
    }
    reset_link(token, link, sizeof(link));
    bool mailed = mail_reset_link(user, link, RESET_TTL_ADMIN);
    char target_buf[32];
    snprintf(target_buf, sizeof(target_buf), "user:%d", target);
    audit_record(admin_uid, "admin.user.password_reset_link", target_buf, "success", mailed ? "shown and mailed" : "shown");
    CWIST_LOG_INFO("Admin issued reset link: target=%d mailed=%d", target, mailed);
    char *pp = get_profile_pic(req->db, admin_uid, role);
    cwist_http_header_add(&res->headers, "Cache-Control", "no-store");
    send_html_res(res, render_admin_reset_link(is_dark(req), role, pp, json_string_or_empty(user, "username"), link, mailed,
                                               is_mobile_request(req)));
    free(pp);
    cJSON_Delete(user);
}

/* ---- Bookmarks ---- */

static bool wants_json(cwist_http_request *req) {
    const char *xr = cwist_http_header_get(req->headers, "X-Requested-With");
    const char *accept = cwist_http_header_get(req->headers, "Accept");
    return (xr && strcasecmp(xr, "fetch") == 0) || (accept && strstr(accept, "application/json"));
}

static void send_json_body(cwist_http_response *res, int status, cJSON *obj) {
    char *json = cJSON_PrintUnformatted(obj);
    res->status_code = (cwist_http_status_t)status;
    cwist_http_header_add(&res->headers, "Content-Type", "application/json");
    cwist_http_header_add(&res->headers, "Cache-Control", "no-store");
    cwist_sstring_assign(res->body, json ? json : "{}");
    if (json) cwist_free(json);
    cJSON_Delete(obj);
}

/* POST /bookmark (post_id, action=add|remove). */
void handler_bookmark_post(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0;
    char role[32] = {0};
    if (!auth_require_login(req, res, &uid, role, sizeof(role))) return;
    cwist_query_map *kv = cwist_query_map_create();
    cwist_query_map_parse(kv, req->body && req->body->data ? req->body->data : "");
    int post_id = atoi(cwist_query_map_get(kv, "post_id") ? cwist_query_map_get(kv, "post_id") : "0");
    const char *action = cwist_query_map_get(kv, "action");
    bool on = !(action && strcmp(action, "remove") == 0);
    cJSON *post = post_id > 0 ? db_post_get_by_id(req->db, post_id) : NULL;
    bool visible = post && ((post_is_public(post) && post_lock_viewer_ok(req, post, uid, role)) ||
                            is_author_or_admin(post, uid, role));
    char slug[256] = "";
    if (post) snprintf(slug, sizeof(slug), "%s", json_string_or_empty(post, "slug"));
    if (post) cJSON_Delete(post);
    bool ok = visible && db_bookmark_set(req->db, uid, post_id, on);
    if (ok && slug[0]) page_cache_invalidate_post(slug);
    const char *back = cwist_query_map_get(kv, "return");
    char back_copy[512] = "";
    if (back && back[0] == '/' && back[1] != '/' && back[1] != '\\') snprintf(back_copy, sizeof(back_copy), "%s", back);
    cwist_query_map_destroy(kv);
    if (wants_json(req)) {
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddBoolToObject(obj, "ok", ok);
        cJSON_AddBoolToObject(obj, "bookmarked", ok ? on : db_bookmark_has(req->db, uid, post_id));
        if (!ok) cJSON_AddStringToObject(obj, "error", visible ? "Could not save the bookmark" : "Post not found");
        send_json_body(res, ok ? 200 : (visible ? 500 : 404), obj);
        return;
    }
    redirect(res, back_copy[0] ? back_copy : "/bookmarks");
}

/* GET /bookmarks?page=N */
void handler_bookmarks_get(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0;
    char role[32] = {0};
    if (!auth_require_login(req, res, &uid, role, sizeof(role))) return;
    const char *page_str = cwist_query_map_get(req->query_params, "page");
    int page = page_str ? atoi(page_str) : 1;
    if (page < 1) page = 1;
    int total = db_bookmark_count(req->db, uid);
    int pages = total > 0 ? (total + 19) / 20 : 1;
    if (page > pages) page = pages;
    cJSON *posts = db_bookmark_list(req->db, uid, 20, (page - 1) * 20);
    char lead[96];
    snprintf(lead, sizeof(lead), total == 1 ? "1 saved post" : "%d saved posts", total);
    if (total == 0) snprintf(lead, sizeof(lead), "No bookmarks yet. Use the Bookmark button on a post to save it here.");
    char *pp = get_profile_pic(req->db, uid, role);
    cwist_http_header_add(&res->headers, "Cache-Control", "private, no-store");
    send_html_res(res, render_post_index("Bookmarks", lead, posts, page, pages, "/bookmarks", NULL, is_dark(req), role, pp,
                                         uid, is_mobile_request(req)));
    free(pp);
    if (posts) cJSON_Delete(posts);
}

/* ---- Personal blocks ---- */

/* POST /user/:id/block (action=block|unblock, return=/path) */
void handler_user_block_post(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0;
    char role[32] = {0};
    if (!auth_require_login(req, res, &uid, role, sizeof(role))) return;
    const char *id_str = cwist_query_map_get(req->path_params, "id");
    int target = id_str ? atoi(id_str) : 0;
    cwist_query_map *kv = cwist_query_map_create();
    cwist_query_map_parse(kv, req->body && req->body->data ? req->body->data : "");
    const char *action = cwist_query_map_get(kv, "action");
    bool on = !(action && strcmp(action, "unblock") == 0);
    const char *back = cwist_query_map_get(kv, "return");
    char back_copy[512] = "";
    if (back && back[0] == '/' && back[1] != '/' && back[1] != '\\') snprintf(back_copy, sizeof(back_copy), "%s", back);
    cwist_query_map_destroy(kv);

    cJSON *user = (target > 0 && target != uid) ? db_user_get_by_id(req->db, target) : NULL;
    bool ok = user && db_user_block_set(req->db, uid, target, on);
    if (user) cJSON_Delete(user);
    if (ok) {
        /* Lists, post pages and comment threads this member sees change:
         * the shared version reaches every worker (see page_cache_key_*). */
        engine_bdr_bump_blocks();
        CWIST_LOG_INFO("User %s: uid=%d target=%d", on ? "blocked" : "unblocked", uid, target);
    }
    if (wants_json(req)) {
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddBoolToObject(obj, "ok", ok);
        cJSON_AddBoolToObject(obj, "blocked", ok ? on : db_user_blocks(req->db, uid, target));
        send_json_body(res, ok ? 200 : 404, obj);
        return;
    }
    redirect(res, back_copy[0] ? back_copy : "/account/blocks");
}

/* GET /account/blocks */
void handler_account_blocks_get(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0;
    char role[32] = {0};
    if (!auth_require_login(req, res, &uid, role, sizeof(role))) return;
    cJSON *blocks = db_user_block_list(req->db, uid);
    char *pp = get_profile_pic(req->db, uid, role);
    cwist_http_header_add(&res->headers, "Cache-Control", "private, no-store");
    send_html_res(res, render_blocked_users(blocks, is_dark(req), role, pp, NULL, is_mobile_request(req)));
    free(pp);
    if (blocks) cJSON_Delete(blocks);
}

/* ---- Content filtering for a viewer's blocks ---- */

static int cmp_int(const void *a, const void *b) {
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}

/* Items in arr whose author_key belongs to someone the viewer blocked are
 * removed (remove=true) or marked "blocked":true for the renderer. */
void social_apply_blocks(cwist_db *db, int viewer_uid, cJSON *arr, const char *author_key, bool remove) {
    if (!db || viewer_uid <= 0 || !cJSON_IsArray(arr)) return;
    int *ids = NULL;
    int n = db_user_block_ids(db, viewer_uid, &ids);
    if (n <= 0) return;
    for (int i = cJSON_GetArraySize(arr) - 1; i >= 0; i--) {
        cJSON *item = cJSON_GetArrayItem(arr, i);
        int author = json_int(item, author_key, 0);
        if (author <= 0 || !bsearch(&author, ids, (size_t)n, sizeof(int), cmp_int)) continue;
        if (remove) {
            cJSON_DeleteItemFromArray(arr, i);
        } else if (!cJSON_GetObjectItem(item, "blocked")) {
            cJSON_AddBoolToObject(item, "blocked", true);
        }
    }
    free(ids);
}

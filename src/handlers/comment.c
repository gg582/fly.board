#define _POSIX_C_SOURCE 200809L
#include "handlers_internal.h"
#include "config/write_policy.h"
#include "db/moderation.h"

/* Comment target, looked up once per request: visibility, cache
 * invalidation, notification and the redirect all need the same post. */
typedef struct {
    bool is_post;
    int id;
    int board_id;
    int author_id;
    char slug[256];
} comment_target_t;

static void comment_target_fill(comment_target_t *t, cJSON *post) {
    t->board_id = json_int(post, "board_id", 0);
    t->author_id = json_int(post, "user_id", 0);
    snprintf(t->slug, sizeof(t->slug), "%s", json_string_or_empty(post, "slug"));
}

static void invalidate_comment_slug(const char *target_type, const char *slug) {
    if (target_type && strcmp(target_type, "post") == 0) {
        /* Also bumps the shared per-post version that every worker's page
         * cache key carries, so no worker keeps the old thread. */
        page_cache_invalidate_post(slug && slug[0] ? slug : NULL);
    } else if (target_type && strcmp(target_type, "file") == 0) {
        /* File detail pages are cached by id; comment events are rare enough
         * that clearing everything is acceptable. */
        page_cache_invalidate_all();
    }
}

static void invalidate_comment_target(cwist_db *db, const char *target_type, int target_id) {
    if (!target_type || !target_type[0] || target_id <= 0) return;
    if (strcmp(target_type, "post") == 0) {
        cJSON *post = db_post_get_by_id(db, target_id);
        if (!post) return;
        invalidate_comment_slug(target_type, json_string_or_empty(post, "slug"));
        cJSON_Delete(post);
    } else {
        invalidate_comment_slug(target_type, NULL);
    }
}

/* Notify the post author (new comment) or the parent comment author (reply)
 * and broadcast the event to federated nodes. Silently skips notification
 * when there is no resolvable recipient, the recipient is the actor, or any
 * lookup fails. */
static void notify_comment_recipient(cwist_db *db, int actor_uid, const char *actor_name,
                                     const comment_target_t *target, int parent_id,
                                     const char *content, int comment_id) {
    int recipient = 0;
    const char *kind = NULL;
    if (parent_id > 0) {
        cJSON *parent = db_comment_get_by_id(db, parent_id);
        if (!parent) return;
        recipient = json_int(parent, "user_id", 0);
        cJSON_Delete(parent);
        kind = "reply";
    } else if (target->is_post) {
        recipient = target->author_id;
        kind = "comment";
    }
    if (!kind || recipient <= 0 || recipient == actor_uid) return;

    const char *slug = target->is_post ? target->slug : "";
    int post_id = target->is_post && slug[0] ? target->id : 0;
    /* Comments are Markdown: notify with plain text cut on a code point. */
    char excerpt[121];
    render_text_excerpt(content, 60, excerpt, sizeof(excerpt));
    if (!excerpt[0]) snprintf(excerpt, sizeof(excerpt), "%s", "(attachment)");

    db_notification_create(db, recipient, actor_name, kind, post_id, slug, comment_id, excerpt);
    fly_nats_publish_comment(g_config.root_url, actor_name, kind, slug, recipient, excerpt, comment_id);
}

static void comment_reject(cwist_http_response *res, cwist_http_status_t status, const char *msg) {
    res->status_code = status;
    cwist_http_header_add(&res->headers, "Content-Type", "text/plain; charset=utf-8");
    cwist_sstring_assign(res->body, msg);
}

void handler_comment_new_post(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0;
    char role[32] = {0};
    auth_is_logged_in(req, &uid, role, sizeof(role));
    if (!write_policy_can_comment(role)) {
        CWIST_LOG_WARN("Comment creation refused by write policy: uid=%d", uid);
        if (!role[0]) {
            /* /comment/new has no GET page to come back to after login. */
            redirect(res, "/login");
        } else {
            res->status_code = CWIST_HTTP_FORBIDDEN;
            cwist_sstring_assign(res->body, "Forbidden: only admins may comment");
        }
        return;
    }
    cwist_query_map *kv = cwist_query_map_create();
    if (req->body && req->body->data) cwist_query_map_parse(kv, req->body->data);
    const char *target_type = cwist_query_map_get(kv, "target_type");
    const char *target_id_str = cwist_query_map_get(kv, "target_id");
    const char *parent_id_str = cwist_query_map_get(kv, "parent_id");
    const char *content = cwist_query_map_get(kv, "content");
    const char *author_name_input = cwist_query_map_get(kv, "author_name");
    const char *referer = cwist_http_header_get(req->headers, "Referer");
    spam_verdict_t verdict = spam_guard_check(req, uid, role, cwist_query_map_get(kv, SPAM_FIELD_TRAP),
                                              cwist_query_map_get(kv, SPAM_FIELD_TOKEN));
    if (verdict != SPAM_OK) {
        cwist_query_map_destroy(kv);
        spam_guard_reject(res, verdict);
        return;
    }

    comment_target_t target = {0};
    target.id = target_id_str ? atoi(target_id_str) : 0;
    target.is_post = target_type && strcmp(target_type, "post") == 0;
    bool known_type = target.is_post || (target_type && strcmp(target_type, "file") == 0);
    if (!known_type || target.id <= 0 || !content || !content[0]) {
        CWIST_LOG_WARN("Comment creation failed: missing fields");
        cwist_query_map_destroy(kv);
        comment_reject(res, CWIST_HTTP_BAD_REQUEST, "The comment is empty or incomplete. Go back and try again.");
        return;
    }
    if (strlen(content) > COMMENT_CONTENT_MAX) {
        cwist_query_map_destroy(kv);
        comment_reject(res, (cwist_http_status_t)413, "Comments are limited to " COMMENT_CONTENT_MAX_STR " characters.");
        return;
    }
    if (target.is_post) {
        /* Unpublished posts take comments only from those who can see them. */
        cJSON *post = db_post_get_by_id(req->db, target.id);
        bool visible = post && (post_is_public(post) || is_author_or_admin(post, uid, role)) &&
                       post_lock_viewer_ok(req, post, uid, role);
        if (post) comment_target_fill(&target, post);
        if (post) cJSON_Delete(post);
        if (!visible) {
            cwist_query_map_destroy(kv);
            comment_reject(res, CWIST_HTTP_NOT_FOUND, "Not found");
            return;
        }
    }

    int parent_id = parent_id_str ? atoi(parent_id_str) : 0;
    if (parent_id > 0) {
        /* A reply must stay in its parent's thread. */
        cJSON *parent = db_comment_get_by_id(req->db, parent_id);
        bool same = parent && json_int(parent, "target_id", 0) == target.id &&
                    strcmp(json_string_or_empty(parent, "target_type"), target_type) == 0;
        if (parent) cJSON_Delete(parent);
        if (!same) parent_id = 0;
    }

    const char *author_name = NULL;
    char author_name_buf[128] = {0};
    if (uid > 0) {
        cJSON *u = db_user_get_by_id(req->db, uid);
        if (u) {
            const char *uname = json_string_or_empty(u, "username");
            if (uname[0]) {
                snprintf(author_name_buf, sizeof(author_name_buf), "%s", uname);
                author_name = author_name_buf;
            }
            cJSON_Delete(u);
        }
        if (!author_name) author_name = strcmp(role, "admin") == 0 ? "Admin" : "User";
    } else {
        author_name = (author_name_input && author_name_input[0]) ? author_name_input : "Anonymous";
    }

    moderation_decision_t moderation = {MODERATION_ALLOW, ""};
    if (!db_board_user_can_moderate(req->db, target.board_id, uid, role))
        moderation = moderation_check_submission(req->db, uid, role, MODERATION_COMMENT, content);
    if (moderation.action == MODERATION_REJECT) {
        cwist_query_map_destroy(kv);
        comment_reject(res, CWIST_HTTP_FORBIDDEN, "New accounts may post three comments in their first 24 hours");
        return;
    }
    bool awaiting_review = moderation.action == MODERATION_PENDING;
    int new_comment_id = awaiting_review
        ? db_moderation_create_pending_comment(req->db, target_type, target.id, uid, author_name, parent_id, content, moderation.reason)
        : db_comment_create(req->db, target_type, target.id, uid, author_name, parent_id, content);
    if (new_comment_id <= 0) {
        CWIST_LOG_ERROR("Comment creation failed: target_type=%s target_id=%d uid=%d", target_type, target.id, uid);
        cwist_query_map_destroy(kv);
        comment_reject(res, CWIST_HTTP_INTERNAL_ERROR, "The comment could not be saved. Please try again.");
        return;
    }
    CWIST_LOG_INFO("Comment created: target_type=%s target_id=%d uid=%d pending=%d", target_type, target.id, uid, awaiting_review);
    if (!awaiting_review) {
        invalidate_comment_slug(target_type, target.slug);
        if (g_config.realtime_enabled && target.is_post)
            (void)fly_nats_publish_realtime_comment(target.id, new_comment_id);
        notify_comment_recipient(req->db, uid, author_name, &target, parent_id, content, new_comment_id);
    } else {
        fly_nats_publish_moderation(req->db, MODERATION_COMMENT, new_comment_id);
    }

    /* Land on the new comment. A pending one is not shown yet. */
    char location[384];
    if (target.is_post && target.slug[0]) {
        if (awaiting_review) snprintf(location, sizeof(location), "/post/%s#comments", target.slug);
        else snprintf(location, sizeof(location), "/post/%s#comment-%d", target.slug, new_comment_id);
    } else if (!target.is_post) {
        if (awaiting_review) snprintf(location, sizeof(location), "/file/%d", target.id);
        else snprintf(location, sizeof(location), "/file/%d#comment-%d", target.id, new_comment_id);
    } else {
        location[0] = '\0';
    }
    cwist_query_map_destroy(kv);
    if (location[0]) {
        redirect(res, location);
        /* 303: reloading the landing page must not resubmit the comment. */
        res->status_code = (cwist_http_status_t)303;
    } else {
        redirect_referer_safe(res, referer, "/");
    }
}

void handler_comment_edit_post(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0;
    char role[32] = {0};
    if (!auth_require_login(req, res, &uid, role, sizeof(role))) return;
    cwist_query_map *kv = cwist_query_map_create();
    if (req->body && req->body->data) cwist_query_map_parse(kv, req->body->data);
    const char *id_str = cwist_query_map_get(kv, "id");
    const char *content = cwist_query_map_get(kv, "content");
    const char *referer = cwist_http_header_get(req->headers, "Referer");
    if (content && strlen(content) > COMMENT_CONTENT_MAX) {
        cwist_query_map_destroy(kv);
        comment_reject(res, (cwist_http_status_t)413, "Comments are limited to " COMMENT_CONTENT_MAX_STR " characters.");
        return;
    }
    if (id_str && content && content[0]) {
        cJSON *comment = db_comment_get_by_id(req->db, atoi(id_str));
        if (db_comment_update(req->db, atoi(id_str), uid, content)) {
            CWIST_LOG_INFO("Comment updated: id=%s uid=%d", id_str, uid);
            if (comment) {
                cJSON *tt = cJSON_GetObjectItem(comment, "target_type");
                cJSON *ti = cJSON_GetObjectItem(comment, "target_id");
                if (tt && cJSON_IsString(tt) && ti && cJSON_IsNumber(ti)) {
                    invalidate_comment_target(req->db, tt->valuestring, ti->valueint);
                }
            }
        } else {
            CWIST_LOG_ERROR("Comment update failed: id=%s uid=%d", id_str, uid);
        }
        if (comment) cJSON_Delete(comment);
    } else {
        CWIST_LOG_WARN("Comment update failed: missing fields");
    }
    cwist_query_map_destroy(kv);
    redirect_referer_safe(res, referer, "/");
}

static bool comment_board_moderator(cwist_db *db, cJSON *comment, int uid, const char *role) {
    if (strcmp(json_string_or_empty(comment, "target_type"), "post") != 0) return moderation_can_review(role);
    cJSON *post = db_post_get_by_id(db, json_int(comment, "target_id", 0));
    bool allowed = post && db_board_user_can_moderate(db, json_int(post, "board_id", 0), uid, role);
    if (post) cJSON_Delete(post);
    return allowed;
}

void handler_comment_delete_get(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0;
    char role[32] = {0};
    if (!auth_require_login(req, res, &uid, role, sizeof(role))) return;
    const char *id_str = cwist_query_map_get(req->path_params, "id");
    const char *referer = cwist_http_header_get(req->headers, "Referer");
    if (id_str && req->method != CWIST_HTTP_POST) {
        /* GET shows a confirmation page; the page's form POSTs the delete
         * (one-click GET deletes are a CSRF hole under SameSite=Lax). */
        char action[96];
        snprintf(action, sizeof(action), "/comment/%s/delete", id_str);
        send_delete_confirm_page(req, res, action, "Delete this comment?", NULL);
        return;
    }
    if (id_str) {
        int cid = atoi(id_str);
        cJSON *comment = db_comment_get_by_id(req->db, cid);
        if (comment) {
            cJSON *tt = cJSON_GetObjectItem(comment, "target_type");
            cJSON *ti = cJSON_GetObjectItem(comment, "target_id");
            bool moderator = comment_board_moderator(req->db, comment, uid, role);
            bool removed = moderator ? db_comment_delete_admin(req->db, cid)
                                     : db_comment_delete(req->db, cid, uid);
            if (removed) {
                CWIST_LOG_INFO("Comment deleted: id=%d uid=%d", cid, uid);
                if (tt && cJSON_IsString(tt) && ti && cJSON_IsNumber(ti)) {
                    invalidate_comment_target(req->db, tt->valuestring, ti->valueint);
                }
            } else {
                CWIST_LOG_WARN("Comment delete failed: id=%d uid=%d", cid, uid);
            }
            cJSON_Delete(comment);
        } else {
            db_comment_delete(req->db, cid, uid);
        }
    }
    redirect_referer_safe(res, referer, "/");
}

/* ---- Admin ---- */

#define _POSIX_C_SOURCE 200809L
#include "handlers_internal.h"
#include "config/config.h"
#include "db/subscriber.h"
#include "render/render.h"
#include "utils/email.h"
#include "utils/spam_guard.h"
#include <ctype.h>
#include <openssl/rand.h>

/* ---------------------------------------------------------------------------
 * Email newsletter subscription (see src/utils/post_schedule.c for the
 * announcement sender). Readers post their address, get a mailed confirm
 * link, and receive a plain-text new-post email carrying an unsubscribe
 * link. Like every other POST form here there is no CSRF token: the SameSite
 * session cookie plus the spam guard are the existing defenses. Unsubscribe
 * is tokenized and always available, even with the newsletter disabled.
 * ------------------------------------------------------------------------- */

/* Copy of the hex-token idiom used in auth.c; 64 hex chars for 32 bytes. */
static bool random_hex(char *out, size_t byte_len) {
    unsigned char bytes[32];
    if (byte_len > sizeof(bytes) || RAND_bytes(bytes, (int)byte_len) != 1) return false;
    for (size_t i = 0; i < byte_len; i++) snprintf(out + (i * 2), 3, "%02x", bytes[i]);
    out[byte_len * 2] = '\0';
    return true;
}

static bool email_looks_valid(const char *email) {
    if (!email) return false;
    size_t len = strlen(email);
    if (len < 3 || len > 254) return false;
    const char *at = strchr(email, '@');
    if (!at || at == email || strchr(at + 1, '@')) return false;
    const char *domain = at + 1;
    if (!*domain || !strchr(domain, '.')) return false;
    if (domain[0] == '.' || domain[strlen(domain) - 1] == '.') return false;
    for (const char *p = email; *p; p++) {
        if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') return false;
    }
    return true;
}

static cwist_sstring *render_subscribe_body(const char *error, const char *notice, const char *email_value) {
    cwist_sstring *b = cwist_sstring_create();
    if (!b) return NULL;
    cwist_sstring_append(b, "<div class='board-line fade-in' style='max-width:520px;margin:40px auto'>");
    cwist_sstring_append(b, "<h2>Subscribe by email</h2>");
    cwist_sstring_append(b, "<p style='color:var(--muted)'>Get new posts by email. No account needed; unsubscribe any time.</p>");
    if (error && error[0]) {
        cwist_sstring_append(b, "<div class='alert' style='margin-bottom:12px'>");
        cwist_sstring_append_escaped(b, error);
        cwist_sstring_append(b, "</div>");
    }
    if (notice && notice[0]) {
        cwist_sstring_append(b, "<div class='alert' style='margin-bottom:12px'>");
        cwist_sstring_append_escaped(b, notice);
        cwist_sstring_append(b, "</div>");
    }
    cwist_sstring_append(b, "<form action='/subscribe' method='post'>");
    cwist_sstring_append(b, "<input type='email' name='email' required placeholder='you@example.com' value='");
    if (email_value && email_value[0]) cwist_sstring_append_escaped(b, email_value);
    cwist_sstring_append(b, "' style='width:100%;margin-bottom:10px'>");
    spam_guard_append_fields(b);
    cwist_sstring_append(b, "<button type='submit' class='btn'>Subscribe</button>");
    cwist_sstring_append(b, "</form></div>");
    return b;
}

static void send_subscribe_page(cwist_http_response *res, const char *error, const char *notice,
                                const char *email_value, bool dark, const char *role, bool mobile) {
    cwist_sstring *body = render_subscribe_body(error, notice, email_value);
    send_html_res(res, render_page("Subscribe", body ? body->data : "", dark, role, NULL, mobile));
    if (body) cwist_sstring_destroy(body);
}

static void send_simple_page(cwist_http_response *res, const char *heading, const char *text,
                             bool dark, const char *role, bool mobile) {
    cwist_sstring *b = cwist_sstring_create();
    if (b) {
        cwist_sstring_append(b, "<div class='board-line fade-in' style='max-width:520px;margin:40px auto'>");
        cwist_sstring_append(b, "<h2>");
        cwist_sstring_append(b, heading);
        cwist_sstring_append(b, "</h2><p style='color:var(--muted)'>");
        cwist_sstring_append(b, text);
        cwist_sstring_append(b, "</p><p><a href='/' class='btn btn-outline'>Back to home</a></p></div>");
    }
    send_html_res(res, render_page(heading, b ? b->data : "", dark, role, NULL, mobile));
    if (b) cwist_sstring_destroy(b);
}

/* GET /subscribe — the standalone subscription form. */
void handler_subscribe_get(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0;
    char role[32] = {0};
    auth_is_logged_in(req, &uid, role, sizeof(role));
    if (!g_config.newsletter_enabled) {
        res->status_code = CWIST_HTTP_NOT_FOUND;
        cwist_sstring_assign(res->body, "Not found");
        return;
    }
    send_subscribe_page(res, NULL, NULL, NULL, is_dark(req), role, is_mobile_request(req));
}

/* POST /subscribe — store a pending subscription and mail the confirm link. */
void handler_subscribe_post(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0;
    char role[32] = {0};
    auth_is_logged_in(req, &uid, role, sizeof(role));
    bool dark = is_dark(req);
    bool mobile = is_mobile_request(req);
    if (!g_config.newsletter_enabled) {
        res->status_code = CWIST_HTTP_NOT_FOUND;
        cwist_sstring_assign(res->body, "Not found");
        return;
    }
    cwist_query_map *kv = cwist_query_map_create();
    if (!kv) { redirect(res, "/subscribe"); return; }
    cwist_query_map_parse(kv, req->body ? req->body->data : "");
    const char *email = cwist_query_map_get(kv, "email");
    const char *trap = cwist_query_map_get(kv, SPAM_FIELD_TRAP);
    const char *form_ts = cwist_query_map_get(kv, SPAM_FIELD_TOKEN);

    spam_verdict_t verdict = spam_guard_check(req, uid, role, trap, form_ts);
    if (verdict != SPAM_OK) {
        cwist_query_map_destroy(kv);
        spam_guard_reject(res, verdict);
        return;
    }
    if (!email_looks_valid(email)) {
        send_subscribe_page(res, "That email address does not look valid.", NULL,
                            email ? email : "", dark, role, mobile);
        cwist_query_map_destroy(kv);
        return;
    }

    char token[65];
    bool sent = false;
    if (random_hex(token, 32) && db_subscriber_add(req->db, email, token)) {
        char link[768];
        snprintf(link, sizeof(link), "%sconfirm-subscription?token=%s", g_config.root_url, token);
        char body[1024];
        snprintf(body, sizeof(body),
                 "Hello,\n\nConfirm your subscription to %s:\n\n%s\n\n"
                 "If you did not request this, ignore the message.\n",
                 g_config.title[0] ? g_config.title : "this site", link);
        sent = email_send(email, "Confirm your subscription", body);
    }
    if (sent) {
        CWIST_LOG_INFO("Subscription confirm mail sent: email='%s'", email);
        send_subscribe_page(res, NULL,
                            "Confirmation email sent. Open the link in it to start your subscription.",
                            NULL, dark, role, mobile);
    } else {
        CWIST_LOG_ERROR("Subscription confirm mail failed: email='%s'", email);
        send_subscribe_page(res, "Could not send the confirmation email; try again later.",
                            NULL, email, dark, role, mobile);
    }
    cwist_query_map_destroy(kv);
}

/* GET /confirm-subscription?token=... — activate the subscription. */
void handler_subscribe_confirm_get(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0;
    char role[32] = {0};
    auth_is_logged_in(req, &uid, role, sizeof(role));
    if (!g_config.newsletter_enabled) { redirect(res, "/"); return; }
    const char *token = cwist_query_map_get(req->query_params, "token");
    if (token && token[0] && db_subscriber_confirm(req->db, token)) {
        CWIST_LOG_INFO("Subscription confirmed");
        send_simple_page(res, "Subscription confirmed",
                         "You will receive new posts by email. Every message carries an unsubscribe link.",
                         is_dark(req), role, is_mobile_request(req));
    } else {
        send_simple_page(res, "Confirmation link invalid",
                         "The link is unknown, already used, or expired. Subscribe again to get a new one.",
                         is_dark(req), role, is_mobile_request(req));
    }
}

/* GET /unsubscribe?token=... — confirmation interstitial (never mutates on
 * GET; the form below posts the token). */
void handler_unsubscribe_get(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0;
    char role[32] = {0};
    auth_is_logged_in(req, &uid, role, sizeof(role));
    const char *token = cwist_query_map_get(req->query_params, "token");
    if (!token || !token[0]) { redirect(res, "/"); return; }
    cwist_sstring *b = cwist_sstring_create();
    if (b) {
        cwist_sstring_append(b, "<div class='board-line fade-in' style='max-width:520px;margin:40px auto'>");
        cwist_sstring_append(b, "<h2>Unsubscribe?</h2>");
        cwist_sstring_append(b, "<p style='color:var(--muted)'>Stop receiving new-post emails from this site.</p>");
        cwist_sstring_append(b, "<form action='/unsubscribe' method='post'>");
        cwist_sstring_append(b, "<input type='hidden' name='token' value='");
        cwist_sstring_append_escaped(b, token);
        cwist_sstring_append(b, "'>");
        cwist_sstring_append(b, "<button type='submit' class='btn'>Unsubscribe</button> ");
        cwist_sstring_append(b, "<a href='/' class='btn btn-outline'>Keep subscribing</a>");
        cwist_sstring_append(b, "</form></div>");
    }
    send_html_res(res, render_page("Unsubscribe", b ? b->data : "", is_dark(req), role, NULL, is_mobile_request(req)));
    if (b) cwist_sstring_destroy(b);
}

/* POST /unsubscribe — delete the subscription holding the token. */
void handler_unsubscribe_post(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0;
    char role[32] = {0};
    auth_is_logged_in(req, &uid, role, sizeof(role));
    cwist_query_map *kv = cwist_query_map_create();
    if (!kv) { redirect(res, "/"); return; }
    cwist_query_map_parse(kv, req->body ? req->body->data : "");
    const char *token = cwist_query_map_get(kv, "token");
    bool gone = token && token[0] && db_subscriber_unsubscribe(req->db, token);
    cwist_query_map_destroy(kv);
    if (gone) {
        CWIST_LOG_INFO("Subscriber unsubscribed");
        send_simple_page(res, "Unsubscribed",
                         "You will no longer receive new-post emails. You can subscribe again any time.",
                         is_dark(req), role, is_mobile_request(req));
    } else {
        send_simple_page(res, "Unsubscribe link invalid",
                         "The link is unknown or already used.",
                         is_dark(req), role, is_mobile_request(req));
    }
}

#define _POSIX_C_SOURCE 200809L
/**
 * @file mail.c
 * @brief Webmail handlers: folder list, message view, local send, delete,
 * read-toggle, trash emptying, recipient autocomplete, admin broadcast.
 *
 * Outbound mail has two paths. Recipients whose localpart matches a local
 * account are inserted straight into that user's INBOX (works even when
 * outbound port 25 to the outside world is unreachable); every other address
 * goes through email_send(), which relays via FLY_SMTP_* or the local Postfix
 * on 127.0.0.1:25. External failures are reported but never block local
 * delivery. A copy of everything the user sends lands in their Sent folder.
 */
#include "handlers_internal.h"
#include "utils/email.h"
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <openssl/rand.h>
#include <sys/wait.h>

#define MAIL_PAGE_SIZE 20

/* Postfix delivers inbound mail as the unprivileged "flymail" user, which
 * cannot write the webmail DB. mail-import therefore drops a copy of each
 * message into /var/mail/fly/spool/<user>.*.eml; here (running as root) we
 * sweep that user's spool files into the DB via the mail-import "dbonly"
 * mode, so messages appear in webmail as soon as the recipient checks mail.
 * IMAP clients see them immediately because they read the Maildir directly. */
static void mail_spool_sweep(cwist_db *db, int uid) {
    cJSON *user = db_user_get_by_id(db, uid);
    if (!user) return;
    const cJSON *name = cJSON_GetObjectItem(user, "username");
    if (!cJSON_IsString(name) || !name->valuestring[0]) { cJSON_Delete(user); return; }
    char prefix[160];
    snprintf(prefix, sizeof(prefix), "%s.", name->valuestring);
    cJSON_Delete(user);

    const char *root = getenv("FLY_MAILDIR_ROOT");
    if (!root || !root[0]) root = "/var/mail/fly";
    char spool[512];
    snprintf(spool, sizeof(spool), "%s/spool", root);
    DIR *d = opendir(spool);
    if (!d) return;

    /* mail-import lives next to the server binary. */
    char tool[512] = "mail-import";
    ssize_t n = readlink("/proc/self/exe", tool, sizeof(tool) - 1);
    if (n > 0) {
        tool[n] = '\0';
        char *slash = strrchr(tool, '/');
        if (slash) snprintf(slash + 1, sizeof(tool) - (size_t)(slash + 1 - tool), "mail-import");
    }

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strncmp(ent->d_name, prefix, strlen(prefix)) != 0) continue;
        char path[800];
        snprintf(path, sizeof(path), "%s/%s", spool, ent->d_name);
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        int in = fileno(f);
        pid_t pid = fork();
        if (pid == 0) {
            dup2(in, STDIN_FILENO);
            fclose(f);
            int nullfd = open("/dev/null", O_WRONLY);
            if (nullfd >= 0) { dup2(nullfd, STDOUT_FILENO); dup2(nullfd, STDERR_FILENO); }
            execl(tool, tool, "dbonly", (char *)NULL);
            _exit(111);
        }
        fclose(f);
        if (pid > 0) {
            int status = 0;
            if (waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0)
                unlink(path);
        }
    }
    closedir(d);
}

static bool folder_valid(const char *folder) {
    return folder && (!strcmp(folder, MAIL_FOLDER_INBOX) ||
                      !strcmp(folder, MAIL_FOLDER_SENT) ||
                      !strcmp(folder, MAIL_FOLDER_TRASH));
}

static void mail_redirect(cwist_http_response *res, const char *folder, const char *msg) {
    char url[512];
    if (msg && msg[0])
        snprintf(url, sizeof(url), "/mail?folder=%s&msg=%s", folder ? folder : "INBOX", msg);
    else
        snprintf(url, sizeof(url), "/mail?folder=%s", folder ? folder : "INBOX");
    redirect(res, url);
}

void handler_mail_get(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0;
    char role[32] = {0};
    if (!auth_require_login(req, res, &uid, role, sizeof(role))) return;

    const char *folder = cwist_query_map_get(req->query_params, "folder");
    if (!folder_valid(folder)) folder = MAIL_FOLDER_INBOX;
    int page = atoi(cwist_query_map_get(req->query_params, "page") ? cwist_query_map_get(req->query_params, "page") : "1");
    if (page < 1) page = 1;

    if (strcmp(folder, MAIL_FOLDER_INBOX) == 0) mail_spool_sweep(req->db, uid);
    int total = db_email_count(req->db, uid, folder);
    int total_pages = total > 0 ? (total + MAIL_PAGE_SIZE - 1) / MAIL_PAGE_SIZE : 1;
    if (page > total_pages) page = total_pages;
    cJSON *emails = db_email_list(req->db, uid, folder, (page - 1) * MAIL_PAGE_SIZE, MAIL_PAGE_SIZE);
    int unread = db_email_unread_count(req->db, uid);
    const char *msg = cwist_query_map_get(req->query_params, "msg");

    char *pp = get_profile_pic(req->db, uid, role);
    cwist_sstring *page_html = render_mail_list(emails ? emails : cJSON_CreateArray(), folder, page,
                                                total_pages, unread, msg, is_dark(req), role, pp,
                                                is_mobile_request(req));
    send_html_res(res, page_html);
    if (emails) cJSON_Delete(emails);
    free(pp);
}

void handler_mail_view_get(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0;
    char role[32] = {0};
    if (!auth_require_login(req, res, &uid, role, sizeof(role))) return;

    int id = atoi(cwist_query_map_get(req->query_params, "id") ? cwist_query_map_get(req->query_params, "id") : "0");
    mail_spool_sweep(req->db, uid);
    cJSON *email = id > 0 ? db_email_get(req->db, uid, id) : NULL;
    if (!email) {
        res->status_code = CWIST_HTTP_NOT_FOUND;
        cwist_sstring_assign(res->body, "Message not found");
        return;
    }
    if (json_int(email, "is_read", 1) == 0) db_email_set_read(req->db, uid, id, true);

    char *pp = get_profile_pic(req->db, uid, role);
    cwist_sstring *page_html = render_mail_view(email, is_dark(req), role, pp, is_mobile_request(req));
    send_html_res(res, page_html);
    cJSON_Delete(email);
    free(pp);
}

/* Split a comma-separated To field into trimmed address slots in-place. */
static int split_recipients(char *to, char **slots, int max_slots) {
    int n = 0;
    char *p = to;
    while (p && n < max_slots) {
        char *comma = strchr(p, ',');
        if (comma) *comma = '\0';
        while (*p == ' ' || *p == '\t') p++;
        char *end = p + strlen(p);
        while (end > p && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) end--;
        *end = '\0';
        if (p[0]) slots[n++] = p;
        p = comma ? comma + 1 : NULL;
    }
    return n;
}

static bool message_id_new(char *out, size_t out_len) {
    unsigned char rnd[8];
    if (RAND_bytes(rnd, sizeof(rnd)) != 1) return false;
    char hex[17] = {0};
    for (size_t i = 0; i < sizeof(rnd); i++) snprintf(hex + i * 2, 3, "%02x", rnd[i]);
    snprintf(out, out_len, "<%lld.%s@%s>", (long long)time(NULL), hex, fly_mail_domain());
    return true;
}

void handler_mail_send_post(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0;
    char role[32] = {0};
    if (!auth_require_login(req, res, &uid, role, sizeof(role))) return;

    cwist_query_map *kv = cwist_query_map_create();
    cwist_query_map_parse(kv, req->body->data);
    const char *to = cwist_query_map_get(kv, "to");
    const char *subject = cwist_query_map_get(kv, "subject");
    const char *body = cwist_query_map_get(kv, "body");
    const char *in_reply_to = cwist_query_map_get(kv, "in_reply_to");

    if (!to || !to[0] || !body || !subject) {
        cwist_query_map_destroy(kv);
        mail_redirect(res, MAIL_FOLDER_INBOX, "send_invalid");
        return;
    }
    char *to_copy = (char *)cwist_alloc(strlen(to) + 1);
    strcpy(to_copy, to);
    char *slots[32];
    int n = split_recipients(to_copy, slots, 32);

    cJSON *sender = db_user_get_by_id(req->db, uid);
    cJSON *sender_name_obj = sender ? cJSON_GetObjectItem(sender, "username") : NULL;
    const char *sender_name = cJSON_IsString(sender_name_obj) ? sender_name_obj->valuestring : "";
    char from_addr[320] = {0};
    if (sender_name && sender_name[0])
        snprintf(from_addr, sizeof(from_addr), "%s@%s", sender_name, fly_mail_domain());
    else
        snprintf(from_addr, sizeof(from_addr), "user%d@%s", uid, fly_mail_domain());

    char message_id[128] = {0};
    if (!message_id_new(message_id, sizeof(message_id)))
        snprintf(message_id, sizeof(message_id), "<%d.%lld@%s>", uid, (long long)time(NULL), fly_mail_domain());

    int local_delivered = 0, external_sent = 0, external_failed = 0;
    char failed_list[512] = {0};
    for (int i = 0; i < n; i++) {
        char addr[256] = {0};
        snprintf(addr, sizeof(addr), "%s", slots[i]);
        char local[128] = {0};
        const char *at = strrchr(addr, '@');
        snprintf(local, sizeof(local), "%s", at && at != addr ? "" : addr);
        if (at && at != addr) {
            size_t ll = (size_t)(at - addr);
            if (ll >= sizeof(local)) ll = sizeof(local) - 1;
            memcpy(local, addr, ll);
            local[ll] = '\0';
        }
        if (!local[0]) continue;

        bool is_local_domain = !at || strcasecmp(at + 1, fly_mail_domain()) == 0;
        cJSON *rcpt = is_local_domain ? db_user_get_by_username(req->db, local) : NULL;
        if (rcpt) {
            int owner = json_int(rcpt, "id", 0);
            if (owner > 0)
                local_delivered += db_email_create(req->db, owner, MAIL_FOLDER_INBOX,
                                                   from_addr, addr, subject, body,
                                                   message_id,
                                                   (in_reply_to && in_reply_to[0]) ? in_reply_to : NULL) > 0;
            cJSON_Delete(rcpt);
        } else if (at) {
            /* External recipient: try the configured SMTP / local Postfix. */
            if (email_send_from(from_addr, addr, subject, body)) {
                external_sent++;
            } else {
                external_failed++;
                if (failed_list[0]) strncat(failed_list, ", ", sizeof(failed_list) - strlen(failed_list) - 1);
                strncat(failed_list, addr, sizeof(failed_list) - strlen(failed_list) - 1);
            }
        }
    }

    /* The sender's own copy, threading included. */
    db_email_create(req->db, uid, MAIL_FOLDER_SENT, from_addr, to, subject, body,
                    message_id, (in_reply_to && in_reply_to[0]) ? in_reply_to : NULL);
    if (sender) cJSON_Delete(sender);
    cwist_free(to_copy);
    cwist_query_map_destroy(kv);

    CWIST_LOG_INFO("Mail send: uid=%d local=%d external_sent=%d external_failed=%d",
                   uid, local_delivered, external_sent, external_failed);
    if (external_failed > 0 && failed_list[0]) {
        char url[768];
        snprintf(url, sizeof(url), "/mail?folder=Sent&msg=send_partial&failed=%s", failed_list);
        redirect(res, url);
    } else if (local_delivered > 0 || external_sent > 0) {
        mail_redirect(res, MAIL_FOLDER_SENT, "sent");
    } else {
        mail_redirect(res, MAIL_FOLDER_SENT, "send_no_local");
    }
}

void handler_mail_delete_post(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0;
    char role[32] = {0};
    if (!auth_require_login(req, res, &uid, role, sizeof(role))) return;
    cwist_query_map *kv = cwist_query_map_create();
    cwist_query_map_parse(kv, req->body->data);
    int id = atoi(cwist_query_map_get(kv, "id") ? cwist_query_map_get(kv, "id") : "0");
    const char *folder = cwist_query_map_get(kv, "folder");
    char folder_copy[16];
    snprintf(folder_copy, sizeof(folder_copy), "%s", (folder && folder_valid(folder)) ? folder : MAIL_FOLDER_INBOX);
    if (id > 0) db_email_set_folder(req->db, uid, id, MAIL_FOLDER_TRASH);
    cwist_query_map_destroy(kv);
    mail_redirect(res, folder_copy, "deleted");
}

void handler_mail_read_post(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0;
    char role[32] = {0};
    if (!auth_require_login(req, res, &uid, role, sizeof(role))) return;
    cwist_query_map *kv = cwist_query_map_create();
    cwist_query_map_parse(kv, req->body->data);
    int id = atoi(cwist_query_map_get(kv, "id") ? cwist_query_map_get(kv, "id") : "0");
    int value = atoi(cwist_query_map_get(kv, "value") ? cwist_query_map_get(kv, "value") : "1");
    const char *folder = cwist_query_map_get(kv, "folder");
    char folder_copy[16];
    snprintf(folder_copy, sizeof(folder_copy), "%s", (folder && folder_valid(folder)) ? folder : MAIL_FOLDER_INBOX);
    if (id > 0) db_email_set_read(req->db, uid, id, value != 0);
    cwist_query_map_destroy(kv);
    mail_redirect(res, folder_copy, NULL);
}

void handler_mail_empty_trash_post(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0;
    char role[32] = {0};
    if (!auth_require_login(req, res, &uid, role, sizeof(role))) return;
    db_email_empty_trash(req->db, uid);
    mail_redirect(res, MAIL_FOLDER_TRASH, "trash_emptied");
}

void handler_mail_users_get(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0;
    char role[32] = {0};
    if (!auth_is_logged_in(req, &uid, role, sizeof(role))) {
        res->status_code = CWIST_HTTP_UNAUTHORIZED;
        cwist_sstring_assign(res->body, "{}");
        return;
    }
    const char *q = cwist_query_map_get(req->query_params, "q");
    if (!q || !q[0]) q = "";
    cJSON *users = db_user_search_prefix(req->db, q, 10);
    char *json = cJSON_Print(users ? users : cJSON_CreateArray());
    res->status_code = CWIST_HTTP_OK;
    cwist_http_header_add(&res->headers, "Content-Type", "application/json; charset=utf-8");
    cwist_sstring_assign(res->body, json ? json : "[]");
    if (json) cwist_free(json);
    if (users) cJSON_Delete(users);
}

/* ---- Admin broadcast: one copy per verified user, inboxes + outbound ---- */

void handler_admin_broadcast_post(cwist_http_request *req, cwist_http_response *res) {
    if (!auth_require_admin(req, res)) return;
    cwist_query_map *kv = cwist_query_map_create();
    cwist_query_map_parse(kv, req->body->data);
    const char *subject = cwist_query_map_get(kv, "subject");
    const char *body = cwist_query_map_get(kv, "body");

    if (!subject || !subject[0] || !body || !body[0]) {
        cwist_query_map_destroy(kv);
        redirect(res, "/admin/dashboard?msg=broadcast_invalid#broadcast");
        return;
    }

    char message_id[128] = {0};
    if (!message_id_new(message_id, sizeof(message_id)))
        snprintf(message_id, sizeof(message_id), "<broadcast.%lld@%s>", (long long)time(NULL), fly_mail_domain());

    int inbox_copies = 0, send_ok = 0, send_failed = 0;
    cJSON *users = db_user_list_verified(req->db);
    cJSON *u = NULL;
    char postmaster[320] = {0};
    snprintf(postmaster, sizeof(postmaster), "postmaster@%s", fly_mail_domain());
    cJSON_ArrayForEach(u, users) {
        int target = json_int(u, "id", 0);
        cJSON *email = cJSON_GetObjectItem(u, "email");
        if (target <= 0) continue;
        /* Local copy for the webmail inbox. */
        if (db_email_create(req->db, target, MAIL_FOLDER_INBOX,
                            postmaster, "",
                            subject, body, message_id, NULL) > 0)
            inbox_copies++;
        /* Outbound copy to the address on file. */
        if (cJSON_IsString(email) && email->valuestring[0]) {
            if (email_send(email->valuestring, subject, body)) send_ok++;
            else send_failed++;
        }
    }
    if (users) cJSON_Delete(users);
    cwist_query_map_destroy(kv);
    CWIST_LOG_INFO("Broadcast sent: inbox=%d smtp_ok=%d smtp_failed=%d", inbox_copies, send_ok, send_failed);
    char url[160];
    snprintf(url, sizeof(url), "/admin/dashboard?msg=broadcast_done&n=%d#broadcast", inbox_copies);
    redirect(res, url);
}

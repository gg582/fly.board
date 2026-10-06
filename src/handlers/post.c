#define _POSIX_C_SOURCE 200809L
#include "handlers_internal.h"
#include "db/sql_escape.h"
#include "config/write_policy.h"
#include "utils/post_schedule.h"
#include <openssl/rand.h>

#define MAX_POST_TITLE_LEN   200
#define MAX_POST_SUMMARY_LEN 1000
#define MAX_POST_CONTENT_LEN (1024 * 1024)

static bool random_hex_local(char *out, size_t byte_len) {
    unsigned char bytes[64];
    if (!out || byte_len == 0 || byte_len > sizeof(bytes)) return false;
    if (RAND_bytes(bytes, (int)byte_len) != 1) return false;
    for (size_t i = 0; i < byte_len; i++) snprintf(out + (i * 2), 3, "%02x", bytes[i]);
    out[byte_len * 2] = '\0';
    return true;
}

#include <ctype.h>

static char *cwist_strdup_local(const char *src) {
    if (!src) return NULL;
    size_t len = strlen(src);
    char *copy = (char *)cwist_alloc(len + 1);
    if (!copy) return NULL;
    memcpy(copy, src, len + 1);
    return copy;
}

static void rewrite_content_legacy_urls(cwist_db *db, char **content) {
    if (!db || !content || !*content || !**content) return;
    const char *prefix = "/file/download/";
    size_t prefix_len = strlen(prefix);
    cwist_sstring *out = cwist_sstring_create();
    if (!out) return;
    const char *p = *content;
    while (*p) {
        const char *found = strstr(p, prefix);
        if (!found) {
            cwist_sstring_append(out, p);
            break;
        }
        cwist_sstring_append_len(out, p, found - p);
        const char *num_start = found + prefix_len;
        if (!isdigit((unsigned char)*num_start)) {
            cwist_sstring_append_len(out, found, prefix_len);
            p = num_start;
            continue;
        }
        int fid = 0;
        const char *num_end = num_start;
        while (isdigit((unsigned char)*num_end)) {
            if (fid > (INT_MAX / 10) || (fid == INT_MAX / 10 && (*num_end - '0') > (INT_MAX % 10))) {
                fid = 0; break;
            }
            fid = fid * 10 + (*num_end - '0');
            num_end++;
        }
        if (fid <= 0) {
            cwist_sstring_append_len(out, found, num_end - found);
            p = num_end;
            continue;
        }
        cJSON *file = db_file_get(db, fid);
        if (file) {
            cJSON_Delete(file);
        }
        cwist_sstring_append_len(out, found, num_end - found);
        p = num_end;
    }
    char *rewritten = cwist_strdup_local(out->data);
    if (rewritten) {
        cwist_free(*content);
        *content = rewritten;
    }
    cwist_sstring_destroy(out);
}

/* Write-policy gate for creating posts: guests are sent to log in, members
 * held back by an admin-only scope get a 403. */
static bool require_post_scope(cwist_http_request *req, cwist_http_response *res, const char *role) {
    if (write_policy_can_post(role)) return true;
    if (!role || !role[0]) {
        int uid = 0;
        char login_role[32] = {0};
        auth_require_login(req, res, &uid, login_role, sizeof(login_role));
        return false;
    }
    res->status_code = CWIST_HTTP_FORBIDDEN;
    cwist_sstring_assign(res->body, "Forbidden: only admins may write posts");
    return false;
}

#define BOARD_ERROR_REQUIRED "Choose a board for this post"
#define BOARD_ERROR_DENIED   "You cannot post to this board"

/* NULL when a post may go to board_id, else the editor error to show. A
 * board must exist, and an admin_only board takes admins or users granted
 * on /board/:id/perms; with require_board on, no board is an error too. */
static const char *post_board_error(cwist_db *db, int board_id, int uid, const char *role) {
    if (board_id <= 0) return write_policy_get().require_board ? BOARD_ERROR_REQUIRED : NULL;
    if (!db_board_can_user_access(db, board_id, uid, strcmp(role, "admin") == 0)) return BOARD_ERROR_DENIED;
    return NULL;
}

/* Boards the viewer may post to, flattened for the editor dropdown. */
static cJSON *editor_boards(cwist_db *db, int uid, const char *role) {
    cJSON *boards = db_board_list(db);
    cJSON *tree = db_board_tree_get_all();
    cJSON *ordered = cJSON_CreateArray();
    append_boards_flat(ordered, boards, tree, 0, 4);
    bool is_admin = strcmp(role, "admin") == 0;
    for (int i = cJSON_GetArraySize(ordered) - 1; i >= 0; i--) {
        int bid = json_int(cJSON_GetArrayItem(ordered, i), "id", 0);
        if (!db_board_can_user_access(db, bid, uid, is_admin)) cJSON_DeleteItemFromArray(ordered, i);
    }
    if (tree) cJSON_Delete(tree);
    if (boards) cJSON_Delete(boards);
    return ordered;
}

/* ---- Series and language fields ---- */

static bool is_admin_role(const char *role) {
    return role && strcmp(role, "admin") == 0;
}

/* Editor data: the writer's series (all for admins) and the posts they may
 * name as the original of a translation. Anonymous writers get neither. */
typedef struct { cJSON *series; cJSON *posts; } editor_options_t;

static editor_options_t editor_options_load(cwist_db *db, int uid, const char *role) {
    editor_options_t o = {0};
    if (uid <= 0) return o;
    int owner = is_admin_role(role) ? 0 : uid;
    o.series = db_series_list(db, owner, false);
    o.posts = db_post_pick_list(db, owner);
    render_set_editor_options(o.series, o.posts);
    return o;
}

static void editor_options_free(editor_options_t *o) {
    if (o->series) cJSON_Delete(o->series);
    if (o->posts) cJSON_Delete(o->posts);
    render_set_editor_options(NULL, NULL);
}

/* Fill "series_title", "lang" and "translation_of" on a post for the editor. */
static void attach_editor_fields(cwist_db *db, cJSON *post) {
    int id = json_int(post, "id", 0);
    int sid = json_int(post, "series_id", 0);
    if (sid > 0) {
        cJSON *series = db_series_get(db, sid);
        cJSON *title = series ? cJSON_GetObjectItem(series, "title") : NULL;
        if (cJSON_IsString(title)) cJSON_AddStringToObject(post, "series_title", title->valuestring);
        if (series) cJSON_Delete(series);
    }
    cJSON *i18n = db_i18n_get(db, "post", id);
    cJSON *lang = i18n ? cJSON_GetObjectItem(i18n, "lang") : NULL;
    cJSON_AddStringToObject(post, "lang", cJSON_IsString(lang) ? lang->valuestring : "");
    int grp = i18n ? json_int(i18n, "grp", 0) : 0;
    if (i18n) cJSON_Delete(i18n);
    int pair = 0;
    if (grp > 0 && grp != id) {
        pair = grp; /* the post the group started from */
    } else if (grp > 0) {
        cJSON *sib = db_i18n_siblings(db, "post", id, false);
        if (cJSON_GetArraySize(sib) > 0) pair = json_int(cJSON_GetArrayItem(sib, 0), "id", 0);
        if (sib) cJSON_Delete(sib);
    }
    cJSON_AddNumberToObject(post, "translation_of", pair);
}

/* Apply the editor's Series / Part / Language / Translation fields. NULL
 * means the field was not submitted and is left alone. */
static void apply_series_and_lang(cwist_db *db, int post_id, int uid, const char *role, const char *series,
                                  const char *series_pos, const char *lang, const char *translation_of) {
    if (post_id <= 0) return;
    bool admin = is_admin_role(role);
    if (series && uid > 0) {
        char title[201];
        snprintf(title, sizeof(title), "%s", series);
        /* trim */
        char *t = title;
        while (*t == ' ' || *t == '\t') t++;
        size_t n = strlen(t);
        while (n > 0 && (t[n - 1] == ' ' || t[n - 1] == '\t' || t[n - 1] == '\r' || t[n - 1] == '\n')) t[--n] = '\0';
        if (!t[0]) {
            db_post_set_series(db, post_id, 0, 0);
        } else {
            int sid = db_series_find(db, t, admin ? 0 : uid);
            if (sid <= 0) sid = db_series_create(db, t, uid);
            if (sid > 0) db_post_set_series(db, post_id, sid, series_pos ? atoi(series_pos) : 0);
        }
    }
    if (lang && i18n_lang_valid(lang)) db_i18n_set(db, "post", post_id, lang, 0);
    if (translation_of) {
        int other = atoi(translation_of); /* "12 · Title" -> 12 */
        if (!translation_of[0] || other <= 0) {
            db_i18n_set(db, "post", post_id, NULL, -1);
        } else if (other != post_id) {
            cJSON *target = db_post_get_by_id(db, other);
            bool allowed = target && (admin || (uid > 0 && json_int(target, "user_id", 0) == uid));
            if (target) cJSON_Delete(target);
            if (allowed) db_i18n_set(db, "post", post_id, NULL, other);
        }
    }
}

static void send_post_editor_error(cwist_http_request *req, cwist_http_response *res, int uid,
                                   const char *role, int initial_board_id, const char *error) {
    cJSON *ordered = editor_boards(req->db, uid, role);
    char *pp = get_profile_pic(req->db, uid, role);
    editor_options_t opts = editor_options_load(req->db, uid, role);
    cwist_sstring *page = render_post_editor(ordered, NULL, NULL, initial_board_id, is_dark(req), role, error, pp, is_mobile_request(req), 0);
    editor_options_free(&opts);
    if (ordered) cJSON_Delete(ordered);
    send_html_res(res, page);
    free(pp);
}

static int days_in_month(int y, int m) {
    static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) return 29;
    return days[m - 1];
}

/* The editor sends publish_at as browser-converted UTC ISO 8601
 * ("2026-10-05T09:30:00.000Z"); normalize it to the stored format. */
static bool parse_publish_at(const char *in, char out[POST_TIME_LEN]) {
    int y, mo, d, h, mi, sec = 0, n = 0;
    if (!in || sscanf(in, "%4d-%2d-%2dT%2d:%2d%n", &y, &mo, &d, &h, &mi, &n) != 5) return false;
    const char *rest = in + n;
    if (*rest == ':') {
        int m = 0;
        if (sscanf(rest, ":%2d%n", &sec, &m) != 1) return false;
        rest += m;
    }
    if (*rest == '.') {
        rest++;
        while (isdigit((unsigned char)*rest)) rest++;
    }
    if (*rest == 'Z') rest++;
    if (*rest != '\0') return false;
    if (y < 1970 || y > 9999 || mo < 1 || mo > 12 || d < 1 || d > days_in_month(y, mo) ||
        h < 0 || h > 23 || mi < 0 || mi > 59 || sec < 0 || sec > 59) return false;
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d", y, mo, d, h, mi, sec);
    memcpy(out, buf, POST_TIME_LEN);
    out[POST_TIME_LEN - 1] = '\0';
    return true;
}

/* Editor buttons: post_action=draft keeps the post private; publish with a
 * publish_at sets the publish time (future = scheduled, past = backdated).
 * Plain publish keeps the publish time of an already public post and uses
 * now for anything else. Guests cannot come back to a draft, so they always
 * publish immediately. */
typedef struct {
    const char *status;
    const char *publish_at;
    char time_buf[POST_TIME_LEN];
} publish_choice_t;

static void choose_publish(publish_choice_t *out, int uid, const char *action,
                           const char *publish_at_in, bool was_public) {
    out->status = POST_STATUS_PUBLISHED;
    out->publish_at = NULL;
    if (uid <= 0) return;
    if (action && strcmp(action, "draft") == 0) {
        out->status = POST_STATUS_DRAFT;
    } else if (parse_publish_at(publish_at_in, out->time_buf)) {
        out->publish_at = out->time_buf;
    } else if (!was_public) {
        post_utc_now(out->time_buf);
        out->publish_at = out->time_buf;
    }
}

static char *dup_form_field(form_field_t *files, const char *name) {
    form_field_t *f = form_find(files, name);
    if (!f) return NULL;
    char *v = (char *)cwist_alloc(f->len + 1);
    if (!v) return NULL;
    memcpy(v, f->data, f->len);
    v[f->len] = 0;
    return v;
}

static char *dup_query_field(cwist_query_map *kv, const char *name) {
    const char *v = cwist_query_map_get(kv, name);
    return v ? cwist_strdup_local(v) : NULL;
}

static void attach_media_meta_to_post(cwist_db *db, const char *media_meta_json, int post_id, int uid, const char *role) {
    if (!db || !media_meta_json || !media_meta_json[0] || post_id <= 0) return;
    cJSON *arr = cJSON_Parse(media_meta_json);
    if (!arr || !cJSON_IsArray(arr)) {
        if (arr) cJSON_Delete(arr);
        return;
    }
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, arr) {
        if (!cJSON_IsObject(item)) continue;
        int fid = json_int(item, "fid", 0);
        const char *mode = cJSON_GetObjectItem(item, "mode") && cJSON_IsString(cJSON_GetObjectItem(item, "mode"))
            ? cJSON_GetObjectItem(item, "mode")->valuestring : "attachment";
        const char *delete_pin = cJSON_GetObjectItem(item, "delete_pin") && cJSON_IsString(cJSON_GetObjectItem(item, "delete_pin"))
            ? cJSON_GetObjectItem(item, "delete_pin")->valuestring : "";
        if (fid <= 0) continue;
        cJSON *file = db_file_get(db, fid);
        if (!file) continue;
        int file_uid = json_int(file, "user_id", 0);
        int existing_post_id = json_int(file, "post_id", 0);
        cJSON *pin_hash = cJSON_GetObjectItem(file, "delete_pin_hash");
        bool pin_ok = delete_pin && delete_pin[0] && pin_hash && pin_hash->valuestring && pin_hash->valuestring[0] &&
                      auth_verify_password(delete_pin, pin_hash->valuestring);
        bool owner_ok = (uid > 0 && file_uid == uid) || (role && strcmp(role, "admin") == 0);
        if ((existing_post_id == 0 || existing_post_id == post_id) && (owner_ok || pin_ok)) {
            db_file_attach_to_post(db, fid, post_id, strcmp(mode, "inline") == 0);
        }
        cJSON_Delete(file);
    }
    cJSON_Delete(arr);
}

void handler_post_list(cwist_http_request *req, cwist_http_response *res) {
    const char *slug = cwist_query_map_get(req->path_params, "slug");
    bool dark = is_dark(req);
    int uid = 0;
    char role[32] = {0};
    auth_is_logged_in(req, &uid, role, sizeof(role));
    bool mobile = is_mobile_request(req);
    cJSON *posts = NULL;
    int page = 1, total_pages = 1;
    const char *page_str = cwist_query_map_get(req->query_params, "page");
    if (page_str) page = atoi(page_str);
    if (page < 1) page = 1;
    int per_page = 20;
    const char *search = cwist_query_map_get(req->query_params, "search");
    const char *search_type = cwist_query_map_get(req->query_params, "search_type");
    bool empty_search = search && !search[0];

    char key[512];
    page_cache_key_board(key, sizeof(key), slug ? slug : "", page, dark, mobile, role, uid, search, search_type);
    const char *cached = NULL;
    size_t cached_len = 0;
    uint32_t ttl = 0;
    if (page_cache_get(key, &cached, &cached_len, &ttl)) {
        send_cached_html_res(res, cached, cached_len, ttl);
        page_cache_release(key);
        return;
    }

    int bid = 0;
    if (slug) {
        cJSON *board = db_board_get_by_slug(req->db, slug);
        if (!board) {
            CWIST_LOG_WARN("Post list: board not found slug='%s'", slug);
            res->status_code = CWIST_HTTP_NOT_FOUND;
            cwist_sstring_assign(res->body, "Board not found");
            return;
        }
        bid = json_int(board, "id", 0);
        cJSON_Delete(board);
    }

    bool leader = false;
    cwist_sstring *shared = reqshare_wait_or_start(key, &leader);
    if (!leader) {
        send_html_res(res, shared);
        return;
    }

    cJSON *children = NULL;
    if (!empty_search) {
        int total = db_post_count_search(req->db, bid, search, search_type);
        total_pages = (total + per_page - 1) / per_page;
        if (total_pages < 1) total_pages = 1;
        if (page > total_pages) page = total_pages;
        posts = db_post_list_search(req->db, bid, search, search_type, per_page, (page - 1) * per_page);
    }
    if (bid > 0) {
        cJSON *child_ids = db_board_tree_get_children(bid);
        if (child_ids && cJSON_GetArraySize(child_ids) > 0) {
            children = cJSON_CreateArray();
            int n = cJSON_GetArraySize(child_ids);
            for (int i = 0; i < n; i++) {
                cJSON *id_item = cJSON_GetArrayItem(child_ids, i);
                int cid = id_item->valueint;
                cJSON *cboard = db_board_get_by_id(req->db, cid);
                if (cboard) {
                    cJSON *cslug = cJSON_GetObjectItem(cboard, "slug");
                    cJSON *cname = cJSON_GetObjectItem(cboard, "name");
                    if (cslug && cslug->valuestring && cname && cname->valuestring) {
                        cJSON *obj = cJSON_CreateObject();
                        cJSON_AddStringToObject(obj, "slug", cslug->valuestring);
                        cJSON_AddStringToObject(obj, "name", cname->valuestring);
                        cJSON_AddItemToArray(children, obj);
                    }
                    cJSON_Delete(cboard);
                }
            }
        }
        if (child_ids) cJSON_Delete(child_ids);
    }

    char *pp = get_profile_pic(req->db, uid, role);
    cJSON *board_i18n = bid > 0 ? db_i18n_get(req->db, "board", bid) : NULL;
    cJSON *board_siblings = bid > 0 ? db_i18n_siblings(req->db, "board", bid, false) : NULL;
    cJSON *board_lang = board_i18n ? cJSON_GetObjectItem(board_i18n, "lang") : NULL;
    render_set_board_translations(cJSON_IsString(board_lang) ? board_lang->valuestring : NULL, board_siblings);
    cwist_sstring *page_html = render_post_list(posts, NULL, dark, role, page, total_pages, slug, search, search_type, pp, uid, mobile, children);
    render_set_board_translations(NULL, NULL);
    if (board_i18n) cJSON_Delete(board_i18n);
    if (board_siblings) cJSON_Delete(board_siblings);
    if (posts) cJSON_Delete(posts);
    if (children) cJSON_Delete(children);
    if (page_html) {
        page_cache_set(key, page_html->data, page_html->size, 60);
        reqshare_finish(key, page_html);
    } else {
        reqshare_finish(key, NULL);
    }
    send_html_res(res, page_html);
    free(pp);
}

/* Route BDR hit on /post/:slug: the page came from the cache, but the view
 * still counts (see engine_async_set_hit_hook). */
void post_bdr_hit(cwist_db *db, const char *path) {
    if (!db || !path || strncmp(path, "/post/", 6) != 0) return;
    const char *slug = path + 6;
    if (!slug[0] || strchr(slug, '/')) return;
    cJSON *post = db_post_get_by_slug(db, slug);
    if (!post && strchr(slug, '%')) {
        char decoded[512];
        size_t j = 0;
        for (size_t i = 0; slug[i] && j + 1 < sizeof(decoded); i++) {
            if (slug[i] == '%' && isxdigit((unsigned char)slug[i + 1]) &&
                isxdigit((unsigned char)slug[i + 2])) {
                char hex[3] = {slug[i + 1], slug[i + 2], '\0'};
                decoded[j++] = (char)strtol(hex, NULL, 16);
                i += 2;
            } else {
                decoded[j++] = slug[i];
            }
        }
        decoded[j] = '\0';
        post = db_post_get_by_slug(db, decoded);
    }
    if (!post) return;
    int post_id = json_int(post, "id", 0);
    if (post_id > 0 && post_is_public(post)) db_post_increment_view(db, post_id);
    cJSON_Delete(post);
}

/* The post's tag names as a JSON string array, attached to @p post as
 * "tags" for the renderers. */
static void attach_post_tags(cwist_db *db, cJSON *post) {
    cJSON *rows = db_tag_list_by_post(db, json_int(post, "id", 0));
    cJSON *names = cJSON_CreateArray();
    cJSON *r = NULL;
    cJSON_ArrayForEach(r, rows) {
        cJSON *name = cJSON_GetObjectItem(r, "name");
        if (cJSON_IsString(name)) cJSON_AddItemToArray(names, cJSON_CreateString(name->valuestring));
    }
    if (rows) cJSON_Delete(rows);
    cJSON_AddItemToObject(post, "tags", names);
}

void handler_post_get(cwist_http_request *req, cwist_http_response *res) {
    const char *slug = cwist_query_map_get(req->path_params, "slug");
    if (!slug) { redirect(res, "/"); return; }
    bool dark = is_dark(req);
    int uid = 0;
    char role[32] = {0};
    auth_is_logged_in(req, &uid, role, sizeof(role));
    bool mobile = is_mobile_request(req);

    char key[512];
    page_cache_key_post(key, sizeof(key), slug, dark, mobile, role, uid);
    const char *cached = NULL;
    size_t cached_len = 0;
    uint32_t ttl = 0;
    if (page_cache_get(key, &cached, &cached_len, &ttl)) {
        send_cached_html_res(res, cached, cached_len, ttl);
        page_cache_release(key);
        return;
    }

    cJSON *post = db_post_get_by_slug(req->db, slug);
    /* Drafts and scheduled posts exist only for their author and admins. */
    if (post && !post_is_public(post) && !is_author_or_admin(post, uid, role)) {
        cJSON_Delete(post);
        post = NULL;
    }
    if (!post) {
        res->status_code = CWIST_HTTP_NOT_FOUND;
        cwist_sstring_assign(res->body, "Not found");
        return;
    }
    bool post_public = post_is_public(post);

    bool leader = false;
    cwist_sstring *shared = reqshare_wait_or_start(key, &leader);
    if (!leader) {
        cJSON_Delete(post);
        send_html_res(res, shared);
        return;
    }

    int post_id = json_int(post, "id", 0);
    /* HEAD requests are routed as GET (see global_middleware); they must not
     * inflate the view count. */
    if (post_id > 0 && post_public && !cwist_http_header_get(req->headers, "X-Fly-Head-Rewrite")) db_post_increment_view(req->db, post_id);
    cJSON *files = db_file_list_by_post(req->db, post_id);
    cJSON *comments = db_comment_list_by_target(req->db, "post", post_id);
    bool verified = false;
    cJSON *sig_json = cJSON_GetObjectItem(post, "pqc_signature");
    if (sig_json && sig_json->valuestring && sig_json->valuestring[0]) {
        cJSON *t = cJSON_GetObjectItem(post, "title");
        cJSON *c = cJSON_GetObjectItem(post, "content");
        if (t && c && t->valuestring && c->valuestring) {
            size_t mlen = strlen(t->valuestring) + 1 + strlen(c->valuestring) + 1;
            char *msg = (char *)cwist_alloc(mlen);
            snprintf(msg, mlen, "%s\n%s", t->valuestring, c->valuestring);
            verified = fly_crypto_verify((const uint8_t *)msg, strlen(msg), sig_json->valuestring);
            cwist_free(msg);
        }
    }
    cJSON *vote_counts = db_post_vote_counts(req->db, post_id);
    int vote_up = 0, vote_down = 0, user_vote = 0;
    if (vote_counts) {
        cJSON *vu = cJSON_GetObjectItem(vote_counts, "up");
        cJSON *vd = cJSON_GetObjectItem(vote_counts, "down");
        if (vu && vu->type == cJSON_Number) vote_up = (int)vu->valuedouble;
        if (vd && vd->type == cJSON_Number) vote_down = (int)vd->valuedouble;
        cJSON_Delete(vote_counts);
    }
    if (uid > 0) user_vote = db_post_user_vote(req->db, post_id, uid);
    char *pp = get_profile_pic(req->db, uid, role);
    int author_id = json_int(post, "user_id", 0);
    char *author_pp = NULL;
    if (author_id > 0) {
        cJSON *author_user = db_user_get_by_id(req->db, author_id);
        if (author_user) {
            cJSON *pic = cJSON_GetObjectItem(author_user, "profile_pic");
            if (pic && pic->valuestring && pic->valuestring[0]) {
                author_pp = strdup(pic->valuestring);
            } else if (auth_is_site_admin(author_id)) {
                author_pp = get_admin_logo();
            }
            cJSON_Delete(author_user);
        }
    }
    attach_post_tags(req->db, post);
    {
        cJSON *i18n = db_i18n_get(req->db, "post", post_id);
        cJSON *lang = i18n ? cJSON_GetObjectItem(i18n, "lang") : NULL;
        cJSON_AddStringToObject(post, "lang", cJSON_IsString(lang) ? lang->valuestring : "");
        if (i18n) cJSON_Delete(i18n);
        cJSON *siblings = db_i18n_siblings(req->db, "post", post_id, true);
        if (siblings) cJSON_AddItemToObject(post, "translations", siblings);
        int sid = json_int(post, "series_id", 0);
        cJSON *series = sid > 0 ? db_series_get(req->db, sid) : NULL;
        if (series) {
            cJSON *parts = db_series_posts(req->db, sid, post_public);
            if (parts) cJSON_AddItemToObject(series, "posts", parts);
            cJSON_AddItemToObject(post, "series", series);
        }
    }
    if (post_public) {
        cJSON *created = cJSON_GetObjectItem(post, "created_at");
        cJSON_AddItemToObject(post, "adjacent",
            db_post_adjacent(req->db, post_id, cJSON_IsString(created) ? created->valuestring : NULL));
        cJSON *related = db_post_related_by_tags(req->db, post_id, 5);
        if (related) cJSON_AddItemToObject(post, "related", related);
    }
    const char *ephemeral_delete_pin = cwist_query_map_get(req->query_params, "delete_pin");
    cwist_sstring *page = render_post_detail(post, files, comments, dark, role, verified, vote_up, vote_down, user_vote, pp, author_pp, uid, ephemeral_delete_pin, mobile);
    if (page) {
        page_cache_set(key, page->data, page->size, 60);
        reqshare_finish(key, page);
    } else {
        reqshare_finish(key, NULL);
    }
    if (author_pp) free(author_pp);
    cJSON_Delete(post);
    if (files) cJSON_Delete(files);
    if (comments) cJSON_Delete(comments);
    send_html_res(res, page);
    free(pp);
}

void handler_post_drafts(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0;
    char role[32] = {0};
    if (!auth_require_login(req, res, &uid, role, sizeof(role))) return;
    bool admin = strcmp(role, "admin") == 0;
    /* Admins see everyone's queue, since they can edit any post. */
    cJSON *posts = db_post_list_unpublished(req->db, admin ? 0 : uid);
    char *pp = get_profile_pic(req->db, uid, role);
    cwist_sstring *page = render_post_drafts(posts, admin, is_dark(req), role, pp, is_mobile_request(req));
    if (posts) cJSON_Delete(posts);
    cwist_http_header_add(&res->headers, "Cache-Control", "no-store, private");
    send_html_res(res, page);
    free(pp);
}

void handler_post_new_get(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0;
    char role[32] = {0};
    auth_is_logged_in(req, &uid, role, sizeof(role));
    if (!require_post_scope(req, res, role)) return;
    char *pp = get_profile_pic(req->db, uid, role);
    cJSON *ordered = editor_boards(req->db, uid, role);
    int initial_board_id = 0;
    const char *board_slug = cwist_query_map_get(req->query_params, "board");
    if (board_slug && board_slug[0]) {
        cJSON *board = db_board_get_by_slug(req->db, board_slug);
        if (board) {
            initial_board_id = json_int(board, "id", 0);
            cJSON_Delete(board);
        }
    }
    int draft_count = uid > 0 ? db_post_count_drafts(req->db, uid) : 0;
    editor_options_t opts = editor_options_load(req->db, uid, role);
    cwist_sstring *page = render_post_editor(ordered, NULL, NULL, initial_board_id, is_dark(req), role, NULL, pp, is_mobile_request(req), draft_count);
    editor_options_free(&opts);
    if (ordered) cJSON_Delete(ordered);
    send_html_res(res, page);
    free(pp);
}

void handler_post_new_post(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0;
    char role[32] = {0};
    bool logged_in = auth_is_logged_in(req, &uid, role, sizeof(role));

    /* If the browser sent a session cookie but we could not verify it, do not
     * silently fall back to anonymous posting. The auth layer already logged
     * the precise failure reason. */
    if (!logged_in && auth_has_session_cookie(req)) {
        auth_require_login(req, res, &uid, role, sizeof(role));
        return;
    }
    if (!require_post_scope(req, res, role)) return;

    const char *ctype = cwist_http_header_get(req->headers, "Content-Type");
    char *title = NULL, *content = NULL, *summary = NULL, *board_id_str = NULL, *media_meta = NULL;
    char *post_action = NULL, *publish_at_in = NULL, *tags_in = NULL;
    char *series_in = NULL, *series_pos_in = NULL, *lang_in = NULL, *translation_in = NULL;
    char *spam_trap = NULL, *spam_token = NULL;
    form_field_t *files = NULL;

    FLY_LOG_DEBUG("ctype=%s body_len=%zu", ctype ? ctype : "NULL", req->body->size);
    FLY_LOG_DEBUG("body first 80 bytes: %.80s", req->body->data);
    if (ctype && strstr(ctype, "multipart/form-data")) {
        const char *bnd = strstr(ctype, "boundary=");
        if (bnd) {
            bnd += 9;
            if (*bnd == '"') bnd++;
            size_t bnd_len = strcspn(bnd, "\"\r\n; ");
            char *boundary = (char *)cwist_alloc(bnd_len + 1);
            memcpy(boundary, bnd, bnd_len);
            boundary[bnd_len] = '\0';
            FLY_LOG_DEBUG("boundary=%s", boundary);
            files = multipart_parse(req->body->data, req->body->size, boundary);
            cwist_free(boundary);
            for (form_field_t *ff = files; ff; ff = ff->next) {
                FLY_LOG_DEBUG("field name=%s len=%zu data=%.20s", ff->name, ff->len, ff->data ? ff->data : "NULL");
            }
            form_field_t *f;
            if ((f = form_find(files, "title"))) title = (char *)cwist_alloc(f->len+1), memcpy(title, f->data, f->len), title[f->len]=0;
            if (title) { char *unescaped = sql_unescape(title); cwist_free(title); title = unescaped; }
            if ((f = form_find(files, "content"))) content = (char *)cwist_alloc(f->len+1), memcpy(content, f->data, f->len), content[f->len]=0;
            if ((f = form_find(files, "summary"))) summary = (char *)cwist_alloc(f->len+1), memcpy(summary, f->data, f->len), summary[f->len]=0;
            if (summary) { char *unescaped = sql_unescape(summary); cwist_free(summary); summary = unescaped; }
            if ((f = form_find(files, "board_id"))) board_id_str = (char *)cwist_alloc(f->len+1), memcpy(board_id_str, f->data, f->len), board_id_str[f->len]=0;
            if ((f = form_find(files, "media_meta"))) media_meta = (char *)cwist_alloc(f->len+1), memcpy(media_meta, f->data, f->len), media_meta[f->len]=0;
            post_action = dup_form_field(files, "post_action");
            publish_at_in = dup_form_field(files, "publish_at");
            tags_in = dup_form_field(files, "tags");
            series_in = dup_form_field(files, "series");
            series_pos_in = dup_form_field(files, "series_pos");
            lang_in = dup_form_field(files, "lang");
            translation_in = dup_form_field(files, "translation_of");
            spam_trap = dup_form_field(files, SPAM_FIELD_TRAP);
            spam_token = dup_form_field(files, SPAM_FIELD_TOKEN);
            FLY_LOG_DEBUG("multipart parsed: title=%s content_len=%zu board_id=%s", title ? title : "NULL", content ? strlen(content) : 0, board_id_str ? board_id_str : "NULL");
        } else {
            FLY_LOG_DEBUG("boundary not found in ctype");
        }
    } else {
        cwist_query_map *kv = cwist_query_map_create(); cwist_query_map_parse(kv, req->body->data);
        title = (char *)cwist_alloc(strlen(cwist_query_map_get(kv, "title") ? cwist_query_map_get(kv, "title") : "")+1);
        strcpy(title, cwist_query_map_get(kv, "title") ? cwist_query_map_get(kv, "title") : "");
        if (title) { char *unescaped = sql_unescape(title); cwist_free(title); title = unescaped; }
        content = (char *)cwist_alloc(strlen(cwist_query_map_get(kv, "content") ? cwist_query_map_get(kv, "content") : "")+1);
        strcpy(content, cwist_query_map_get(kv, "content") ? cwist_query_map_get(kv, "content") : "");
        summary = (char *)cwist_alloc(strlen(cwist_query_map_get(kv, "summary") ? cwist_query_map_get(kv, "summary") : "")+1);
        strcpy(summary, cwist_query_map_get(kv, "summary") ? cwist_query_map_get(kv, "summary") : "");
        if (summary) { char *unescaped = sql_unescape(summary); cwist_free(summary); summary = unescaped; }
        board_id_str = (char *)cwist_alloc(strlen(cwist_query_map_get(kv, "board_id") ? cwist_query_map_get(kv, "board_id") : "0")+1);
        strcpy(board_id_str, cwist_query_map_get(kv, "board_id") ? cwist_query_map_get(kv, "board_id") : "0");
        media_meta = (char *)cwist_alloc(strlen(cwist_query_map_get(kv, "media_meta") ? cwist_query_map_get(kv, "media_meta") : "[]")+1);
        strcpy(media_meta, cwist_query_map_get(kv, "media_meta") ? cwist_query_map_get(kv, "media_meta") : "[]");
        post_action = dup_query_field(kv, "post_action");
        publish_at_in = dup_query_field(kv, "publish_at");
        tags_in = dup_query_field(kv, "tags");
        series_in = dup_query_field(kv, "series");
        series_pos_in = dup_query_field(kv, "series_pos");
        lang_in = dup_query_field(kv, "lang");
        translation_in = dup_query_field(kv, "translation_of");
        spam_trap = dup_query_field(kv, SPAM_FIELD_TRAP);
        spam_token = dup_query_field(kv, SPAM_FIELD_TOKEN);
        cwist_query_map_destroy(kv);
    }

    spam_verdict_t verdict = spam_guard_check(req, uid, role, spam_trap, spam_token);
    cwist_free(spam_trap);
    cwist_free(spam_token);
    if (verdict != SPAM_OK) {
        cwist_free(title); cwist_free(content); cwist_free(summary); cwist_free(board_id_str); cwist_free(media_meta); cwist_free(post_action); cwist_free(publish_at_in); cwist_free(tags_in); cwist_free(series_in); cwist_free(series_pos_in); cwist_free(lang_in); cwist_free(translation_in);
        multipart_free(files);
        spam_guard_reject(res, verdict);
        return;
    }

    if (!title || !content || !title[0] || !content[0]) {
        CWIST_LOG_WARN("Post creation failed: missing title or content uid=%d", uid);
        send_post_editor_error(req, res, uid, role, board_id_str ? atoi(board_id_str) : 0, "Title and content required");
        cwist_free(title); cwist_free(content); cwist_free(summary); cwist_free(board_id_str); cwist_free(media_meta); cwist_free(post_action); cwist_free(publish_at_in); cwist_free(tags_in); cwist_free(series_in); cwist_free(series_pos_in); cwist_free(lang_in); cwist_free(translation_in);
        multipart_free(files);
        return;
    }

    if (strlen(title) > MAX_POST_TITLE_LEN ||
        (summary && strlen(summary) > MAX_POST_SUMMARY_LEN) ||
        strlen(content) > MAX_POST_CONTENT_LEN) {
        CWIST_LOG_WARN("Post creation failed: input too long uid=%d", uid);
        send_post_editor_error(req, res, uid, role, board_id_str ? atoi(board_id_str) : 0, "Title, summary, or content is too long");
        cwist_free(title); cwist_free(content); cwist_free(summary); cwist_free(board_id_str); cwist_free(media_meta); cwist_free(post_action); cwist_free(publish_at_in); cwist_free(tags_in); cwist_free(series_in); cwist_free(series_pos_in); cwist_free(lang_in); cwist_free(translation_in);
        multipart_free(files);
        return;
    }

    int board_id = board_id_str ? atoi(board_id_str) : 0;
    const char *board_error = post_board_error(req->db, board_id, uid, role);
    if (board_error) {
        CWIST_LOG_WARN("Post creation refused: %s uid=%d board_id=%d", board_error, uid, board_id);
        send_post_editor_error(req, res, uid, role, 0, board_error);
        cwist_free(title); cwist_free(content); cwist_free(summary); cwist_free(board_id_str); cwist_free(media_meta); cwist_free(post_action); cwist_free(publish_at_in); cwist_free(tags_in); cwist_free(series_in); cwist_free(series_pos_in); cwist_free(lang_in); cwist_free(translation_in);
        multipart_free(files);
        return;
    }
    rewrite_content_legacy_urls(req->db, &content);
    char *sl = generate_slug(title);

    /* PQC sign: title + "\n" + content */
    size_t msg_len = (title ? strlen(title) : 0) + 1 + (content ? strlen(content) : 0);
    char *msg = (char *)cwist_alloc(msg_len + 1);
    snprintf(msg, msg_len + 1, "%s\n%s", title ? title : "", content ? content : "");
    char *sig_b64 = NULL;
    fly_crypto_sign((const uint8_t *)msg, strlen(msg), &sig_b64);
    cwist_free(msg);

    publish_choice_t pub;
    choose_publish(&pub, uid, post_action, publish_at_in, false);
    char now[POST_TIME_LEN];
    post_utc_now(now);
    bool public_now = strcmp(pub.status, POST_STATUS_PUBLISHED) == 0 &&
                      (!pub.publish_at || strcmp(pub.publish_at, now) <= 0);

    int created_id = 0;
    char *created_slug = NULL;
    created_id = db_post_create_with_auto_slug(req->db, board_id, uid, title, sl, content, summary ? summary : "", sig_b64 ? sig_b64 : "", 0, 0, "", pub.status, pub.publish_at, &created_slug);
    if (!created_slug) {
        CWIST_LOG_ERROR("Post creation failed: uid=%d board_id=%d", uid, board_id);
        res->status_code = CWIST_HTTP_INTERNAL_ERROR;
        cwist_sstring_assign(res->body, "Post creation failed");
        if (sig_b64) cwist_free(sig_b64);
        cwist_free(sl);
        cwist_free(title); cwist_free(content); cwist_free(summary); cwist_free(board_id_str); cwist_free(media_meta); cwist_free(post_action); cwist_free(publish_at_in); cwist_free(tags_in); cwist_free(series_in); cwist_free(series_pos_in); cwist_free(lang_in); cwist_free(translation_in);
        multipart_free(files);
        return;
    }
    db_tag_set_for_post(req->db, created_id, tags_in);
    apply_series_and_lang(req->db, created_id, uid, role, series_in, series_pos_in, lang_in, translation_in);
    CWIST_LOG_INFO("Post created: uid=%d slug='%s' board_id=%d status=%s publish_at=%s", uid, created_slug, board_id,
                   pub.status, pub.publish_at ? pub.publish_at : "now");
    if (sig_b64) cwist_free(sig_b64);

    if (!public_now) {
        /* Drafts and scheduled posts are not on any listing yet. */
        if (pub.publish_at) post_schedule_note(pub.publish_at);
        attach_media_meta_to_post(req->db, media_meta, created_id, uid, role);
        cwist_free(sl);
        cwist_free(created_slug);
        cwist_free(title); cwist_free(content); cwist_free(summary); cwist_free(board_id_str); cwist_free(media_meta); cwist_free(post_action); cwist_free(publish_at_in); cwist_free(tags_in); cwist_free(series_in); cwist_free(series_pos_in); cwist_free(lang_in); cwist_free(translation_in);
        multipart_free(files);
        redirect(res, "/account/drafts");
        return;
    }

    if (uid == 0) {
        char delete_pin[13];
        char delete_pin_hash[512];
        delete_pin[0] = '\0';
        if (random_hex_local(delete_pin, 6) && auth_hash_password(delete_pin, delete_pin_hash, sizeof(delete_pin_hash))) {
            (void)db_post_set_delete_pin_hash(req->db, created_id, delete_pin_hash);
            char redirect_with_pin[1024];
            snprintf(redirect_with_pin, sizeof(redirect_with_pin), "/post/%s?delete_pin=%s", created_slug, delete_pin);
            post_schedule_announce(req->db);
            attach_media_meta_to_post(req->db, media_meta, created_id, uid, role);
            cwist_free(sl);
            cwist_free(created_slug);
            cwist_free(title); cwist_free(content); cwist_free(summary); cwist_free(board_id_str); cwist_free(media_meta); cwist_free(post_action); cwist_free(publish_at_in); cwist_free(tags_in); cwist_free(series_in); cwist_free(series_pos_in); cwist_free(lang_in); cwist_free(translation_in);
            multipart_free(files);
            redirect(res, redirect_with_pin);
            return;
        }
    }

    /* Publish post metadata to NATS for distributed subscribers */
    post_schedule_announce(req->db);

    attach_media_meta_to_post(req->db, media_meta, created_id, uid, role);

    /* New posts appear on home and board listings, so clear those caches. */
    page_cache_invalidate_all();

    cwist_free(sl);
    cwist_free(created_slug);
    cwist_free(title); cwist_free(content); cwist_free(summary); cwist_free(board_id_str); cwist_free(media_meta); cwist_free(post_action); cwist_free(publish_at_in); cwist_free(tags_in); cwist_free(series_in); cwist_free(series_pos_in); cwist_free(lang_in); cwist_free(translation_in);
    multipart_free(files);
    redirect(res, "/");
}

void handler_post_edit_get(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0;
    char role[32] = {0};
    if (!auth_require_login(req, res, &uid, role, sizeof(role))) return;
    const char *id_str = cwist_query_map_get(req->path_params, "id");
    if (!id_str) { redirect(res, "/"); return; }
    cJSON *post = db_post_get_by_id(req->db, atoi(id_str));
    if (!post) { redirect(res, "/"); return; }
    if (!is_author_or_admin(post, uid, role)) {
        res->status_code = CWIST_HTTP_FORBIDDEN;
        cwist_sstring_assign(res->body, "Forbidden");
        cJSON_Delete(post);
        return;
    }
    cJSON *ordered = editor_boards(req->db, uid, role);
    char *pp = get_profile_pic(req->db, uid, role);
    int post_id_val = json_int(post, "id", 0);
    cJSON *files = db_file_list_by_post(req->db, post_id_val);
    attach_post_tags(req->db, post);
    const char *error = cwist_query_map_get(req->query_params, "error");
    const char *error_msg = NULL;
    if (error && strcmp(error, "board") == 0) error_msg = BOARD_ERROR_REQUIRED;
    else if (error && strcmp(error, "board_denied") == 0) error_msg = BOARD_ERROR_DENIED;
    attach_editor_fields(req->db, post);
    editor_options_t opts = editor_options_load(req->db, uid, role);
    cwist_sstring *page = render_post_editor(ordered, post, files, 0, is_dark(req), role, error_msg, pp, is_mobile_request(req), 0);
    editor_options_free(&opts);
    cJSON_Delete(post);
    if (files) cJSON_Delete(files);
    if (ordered) cJSON_Delete(ordered);
    send_html_res(res, page);
    free(pp);
}

void handler_post_edit_post(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0;
    char role[32] = {0};
    if (!auth_require_login(req, res, &uid, role, sizeof(role))) return;

    const char *ctype = cwist_http_header_get(req->headers, "Content-Type");
    char *title = NULL, *content = NULL, *summary = NULL, *id_str = NULL, *board_id_str = NULL, *media_meta = NULL;
    char *post_action = NULL, *publish_at_in = NULL, *tags_in = NULL;
    char *series_in = NULL, *series_pos_in = NULL, *lang_in = NULL, *translation_in = NULL;
    form_field_t *files = NULL;

    const char *path_id = cwist_query_map_get(req->path_params, "id");
    if (path_id) {
        id_str = (char *)cwist_alloc(strlen(path_id)+1);
        strcpy(id_str, path_id);
    }

    /* Deduplicate concurrent Edit requests for the same post.
     * If the user clicked Submit multiple times (or the browser retried),
     * only the first request executes the DB write.  Subsequent in-flight
     * duplicates receive 409 and are silently redirected by the browser. */
    char wl_key[64];
    snprintf(wl_key, sizeof(wl_key), "edit:post:%s", id_str ? id_str : "");
    if (!reqshare_write_lock_try(wl_key)) {
        CWIST_LOG_WARN("Post edit deduplicated (concurrent duplicate): id=%s uid=%d", id_str ? id_str : "", uid);
        cwist_free(id_str);
        res->status_code = (cwist_http_status_t)409;
        cwist_sstring_assign(res->body, "Duplicate request – the post is already being saved.");
        return;
    }

    if (ctype && strstr(ctype, "multipart/form-data")) {
        const char *bnd = strstr(ctype, "boundary=");
        if (bnd) {
            bnd += 9;
            if (*bnd == '"') bnd++;
            size_t bnd_len = strcspn(bnd, "\"\r\n; ");
            char *boundary = (char *)cwist_alloc(bnd_len + 1);
            memcpy(boundary, bnd, bnd_len);
            boundary[bnd_len] = '\0';
            files = multipart_parse(req->body->data, req->body->size, boundary);
            cwist_free(boundary);
            form_field_t *f;

            if ((f = form_find(files, "title"))) title = (char *)cwist_alloc(f->len+1), memcpy(title, f->data, f->len), title[f->len]=0;
            if (title) { char *unescaped = sql_unescape(title); cwist_free(title); title = unescaped; }
            if ((f = form_find(files, "content"))) content = (char *)cwist_alloc(f->len+1), memcpy(content, f->data, f->len), content[f->len]=0;
            if ((f = form_find(files, "summary"))) summary = (char *)cwist_alloc(f->len+1), memcpy(summary, f->data, f->len), summary[f->len]=0;
            if (summary) { char *unescaped = sql_unescape(summary); cwist_free(summary); summary = unescaped; }
            if ((f = form_find(files, "board_id"))) board_id_str = (char *)cwist_alloc(f->len+1), memcpy(board_id_str, f->data, f->len), board_id_str[f->len]=0;
            if ((f = form_find(files, "media_meta"))) media_meta = (char *)cwist_alloc(f->len+1), memcpy(media_meta, f->data, f->len), media_meta[f->len]=0;
            post_action = dup_form_field(files, "post_action");
            publish_at_in = dup_form_field(files, "publish_at");
            tags_in = dup_form_field(files, "tags");
            series_in = dup_form_field(files, "series");
            series_pos_in = dup_form_field(files, "series_pos");
            lang_in = dup_form_field(files, "lang");
            translation_in = dup_form_field(files, "translation_of");
        }
    } else {
        cwist_query_map *kv = cwist_query_map_create(); cwist_query_map_parse(kv, req->body->data);

        title = (char *)cwist_alloc(strlen(cwist_query_map_get(kv, "title") ? cwist_query_map_get(kv, "title") : "")+1);
        strcpy(title, cwist_query_map_get(kv, "title") ? cwist_query_map_get(kv, "title") : "");
        if (title) { char *unescaped = sql_unescape(title); cwist_free(title); title = unescaped; }
        content = (char *)cwist_alloc(strlen(cwist_query_map_get(kv, "content") ? cwist_query_map_get(kv, "content") : "")+1);
        strcpy(content, cwist_query_map_get(kv, "content") ? cwist_query_map_get(kv, "content") : "");
        summary = (char *)cwist_alloc(strlen(cwist_query_map_get(kv, "summary") ? cwist_query_map_get(kv, "summary") : "")+1);
        strcpy(summary, cwist_query_map_get(kv, "summary") ? cwist_query_map_get(kv, "summary") : "");
        if (summary) { char *unescaped = sql_unescape(summary); cwist_free(summary); summary = unescaped; }
        board_id_str = (char *)cwist_alloc(strlen(cwist_query_map_get(kv, "board_id") ? cwist_query_map_get(kv, "board_id") : "0")+1);
        strcpy(board_id_str, cwist_query_map_get(kv, "board_id") ? cwist_query_map_get(kv, "board_id") : "0");
        media_meta = (char *)cwist_alloc(strlen(cwist_query_map_get(kv, "media_meta") ? cwist_query_map_get(kv, "media_meta") : "[]")+1);
        strcpy(media_meta, cwist_query_map_get(kv, "media_meta") ? cwist_query_map_get(kv, "media_meta") : "[]");
        post_action = dup_query_field(kv, "post_action");
        publish_at_in = dup_query_field(kv, "publish_at");
        tags_in = dup_query_field(kv, "tags");
        series_in = dup_query_field(kv, "series");
        series_pos_in = dup_query_field(kv, "series_pos");
        lang_in = dup_query_field(kv, "lang");
        translation_in = dup_query_field(kv, "translation_of");
        cwist_query_map_destroy(kv);
    }

    if (!id_str || !title || !content || !title[0] || !content[0]) {
        reqshare_write_lock_release(wl_key);
        cwist_free(title); cwist_free(content); cwist_free(summary); cwist_free(id_str); cwist_free(board_id_str); cwist_free(media_meta); cwist_free(post_action); cwist_free(publish_at_in); cwist_free(tags_in); cwist_free(series_in); cwist_free(series_pos_in); cwist_free(lang_in); cwist_free(translation_in);
        multipart_free(files);
        redirect(res, "/");
        return;
    }

    if (strlen(title) > MAX_POST_TITLE_LEN ||
        (summary && strlen(summary) > MAX_POST_SUMMARY_LEN) ||
        strlen(content) > MAX_POST_CONTENT_LEN) {
        CWIST_LOG_WARN("Post edit failed: input too long id=%s uid=%d", id_str, uid);
        reqshare_write_lock_release(wl_key);
        cwist_free(title); cwist_free(content); cwist_free(summary); cwist_free(id_str); cwist_free(board_id_str); cwist_free(media_meta); cwist_free(post_action); cwist_free(publish_at_in); cwist_free(tags_in); cwist_free(series_in); cwist_free(series_pos_in); cwist_free(lang_in); cwist_free(translation_in);
        multipart_free(files);
        redirect(res, "/");
        return;
    }

    cJSON *post = db_post_get_by_id(req->db, atoi(id_str));
    if (!post) {
        CWIST_LOG_WARN("Post edit failed: post not found id=%s uid=%d", id_str, uid);
        reqshare_write_lock_release(wl_key);
        cwist_free(title); cwist_free(content); cwist_free(summary); cwist_free(id_str); cwist_free(board_id_str); cwist_free(media_meta); cwist_free(post_action); cwist_free(publish_at_in); cwist_free(tags_in); cwist_free(series_in); cwist_free(series_pos_in); cwist_free(lang_in); cwist_free(translation_in);
        multipart_free(files);
        redirect(res, "/");
        return;
    }
    if (!is_author_or_admin(post, uid, role)) {
        CWIST_LOG_WARN("Post edit forbidden: id=%s uid=%d role=%s", id_str, uid, role);
        res->status_code = CWIST_HTTP_FORBIDDEN;
        cwist_sstring_assign(res->body, "Forbidden");
        cJSON_Delete(post);
        reqshare_write_lock_release(wl_key);
        cwist_free(title); cwist_free(content); cwist_free(summary); cwist_free(id_str); cwist_free(board_id_str); cwist_free(media_meta); cwist_free(post_action); cwist_free(publish_at_in); cwist_free(tags_in); cwist_free(series_in); cwist_free(series_pos_in); cwist_free(lang_in); cwist_free(translation_in);
        multipart_free(files);
        return;
    }
    int board_id = board_id_str ? atoi(board_id_str) : 0;
    const char *board_error = post_board_error(req->db, board_id, uid, role);
    if (board_error) {
        CWIST_LOG_WARN("Post edit refused: %s id=%s uid=%d board_id=%d", board_error, id_str, uid, board_id);
        char edit_url[96];
        snprintf(edit_url, sizeof(edit_url), "/post/%d/edit?error=%s", json_int(post, "id", 0),
                 strcmp(board_error, BOARD_ERROR_DENIED) == 0 ? "board_denied" : "board");
        cJSON_Delete(post);
        reqshare_write_lock_release(wl_key);
        cwist_free(title); cwist_free(content); cwist_free(summary); cwist_free(id_str); cwist_free(board_id_str); cwist_free(media_meta); cwist_free(post_action); cwist_free(publish_at_in); cwist_free(tags_in); cwist_free(series_in); cwist_free(series_pos_in); cwist_free(lang_in); cwist_free(translation_in);
        multipart_free(files);
        redirect(res, edit_url);
        return;
    }
    cJSON *slug_obj = cJSON_GetObjectItem(post, "slug");
    char *post_slug = (slug_obj && slug_obj->valuestring) ? strdup(slug_obj->valuestring) : NULL;
    bool was_public = post_is_public(post);
    publish_choice_t pub;
    choose_publish(&pub, uid, post_action, publish_at_in, was_public);
    cJSON_Delete(post);

    rewrite_content_legacy_urls(req->db, &content);
    size_t msg_len2 = (title ? strlen(title) : 0) + 1 + (content ? strlen(content) : 0);
    char *msg2 = (char *)cwist_alloc(msg_len2 + 1);
    snprintf(msg2, msg_len2 + 1, "%s\n%s", title ? title : "", content ? content : "");
    char *sig_b642 = NULL;
    fly_crypto_sign((const uint8_t *)msg2, strlen(msg2), &sig_b642);
    cwist_free(msg2);
    if (db_post_update(req->db, atoi(id_str), board_id, title, content, summary ? summary : "", sig_b642 ? sig_b642 : "", 0, 0, "", pub.status, pub.publish_at)) {
        CWIST_LOG_INFO("Post updated: id=%s uid=%d board_id=%d status=%s publish_at=%s", id_str, uid, board_id,
                       pub.status, pub.publish_at ? pub.publish_at : "kept");
        if (pub.publish_at) post_schedule_note(pub.publish_at);
    } else {
        CWIST_LOG_ERROR("Post update failed: id=%s uid=%d", id_str, uid);
    }
    if (sig_b642) cwist_free(sig_b642);

    attach_media_meta_to_post(req->db, media_meta, atoi(id_str), uid, role);
    if (tags_in) db_tag_set_for_post(req->db, atoi(id_str), tags_in);
    apply_series_and_lang(req->db, atoi(id_str), uid, role, series_in, series_pos_in, lang_in, translation_in);

    cJSON *saved = db_post_get_by_id(req->db, atoi(id_str));
    bool public_now = post_is_public(saved);
    if (saved) cJSON_Delete(saved);
    if (public_now != was_public) post_schedule_bump();
    if (post_slug) {
        /* A live post's edit is re-broadcast; a newly public one is
         * announced once through its claim flag. */
        if (public_now && was_public) fly_nats_publish_post(title, post_slug, summary ? summary : "");
        else if (public_now) post_schedule_announce(req->db);
        page_cache_invalidate_post(post_slug);
    }
    if (id_str) {
        page_cache_invalidate_post(id_str);
    }
    page_cache_invalidate_all();

    reqshare_write_lock_release(wl_key);
    cwist_free(title); cwist_free(content); cwist_free(summary); cwist_free(board_id_str); cwist_free(media_meta); cwist_free(post_action); cwist_free(publish_at_in); cwist_free(tags_in); cwist_free(series_in); cwist_free(series_pos_in); cwist_free(lang_in); cwist_free(translation_in);
    multipart_free(files);
    char redir_target[128];
    if (!public_now) {
        snprintf(redir_target, sizeof(redir_target), "/account/drafts");
    } else if (post_slug && post_slug[0]) {
        snprintf(redir_target, sizeof(redir_target), "/post/%s", post_slug);
    } else {
        snprintf(redir_target, sizeof(redir_target), "/post/%s", id_str ? id_str : "");
    }
    if (post_slug) free(post_slug);
    cwist_free(id_str);
    redirect(res, redir_target);
}
/* Delete a post with its files and comments and drop the cached pages that
 * show it. Shared by the delete route and the report queue. */
bool post_delete_everything(cwist_db *db, int post_id, const char *slug) {
    /* Delete files and the post record in one main-DB transaction so a crash
     * in the middle cannot leave a post pointing to deleted files. */
    bool tx_ok = db_transaction_begin(db);
    db_file_delete_by_post(db, post_id);
    bool post_deleted = db_post_delete(db, post_id);
    if (tx_ok) {
        if (post_deleted) {
            if (!db_transaction_commit(db)) {
                db_transaction_rollback(db);
                post_deleted = false;
            }
        } else {
            db_transaction_rollback(db);
        }
    }
    if (!post_deleted) return false;
    /* Comments live in a separate database; best-effort cleanup after the
     * main transaction commits. */
    db_comment_delete_by_target("post", post_id);
    if (slug) page_cache_invalidate_post(slug);
    else page_cache_invalidate_all();
    return true;
}

void handler_post_delete(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0;
    char role[32] = {0};
    bool logged_in = auth_is_logged_in(req, &uid, role, sizeof(role));
    const char *id_str = cwist_query_map_get(req->path_params, "id");
    const char *delete_pin = cwist_query_map_get(req->query_params, "delete_pin");

    /* A request that carried a session cookie but failed verification is a
     * logged-in flow that lost auth, not an anonymous delete attempt. */
    if (!logged_in && auth_has_session_cookie(req)) {
        auth_require_login(req, res, &uid, role, sizeof(role));
        return;
    }

    if (!id_str) { redirect(res, "/"); return; }
    cJSON *post = db_post_get_by_id(req->db, atoi(id_str));
    if (!post) { CWIST_LOG_WARN("Post delete failed: not found id=%s uid=%d", id_str, uid); redirect(res, "/"); return; }
    cJSON *pin_hash = cJSON_GetObjectItem(post, "delete_pin_hash");
    bool pin_ok = delete_pin && pin_hash && pin_hash->valuestring && pin_hash->valuestring[0] &&
                  auth_verify_password(delete_pin, pin_hash->valuestring);

    if (!is_author_or_admin(post, uid, role) && !pin_ok) {
        CWIST_LOG_WARN("Post delete forbidden: id=%s uid=%d role=%s", id_str, uid, role);
        res->status_code = CWIST_HTTP_FORBIDDEN;
        cwist_sstring_assign(res->body, "Forbidden");
        cJSON_Delete(post);
        return;
    }
    bool was_unpublished = !post_is_public(post);
    cJSON *slug_item = cJSON_GetObjectItem(post, "slug");
    char *deleted_slug = (slug_item && cJSON_IsString(slug_item) && slug_item->valuestring) ? strdup(slug_item->valuestring) : NULL;
    cJSON_Delete(post);
    int post_id = atoi(id_str);

    bool post_deleted = post_delete_everything(req->db, post_id, deleted_slug);
    free(deleted_slug);

    if (post_deleted) {
        CWIST_LOG_INFO("Post deleted: id=%s uid=%d", id_str, uid);
        const char *ref = cwist_http_header_get(req->headers, "Referer");
        if ((ref && strstr(ref, "/drafts")) || was_unpublished) {
            redirect(res, "/account/drafts");
        } else {
            redirect(res, "/");
        }
    } else {
        CWIST_LOG_ERROR("Post delete failed: id=%s uid=%d", id_str, uid);
        res->status_code = CWIST_HTTP_INTERNAL_ERROR;
        cwist_sstring_assign(res->body, "Failed to delete post");
    }
}

/* ---- Files ---- */

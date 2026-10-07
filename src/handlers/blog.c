#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "handlers_internal.h"
#include <ctype.h>

/* Blog browsing: tag pages, the archive, and RSS feeds (site, board, tag).
 * Every listing here shows public posts only (POST_PUBLIC_SQL). Anonymous
 * replies are kept by the route-level BDR cache, so these handlers do not
 * use the page cache. */

#define BLOG_PER_PAGE 20
#define FEED_ITEMS 20

/* Percent-decode one path segment (cwist hands path params over raw).
 * Returns false on a malformed escape or an embedded NUL. */
static bool decode_segment(const char *in, char *out, size_t out_size) {
    size_t o = 0;
    for (size_t i = 0; in[i]; i++) {
        if (o + 1 >= out_size) return false;
        if (in[i] == '%') {
            if (!isxdigit((unsigned char)in[i + 1]) || !isxdigit((unsigned char)in[i + 2])) return false;
            char hex[3] = {in[i + 1], in[i + 2], '\0'};
            char c = (char)strtol(hex, NULL, 16);
            if (c == '\0') return false;
            out[o++] = c;
            i += 2;
        } else {
            out[o++] = in[i];
        }
    }
    out[o] = '\0';
    return true;
}

static void append_segment(cwist_sstring *s, const char *seg) {
    static const char hex[] = "0123456789ABCDEF";
    for (const unsigned char *p = (const unsigned char *)seg; *p; p++) {
        if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~') {
            cwist_sstring_append_len(s, (const char *)p, 1);
        } else {
            char esc[4] = {'%', hex[*p >> 4], hex[*p & 15], '\0'};
            cwist_sstring_append(s, esc);
        }
    }
}

static int page_param(cwist_http_request *req) {
    const char *p = cwist_query_map_get(req->query_params, "page");
    int page = p ? atoi(p) : 1;
    return page < 1 ? 1 : page;
}

static void not_found(cwist_http_response *res, const char *msg) {
    res->status_code = CWIST_HTTP_NOT_FOUND;
    cwist_sstring_assign(res->body, msg);
}

/* ---- Tag and month listings ---- */

static void send_index(cwist_http_request *req, cwist_http_response *res, const char *heading, const char *lead,
                       cJSON *posts, int page, int total_pages, const char *base_path, const char *feed_path) {
    int uid = 0;
    char role[32] = {0};
    auth_is_logged_in(req, &uid, role, sizeof(role));
    char *pp = get_profile_pic(req->db, uid, role);
    cwist_sstring *html = render_post_index(heading, lead, posts, page, total_pages, base_path, feed_path,
                                            is_dark(req), role, pp, uid, is_mobile_request(req));
    send_html_res(res, html);
    free(pp);
}

void handler_tag_get(cwist_http_request *req, cwist_http_response *res) {
    const char *raw = cwist_query_map_get(req->path_params, "name");
    char tag[DB_TAG_MAX_BYTES];
    if (!raw || !decode_segment(raw, tag, sizeof(tag)) || !db_tag_name_valid(tag)) {
        not_found(res, "Tag not found");
        return;
    }
    int total = db_post_count_by_tag(req->db, tag);
    if (total == 0) {
        not_found(res, "Tag not found");
        return;
    }
    int total_pages = (total + BLOG_PER_PAGE - 1) / BLOG_PER_PAGE;
    int page = page_param(req);
    if (page > total_pages) page = total_pages;
    cJSON *posts = db_post_list_by_tag(req->db, tag, BLOG_PER_PAGE, (page - 1) * BLOG_PER_PAGE);

    cwist_sstring *base = cwist_sstring_create();
    cwist_sstring_assign(base, "/tag/");
    append_segment(base, tag);
    cwist_sstring *feed = cwist_sstring_create();
    cwist_sstring_assign(feed, base->data);
    cwist_sstring_append(feed, "/rss.xml");
    char heading[DB_TAG_MAX_BYTES + 2];
    snprintf(heading, sizeof(heading), "#%s", tag);
    char lead[48];
    snprintf(lead, sizeof(lead), "%d post%s", total, total == 1 ? "" : "s");

    send_index(req, res, heading, lead, posts, page, total_pages, base->data, feed->data);
    cwist_sstring_destroy(base);
    cwist_sstring_destroy(feed);
    if (posts) cJSON_Delete(posts);
}

void handler_archive_month_get(cwist_http_request *req, cwist_http_response *res) {
    const char *ym = cwist_query_map_get(req->path_params, "ym");
    int total = ym ? db_post_count_by_month(req->db, ym) : 0;
    if (total == 0) {
        not_found(res, "No posts for that month");
        return;
    }
    int total_pages = (total + BLOG_PER_PAGE - 1) / BLOG_PER_PAGE;
    int page = page_param(req);
    if (page > total_pages) page = total_pages;
    cJSON *posts = db_post_list_by_month(req->db, ym, BLOG_PER_PAGE, (page - 1) * BLOG_PER_PAGE);
    /* ym was validated by the count query: exactly "YYYY-MM". */
    char base[32];
    snprintf(base, sizeof(base), "/archive/%.7s", ym);
    char lead[48];
    snprintf(lead, sizeof(lead), "%d post%s", total, total == 1 ? "" : "s");
    send_index(req, res, base + 9, lead, posts, page, total_pages, base, NULL);
    if (posts) cJSON_Delete(posts);
}

void handler_archive_get(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0;
    char role[32] = {0};
    auth_is_logged_in(req, &uid, role, sizeof(role));
    cJSON *months = db_post_archive_months(req->db);
    cJSON *tags = db_tag_list_public(req->db);
    cJSON *series = db_series_list(req->db, 0, true);
    char *pp = get_profile_pic(req->db, uid, role);
    cwist_sstring *html = render_archive(months, tags, series, is_dark(req), role, pp, is_mobile_request(req));
    if (series) cJSON_Delete(series);
    send_html_res(res, html);
    free(pp);
    if (months) cJSON_Delete(months);
    if (tags) cJSON_Delete(tags);
}

void handler_tags_get(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    redirect(res, "/archive#tags");
}

/* ---- RSS ---- */

static void append_root(cwist_sstring *s, const char *path) {
    size_t rl = strlen(g_config.root_url);
    if (rl > 0 && g_config.root_url[rl - 1] == '/') rl--;
    cwist_sstring_append_len(s, g_config.root_url, rl);
    cwist_sstring_append(s, path);
}

/* "YYYY-MM-DD HH:MM:SS" (UTC) -> RFC 1123 date, as RSS and HTTP want it. */
static void http_date(const char *sql_time, char *out, size_t out_size) {
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    out[0] = '\0';
    if (!sql_time || sscanf(sql_time, "%d-%d-%d %d:%d:%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
                            &tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 6) {
        return;
    }
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    time_t t = timegm(&tm);
    struct tm norm;
    gmtime_r(&t, &norm);
    static const char *const days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char *const months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                         "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    /* Fixed English names: strftime's %a/%b follow the process locale. */
    snprintf(out, out_size, "%s, %02d %s %04d %02d:%02d:%02d GMT", days[norm.tm_wday], norm.tm_mday,
             months[norm.tm_mon], norm.tm_year + 1900, norm.tm_hour, norm.tm_min, norm.tm_sec);
}

/* Rendered post HTML for content:encoded. Root-relative links and images
 * become absolute (feed readers have no base URL), and the result is
 * wrapped in CDATA with any "]]>" split across sections. */
static void append_cdata_html(cwist_sstring *rss, const char *md) {
    cwist_sstring *html = render_markdown_to_html(md ? md : "");
    if (!html) return;
    cwist_sstring_append(rss, "<![CDATA[");
    const char *p = html->data ? html->data : "";
    while (*p) {
        if (strncmp(p, "]]>", 3) == 0) {
            cwist_sstring_append(rss, "]]]]><![CDATA[>");
            p += 3;
            continue;
        }
        static const char *const attrs[] = {"src=\"/", "href=\"/", "src='/", "href='/", "poster=\"/", "poster='/"};
        bool rewritten = false;
        for (size_t i = 0; i < sizeof(attrs) / sizeof(attrs[0]); i++) {
            size_t n = strlen(attrs[i]);
            /* "//host" is protocol-relative, not root-relative. */
            if (strncmp(p, attrs[i], n) == 0 && p[n] != '/') {
                cwist_sstring_append_len(rss, p, n - 1);
                append_root(rss, "/");
                p += n;
                rewritten = true;
                break;
            }
        }
        if (rewritten) continue;
        cwist_sstring_append_len(rss, p, 1);
        p++;
    }
    cwist_sstring_append(rss, "]]>");
    cwist_sstring_destroy(html);
}

static void send_feed(cwist_http_request *req, cwist_http_response *res, cJSON *posts,
                      const char *title, const char *link_path, const char *self_path) {
    /* Newest edit across the items (and the item count, so a deletion
     * changes it too) drives the validators. */
    const char *newest = "";
    int n = posts ? cJSON_GetArraySize(posts) : 0;
    for (int i = 0; i < n; i++) {
        cJSON *upd = cJSON_GetObjectItem(cJSON_GetArrayItem(posts, i), "updated_at");
        if (upd && cJSON_IsString(upd) && strcmp(upd->valuestring, newest) > 0) newest = upd->valuestring;
    }
    char etag[160] = {0};
    char last_modified[64] = {0};
    if (newest[0]) {
        snprintf(etag, sizeof(etag), "\"fly-%.19s-%d\"", newest, n);
        for (char *e = etag; *e; e++) if (*e == ' ' || *e == ':') *e = '-';
        http_date(newest, last_modified, sizeof(last_modified));
    }
    const char *if_none = cwist_http_header_get(req->headers, "If-None-Match");
    const char *if_mod = cwist_http_header_get(req->headers, "If-Modified-Since");
    if (etag[0] && ((if_none && strcmp(if_none, etag) == 0) ||
                    (!if_none && if_mod && strcmp(if_mod, last_modified) == 0))) {
        res->status_code = 304;
        cwist_http_header_add(&res->headers, "ETag", etag);
        return;
    }

    cwist_sstring *rss = cwist_sstring_create();
    cwist_sstring_assign(rss, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<rss version=\"2.0\" xmlns:atom=\"http://www.w3.org/2005/Atom\" "
        "xmlns:content=\"http://purl.org/rss/1.0/modules/content/\" "
        "xmlns:dc=\"http://purl.org/dc/elements/1.1/\">\n<channel>\n");
    cwist_sstring_append(rss, "<title>"); cwist_sstring_append_escaped(rss, title); cwist_sstring_append(rss, "</title>\n");
    cwist_sstring_append(rss, "<link>"); append_root(rss, link_path); cwist_sstring_append(rss, "</link>\n");
    cwist_sstring_append(rss, "<atom:link href=\"");
    append_root(rss, self_path);
    cwist_sstring_append(rss, "\" rel=\"self\" type=\"application/rss+xml\"/>\n");
    cwist_sstring_append(rss, "<description>"); cwist_sstring_append_escaped(rss, g_config.subtitle); cwist_sstring_append(rss, "</description>\n");
    cwist_sstring_append(rss, "<language>ko</language>\n");
    cwist_sstring_append(rss, "<generator>fly.board</generator>\n");
    if (last_modified[0]) {
        cwist_sstring_append(rss, "<lastBuildDate>"); cwist_sstring_append(rss, last_modified); cwist_sstring_append(rss, "</lastBuildDate>\n");
    }

    char date[64];
    for (int i = 0; i < n; i++) {
        cJSON *p = cJSON_GetArrayItem(posts, i);
        cJSON *slug = cJSON_GetObjectItem(p, "slug");
        if (!slug || !cJSON_IsString(slug) || !slug->valuestring[0]) continue;
        cJSON *ptitle = cJSON_GetObjectItem(p, "title");
        cJSON *summary = cJSON_GetObjectItem(p, "summary");
        cJSON *content = cJSON_GetObjectItem(p, "content");
        cJSON *author = cJSON_GetObjectItem(p, "author_name");
        cJSON *created = cJSON_GetObjectItem(p, "created_at");
        cwist_sstring *url = cwist_sstring_create();
        cwist_sstring_assign(url, "/post/");
        append_segment(url, slug->valuestring);

        cwist_sstring_append(rss, "<item>\n<title>");
        cwist_sstring_append_escaped(rss, cJSON_IsString(ptitle) ? ptitle->valuestring : "");
        cwist_sstring_append(rss, "</title>\n<link>");
        append_root(rss, url->data);
        cwist_sstring_append(rss, "</link>\n<guid isPermaLink=\"true\">");
        append_root(rss, url->data);
        cwist_sstring_append(rss, "</guid>\n");
        cwist_sstring_destroy(url);
        if (cJSON_IsString(author) && author->valuestring[0]) {
            cwist_sstring_append(rss, "<dc:creator>");
            cwist_sstring_append_escaped(rss, author->valuestring);
            cwist_sstring_append(rss, "</dc:creator>\n");
        }
        cJSON *tags = db_tag_list_by_post(req->db, json_int(p, "id", 0));
        cJSON *t = NULL;
        cJSON_ArrayForEach(t, tags) {
            cJSON *name = cJSON_GetObjectItem(t, "name");
            if (!cJSON_IsString(name)) continue;
            cwist_sstring_append(rss, "<category>");
            cwist_sstring_append_escaped(rss, name->valuestring);
            cwist_sstring_append(rss, "</category>\n");
        }
        if (tags) cJSON_Delete(tags);
        cwist_sstring_append(rss, "<description>");
        cwist_sstring_append_escaped(rss, cJSON_IsString(summary) ? summary->valuestring : "");
        cwist_sstring_append(rss, "</description>\n<content:encoded>");
        append_cdata_html(rss, cJSON_IsString(content) ? content->valuestring : "");
        cwist_sstring_append(rss, "</content:encoded>\n");
        http_date(cJSON_IsString(created) ? created->valuestring : NULL, date, sizeof(date));
        if (date[0]) {
            cwist_sstring_append(rss, "<pubDate>"); cwist_sstring_append(rss, date); cwist_sstring_append(rss, "</pubDate>\n");
        }
        cwist_sstring_append(rss, "</item>\n");
    }
    cwist_sstring_append(rss, "</channel>\n</rss>\n");

    cwist_http_header_add(&res->headers, "Content-Type", "application/rss+xml; charset=utf-8");
    cwist_http_header_add(&res->headers, "Cache-Control", "public, max-age=300");
    if (etag[0]) cwist_http_header_add(&res->headers, "ETag", etag);
    if (last_modified[0]) cwist_http_header_add(&res->headers, "Last-Modified", last_modified);
    cwist_sstring_assign(res->body, rss->data);
    cwist_sstring_destroy(rss);
}

void handler_rss_xml(cwist_http_request *req, cwist_http_response *res) {
    cJSON *posts = db_post_feed(req->db, 0, FEED_ITEMS);
    send_feed(req, res, posts, g_config.title, "/", "/rss.xml");
    if (posts) cJSON_Delete(posts);
}

void handler_board_rss_xml(cwist_http_request *req, cwist_http_response *res) {
    const char *slug = cwist_query_map_get(req->path_params, "slug");
    cJSON *board = slug ? db_board_get_by_slug(req->db, slug) : NULL;
    /* Feeds are fetched anonymously, so only boards anyone may read have one. */
    if (!board || json_int(board, "admin_only", 0) || json_int(board, "read_perm", 0) > 0) {
        if (board) cJSON_Delete(board);
        not_found(res, "Board not found");
        return;
    }
    cJSON *name = cJSON_GetObjectItem(board, "name");
    char title[512];
    snprintf(title, sizeof(title), "%s - %s", g_config.title,
             cJSON_IsString(name) ? name->valuestring : slug);
    cwist_sstring *link = cwist_sstring_create();
    cwist_sstring_assign(link, "/board/");
    append_segment(link, slug);
    cwist_sstring *self = cwist_sstring_create();
    cwist_sstring_assign(self, link->data);
    cwist_sstring_append(self, "/rss.xml");
    cJSON *posts = db_post_feed(req->db, json_int(board, "id", 0), FEED_ITEMS);
    send_feed(req, res, posts, title, link->data, self->data);
    if (posts) cJSON_Delete(posts);
    cwist_sstring_destroy(link);
    cwist_sstring_destroy(self);
    cJSON_Delete(board);
}

void handler_tag_rss_xml(cwist_http_request *req, cwist_http_response *res) {
    const char *raw = cwist_query_map_get(req->path_params, "name");
    char tag[DB_TAG_MAX_BYTES];
    if (!raw || !decode_segment(raw, tag, sizeof(tag)) || !db_tag_name_valid(tag)) {
        not_found(res, "Tag not found");
        return;
    }
    if (db_post_count_by_tag(req->db, tag) == 0) {
        not_found(res, "Tag not found");
        return;
    }
    cJSON *posts = db_post_list_by_tag(req->db, tag, FEED_ITEMS, 0);
    char title[512];
    snprintf(title, sizeof(title), "%s - #%s", g_config.title, tag);
    cwist_sstring *link = cwist_sstring_create();
    cwist_sstring_assign(link, "/tag/");
    append_segment(link, tag);
    cwist_sstring *self = cwist_sstring_create();
    cwist_sstring_assign(self, link->data);
    cwist_sstring_append(self, "/rss.xml");
    send_feed(req, res, posts, title, link->data, self->data);
    if (posts) cJSON_Delete(posts);
    cwist_sstring_destroy(link);
    cwist_sstring_destroy(self);
}

/* Tags in use on public posts, most used first, for the editor's tag
 * suggestions: [{"name": "...", "n": 3}, ...]. */
void handler_api_tags(cwist_http_request *req, cwist_http_response *res) {
    cJSON *tags = db_tag_list_public(req->db);
    char *json = tags ? cJSON_PrintUnformatted(tags) : NULL;
    cwist_http_header_add(&res->headers, "Content-Type", "application/json; charset=utf-8");
    cwist_http_header_add(&res->headers, "Cache-Control", "no-cache");
    cwist_sstring_assign(res->body, json ? json : "[]");
    cwist_free(json);
    if (tags) cJSON_Delete(tags);
}

/* ---- Series ---- */

void handler_series_index(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0;
    char role[32] = {0};
    auth_is_logged_in(req, &uid, role, sizeof(role));
    cJSON *series = db_series_list(req->db, 0, true);
    char *pp = get_profile_pic(req->db, uid, role);
    cwist_sstring *html = render_series_index(series, is_dark(req), role, pp, is_mobile_request(req));
    send_html_res(res, html);
    free(pp);
    if (series) cJSON_Delete(series);
}

/* The series named by :id, or NULL after sending 404. */
static cJSON *series_from_path(cwist_http_request *req, cwist_http_response *res) {
    const char *id = cwist_query_map_get(req->path_params, "id");
    cJSON *series = (id && atoi(id) > 0) ? db_series_get(req->db, atoi(id)) : NULL;
    if (!series) not_found(res, "Series not found");
    return series;
}

static bool can_edit_series(cJSON *series, int uid, const char *role) {
    if (strcmp(role, "admin") == 0) return true;
    return uid > 0 && json_int(series, "user_id", 0) == uid;
}

static const char *series_lang(cwist_db *db, int id, char *buf, size_t size) {
    cJSON *i18n = db_i18n_get(db, "series", id);
    cJSON *lang = i18n ? cJSON_GetObjectItem(i18n, "lang") : NULL;
    snprintf(buf, size, "%s", cJSON_IsString(lang) ? lang->valuestring : "");
    if (i18n) cJSON_Delete(i18n);
    return buf;
}

void handler_series_get(cwist_http_request *req, cwist_http_response *res) {
    cJSON *series = series_from_path(req, res);
    if (!series) return;
    int uid = 0;
    char role[32] = {0};
    auth_is_logged_in(req, &uid, role, sizeof(role));
    int sid = json_int(series, "id", 0);
    bool editor = can_edit_series(series, uid, role);
    cJSON *posts = db_series_posts(req->db, sid, !editor);
    if (!editor && cJSON_GetArraySize(posts) == 0) {
        /* Nothing public yet: the series does not exist for readers. */
        if (posts) cJSON_Delete(posts);
        cJSON_Delete(series);
        not_found(res, "Series not found");
        return;
    }
    cJSON *siblings = db_i18n_siblings(req->db, "series", sid, false);
    char lang[16];
    series_lang(req->db, sid, lang, sizeof(lang));
    char *pp = get_profile_pic(req->db, uid, role);
    cwist_sstring *html = render_series_detail(series, posts, siblings, lang, editor, is_dark(req), role, pp,
                                               is_mobile_request(req));
    send_html_res(res, html);
    free(pp);
    if (posts) cJSON_Delete(posts);
    if (siblings) cJSON_Delete(siblings);
    cJSON_Delete(series);
}

void handler_series_rss_xml(cwist_http_request *req, cwist_http_response *res) {
    cJSON *series = series_from_path(req, res);
    if (!series) return;
    int sid = json_int(series, "id", 0);
    cJSON *posts = db_series_feed(req->db, sid, FEED_ITEMS);
    if (!posts || cJSON_GetArraySize(posts) == 0) {
        if (posts) cJSON_Delete(posts);
        cJSON_Delete(series);
        not_found(res, "Series not found");
        return;
    }
    char title[512], link[48], self[64];
    cJSON *t = cJSON_GetObjectItem(series, "title");
    snprintf(title, sizeof(title), "%s - %s", g_config.title, cJSON_IsString(t) ? t->valuestring : "");
    snprintf(link, sizeof(link), "/series/%d", sid);
    snprintf(self, sizeof(self), "/series/%d/rss.xml", sid);
    send_feed(req, res, posts, title, link, self);
    cJSON_Delete(posts);
    cJSON_Delete(series);
}

static void render_edit_page(cwist_http_request *req, cwist_http_response *res, cJSON *series, int uid,
                             const char *role, const char *error) {
    int sid = json_int(series, "id", 0);
    cJSON *posts = db_series_posts(req->db, sid, false);
    cJSON *choices = db_series_list(req->db, strcmp(role, "admin") == 0 ? 0 : uid, false);
    char lang[16];
    series_lang(req->db, sid, lang, sizeof(lang));
    cJSON *siblings = db_i18n_siblings(req->db, "series", sid, false);
    int paired = cJSON_GetArraySize(siblings) > 0 ? json_int(cJSON_GetArrayItem(siblings, 0), "id", 0) : 0;
    char *pp = get_profile_pic(req->db, uid, role);
    cwist_sstring *html = render_series_edit(series, posts, choices, lang, paired, error, is_dark(req), role, pp,
                                             is_mobile_request(req));
    cwist_http_header_add(&res->headers, "Cache-Control", "no-store, private");
    send_html_res(res, html);
    free(pp);
    if (posts) cJSON_Delete(posts);
    if (choices) cJSON_Delete(choices);
    if (siblings) cJSON_Delete(siblings);
}

void handler_series_edit_get(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0;
    char role[32] = {0};
    if (!auth_require_login(req, res, &uid, role, sizeof(role))) return;
    cJSON *series = series_from_path(req, res);
    if (!series) return;
    if (!can_edit_series(series, uid, role)) {
        cJSON_Delete(series);
        res->status_code = CWIST_HTTP_FORBIDDEN;
        cwist_sstring_assign(res->body, "Forbidden");
        return;
    }
    render_edit_page(req, res, series, uid, role, NULL);
    cJSON_Delete(series);
}

void handler_series_edit_post(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0;
    char role[32] = {0};
    if (!auth_require_login(req, res, &uid, role, sizeof(role))) return;
    cJSON *series = series_from_path(req, res);
    if (!series) return;
    if (!can_edit_series(series, uid, role)) {
        cJSON_Delete(series);
        res->status_code = CWIST_HTTP_FORBIDDEN;
        cwist_sstring_assign(res->body, "Forbidden");
        return;
    }
    int sid = json_int(series, "id", 0);
    cwist_query_map *kv = cwist_query_map_create();
    if (req->body && req->body->data) cwist_query_map_parse(kv, req->body->data);
    const char *title = cwist_query_map_get(kv, "title");
    const char *desc = cwist_query_map_get(kv, "description");
    const char *lang = cwist_query_map_get(kv, "lang");
    const char *pair = cwist_query_map_get(kv, "translation_of");
    if (!title || !title[0] || strlen(title) > 200 || (desc && strlen(desc) > 2000) || (lang && !i18n_lang_valid(lang))) {
        cwist_query_map_destroy(kv);
        render_edit_page(req, res, series, uid, role, "Title is required (200 characters at most), description 2000.");
        cJSON_Delete(series);
        return;
    }
    db_series_update(req->db, sid, title, desc);
    int pair_id = pair ? atoi(pair) : 0;
    if (pair_id > 0) {
        cJSON *other = db_series_get(req->db, pair_id);
        bool ok = other && can_edit_series(other, uid, role);
        if (other) cJSON_Delete(other);
        db_i18n_set(req->db, "series", sid, lang, ok ? pair_id : 0);
    } else {
        db_i18n_set(req->db, "series", sid, lang, -1);
    }
    /* Reorder / remove parts: sort the remaining parts by the numbers the
     * form sent (ties keep the current order), then number them 1..n. */
    cJSON *posts = db_series_posts(req->db, sid, false);
    int n = cJSON_GetArraySize(posts), kept = 0;
    struct part { int id, want, was; } *parts = calloc((size_t)(n > 0 ? n : 1), sizeof(*parts));
    cJSON *p = NULL;
    int idx = 0;
    cJSON_ArrayForEach(p, posts) {
        int pid = json_int(p, "id", 0);
        char key[48];
        snprintf(key, sizeof(key), "remove_%d", pid);
        if (cwist_query_map_get(kv, key)) {
            db_post_set_series(req->db, pid, 0, 0);
            idx++;
            continue;
        }
        snprintf(key, sizeof(key), "pos_%d", pid);
        const char *pos = cwist_query_map_get(kv, key);
        if (parts) parts[kept++] = (struct part){pid, pos && atoi(pos) > 0 ? atoi(pos) : idx + 1, idx};
        idx++;
    }
    for (int i = 1; parts && i < kept; i++) { /* insertion sort: stable */
        struct part cur = parts[i];
        int j = i - 1;
        while (j >= 0 && (parts[j].want > cur.want || (parts[j].want == cur.want && parts[j].was > cur.was))) {
            parts[j + 1] = parts[j];
            j--;
        }
        parts[j + 1] = cur;
    }
    if (parts && kept > 0) {
        int *ids = calloc((size_t)kept, sizeof(int));
        for (int i = 0; ids && i < kept; i++) ids[i] = parts[i].id;
        if (ids) db_series_set_order(req->db, sid, ids, kept);
        free(ids);
    }
    free(parts);
    if (posts) cJSON_Delete(posts);
    cwist_query_map_destroy(kv);
    cJSON_Delete(series);
    page_cache_invalidate_all();
    char url[48];
    snprintf(url, sizeof(url), "/series/%d", sid);
    redirect(res, url);
}

void handler_series_delete_post(cwist_http_request *req, cwist_http_response *res) {
    int uid = 0;
    char role[32] = {0};
    if (!auth_require_login(req, res, &uid, role, sizeof(role))) return;
    cJSON *series = series_from_path(req, res);
    if (!series) return;
    bool ok = can_edit_series(series, uid, role) && db_series_delete(req->db, json_int(series, "id", 0));
    cJSON_Delete(series);
    if (!ok) {
        res->status_code = CWIST_HTTP_FORBIDDEN;
        cwist_sstring_assign(res->body, "Forbidden");
        return;
    }
    page_cache_invalidate_all();
    redirect(res, "/series");
}

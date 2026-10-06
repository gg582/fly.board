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
    char *pp = get_profile_pic(req->db, uid, role);
    cwist_sstring *html = render_archive(months, tags, is_dark(req), role, pp, is_mobile_request(req));
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

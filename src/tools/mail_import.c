#define _POSIX_C_SOURCE 200809L
/**
 * @file mail_import.c
 * @brief Postfix pipe delivery agent for fly.board webmail.
 *
 * Reads one RFC822 message from stdin and:
 *   1. parses headers (From/To/Subject/Message-ID/In-Reply-To/Date) and the
 *      text/plain body, decoding quoted-printable / base64 and selecting the
 *      text/plain part of multipart/alternative (no third-party MIME deps);
 *   2. resolves the recipient (argv[1] as given by Postfix ${recipient}, or
 *      the To: header localpart) to a users row and inserts the message into
 *      the emails table (folder INBOX);
 *   3. saves Content-Disposition: attachment parts under
 *      public/uploads/mail/<email_id>/ and lists them at the end of the body;
 *   4. stores the raw message in the recipient's Maildir
 *      ($FLY_MAILDIR_ROOT/<user>/new/, default /var/mail/fly) so Dovecot
 *      IMAP clients see inbound mail too (best effort: a missing or
 *      unwritable Maildir never fails the webmail delivery).
 *
 * Exit 0 when the message was accepted for at least one local user, 1 when
 * no recipient matched, 111 on transient errors.
 */
#include "db/db_internal.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <pwd.h>
#include <unistd.h>

/* ------------------------------------------------------------------ utils */

static char *read_all_stdin(size_t *out_len) {
    size_t cap = 65536, len = 0;
    char *buf = malloc(cap + 1);
    if (!buf) return NULL;
    ssize_t n;
    while ((n = read(0, buf + len, cap - len)) > 0) {
        len += (size_t)n;
        if (len == cap) {
            cap *= 2;
            char *nb = realloc(buf, cap + 1);
            if (!nb) { free(buf); return NULL; }
            buf = nb;
        }
    }
    if (n < 0) { free(buf); return NULL; }
    buf[len] = '\0';
    *out_len = len;
    return buf;
}

/* Bounded variant: never scans past hay_len (raw MIME parts are slices of
 * the message and are not NUL-terminated). */
static size_t find_ci_n(const char *hay, size_t hay_len, const char *needle) {
    size_t nn = strlen(needle);
    for (size_t i = 0; i + nn <= hay_len; i++) {
        size_t j = 0;
        while (j < nn && tolower((unsigned char)hay[i + j]) == tolower((unsigned char)needle[j])) j++;
        if (j == nn) return i;
    }
    return (size_t)-1;
}

/* Case-insensitive substring search for header names ("Content-Type:" etc). */
static size_t find_ci(const char *hay, const char *needle) {
    return find_ci_n(hay, strlen(hay), needle);
}

/* ------------------------------------------------------------------ base64 */

static int b64_val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static char *b64_decode(const char *in, size_t in_len, size_t *out_len) {
    size_t cap = in_len + 1, o = 0;
    char *out = malloc(cap);
    if (!out) return NULL;
    int acc = 0, nbits = 0;
    for (size_t i = 0; i < in_len; i++) {
        char c = in[i];
        if (c == '=' || c == '\r' || c == '\n' || c == ' ' || c == '\t') continue;
        int v = b64_val(c);
        if (v < 0) continue;
        acc = (acc << 6) | v;
        nbits += 6;
        if (nbits >= 8) {
            nbits -= 8;
            out[o++] = (char)((acc >> nbits) & 0xFF);
        }
    }
    out[o] = '\0';
    *out_len = o;
    return out;
}

/* ------------------------------------------------------- quoted-printable */

static char *qp_decode(const char *in, size_t in_len, size_t *out_len) {
    size_t cap = in_len + 1, o = 0;
    char *out = malloc(cap);
    if (!out) return NULL;
    for (size_t i = 0; i < in_len; i++) {
        if (in[i] == '=' && i + 2 < in_len && isxdigit((unsigned char)in[i + 1]) && isxdigit((unsigned char)in[i + 2])) {
            unsigned int v;
            char hex[3] = {in[i + 1], in[i + 2], '\0'};
            sscanf(hex, "%2x", &v);
            out[o++] = (char)v;
            i += 2;
        } else if (in[i] == '=' && (i + 1 >= in_len || in[i + 1] == '\n' || (in[i + 1] == '\r' && i + 2 < in_len && in[i + 2] == '\n'))) {
            /* Soft line break: skip. */
            if (i + 1 < in_len && in[i + 1] == '\r') i += 2;
            else i += 1;
        } else {
            out[o++] = in[i];
        }
    }
    out[o] = '\0';
    *out_len = o;
    return out;
}

/* -------------------------------------------------------- encoded-words */
/* Decode =?UTF-8?B?...?= / =?UTF-8?Q?...?= occurrences in a header value. */

static void decode_encoded_words(const char *in, char *out, size_t out_size) {
    size_t o = 0;
    const char *p = in;
    while (*p && o + 1 < out_size) {
        /* Encoded word: =?charset?B?...?= or =?charset?Q?...?= */
        if (p[0] == '=' && p[1] == '?') {
            const char *q1 = strchr(p + 2, '?'); /* ends the charset */
            if (q1 && (q1[1] == 'B' || q1[1] == 'b' || q1[1] == 'Q' || q1[1] == 'q') && q1[2] == '?') {
                const char *enc = q1 + 3;         /* payload start */
                const char *q3 = strstr(enc, "?=");
                if (q3) {
                    bool is_b64 = (q1[1] == 'B' || q1[1] == 'b');
                    size_t dec_len = 0;
                    char *dec = is_b64 ? b64_decode(enc, (size_t)(q3 - enc), &dec_len)
                                       : qp_decode(enc, (size_t)(q3 - enc), &dec_len);
                    if (dec) {
                        for (size_t i = 0; i < dec_len && o + 1 < out_size; i++) out[o++] = dec[i];
                        free(dec);
                        p = q3 + 2;
                        continue;
                    }
                }
            }
        }
        out[o++] = *p++;
    }
    out[o] = '\0';
}

/* ------------------------------------------------------------- headers */

typedef struct {
    char *from;
    char *to;
    char *subject;
    char *message_id;
    char *in_reply_to;
    char *content_type;
    char *content_transfer_encoding;
} mail_headers_t;

static char *hdr_strdup(const char *v) {
    char dec[2048];
    decode_encoded_words(v, dec, sizeof(dec));
    char *s = malloc(strlen(dec) + 1);
    strcpy(s, dec);
    /* Trim leading/trailing whitespace. */
    char *start = s;
    while (*start == ' ' || *start == '\t') start++;
    char *end = start + strlen(start);
    while (end > start && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) end--;
    *end = '\0';
    if (start != s) memmove(s, start, strlen(start) + 1);
    return s;
}

/* Parse the header block [hdr, hdr_end) into mail_headers_t. Lines starting
 * with whitespace continue the previous header. */
static void parse_headers(const char *hdr, size_t hdr_len, mail_headers_t *out) {
    memset(out, 0, sizeof(*out));
    size_t i = 0;
    char *cur_name = NULL;
    char cur_val[4096];
    cur_val[0] = '\0';
    while (i < hdr_len) {
        size_t line_end = i;
        while (line_end < hdr_len && hdr[line_end] != '\n') line_end++;
        size_t line_len = line_end - i;
        if (line_len > 0 && hdr[i + line_len - 1] == '\r') line_len--;
        const char *line = hdr + i;
        if (line_len > 0 && (line[0] == ' ' || line[0] == '\t')) {
            /* Continuation. */
            if (cur_name) {
                size_t cl = strlen(cur_val);
                if (cl + line_len + 2 < sizeof(cur_val)) {
                    cur_val[cl] = ' ';
                    memcpy(cur_val + cl + 1, line, line_len);
                    cur_val[cl + 1 + line_len] = '\0';
                }
            }
        } else {
            const char *colon = memchr(line, ':', line_len);
            if (colon) {
                /* Flush previous. */
                if (cur_name) {
                    char *v = hdr_strdup(cur_val);
                    if (!strcasecmp(cur_name, "From")) out->from = v;
                    else if (!strcasecmp(cur_name, "To")) out->to = v;
                    else if (!strcasecmp(cur_name, "Subject")) out->subject = v;
                    else if (!strcasecmp(cur_name, "Message-ID")) out->message_id = v;
                    else if (!strcasecmp(cur_name, "In-Reply-To")) out->in_reply_to = v;
                    else if (!strcasecmp(cur_name, "Content-Type")) out->content_type = v;
                    else if (!strcasecmp(cur_name, "Content-Transfer-Encoding")) out->content_transfer_encoding = v;
                    else free(v);
                }
                free(cur_name);
                size_t name_len = (size_t)(colon - line);
                cur_name = malloc(name_len + 1);
                memcpy(cur_name, line, name_len);
                cur_name[name_len] = '\0';
                size_t val_off = (size_t)(colon - line) + 1;
                size_t val_len = line_len > val_off ? line_len - val_off : 0;
                if (val_len >= sizeof(cur_val)) val_len = sizeof(cur_val) - 1;
                memcpy(cur_val, line + val_off, val_len);
                cur_val[val_len] = '\0';
            }
        }
        i = line_end < hdr_len ? line_end + 1 : line_end;
    }
    if (cur_name) {
        char *v = hdr_strdup(cur_val);
        if (!strcasecmp(cur_name, "From")) out->from = v;
        else if (!strcasecmp(cur_name, "To")) out->to = v;
        else if (!strcasecmp(cur_name, "Subject")) out->subject = v;
        else if (!strcasecmp(cur_name, "Message-ID")) out->message_id = v;
        else if (!strcasecmp(cur_name, "In-Reply-To")) out->in_reply_to = v;
        else if (!strcasecmp(cur_name, "Content-Type")) out->content_type = v;
        else if (!strcasecmp(cur_name, "Content-Transfer-Encoding")) out->content_transfer_encoding = v;
        else free(v);
        free(cur_name);
    }
}

/* --------------------------------------------------------------- parts */

typedef struct {
    char *body;          /* decoded text, or NULL */
    size_t body_len;
    char **attachments;  /* saved file names (relative paths) */
    int n_attach;
    int cap_attach;
} parse_result_t;

static char *substr_dup(const char *p, size_t len) {
    char *s = malloc(len + 1);
    memcpy(s, p, len);
    s[len] = '\0';
    return s;
}

static char *extract_boundary(const char *content_type) {
    const char *b = content_type ? find_ci(content_type, "boundary") == (size_t)-1 ? NULL : content_type : NULL;
    if (!b) return NULL;
    b = strchr(content_type + find_ci(content_type, "boundary"), '=');
    if (!b) return NULL;
    b++;
    while (*b == ' ' || *b == '\t') b++;
    if (*b == '"') {
        b++;
        const char *e = strchr(b, '"');
        return e ? substr_dup(b, (size_t)(e - b)) : NULL;
    }
    const char *e = b;
    while (*e && *e != ';' && *e != ' ' && *e != '\t' && *e != '\r' && *e != '\n') e++;
    return substr_dup(b, (size_t)(e - b));
}

static char *extract_param(const char *header, const char *name) {
    /* Find name= inside a Content-Type/Content-Disposition value. */
    size_t idx = find_ci(header, name);
    if (idx == (size_t)-1) return NULL;
    const char *p = header + idx + strlen(name);
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '=') return NULL;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '"') {
        p++;
        const char *e = strchr(p, '"');
        return e ? substr_dup(p, (size_t)(e - p)) : NULL;
    }
    const char *e = p;
    while (*e && *e != ';' && *e != ' ' && *e != '\t' && *e != '\r' && *e != '\n') e++;
    return substr_dup(p, (size_t)(e - p));
}

/* Decode a part body according to its CTE header. Caller frees. */
static char *decode_body(const char *data, size_t len, const char *cte, size_t *out_len) {
    if (cte && (find_ci(cte, "base64") != (size_t)-1)) return b64_decode(data, len, out_len);
    if (cte && (find_ci(cte, "quoted-printable") != (size_t)-1)) return qp_decode(data, len, out_len);
    char *out = malloc(len + 1);
    memcpy(out, data, len);
    out[len] = '\0';
    *out_len = len;
    return out;
}

/* Sanitize an attachment filename to a bare basename. */
static char *sanitize_name(const char *name) {
    const char *base = name ? strrchr(name, '/') : NULL;
    const char *win = name ? strrchr(name, '\\') : NULL;
    const char *b = name;
    if (base && base + 1 > b) b = base + 1;
    if (win && win + 1 > b) b = win + 1;
    char *s = substr_dup(b, strlen(b));
    for (char *p = s; *p; p++) {
        if (*p < 32 || *p == ':' || *p == '?' || *p == '*' || *p == '"' || *p == '<' || *p == '>' || *p == '|') *p = '_';
    }
    if (!s[0]) { free(s); s = substr_dup("attachment.bin", 14); }
    return s;
}

static void parse_part(const char *phdr, size_t phdr_len, const char *pdata, size_t pdata_len,
                       parse_result_t *res, int depth);

/* Process a multipart body: split on --boundary, recurse into parts. */
static void parse_multipart(const char *body, size_t body_len, const char *boundary,
                            parse_result_t *res, int depth) {
    if (depth > 8 || !boundary || !boundary[0]) return;
    char *marker = malloc(strlen(boundary) + 3);
    sprintf(marker, "--%s", boundary);
    size_t mlen = strlen(marker);
    /* Find each part between markers. */
    size_t pos = 0;
    /* Skip preamble: find first marker. */
    size_t first = find_ci_n(body, body_len, marker);
    if (first == (size_t)-1) { free(marker); return; }
    pos = first + mlen;
    while (pos < body_len) {
        /* End marker "--boundary--" */
        if (pos + 2 <= body_len && body[pos] == '-' && body[pos + 1] == '-') break;
        if (pos < body_len && body[pos] == '\r' && pos + 1 < body_len && body[pos + 1] == '\n') pos += 2;
        else if (pos < body_len && body[pos] == '\n') pos += 1;
        size_t part_start = pos;
        /* Find next marker. */
        size_t next = (size_t)-1;
        for (size_t i = part_start; i + mlen <= body_len; i++) {
            if (body[i] == '-' && body[i + 1] == '-' && !memcmp(body + i, marker, mlen)) {
                /* Must be at a line start (preceded by \n or at start). */
                if (i == 0 || body[i - 1] == '\n') { next = i; break; }
            }
        }
        size_t part_end = next == (size_t)-1 ? body_len : next;
        /* Trim trailing CRLF before the marker. */
        while (part_end > part_start && (body[part_end - 1] == '\n' || body[part_end - 1] == '\r')) part_end--;
        /* Split part into header/body at a blank line. */
        size_t hdr_end = part_start;
        const char *sep = NULL;
        for (size_t i = part_start; i < part_end; i++) {
            if (body[i] == '\n') {
                if (i + 1 < part_end && body[i + 1] == '\n') { sep = body + i; break; }
                if (i + 3 < part_end && body[i + 1] == '\r' && body[i + 2] == '\n' && body[i + 3] == '\r' &&
                    i + 4 < part_end && body[i + 4] == '\n') { sep = body + i; break; }
            }
        }
        if (sep) {
            hdr_end = (size_t)(sep - body);
            const char *pb = sep + 1;
            if (pb < body + part_end && *pb == '\r') pb++;
            if (pb < body + part_end && *pb == '\n') pb++;
            parse_part(body + part_start, hdr_end - part_start, pb,
                       (size_t)((body + part_end) - pb), res, depth + 1);
        } else {
            parse_part(body + part_start, 0, body + part_start, part_end - part_start, res, depth + 1);
        }
        if (next == (size_t)-1) break;
        pos = next + mlen;
    }
    free(marker);
}

static void parse_part(const char *phdr, size_t phdr_len, const char *pdata, size_t pdata_len,
                       parse_result_t *res, int depth) {
    mail_headers_t h;
    if (phdr_len > 0) parse_headers(phdr, phdr_len, &h);
    else memset(&h, 0, sizeof(h));

    const char *ct = h.content_type ? h.content_type : "text/plain";
    const char *cd = NULL;
    (void)cd;

    /* Attachments: Content-Disposition: attachment[; filename=...] or any
     * part carrying a filename parameter. */
    bool is_attachment = false;
    char *filename = NULL;
    /* Content-Disposition lives in this part's headers; parse_headers only
     * captured a fixed set, so scan the raw header block directly. */
    {
        size_t idx = phdr_len ? find_ci_n(phdr, phdr_len, "Content-Disposition") : (size_t)-1;
        if (idx != (size_t)-1) {
            const char *p = phdr + idx + strlen("Content-Disposition");
            const char *eol = memchr(p, '\n', phdr_len - (size_t)(p - phdr));
            size_t dl = eol ? (size_t)(eol - p) : phdr_len - (size_t)(p - phdr);
            char *disp = substr_dup(p, dl);
            if (find_ci(disp, "attachment") != (size_t)-1) is_attachment = true;
            char *fn = extract_param(disp, "filename");
            if (fn) { filename = fn; is_attachment = true; }
            free(disp);
        }
        if (!filename) {
            char *fn = extract_param(ct, "name");
            if (fn) { filename = fn; is_attachment = true; }
        }
    }

    if (is_attachment && filename && pdata_len > 0) {
        /* Stash raw bytes: store as (name, raw, len) triple by writing the
         * file later. We keep raw data in memory keyed through res via a
         * parallel growable array of blobs. */
        size_t dec_len = 0;
        char *dec = decode_body(pdata, pdata_len, h.content_transfer_encoding, &dec_len);
        if (dec) {
            if (res->n_attach == res->cap_attach) {
                res->cap_attach = res->cap_attach ? res->cap_attach * 2 : 8;
                res->attachments = realloc(res->attachments, sizeof(char *) * (size_t)res->cap_attach * 2);
            }
            char *safe = sanitize_name(filename);
            /* Encode as "name\0" + raw bytes? Keep it simple: write the file
             * right away into a temp dir derived later. Instead we store
             * "name" and push raw into a side channel via body pointer. */
            res->attachments[res->n_attach * 2] = safe;
            /* Store the decoded payload as an allocated blob referenced by
             * the odd slot, tracked as a malloc'd buffer with its length in
             * a trailing length array. Simpler: base64 the payload into a
             * string slot (attachments are small in practice). */
            char *b64 = NULL;
            {
                static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
                size_t cap = ((dec_len + 2) / 3) * 4 + 1;
                b64 = malloc(cap);
                size_t o = 0;
                for (size_t i = 0; i < dec_len; i += 3) {
                    unsigned v = (unsigned char)dec[i] << 16;
                    if (i + 1 < dec_len) v |= (unsigned char)dec[i + 1] << 8;
                    if (i + 2 < dec_len) v |= (unsigned char)dec[i + 2];
                    b64[o++] = tbl[(v >> 18) & 63];
                    b64[o++] = tbl[(v >> 12) & 63];
                    b64[o++] = (i + 1 < dec_len) ? tbl[(v >> 6) & 63] : '=';
                    b64[o++] = (i + 2 < dec_len) ? tbl[v & 63] : '=';
                }
                b64[o] = '\0';
            }
            res->attachments[res->n_attach * 2 + 1] = b64;
            res->n_attach++;
            free(dec);
        }
    } else if (find_ci(ct, "multipart/") != (size_t)-1) {
        char *boundary = extract_boundary(ct);
        if (boundary) {
            parse_multipart(pdata, pdata_len, boundary, res, depth);
            free(boundary);
        }
    } else if (find_ci(ct, "text/plain") != (size_t)-1 && !res->body) {
        size_t dec_len = 0;
        char *dec = decode_body(pdata, pdata_len, h.content_transfer_encoding, &dec_len);
        if (dec) {
            res->body = dec;
            res->body_len = dec_len;
        }
    }
    /* text/html and other non-text parts are dropped for the webmail body. */
    free(h.from); free(h.to); free(h.subject); free(h.message_id);
    free(h.in_reply_to); free(h.content_type); free(h.content_transfer_encoding);
    free(filename);
}

/* ------------------------------------------------------- recipient/user */

static const char *db_path(void) {
    static char exe_relative[512];
    const char *p = getenv("FLY_DB_PATH");
    if (p && p[0]) return p;
    if (access(FLY_DB_MAIN_PATH, R_OK) == 0) return FLY_DB_MAIN_PATH;
    /* Invoked by the Postfix pipe transport with an unrelated cwd: fall back
     * to the database next to the binary (/proc/self/exe). */
    ssize_t n = readlink("/proc/self/exe", exe_relative, sizeof(exe_relative) - 1);
    if (n > 0) {
        exe_relative[n] = '\0';
        char *slash = strrchr(exe_relative, '/');
        if (slash) {
            snprintf(slash + 1, sizeof(exe_relative) - (size_t)(slash + 1 - exe_relative),
                     "data/blog.db");
            if (access(exe_relative, R_OK) == 0) return exe_relative;
        }
    }
    return FLY_DB_MAIN_PATH;
}

/* Reserved addresses all land in the site admin's mailbox. */
static const char *const k_reserved[] = {
    "postmaster", "abuse", "admin", "administrator", "support", "help",
    "noreply", "mailer-daemon", "root", "info", "webmaster", "hostmaster",
    "noc", "security", NULL
};

static bool is_reserved(const char *local) {
    for (int i = 0; k_reserved[i]; i++)
        if (!strcasecmp(local, k_reserved[i])) return true;
    return false;
}

static int find_owner(sqlite3 *conn, const char *address, char *out_username, size_t out_len) {
    char local[128] = {0};
    snprintf(local, sizeof(local), "%s", address);
    char *at = strrchr(local, '@');
    if (at) *at = '\0';
    if (!local[0]) return 0;

    const char *sql = is_reserved(local)
        ? "SELECT id, username FROM users WHERE role='admin' ORDER BY id LIMIT 1"
        : "SELECT id, username FROM users WHERE username = ?1 OR username = ?2 LIMIT 1";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(conn, sql, -1, &stmt, NULL) != SQLITE_OK) return 0;
    if (!is_reserved(local)) {
        char lower[128];
        size_t ln = strlen(local);
        for (size_t i = 0; i <= ln && i < sizeof(lower) - 1; i++)
            lower[i] = (char)((local[i] >= 'A' && local[i] <= 'Z') ? local[i] + 32 : local[i]);
        sqlite3_bind_text(stmt, 1, local, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, lower, -1, SQLITE_STATIC);
    }
    int uid = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        uid = sqlite3_column_int(stmt, 0);
        const unsigned char *un = sqlite3_column_text(stmt, 1);
        if (un) snprintf(out_username, out_len, "%s", (const char *)un);
    }
    sqlite3_finalize(stmt);
    return uid;
}

/* ---------------------------------------------------------------- maildir */

static void maildir_deliver(const char *username, const char *raw, size_t raw_len) {
    const char *root = getenv("FLY_MAILDIR_ROOT");
    if (!root || !root[0]) root = "/var/mail/fly";
    /* mkdir -p <root>/<user>/new (created on demand; a missing or
     * unwritable Maildir never fails the webmail delivery). */
    char dir[512];
    snprintf(dir, sizeof(dir), "%s", root);
    mkdir(dir, 0700);
    snprintf(dir, sizeof(dir), "%s/%s", root, username);
    mkdir(dir, 0700);
    snprintf(dir, sizeof(dir), "%s/%s/new", root, username);
    mkdir(dir, 0700);
    char path[600];
    snprintf(path, sizeof(path), "%s/%lld.%ld.mail.oborona.zip", dir,
             (long long)time(NULL), (long)getpid());
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite(raw, 1, raw_len, f);
    fclose(f);
    /* Dovecot refuses UID 0, so hand the mailbox to the mail user when one
     * is configured (FLY_MAIL_USER, default "flymail" if it exists). */
    const char *mu = getenv("FLY_MAIL_USER");
    if (!mu || !mu[0]) mu = "flymail";
    struct passwd *pw = getpwnam(mu);
    if (pw) {
        char userdir[512];
        snprintf(userdir, sizeof(userdir), "%s/%s", root, username);
        chown(userdir, pw->pw_uid, pw->pw_gid);
        chown(dir, pw->pw_uid, pw->pw_gid);
        chown(path, pw->pw_uid, pw->pw_gid);
    }
}

/* ------------------------------------------------------------------- main */

/* Writes the raw message into the spool directory; the fly_board web server
 * (running as root) sweeps this directory into the webmail DB when the
 * recipient opens their inbox. Returns false on hard failure. */
static bool spool_write(const char *username, const char *raw, size_t raw_len) {
    const char *root = getenv("FLY_MAILDIR_ROOT");
    if (!root || !root[0]) root = "/var/mail/fly";
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/spool", root);
    mkdir(dir, 0775);
    char path[600];
    snprintf(path, sizeof(path), "%s/%s.%lld.%ld.eml", dir, username,
             (long long)time(NULL), (long)getpid());
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    fwrite(raw, 1, raw_len, f);
    fclose(f);
    return true;
}

/* Inserts the parsed message into the webmail DB (as the user resolved from
 * rcpt) and persists attachments. Requires a writable DB (root). */
static int db_deliver(const mail_headers_t *h, const parse_result_t *res,
                      const char *rcpt, const char *body) {
    sqlite3 *conn = NULL;
    if (sqlite3_open_v2(db_path(), &conn, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK) {
        if (conn) sqlite3_close(conn);
        fprintf(stderr, "mail-import: cannot open %s\n", db_path());
        return 111;
    }
    db_configure_connection(conn);
    /* Self-contained migration guard: the table must exist even if this tool
     * runs before the web server has ever started. */
    sqlite3_exec(conn,
        "CREATE TABLE IF NOT EXISTS emails ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  owner_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,"
        "  folder TEXT NOT NULL DEFAULT 'INBOX',"
        "  from_addr TEXT, to_addrs TEXT, subject TEXT,"
        "  body_text TEXT, message_id TEXT, in_reply_to TEXT,"
        "  is_read INTEGER DEFAULT 0,"
        "  created_at DATETIME DEFAULT CURRENT_TIMESTAMP)", NULL, NULL, NULL);
    sqlite3_exec(conn, "CREATE INDEX IF NOT EXISTS idx_emails_owner_folder ON emails(owner_id, folder, created_at)", NULL, NULL, NULL);

    char username[128] = {0};
    int owner = find_owner(conn, rcpt, username, sizeof(username));
    if (owner <= 0) {
        fprintf(stderr, "mail-import: no local user for recipient %s\n", rcpt);
        sqlite3_close(conn);
        return 1;
    }

    int email_id = db_email_create_conn(conn, owner, "INBOX",
                                        h->from ? h->from : "", rcpt,
                                        h->subject ? h->subject : "(no subject)",
                                        body, h->message_id, h->in_reply_to);
    if (email_id <= 0) {
        fprintf(stderr, "mail-import: insert failed for %s\n", rcpt);
        sqlite3_close(conn);
        return 111;
    }

    /* Persist attachments under public/uploads/mail/<id>/ and list them at
     * the end of the body. The stored blob slots are base64 strings. */
    if (res->n_attach > 0) {
        char dir[256];
        snprintf(dir, sizeof(dir), "public/uploads/mail/%d", email_id);
        mkdir("public/uploads", 0755);
        mkdir("public/uploads/mail", 0755);
        if (mkdir(dir, 0755) == 0 || access(dir, F_OK) == 0) {
            char listing[2048] = {0};
            size_t lo = 0;
            lo += (size_t)snprintf(listing + lo, sizeof(listing) - lo, "\n\n--- Attachments ---\n");
            for (int i = 0; i < res->n_attach; i++) {
                const char *safe = res->attachments[i * 2];
                const char *b64 = res->attachments[i * 2 + 1];
                size_t blen = 0;
                char *blob = b64_decode(b64, strlen(b64), &blen);
                if (!blob) continue;
                char path[512];
                snprintf(path, sizeof(path), "%s/%s", dir, safe);
                FILE *f = fopen(path, "wb");
                if (f) {
                    fwrite(blob, 1, blen, f);
                    fclose(f);
                }
                free(blob);
                lo += (size_t)snprintf(listing + lo, sizeof(listing) - lo, "  %s\n", safe);
            }
            db_email_append_body_conn(conn, owner, email_id, listing);
        }
    }

    sqlite3_close(conn);
    printf("imported for %s (email id %d)\n", username, email_id);
    return 0;
}

int main(int argc, char **argv) {
    /* Modes:
     *   default          Postfix pipe delivery: write Maildir + spool; also
     *                    insert into the webmail DB when running as root
     *                    (direct local delivery). Non-root (the default
     *                    flymail pipe user) skips the DB — the web server
     *                    later sweeps the spool into the DB.
     *   dbonly [rcpt]    Re-import one raw message from stdin into the webmail
     *                    DB only (used by the fly_board spool sweep, as root).
     */
    bool dbonly = argc > 1 && strcmp(argv[1], "dbonly") == 0;

    size_t raw_len = 0;
    char *raw = read_all_stdin(&raw_len);
    if (!raw || raw_len == 0) {
        fprintf(stderr, "mail-import: empty message\n");
        free(raw);
        return 111;
    }

    /* Split header block from body. */
    const char *sep = strstr(raw, "\r\n\r\n");
    size_t hdr_len, body_off;
    if (sep) { hdr_len = (size_t)(sep - raw); body_off = hdr_len + 4; }
    else {
        sep = strstr(raw, "\n\n");
        if (!sep) { hdr_len = raw_len; body_off = raw_len; }
        else { hdr_len = (size_t)(sep - raw); body_off = hdr_len + 2; }
    }

    mail_headers_t h;
    parse_headers(raw, hdr_len, &h);

    /* Recipient: explicit argv (Postfix ${recipient}, or dbonly's argv[2])
     * wins; fall back to the To: header localpart. */
    const char *rcpt_arg = NULL;
    if (dbonly) { if (argc > 2 && argv[2][0]) rcpt_arg = argv[2]; }
    else if (argc > 1 && argv[1][0]) rcpt_arg = argv[1];

    char rcpt[256] = {0};
    if (rcpt_arg) {
        snprintf(rcpt, sizeof(rcpt), "%s", rcpt_arg);
    } else if (h.to && h.to[0]) {
        /* Take the first address in the To list. */
        const char *lt = strchr(h.to, '<');
        const char *gt = lt ? strchr(lt, '>') : NULL;
        if (lt && gt && gt > lt + 1) {
            snprintf(rcpt, sizeof(rcpt), "%.*s", (int)(gt - lt - 1), lt + 1);
        } else {
            size_t n = strcspn(h.to, ",; \t");
            if (n >= sizeof(rcpt)) n = sizeof(rcpt) - 1;
            memcpy(rcpt, h.to, n);
            rcpt[n] = '\0';
        }
    }
    if (!rcpt[0]) {
        fprintf(stderr, "mail-import: no recipient\n");
        free(raw);
        return 1;
    }

    /* Localpart of the recipient — names the mailbox without needing the DB. */
    char username[128] = {0};
    snprintf(username, sizeof(username), "%s", rcpt);
    char *at = strrchr(username, '@');
    if (at) *at = '\0';
    if (!username[0]) {
        fprintf(stderr, "mail-import: empty localpart\n");
        free(raw);
        return 1;
    }

    /* Parse body parts. */
    parse_result_t res;
    memset(&res, 0, sizeof(res));
    const char *ct = h.content_type ? h.content_type : "text/plain";
    if (find_ci(ct, "multipart/") != (size_t)-1) {
        char *boundary = extract_boundary(ct);
        if (boundary) {
            parse_multipart(raw + body_off, raw_len - body_off, boundary, &res, 0);
            free(boundary);
        }
    } else {
        parse_part(raw, hdr_len, raw + body_off, raw_len - body_off, &res, 0);
    }

    /* Normalize the body: CRLF -> LF, NUL-safe via length. */
    char *body = res.body;
    size_t body_len = res.body_len;
    if (!body) {
        body = substr_dup("", 0);
        body_len = 0;
    }
    {
        size_t o = 0;
        for (size_t i = 0; i < body_len; i++) {
            if (body[i] == '\r' && i + 1 < body_len && body[i + 1] == '\n') continue;
            body[o++] = body[i];
        }
        body[o] = '\0';
        body_len = o;
    }

    int rc = 0;
    if (!dbonly) {
        maildir_deliver(username, raw, raw_len);
        spool_write(username, raw, raw_len);
        /* Direct DB delivery is only possible when running as root; otherwise
         * the fly_board spool sweep picks the message up. */
        if (geteuid() == 0) {
            int dbrc = db_deliver(&h, &res, rcpt, body);
            if (dbrc != 0) rc = dbrc;
        }
    } else {
        rc = db_deliver(&h, &res, rcpt, body);
    }

    for (int i = 0; i < res.n_attach * 2; i++) free(res.attachments[i]);
    free(res.attachments);
    free(body);
    free(raw);
    free(h.from); free(h.to); free(h.subject); free(h.message_id);
    free(h.in_reply_to); free(h.content_type); free(h.content_transfer_encoding);
    return rc;
}

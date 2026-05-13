#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "utils.h"
#include <cwist/core/mem/alloc.h>
#include <cwist/core/log.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

char *file_read(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long len = ftell(f);
    if (len < 0) { fclose(f); return NULL; }
    rewind(f);
    char *buf = (char *)cwist_alloc((size_t)len + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t r = fread(buf, 1, (size_t)len, f);
    fclose(f);
    buf[r] = '\0';
    if (out_len) *out_len = r;
    return buf;
}

bool file_write(const char *path, const void *data, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    size_t w = fwrite(data, 1, len, f);
    fclose(f);
    return w == len;
}

bool dir_ensure(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) return true;
    return mkdir(path, 0755) == 0;
}

const char *mime_type(const char *filename) {
    const char *dot = strrchr(filename, '.');
    if (!dot) return "application/octet-stream";
    if (strcasecmp(dot, ".html") == 0 || strcasecmp(dot, ".htm") == 0) return "text/html";
    if (strcasecmp(dot, ".css") == 0) return "text/css";
    if (strcasecmp(dot, ".js") == 0) return "application/javascript";
    if (strcasecmp(dot, ".json") == 0) return "application/json";
    if (strcasecmp(dot, ".png") == 0) return "image/png";
    if (strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0) return "image/jpeg";
    if (strcasecmp(dot, ".gif") == 0) return "image/gif";
    if (strcasecmp(dot, ".svg") == 0) return "image/svg+xml";
    if (strcasecmp(dot, ".webp") == 0) return "image/webp";
    if (strcasecmp(dot, ".mp4") == 0) return "video/mp4";
    if (strcasecmp(dot, ".webm") == 0) return "video/webm";
    if (strcasecmp(dot, ".mp3") == 0) return "audio/mpeg";
    if (strcasecmp(dot, ".ogg") == 0) return "audio/ogg";
    if (strcasecmp(dot, ".wav") == 0) return "audio/wav";
    if (strcasecmp(dot, ".pdf") == 0) return "application/pdf";
    if (strcasecmp(dot, ".zip") == 0) return "application/zip";
    if (strcasecmp(dot, ".txt") == 0 || strcasecmp(dot, ".md") == 0) return "text/plain";
    return "application/octet-stream";
}

char *generate_slug(const char *title) {
    size_t len = strlen(title);
    char *slug = (char *)cwist_alloc(len * 3 + 1);
    if (!slug) return NULL;
    size_t j = 0;
    for (size_t i = 0; i < len && j < len * 3; i++) {
        unsigned char c = (unsigned char)title[i];
        if (isalnum(c)) {
            slug[j++] = (char)tolower(c);
        } else if (c == ' ' || c == '-' || c == '_') {
            if (j == 0 || slug[j-1] != '-') slug[j++] = '-';
        }
    }
    if (j > 0 && slug[j-1] == '-') j--;
    slug[j] = '\0';
    if (j == 0) { slug[0] = 'p'; slug[1] = 'o'; slug[2] = 's'; slug[3] = 't'; slug[4] = '\0'; }
    return slug;
}

char *escape_html(const char *src) {
    size_t len = strlen(src);
    size_t extra = 0;
    for (size_t i = 0; i < len; i++) {
        switch (src[i]) {
            case '&': extra += 4; break;
            case '<': extra += 3; break;
            case '>': extra += 3; break;
            case '"': extra += 5; break;
            case '\'': extra += 4; break;
        }
    }
    char *out = (char *)cwist_alloc(len + extra + 1);
    if (!out) return NULL;
    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        switch (src[i]) {
            case '&': memcpy(out+j, "&amp;", 5); j+=5; break;
            case '<': memcpy(out+j, "&lt;", 4); j+=4; break;
            case '>': memcpy(out+j, "&gt;", 4); j+=4; break;
            case '"': memcpy(out+j, "&quot;", 6); j+=6; break;
            case '\'': memcpy(out+j, "&#x27;", 6); j+=6; break;
            default: out[j++] = src[i]; break;
        }
    }
    out[j] = '\0';
    return out;
}

char *unescape_html(const char *src) {
    size_t len = strlen(src);
    char *out = (char *)cwist_alloc(len + 1);
    if (!out) return NULL;
    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        if (src[i] == '&' && strncmp(src + i, "&amp;", 5) == 0) {
            out[j++] = '&'; i += 4;
        } else if (src[i] == '&' && strncmp(src + i, "&lt;", 4) == 0) {
            out[j++] = '<'; i += 3;
        } else if (src[i] == '&' && strncmp(src + i, "&gt;", 4) == 0) {
            out[j++] = '>'; i += 3;
        } else if (src[i] == '&' && strncmp(src + i, "&quot;", 6) == 0) {
            out[j++] = '"'; i += 5;
        } else if (src[i] == '&' && strncmp(src + i, "&#x27;", 6) == 0) {
            out[j++] = '\''; i += 5;
        } else {
            out[j++] = src[i];
        }
    }
    out[j] = '\0';
    return out;
}

/* ---- Multipart parser (via multipart-parser-c) ---- */
#include "multipart_parser.h"

typedef struct {
    form_field_t *head;
    form_field_t *tail;
    char hdr[256];
    char name[128];
    char filename[256];
    char ctype[128];
    char *text;
    size_t text_len;
    size_t text_cap;
    FILE *fp;
    char path[512];
    bool is_file;
    size_t file_size;
} mp_ctx_t;

static void mp_flush(mp_ctx_t *ctx) {
    if (!ctx->name[0]) goto reset;
    form_field_t *f = (form_field_t *)cwist_alloc(sizeof(form_field_t));
    memset(f, 0, sizeof(*f));
    f->name = (char *)cwist_alloc(strlen(ctx->name)+1); strcpy(f->name, ctx->name);
    if (ctx->is_file) {
        if (ctx->fp) { fclose(ctx->fp); ctx->fp = NULL; }
        f->filename = (char *)cwist_alloc(strlen(ctx->filename)+1); strcpy(f->filename, ctx->filename);
        if (ctx->ctype[0]) { f->content_type = (char *)cwist_alloc(strlen(ctx->ctype)+1); strcpy(f->content_type, ctx->ctype); }
        if (ctx->path[0]) {
            f->data = (char *)cwist_alloc(strlen(ctx->path)+1); strcpy(f->data, ctx->path);
            f->len = strlen(ctx->path);
        }
        f->file_size = ctx->file_size;
    } else {
        f->data = ctx->text ? ctx->text : (char *)cwist_alloc(1);
        f->data[ctx->text_len] = '\0';
        f->len = ctx->text_len;
    }
    if (!ctx->head) ctx->head = ctx->tail = f; else { ctx->tail->next = f; ctx->tail = f; }
reset:
    ctx->name[0] = '\0';
    ctx->filename[0] = '\0';
    ctx->ctype[0] = '\0';
    ctx->text = NULL; ctx->text_len = 0; ctx->text_cap = 0;
    ctx->is_file = false; ctx->file_size = 0;
    if (ctx->fp) { fclose(ctx->fp); ctx->fp = NULL; }
    ctx->path[0] = '\0';
}

static int mp_hf(multipart_parser *p, const char *at, size_t len) {
    mp_ctx_t *ctx = (mp_ctx_t *)multipart_parser_get_data(p);
    if (len > sizeof(ctx->hdr)-1) len = sizeof(ctx->hdr)-1;
    memcpy(ctx->hdr, at, len); ctx->hdr[len] = '\0';
    return 0;
}

static int mp_hv(multipart_parser *p, const char *at, size_t len) {
    mp_ctx_t *ctx = (mp_ctx_t *)multipart_parser_get_data(p);
    if (strncasecmp(ctx->hdr, "Content-Disposition", 19) == 0) {
        const char *n = (const char *)memmem(at, len, "name=\"", 6);
        if (n) {
            n += 6;
            const char *ne = (const char *)memchr(n, '\"', len - (size_t)(n - at));
            if (ne && (size_t)(ne - n) < sizeof(ctx->name)) {
                memcpy(ctx->name, n, (size_t)(ne - n)); ctx->name[ne - n] = '\0';
            }
        }
        const char *fn = (const char *)memmem(at, len, "filename=\"", 10);
        if (fn) {
            fn += 10;
            const char *fne = (const char *)memchr(fn, '\"', len - (size_t)(fn - at));
            if (fne && (size_t)(fne - fn) < sizeof(ctx->filename)) {
                memcpy(ctx->filename, fn, (size_t)(fne - fn)); ctx->filename[fne - fn] = '\0';
                ctx->is_file = true;
                dir_ensure("public/uploads");
                snprintf(ctx->path, sizeof(ctx->path), "public/uploads/%ld_%s", (long)time(NULL), ctx->filename);
                ctx->fp = fopen(ctx->path, "wb");
            }
        }
    } else if (strncasecmp(ctx->hdr, "Content-Type", 12) == 0) {
        if (len < sizeof(ctx->ctype)) { memcpy(ctx->ctype, at, len); ctx->ctype[len] = '\0'; }
    }
    return 0;
}

static int mp_begin(multipart_parser *p) {
    (void)p;
    return 0;
}

static int mp_data(multipart_parser *p, const char *at, size_t len) {
    mp_ctx_t *ctx = (mp_ctx_t *)multipart_parser_get_data(p);
    if (len == 0) return 0;
    if (ctx->is_file && ctx->fp) {
        fwrite(at, 1, len, ctx->fp);
        ctx->file_size += len;
    } else {
        if (!ctx->text) {
            ctx->text_cap = len + 1;
            ctx->text = (char *)cwist_alloc(ctx->text_cap);
        } else if (ctx->text_len + len + 1 > ctx->text_cap) {
            ctx->text_cap = ctx->text_len + len + 1;
            char *nd = (char *)cwist_alloc(ctx->text_cap);
            memcpy(nd, ctx->text, ctx->text_len);
            cwist_free(ctx->text);
            ctx->text = nd;
        }
        memcpy(ctx->text + ctx->text_len, at, len);
        ctx->text_len += len;
    }
    return 0;
}

static int mp_end(multipart_parser *p) {
    mp_ctx_t *ctx = (mp_ctx_t *)multipart_parser_get_data(p);
    mp_flush(ctx);
    return 0;
}

form_field_t *multipart_parse(const char *body, size_t body_len, const char *boundary) {
    if (!body || !boundary || body_len == 0) return NULL;
    char dashb[256];
    snprintf(dashb, sizeof(dashb), "--%s", boundary);
    multipart_parser_settings s = {0};
    s.on_header_field = mp_hf;
    s.on_header_value = mp_hv;
    s.on_part_data_begin = mp_begin;
    s.on_part_data = mp_data;
    s.on_part_data_end = mp_end;
    multipart_parser *parser = multipart_parser_init(dashb, &s);
    if (!parser) return NULL;
    mp_ctx_t ctx = {0};
    multipart_parser_set_data(parser, &ctx);
    multipart_parser_execute(parser, body, body_len);
    multipart_parser_free(parser);
    return ctx.head;
}

void multipart_free(form_field_t *fields) {
    while (fields) {
        form_field_t *n = fields->next;
        if (fields->name) cwist_free(fields->name);
        if (fields->filename) cwist_free(fields->filename);
        if (fields->content_type) cwist_free(fields->content_type);
        if (fields->data) cwist_free(fields->data);
        cwist_free(fields);
        fields = n;
    }
}

form_field_t *form_find(form_field_t *fields, const char *name) {
    for (form_field_t *f = fields; f; f = f->next)
        if (strcmp(f->name, name) == 0) return f;
    return NULL;
}


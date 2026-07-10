#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "utils.h"
#include "db/db.h"
#include "config/config.h"
#include <cwist/core/mem/alloc.h>
#include <cwist/core/log.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <stb_image.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <magic.h>

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
    if (strcasecmp(dot, ".ico") == 0) return "image/x-icon";
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
    bool write_error;
} mp_ctx_t;

static void mp_flush(mp_ctx_t *ctx) {
    if (!ctx->name[0]) goto reset;
    if (ctx->is_file && ctx->write_error) {
        if (ctx->path[0]) unlink(ctx->path);
        goto reset;
    }
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
    ctx->write_error = false;
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
                if (!ctx->fp) ctx->write_error = true;
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
        if (ctx->write_error) return 0;
        size_t written = fwrite(at, 1, len, ctx->fp);
        if (written != len) {
            ctx->write_error = true;
            fclose(ctx->fp);
            ctx->fp = NULL;
        } else {
            ctx->file_size += len;
        }
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

bool mime_type_from_data(const char *file_path, char *out, size_t out_len) {
    if (!file_path || !out || out_len == 0) return false;
    magic_t magic = magic_open(MAGIC_MIME_TYPE | MAGIC_SYMLINK);
    if (!magic) return false;

    static const char *magic_paths[] = {
        NULL,                           /* system default */
        "third_party/file/magic/magic.mgc",
        "/usr/share/misc/magic.mgc",
        "/usr/share/file/magic.mgc",
        "/usr/lib/file/magic.mgc",
        "/etc/magic",
    };

    bool loaded = false;
    for (size_t i = 0; i < sizeof(magic_paths) / sizeof(magic_paths[0]); ++i) {
        if (magic_load(magic, magic_paths[i]) == 0) {
            loaded = true;
            break;
        }
    }
    if (!loaded) {
        magic_close(magic);
        return false;
    }

    const char *mime = magic_file(magic, file_path);
    bool ok = false;
    if (mime && strcmp(mime, "application/octet-stream") != 0) {
        strncpy(out, mime, out_len - 1);
        out[out_len - 1] = '\0';
        ok = true;
    }
    magic_close(magic);
    return ok;
}
/* Retrieve width and height of image or video */
bool get_media_dimensions(const char *src, bool is_video, int *w, int *h) {
    if (!src || !w || !h) return false;
    if (!is_video) {
        // Image: use stb_image info from file
        if (stbi_info(src, w, h, NULL)) return true;
    } else {
        // Video: use ffprobe to get dimensions
        char cmd[256];
        snprintf(cmd, sizeof(cmd),
                 "ffprobe -v error -select_streams v:0 -show_entries stream=width,height -of csv=p=0 '%s'",
                 src);
        FILE *p = popen(cmd, "r");
        if (!p) return false;
        int vw = 0, vh = 0;
        if (fscanf(p, "%d,%d", &vw, &vh) == 2) {
            *w = vw;
            *h = vh;
            pclose(p);
            return true;
        }
        pclose(p);
    }
    return false;
}

bool process_file_upload(cwist_db *db, form_field_t *f, int uid, int post_id, int media_quality_score, upload_result_t *out) {
    memset(out, 0, sizeof(*out));

    if (!f || !f->filename || !f->data || f->data[0] == '\0') {
        snprintf(out->error, sizeof(out->error), "no file");
        return false;
    }

    char *original_filename = f->filename;
    char *unique_filename = db_file_unique_filename(db, post_id, original_filename);
    if (unique_filename) {
        f->filename = unique_filename;
    } else {
        original_filename = NULL;
    }

    strncpy(out->filename, f->filename, sizeof(out->filename) - 1);
    strncpy(out->file_path, f->data, sizeof(out->file_path) - 1);
    out->file_size = f->file_size;
    if (g_config.max_upload_size > 0 && (long long)out->file_size > g_config.max_upload_size) {
        if (out->file_path[0]) unlink(out->file_path);
        if (original_filename && original_filename != f->filename) cwist_free(original_filename);
        snprintf(out->error, sizeof(out->error), "upload too large");
        return false;
    }

    char detected_mime[127] = {0};
    if (!mime_type_from_data(f->data, detected_mime, sizeof(detected_mime))) {
        const char *fallback = mime_type(f->filename);
        strncpy(detected_mime, fallback, sizeof(detected_mime) - 1);
    }
    strncpy(out->mime_type, detected_mime, sizeof(out->mime_type) - 1);

    int fid = 0;
    for (int attempt = 0; attempt < 10 && f->filename; attempt++) {
        fid = db_file_create_volume_get_id(db, post_id, uid, f->filename, out->mime_type, f->data, f->file_size);
        if (fid != -1) break;
        char *next = db_file_unique_filename(db, post_id, original_filename ? original_filename : f->filename);
        if (next) {
            cwist_free(f->filename);
            f->filename = next;
            strncpy(out->filename, f->filename, sizeof(out->filename) - 1);
        } else {
            break;
        }
    }
    if (original_filename && original_filename != f->filename) cwist_free(original_filename);
    if (fid > 0) {
        snprintf(out->url, sizeof(out->url), "/file/download/%d", fid);
        char thumb_path[512] = {0};
        char preview_path[512] = {0};
        int image_max_w = 0, image_max_h = 0, video_max_h = 0;
        int src_w = 0, src_h = 0;
        /* Determine source dimensions */
        if (strncmp(out->mime_type, "image/", 6) == 0) {
            get_media_dimensions(f->data, false, &src_w, &src_h);
        } else if (strncmp(out->mime_type, "video/", 6) == 0) {
            get_media_dimensions(f->data, true, &src_w, &src_h);
        }
        media_preview_dimensions_from_score(media_quality_score, src_w, src_h,
                                           &image_max_w, &image_max_h, &video_max_h);
        if (strncmp(out->mime_type, "image/", 6) == 0) {
            snprintf(thumb_path, sizeof(thumb_path), "public/uploads/.thumbs/%d.webp", fid);
            if (!generate_image_thumb(f->data, thumb_path, image_max_w, image_max_h)) thumb_path[0] = '\0';
        } else if (strncmp(out->mime_type, "video/", 6) == 0) {
            snprintf(thumb_path, sizeof(thumb_path), "public/uploads/.thumbs/%d.webp", fid);
            if (!generate_video_thumb(f->data, thumb_path, 1280, 720)) thumb_path[0] = '\0';
            snprintf(preview_path, sizeof(preview_path), "public/uploads/.previews/%d.mp4", fid);
            if (!generate_video_preview(f->data, preview_path, video_max_h)) preview_path[0] = '\0';
        } else if (strncmp(out->mime_type, "audio/", 6) == 0) {
            snprintf(preview_path, sizeof(preview_path), "public/uploads/.previews/%d.mp3", fid);
            if (!generate_audio_preview(f->data, preview_path, 192)) preview_path[0] = '\0';
        }
        if (thumb_path[0] || preview_path[0]) {
            db_file_set_preview_paths(db, fid, thumb_path, preview_path);
        }
    }

    if (strncmp(out->mime_type, "image/", 6) == 0) {
        char preview_url[64] = {0};
        if (fid > 0) snprintf(preview_url, sizeof(preview_url), "/file/preview/%d", fid);
        snprintf(out->html, sizeof(out->html),
            "<img src=\"%s\" data-tasfa-src=\"%s\" data-tasfa-original=\"%s\" alt=\"%s\" style=\"max-width:100%%;height:auto;display:block\">",
            preview_url, out->url, out->url, out->filename);
    } else if (strncmp(out->mime_type, "video/", 6) == 0) {
        char play_url[576];
        snprintf(play_url, sizeof(play_url), "%s?preview=1", out->url);
        snprintf(out->html, sizeof(out->html),
            "<div class=\"media-video-placeholder\"><div class=\"media-video-title\">%s</div><div class=\"media-video-frame\"><button type=\"button\" class=\"media-load-btn media-video-open\" data-tasfa-video-link=\"%s\">Click to Load</button></div></div>",
            out->filename, play_url);
    } else if (strncmp(out->mime_type, "audio/", 6) == 0) {
        snprintf(out->html, sizeof(out->html),
            "<audio controls src=\"%s\"></audio>",
            out->url);
    } else {
        snprintf(out->html, sizeof(out->html),
            "[%s](%s)",
            out->filename, out->url);
    }
    if (fid <= 0) {
        snprintf(out->error, sizeof(out->error), "db insert failed");
        return false;
    }
    out->file_id = fid;
    out->ok = true;
    return true;
}

size_t utf8_truncate_len(const char *str, size_t max_bytes) {
    if (!str) return 0;
    size_t len = strlen(str);
    if (len <= max_bytes) return len;
    size_t i = max_bytes;
    while (i > 0 && ((unsigned char)str[i] & 0xC0) == 0x80) {
        i--;
    }
    return i;
}

void get_file_timestamp_str(const char *file_path, char *out_ts, size_t max_len) {
    if (!file_path || strncmp(file_path, "public/uploads/", 15) != 0) {
        snprintf(out_ts, max_len, "%ld000", (long)time(NULL));
        return;
    }
    const char *p = file_path + 15;
    size_t i = 0;
    while (p[i] && p[i] != '_' && p[i] != '.' && i < max_len - 4) {
        if (!isdigit((unsigned char)p[i])) break;
        out_ts[i] = p[i];
        i++;
    }
    out_ts[i] = '\0';
    if (i >= 10) {
        strcat(out_ts, "000");
    } else {
        snprintf(out_ts, max_len, "%ld000", (long)time(NULL));
    }
}

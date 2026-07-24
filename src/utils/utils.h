#ifndef DOCKER_BLOG_UTILS_H
#define DOCKER_BLOG_UTILS_H

#include <stddef.h>
#include <stdbool.h>
#include <cwist/core/db/sql.h>

#include "media_preview.h"

char *url_decode(const char *src);
char *file_read(const char *path, size_t *out_len);
bool file_write(const char *path, const void *data, size_t len);
bool dir_ensure(const char *path);
const char *mime_type(const char *filename);

char *generate_slug(const char *title);

/* Simple multipart/form-data parser */
typedef struct form_field {
    char *name;
    char *filename;
    char *content_type;
    char *data;
    size_t len;
    size_t file_size;
    struct form_field *next;
} form_field_t;

form_field_t *multipart_parse(const char *body, size_t body_len, const char *boundary);
void multipart_free(form_field_t *fields);
form_field_t *form_find(form_field_t *fields, const char *name);

/* File upload result */
typedef struct {
    bool ok;
    int file_id;
    char filename[256];
    char mime_type[128];
    char url[512];
    char html[1536];
    size_t file_size;
    char file_path[512];
    char error[256];
} upload_result_t;

bool mime_type_from_data(const char *file_path, char *out, size_t out_len);
bool process_file_upload(cwist_db *db, form_field_t *f, int uid, int post_id, int media_quality_score, upload_result_t *out);
void get_file_timestamp_str(const char *file_path, char *out_ts, size_t max_len);

/* Truncate UTF-8 string at a valid character boundary (returns byte length <= max_bytes) */
size_t utf8_truncate_len(const char *str, size_t max_bytes);

/* Validate that path is under one of the allowed public prefixes and contains
 * no directory traversal.  Returns true only for safe, relative paths. */
bool is_safe_public_path(const char *path);

/* Sanitize a multipart filename to basename, removing any path components and
 * rejecting unsafe characters.  Returns a newly allocated string or NULL. */
char *sanitize_filename(const char *filename);
#endif

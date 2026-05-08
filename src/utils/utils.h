#ifndef DOCKER_BLOG_UTILS_H
#define DOCKER_BLOG_UTILS_H

#include <stddef.h>
#include <stdbool.h>

char *url_decode(const char *src);
char *file_read(const char *path, size_t *out_len);
bool file_write(const char *path, const void *data, size_t len);
bool dir_ensure(const char *path);
const char *mime_type(const char *filename);

char *generate_slug(const char *title);
char *escape_html(const char *src);

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

/* Simple url-encoded body parser */
typedef struct form_kv {
    char *key;
    char *value;
    struct form_kv *next;
} form_kv_t;

form_kv_t *parse_urlencoded(const char *body);
void form_kv_free(form_kv_t *kv);
const char *form_kv_get(form_kv_t *kv, const char *key);

#endif

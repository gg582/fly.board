#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "utils/utils.h"
#include <cwist/core/mem/alloc.h>

int main(void) {
    /* Simulated curl-generated multipart body with CRLF line endings */
    const char *body =
        "------WebKitFormBoundary7MA4YWxkTrZu0gW\r\n"
        "Content-Disposition: form-data; name=\"title\"\r\n"
        "\r\n"
        "Hello World\r\n"
        "------WebKitFormBoundary7MA4YWxkTrZu0gW\r\n"
        "Content-Disposition: form-data; name=\"content\"\r\n"
        "\r\n"
        "This is the content.\r\n"
        "------WebKitFormBoundary7MA4YWxkTrZu0gW\r\n"
        "Content-Disposition: form-data; name=\"board_id\"\r\n"
        "\r\n"
        "1\r\n"
        "------WebKitFormBoundary7MA4YWxkTrZu0gW\r\n"
        "Content-Disposition: form-data; name=\"empty_field\"\r\n"
        "\r\n"
        "\r\n"
        "------WebKitFormBoundary7MA4YWxkTrZu0gW\r\n"
        "Content-Disposition: form-data; name=\"attachment\"; filename=\"test.txt\"\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        "file data here\r\n"
        "------WebKitFormBoundary7MA4YWxkTrZu0gW--\r\n";

    size_t body_len = strlen(body);
    form_field_t *fields = multipart_parse(body, body_len, "----WebKitFormBoundary7MA4YWxkTrZu0gW");
    assert(fields != NULL);

    form_field_t *f;

    f = form_find(fields, "title");
    assert(f != NULL);
    assert(strcmp(f->name, "title") == 0);
    assert(f->len == 11);
    assert(strcmp(f->data, "Hello World") == 0);

    f = form_find(fields, "content");
    assert(f != NULL);
    assert(f->len == 20);
    assert(strcmp(f->data, "This is the content.") == 0);

    f = form_find(fields, "board_id");
    assert(f != NULL);
    assert(f->len == 1);
    assert(strcmp(f->data, "1") == 0);

    f = form_find(fields, "empty_field");
    assert(f != NULL);
    assert(f->len == 0);
    assert(f->data != NULL);
    assert(f->data[0] == '\0');

    f = form_find(fields, "attachment");
    assert(f != NULL);
    assert(strcmp(f->filename, "test.txt") == 0);
    assert(strcmp(f->content_type, "text/plain") == 0);
    assert(f->len == 14);
    assert(strcmp(f->data, "file data here") == 0);

    multipart_free(fields);
    printf("multipart_parse unit test: PASSED\n");
    return 0;
}

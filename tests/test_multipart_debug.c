#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "utils/utils.h"
#include <cwist/core/mem/alloc.h>

int main(void) {
    const char *body =
        "------WebKitFormBoundary7MA4YWxkTrZu0gW\r\n"
        "Content-Disposition: form-data; name=\"title\"\r\n"
        "\r\n"
        "Hello World\r\n"
        "------WebKitFormBoundary7MA4YWxkTrZu0gW--\r\n";
    size_t body_len = strlen(body);
    printf("body_len=%zu\n", body_len);
    form_field_t *fields = multipart_parse(body, body_len, "----WebKitFormBoundary7MA4YWxkTrZu0gW");
    printf("fields=%p\n", (void*)fields);
    if (fields) {
        for (form_field_t *f = fields; f; f = f->next) {
            printf("name=%s len=%zu data=%s\n", f->name, f->len, f->data ? f->data : "(null)");
        }
        multipart_free(fields);
    }
    return 0;
}

#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include "multipart_parser.h"

static int on_hf(multipart_parser *p, const char *at, size_t len) {
    (void)p;
    printf("hf: '%.*s'\n", (int)len, at);
    return 0;
}
static int on_hv(multipart_parser *p, const char *at, size_t len) {
    (void)p;
    printf("hv: '%.*s'\n", (int)len, at);
    return 0;
}
static int on_pd(multipart_parser *p, const char *at, size_t len) {
    (void)p;
    printf("pd: '%.*s'\n", (int)len, at);
    return 0;
}
static int on_begin(multipart_parser *p) {
    (void)p;
    printf("begin\n");
    return 0;
}
static int on_end(multipart_parser *p) {
    (void)p;
    printf("part_end\n");
    return 0;
}
static int on_body_end(multipart_parser *p) {
    (void)p;
    printf("body_end\n");
    return 0;
}

int main(void) {
    const char *body =
        "------WebKitFormBoundary7MA4YWxkTrZu0gW\r\n"
        "Content-Disposition: form-data; name=\"title\"\r\n"
        "\r\n"
        "Hello World\r\n"
        "------WebKitFormBoundary7MA4YWxkTrZu0gW--\r\n";
    size_t body_len = strlen(body);
    multipart_parser_settings s = {0};
    s.on_header_field = on_hf;
    s.on_header_value = on_hv;
    s.on_part_data = on_pd;
    s.on_part_data_begin = on_begin;
    s.on_part_data_end = on_end;
    s.on_body_end = on_body_end;
    multipart_parser *p = multipart_parser_init("----WebKitFormBoundary7MA4YWxkTrZu0gW", &s);
    size_t parsed = multipart_parser_execute(p, body, body_len);
    printf("parsed=%zu body_len=%zu\n", parsed, body_len);
    multipart_parser_free(p);
    return 0;
}

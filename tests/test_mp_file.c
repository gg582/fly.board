#include <stdio.h>
#include <string.h>
#include "multipart_parser.h"

static int pd(multipart_parser *p, const char *at, size_t len) {
    (void)p;
    printf("part_data: len=%zu data='%.*s'\n", len, (int)len, at);
    return 0;
}
static int begin(multipart_parser *p) { (void)p; printf("begin\n"); return 0; }
static int end(multipart_parser *p) { (void)p; printf("end\n"); return 0; }

int main(void) {
    const char *body = "------b\r\nContent-Disposition: form-data; name=\"file\"; filename=\"t.txt\"\r\n\r\nhello\r\n------b--\r\n";
    multipart_parser_settings s = {0};
    s.on_part_data = pd;
    s.on_part_data_begin = begin;
    s.on_part_data_end = end;
    multipart_parser *p = multipart_parser_init("------b", &s);
    size_t parsed = multipart_parser_execute(p, body, strlen(body));
    printf("parsed=%zu\n", parsed);
    multipart_parser_free(p);
    return 0;
}

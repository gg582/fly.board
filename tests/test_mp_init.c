#include <stdio.h>
#include <string.h>
#include "multipart_parser.h"

int main(void) {
    const char *b = "----WebKitFormBoundary7MA4YWxkTrZu0gW";
    multipart_parser_settings s = {0};
    multipart_parser *p = multipart_parser_init(b, &s);
    printf("boundary=%s\n", b);
    printf("mp_boundary=%s\n", p->multipart_boundary);
    printf("boundary_length=%zu\n", p->boundary_length);
    printf("b[4]=%c mp[4]=%c\n", b[4], p->multipart_boundary[4]);
    multipart_parser_free(p);
    return 0;
}

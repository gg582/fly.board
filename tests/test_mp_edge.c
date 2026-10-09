/* Unit test: multipart parser edge cases: filename with spaces, empty field
 * value, field without filename, boundary-like data inside a value.
 *
 * Build:
 *   gcc -O1 -I/home/yjlee/cwist/lib/multipart-parser-c \
 *       tests/test_mp_edge.c /home/yjlee/cwist/lib/multipart-parser-c/multipart_parser.o \
 *       -o /tmp/test_mp_edge
 *   (if multipart_parser.o is missing: build it first with
 *    gcc -O1 -I/home/yjlee/cwist/lib/multipart-parser-c -c \
 *        /home/yjlee/cwist/lib/multipart-parser-c/multipart_parser.c -o /tmp/multipart_parser.o)
 * Run: /tmp/test_mp_edge
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "multipart_parser.h"

static int failures = 0;
#define CHECK(cond, name) do { \
    if (cond) printf("PASS: %s\n", name); \
    else { printf("FAIL: %s (line %d)\n", name, __LINE__); failures++; } \
} while (0)

typedef struct {
    char filename[256];
    char name[256];
    char data[1024];
    size_t data_len;
    int parts;
} capture_t;

static int on_hf(multipart_parser *p, const char *at, size_t len) {
    capture_t *c = (capture_t *)multipart_parser_get_data(p);
    (void)c; (void)at; (void)len;
    return 0;
}
static int on_hv(multipart_parser *p, const char *at, size_t len) {
    capture_t *c = (capture_t *)multipart_parser_get_data(p);
    /* keep only the disposition value (the one carrying filename=) */
    if (len > 10 && strncmp(at, "form-data;", 10) == 0 &&
        len < sizeof(c->filename) - 1) {
        memcpy(c->filename, at, len);
        c->filename[len] = '\0';
    }
    return 0;
}
static int on_pd(multipart_parser *p, const char *at, size_t len) {
    capture_t *c = (capture_t *)multipart_parser_get_data(p);
    if (c->data_len + len < sizeof(c->data)) {
        memcpy(c->data + c->data_len, at, len);
        c->data_len += len;
        c->data[c->data_len] = '\0';
    }
    return 0;
}
static int on_begin(multipart_parser *p) {
    capture_t *c = (capture_t *)multipart_parser_get_data(p);
    c->parts++;
    c->filename[0] = '\0';
    c->data_len = 0;
    c->data[0] = '\0';
    return 0;
}
static int on_end(multipart_parser *p) { (void)p; return 0; }

static void parse(const char *body, capture_t *c) {
    memset(c, 0, sizeof(*c));
    multipart_parser_settings s = {0};
    s.on_header_field = on_hf;
    s.on_header_value = on_hv;
    s.on_part_data = on_pd;
    s.on_part_data_begin = on_begin;
    s.on_part_data_end = on_end;
    multipart_parser *p = multipart_parser_init("--BOUNDARY", &s);
    multipart_parser_set_data(p, c);
    multipart_parser_execute(p, body, strlen(body));
    multipart_parser_free(p);
}

int main(void) {
    /* filename with spaces (quoted) */
    capture_t c;
    parse(
        "--BOUNDARY\r\n"
        "Content-Disposition: form-data; name=\"upload\"; filename=\"my file.txt\"\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        "file body\r\n"
        "--BOUNDARY--\r\n", &c);
    CHECK(c.parts == 1, "one part parsed");
    CHECK(strstr(c.filename, "filename=\"my file.txt\"") != NULL,
          "filename with spaces captured");
    CHECK(strcmp(c.data, "file body") == 0, "file body captured");

    /* empty field value */
    parse(
        "--BOUNDARY\r\n"
        "Content-Disposition: form-data; name=\"empty\"\r\n"
        "\r\n"
        "\r\n"
        "--BOUNDARY--\r\n", &c);
    CHECK(c.parts == 1, "empty-field part parsed");
    CHECK(c.data_len == 0, "empty field yields no data");

    /* value containing boundary-like text must not split the part */
    parse(
        "--BOUNDARY\r\n"
        "Content-Disposition: form-data; name=\"note\"\r\n"
        "\r\n"
        "line with --BOUNDARY-ish text\r\n"
        "--BOUNDARY--\r\n", &c);
    CHECK(c.parts == 1, "boundary-like content stays one part");
    CHECK(strstr(c.data, "--BOUNDARY-ish") != NULL, "boundary-like content preserved");

    printf("%s\n", failures == 0 ? "ALL PASS" : "SOME FAILED");
    return failures == 0 ? 0 : 1;
}

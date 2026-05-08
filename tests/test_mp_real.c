#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "multipart_parser.h"

typedef struct {
    char *data;
    size_t len;
} ctx_t;

static int pd(multipart_parser *p, const char *at, size_t len) {
    ctx_t *ctx = (ctx_t *)multipart_parser_get_data(p);
    if (!ctx->data) {
        ctx->data = (char *)malloc(len + 1);
        memcpy(ctx->data, at, len);
        ctx->len = len;
    } else {
        char *nd = (char *)malloc(ctx->len + len + 1);
        memcpy(nd, ctx->data, ctx->len);
        memcpy(nd + ctx->len, at, len);
        free(ctx->data);
        ctx->data = nd;
        ctx->len += len;
    }
    printf("pd: len=%zu total=%zu\n", len, ctx->len);
    return 0;
}
static int begin(multipart_parser *p) { (void)p; printf("begin\n"); return 0; }
static int end(multipart_parser *p) { (void)p; printf("end\n"); return 0; }

int main(void) {
    // Read actual curl body from a file or simulate
    const char *bnd = "------------------------soZxkQZEzYj2xtjBt4MNyk";
    const char *body =
        "--------------------------soZxkQZEzYj2xtjBt4MNyk\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"fly_board.log\"\r\n"
        "\r\n"
        "hello world\n"
        "second line\n"
        "\r\n"
        "--------------------------soZxkQZEzYj2xtjBt4MNyk--\r\n";
    multipart_parser_settings s = {0};
    s.on_part_data = pd;
    s.on_part_data_begin = begin;
    s.on_part_data_end = end;
    char dash_bnd[256];
    snprintf(dash_bnd, sizeof(dash_bnd), "--%s", bnd);
    multipart_parser *p = multipart_parser_init(dash_bnd, &s);
    ctx_t ctx = {0};
    multipart_parser_set_data(p, &ctx);
    size_t parsed = multipart_parser_execute(p, body, strlen(body));
    printf("parsed=%zu body_len=%zu data_len=%zu data='%.*s'\n", parsed, strlen(body), ctx.len, (int)ctx.len, ctx.data ? ctx.data : "");
    free(ctx.data);
    multipart_parser_free(p);
    return 0;
}

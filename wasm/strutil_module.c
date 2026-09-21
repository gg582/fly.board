/**
 * @file strutil_module.c
 * @brief WASI preview1 module exposing the pure string utilities.
 *
 * The implementations live in src/utils/strutil_pure.h and are the exact
 * functions the server runs (utils.c and sql_escape.c are thin wrappers
 * around them), so module output matches production by construction; the
 * parity test still checks the framing end to end.
 *
 * Protocol (stdin -> stdout):
 *   op 1 slug:              in  string            -> out slug ("" when NULL)
 *   op 2 truncate:          in  u64be max, string -> out u64be length
 *   op 3 safe path:         in  string            -> out u8 (0/1)
 *   op 4 sanitize filename: in  string            -> out string ("" when NULL)
 *   op 5 sql escape:        in  string            -> out string ("" when NULL)
 *   op 6 sql unescape:      in  string            -> out string ("" when NULL)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/utils/strutil_pure.h"

static unsigned char *read_all(size_t *len_out) {
    size_t cap = 1 << 12, len = 0;
    unsigned char *buf = (unsigned char *)malloc(cap);
    if (!buf)
        return NULL;
    for (;;) {
        if (len == cap) {
            cap *= 2;
            unsigned char *nb = (unsigned char *)realloc(buf, cap);
            if (!nb) {
                free(buf);
                return NULL;
            }
            buf = nb;
        }
        size_t n = fread(buf + len, 1, cap - len, stdin);
        len += n;
        if (n == 0)
            break;
    }
    *len_out = len;
    return buf;
}

static int write_all(const void *buf, size_t len) {
    while (len > 0) {
        size_t n = fwrite(buf, 1, len, stdout);
        if (n == 0)
            return -1;
        buf = (const char *)buf + n;
        len -= n;
    }
    return fflush(stdout) == 0 ? 0 : -1;
}

int main(void) {
    int op = fgetc(stdin);
    if (op < 1 || op > 6)
        return 2;
    size_t in_len = 0;
    unsigned char *in = read_all(&in_len);
    if (!in)
        return 3;

    if (op == 2) {
        if (in_len < 8) {
            free(in);
            return 2;
        }
        uint64_t max = (uint64_t)in[0] << 56 | (uint64_t)in[1] << 48 | (uint64_t)in[2] << 40 |
                       (uint64_t)in[3] << 32 | (uint64_t)in[4] << 24 | (uint64_t)in[5] << 16 |
                       (uint64_t)in[6] << 8 | (uint64_t)in[7];
        /* Input string must be NUL-terminated inside the buffer. */
        if (memchr(in + 8, '\0', in_len - 8) == NULL) {
            free(in);
            return 2;
        }
        uint64_t n = fb_utf8_truncate_len((const char *)in + 8, (size_t)max);
        unsigned char out[8] = {(unsigned char)(n >> 56), (unsigned char)(n >> 48),
                                (unsigned char)(n >> 40), (unsigned char)(n >> 32),
                                (unsigned char)(n >> 24), (unsigned char)(n >> 16),
                                (unsigned char)(n >> 8), (unsigned char)n};
        free(in);
        return write_all(out, 8);
    }

    if (memchr(in, '\0', in_len) == NULL) {
        free(in);
        return 2;
    }
    const char *s = (const char *)in;

    if (op == 3) {
        unsigned char verdict = fb_is_safe_public_path(s) ? 1 : 0;
        free(in);
        return write_all(&verdict, 1);
    }

    char *res = NULL;
    if (op == 1)
        res = fb_generate_slug(s);
    else if (op == 4)
        res = fb_sanitize_filename(s);
    else if (op == 5)
        res = fb_sql_escape(s);
    else
        res = fb_sql_unescape(s);
    int rc = write_all(res ? res : "", res ? strlen(res) : 0);
    free(res);
    free(in);
    return rc;
}

/**
 * @file brotli_module.c
 * @brief WASI preview1 module exposing brotli compress/decompress.
 *
 * The vendored brotli 1.2 C sources (third_party/brotli) are pure C and
 * compile cleanly to wasm32-wasi; this wrapper adds the framed stdin/stdout
 * protocol shared with the other fly.board WASM modules:
 *
 *   in:  u8 op (1 = compress, 2 = decompress), u8 quality (compress only,
 *        ignored on decompress), then the raw payload
 *   out: processed payload on success; exits non-zero on failure
 *
 * Interop with the system brotli used in production is proven by
 * wasm/test_brotli_diff.py: module compress -> system decompress and
 * system compress -> module decompress over randomized and pathological
 * inputs.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <brotli/decode.h>
#include <brotli/encode.h>

static unsigned char *read_all(size_t *len_out) {
    size_t cap = 1 << 16, len = 0;
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

static int write_all(const unsigned char *buf, size_t len) {
    while (len > 0) {
        size_t n = fwrite(buf, 1, len, stdout);
        if (n == 0)
            return -1;
        buf += n;
        len -= n;
    }
    return fflush(stdout) == 0 ? 0 : -1;
}

int main(void) {
    int op = fgetc(stdin);
    int quality = fgetc(stdin);
    if ((op != 1 && op != 2) || quality < 0)
        return 2;

    size_t in_len = 0;
    unsigned char *in = read_all(&in_len);
    if (!in)
        return 3;

    size_t out_len = 0;
    unsigned char *out = NULL;
    int ok;
    if (op == 1) {
        size_t cap = in_len + in_len / 2 + 1024;
        out = (unsigned char *)malloc(cap);
        if (!out) {
            free(in);
            return 3;
        }
        out_len = cap;
        if (quality > 11)
            quality = 11;
        ok = BrotliEncoderCompress((int)quality, BROTLI_DEFAULT_WINDOW, BROTLI_MODE_GENERIC,
                                   in_len, in, &out_len, out)
                 ? 0
                 : 1;
    } else {
        /* One-shot BrotliDecoderDecompress needs the whole output buffer up
         * front (it errors instead of reporting NEEDS_MORE_OUTPUT for
         * distance-heavy streams), so decompress incrementally and stream the
         * result to stdout. */
        ok = 0;
        BrotliDecoderState *dec = BrotliDecoderCreateInstance(NULL, NULL, NULL);
        if (!dec) {
            ok = 1;
        } else {
            const unsigned char *next_in = in;
            size_t avail_in = in_len;
            unsigned char chunk[16384];
            size_t total = 0;
            for (;;) {
                unsigned char *next_out = chunk;
                size_t avail_out = sizeof(chunk);
                BrotliDecoderResult res = BrotliDecoderDecompressStream(
                    dec, &avail_in, &next_in, &avail_out, &next_out, &total);
                size_t produced = sizeof(chunk) - avail_out;
                if (produced > 0 && write_all(chunk, produced) != 0) {
                    ok = 1;
                    break;
                }
                if (res == BROTLI_DECODER_RESULT_SUCCESS)
                    break;
                if (res == BROTLI_DECODER_RESULT_ERROR ||
                    (res == BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT && avail_in == 0)) {
                    ok = 1;
                    break;
                }
                if (total > (256u << 20)) { /* decompression bomb cap */
                    ok = 1;
                    break;
                }
            }
            BrotliDecoderDestroyInstance(dec);
        }
    }

    if (ok == 0 && op == 1)
        ok = write_all(out, out_len);
    free(out);
    free(in);
    return ok;
}

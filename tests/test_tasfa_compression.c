/* Real production code and libraries; only allocator/header and faults are
 * substituted. Public constants below must match tasfa_internal.h. */
#define TASFA_INTERNAL_H
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/evp.h>
#include <zlib.h>
#include <zstd.h>
#include <brotli/encode.h>
#include <brotli/decode.h>
#define TASFA_COMPRESS_MIN_GAIN_BYTES 1024
typedef enum { TASFA_COMPRESS_NONE, TASFA_COMPRESS_ZSTD,
               TASFA_COMPRESS_BROTLI, TASFA_COMPRESS_GZIP } tasfa_compress_type_t;
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); exit(1); } } while (0)
static size_t calls[3], bytes[3], allocations, live, cases;
static int levels[3], fault = -1;
static bool fail_alloc, fail_init;
static const void *seen[3];
static void *allocate(size_t n) {
    allocations++;
    if (fail_alloc) return NULL;
    void *p = malloc(n);
    if (p) live++;
    return p;
}
static void release(void *p) { if (p) { CHECK(live > 0); live--; free(p); } }
#define cwist_alloc allocate
#define cwist_free release
static void record(int i, const void *p, size_t n) { calls[i]++; bytes[i] += n; seen[i] = p; }
static size_t counted_zstd(void *dst, size_t cap, const void *src, size_t n, int level) {
    record(1, src, n); levels[1] = level;
    if (fault == 1) return (size_t)-1;
    return ZSTD_compress(dst, cap, src, n, level);
}
static BROTLI_BOOL counted_br(int quality, int window, BrotliEncoderMode mode,
                              size_t n, const uint8_t *src, size_t *cap, uint8_t *dst) {
    record(0, src, n); levels[0] = quality;
    if (fault == 0) return BROTLI_FALSE;
    return BrotliEncoderCompress(quality, window, mode, n, src, cap, dst);
}
static int counted_init(z_streamp s, int level, int method, int window, int mem,
                         int strategy, const char *version, int size) {
    levels[2] = level;
    if (fail_init) return Z_MEM_ERROR;
    return deflateInit2_(s, level, method, window, mem, strategy, version, size);
}
static int counted_deflate(z_streamp s, int flush) {
    record(2, s->next_in, s->avail_in);
    if (fault == 2) return Z_STREAM_ERROR;
    return deflate(s, flush);
}
#define ZSTD_compress counted_zstd
#define BrotliEncoderCompress counted_br
#define deflateInit2_ counted_init
#define deflate counted_deflate
#include "../src/handlers/tasfa/crypto.c"
#undef ZSTD_compress
#undef BrotliEncoderCompress
#undef deflateInit2_
#undef deflate

/* Explicit capability contract: bit0=br, bit1=zstd, bit2=gzip. */
static const int selected[8] = {-1, 0, 1, 1, 2, 0, 1, 1};
static const tasfa_compress_type_t types[3] = {
    TASFA_COMPRESS_BROTLI, TASFA_COMPRESS_ZSTD, TASFA_COMPRESS_GZIP
};
static void reset(void) {
    CHECK(live == 0);
    memset(calls, 0, sizeof(calls)); memset(bytes, 0, sizeof(bytes));
    memset(levels, 0, sizeof(levels)); memset(seen, 0, sizeof(seen));
    allocations = 0; fault = -1; fail_alloc = false; fail_init = false;
}
static void noise(unsigned char *p, size_t n) {
    uint32_t state = 0x7129ab37;
    for (size_t i = 0; i < n; i++) {
        state ^= state << 13; state ^= state >> 17; state ^= state << 5;
        p[i] = (unsigned char)state;
    }
}
static void work_contract(const unsigned char *input, size_t n, unsigned caps, bool attempted) {
    int codec = selected[caps];
    for (int i = 0; i < 3; i++) {
        size_t expected = attempted && codec == i ? 1 : 0;
        CHECK(calls[i] == expected);
        CHECK(bytes[i] == expected * n);
        if (expected) { CHECK(seen[i] == input); CHECK(levels[i] == 1); }
    }
}
static void roundtrip(const unsigned char *input, size_t n, unsigned caps, bool compresses) {
    reset(); cases++;
    unsigned char *out = (void *)1;
    size_t len = 999;
    tasfa_compress_type_t type = TASFA_COMPRESS_GZIP;
    bool ok = tasfa_compress_alloc(input, n, &out, &len, &type, caps & 2, caps & 1, caps & 4);
    CHECK(ok == compresses);
    work_contract(input, n, caps, input && n > 1024 && caps);
    if (!ok) { CHECK(out == NULL); CHECK(len == 0); CHECK(type == TASFA_COMPRESS_NONE); }
    else { CHECK(type == types[selected[caps]]); CHECK(len < n - 1024); }
    const unsigned char *payload = ok ? out : input;
    size_t size = ok ? len : n;
    unsigned char *plain = malloc(n + 1), *decoded = malloc(size + 1), *cipher = malloc(size + 16);
    CHECK(plain && decoded && cipher);
    for (int encrypted = 0; encrypted < 2; encrypted++) {
        /* Existing empty AES defect is outside this change. */
        if (!n && encrypted) continue;
        const unsigned char *wire = payload;
        if (encrypted) {
            unsigned char key[32] = {1}, iv[12] = {2}; size_t cipher_len = 0;
            CHECK(encrypt_stream_block(key, iv, 7, "test-session", payload, size, cipher, &cipher_len));
            CHECK(cipher_len == size + 16);
            CHECK(decrypt_stream_block(key, iv, 7, "test-session", cipher, cipher_len, decoded, size));
            wire = decoded;
        }
        if (ok) CHECK(tasfa_decompress_to(wire, size, plain, n, type));
        else if (n) memcpy(plain, wire, n);
        if (n) CHECK(memcmp(plain, input, n) == 0);
    }
    free(plain); free(decoded); free(cipher); release(out); CHECK(live == 0);
}
static void failure_cases(void) {
    unsigned char input[16384]; memset(input, 'a', sizeof(input));
    for (unsigned caps = 1; caps < 8; caps++) {
        for (int kind = 0; kind < 3; kind++) {
            reset();
            fault = kind == 0 ? selected[caps] : -1;
            fail_alloc = kind == 1;
            fail_init = kind == 2 && selected[caps] == 2;
            if (kind == 2 && !fail_init) continue;
            cases++;
            unsigned char *out = (void *)1; size_t len = 99;
            tasfa_compress_type_t type = TASFA_COMPRESS_GZIP;
            CHECK(!tasfa_compress_alloc(input, sizeof(input), &out, &len, &type, caps & 2, caps & 1, caps & 4));
            CHECK(out == NULL && len == 0 && type == TASFA_COMPRESS_NONE);
            work_contract(input, sizeof(input), caps, kind == 0);
            CHECK(allocations == (fail_init ? 0 : 1)); CHECK(live == 0);
        }
    }
    reset();
    unsigned char *out = (void *)1; size_t len = 99;
    tasfa_compress_type_t type = TASFA_COMPRESS_GZIP;
    CHECK(!tasfa_compress_alloc(NULL, 12345, &out, &len, &type, true, true, true));
    CHECK(out == NULL && len == 0 && type == TASFA_COMPRESS_NONE);
    CHECK(!tasfa_compress_alloc(input, sizeof(input), NULL, &len, &type, true, true, true));
    CHECK(!tasfa_compress_alloc(input, sizeof(input), &out, NULL, &type, true, true, true));
    CHECK(!tasfa_compress_alloc(input, sizeof(input), &out, &len, NULL, true, true, true));
    CHECK(allocations == 0); cases += 4;
}
static bool direct_compress(unsigned caps, const unsigned char *p, size_t n, unsigned char **out, size_t *len) {
    if (caps == 1) return tasfa_brotli_compress_alloc(p, n, out, len);
    if (caps == 2) return tasfa_zstd_compress_alloc(p, n, out, len);
    return tasfa_gzip_compress_alloc(p, n, out, len);
}
static void strict_gain_boundaries(void) {
    unsigned char input[2048]; memset(input, 'a', sizeof(input));
    for (unsigned caps = 1; caps <= 4; caps *= 2) {
        bool boundary = false;
        for (size_t n = 1025; n < sizeof(input); n++) {
            reset(); unsigned char *out = NULL; size_t len = 0;
            CHECK(direct_compress(caps, input, n, &out, &len)); release(out);
            if (len + 1024 == n) {
                roundtrip(input, n, caps, false);
                roundtrip(input, n + 1, caps, true);
                boundary = true; break;
            }
        }
        CHECK(boundary);
    }
}
int main(void) {
    const size_t sizes[] = {131072, 0, 1, 1023, 1024, 1025, 4097, 131071, 131073,
                           8 * 1024 * 1024, 32 * 1024 * 1024, 77777};
    for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
        size_t n = sizes[s]; unsigned char *input = malloc(n + 1); CHECK(input);
        noise(input, n);
        for (unsigned caps = 0; caps < 8; caps++) roundtrip(input, n, caps, false);
        memset(input, 'a', n);
        for (unsigned caps = 0; caps < 8; caps++) roundtrip(input, n, caps, caps && n > 1025);
        free(input);
    }
    const size_t n = 32 * 1024 * 1024, window = 16384;
    const size_t offsets[] = {0, (n - window) / 2, n - window};
    unsigned char *input = malloc(n); CHECK(input);
    /* Historical missed-sampling counterexample MUST now compress. */
    memset(input, 'a', n);
    for (size_t i = 0; i < 3; i++) noise(input + offsets[i], window);
    for (unsigned caps = 0; caps < 8; caps++) roundtrip(input, n, caps, caps != 0);
    /* Useful regions at the old sampled positions also retain savings. */
    for (size_t i = 0; i < 3; i++) {
        noise(input, n); memset(input + offsets[i], 'a', window);
        roundtrip(input, n, 7, true);
    }
    noise(input, n); reset(); unsigned char *packed = NULL; size_t packed_len = 0;
    CHECK(tasfa_gzip_compress_alloc(input, n, &packed, &packed_len));
    /* Copy out of instrumented ownership so each dispatcher case starts clean. */
    unsigned char *copy = malloc(packed_len); CHECK(copy); memcpy(copy, packed, packed_len); release(packed);
    for (unsigned caps = 0; caps < 8; caps++) roundtrip(copy, packed_len, caps, false);
    free(copy); free(input);
    strict_gain_boundaries(); failure_cases();
    CHECK(live == 0);
    printf("TASFA single-pass compression/crypto: %zu cases passed\n", cases);
    return 0;
}

/* Compile the actual production dispatcher/crypto implementation without CWIST.
 * Only its umbrella header and allocator are replaced; codecs and AES are real.
 * Keep these public enum/constant declarations aligned with tasfa_internal.h. */
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
#define cwist_alloc malloc
#define cwist_free free
typedef enum {
    TASFA_COMPRESS_NONE = 0, TASFA_COMPRESS_ZSTD,
    TASFA_COMPRESS_BROTLI, TASFA_COMPRESS_GZIP
} tasfa_compress_type_t;

static size_t calls[3], bytes[3], largest[3];
static void record(size_t codec, size_t len) {
    calls[codec]++; bytes[codec] += len;
    if (len > largest[codec]) largest[codec] = len;
}
static size_t counted_zstd(void *dst, size_t cap, const void *src, size_t len, int level) {
    record(1, len);
    return ZSTD_compress(dst, cap, src, len, level);
}
static BROTLI_BOOL counted_brotli(int quality, int window, BrotliEncoderMode mode,
                                 size_t len, const uint8_t *src, size_t *cap, uint8_t *dst) {
    record(0, len);
    return BrotliEncoderCompress(quality, window, mode, len, src, cap, dst);
}
static int counted_deflate(z_streamp stream, int flush) {
    record(2, stream->avail_in);
    return deflate(stream, flush);
}
#define ZSTD_compress counted_zstd
#define BrotliEncoderCompress counted_brotli
#define deflate counted_deflate
#include "../src/handlers/tasfa/crypto.c"
#undef ZSTD_compress
#undef BrotliEncoderCompress
#undef deflate

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); exit(1); \
} } while (0)
static void reset_work(void) {
    memset(calls, 0, sizeof(calls)); memset(bytes, 0, sizeof(bytes));
    memset(largest, 0, sizeof(largest));
}
static void noise(unsigned char *p, size_t n) {
    uint32_t state = 0x7129ab37;
    for (size_t i = 0; i < n; i++) {
        state ^= state << 13; state ^= state >> 17; state ^= state << 5;
        p[i] = (unsigned char)state;
    }
}
static void roundtrip(const unsigned char *input, size_t n, unsigned caps,
                      int expected_type, bool bounded) {
    reset_work();
    unsigned char *compressed = (void *)1;
    size_t compressed_len = 999;
    tasfa_compress_type_t type = TASFA_COMPRESS_GZIP;
    bool ok = tasfa_compress_alloc(input, n, &compressed, &compressed_len, &type,
                                   caps & 2, caps & 1, caps & 4);
    CHECK((int)type == expected_type);
    CHECK(ok == (type != TASFA_COMPRESS_NONE));
    if (!ok) { CHECK(compressed == NULL); CHECK(compressed_len == 0); }
    else {
        CHECK(compressed_len + 1024 < n);
        CHECK((type == TASFA_COMPRESS_BROTLI && (caps & 1)) ||
              (type == TASFA_COMPRESS_ZSTD && (caps & 2)) ||
              (type == TASFA_COMPRESS_GZIP && (caps & 4)));
    }
    if (bounded) {
        CHECK(largest[0] <= 16384 && largest[1] <= 16384 && largest[2] <= 16384);
        CHECK(bytes[0] + bytes[1] + bytes[2] <= 3 * 16384);
        CHECK(calls[0] + calls[1] + calls[2] <= 3);
    }
    if (!caps || n <= 1024) CHECK(calls[0] + calls[1] + calls[2] == 0);
    const unsigned char *payload = ok ? compressed : input;
    size_t payload_len = ok ? compressed_len : n;
    unsigned char *plain = malloc(n + 1), *decrypted = malloc(payload_len + 1);
    unsigned char *cipher = malloc(payload_len + 16);
    CHECK(plain && decrypted && cipher);
    for (int encrypted = 0; encrypted <= 1; encrypted++) {
        const unsigned char *wire = payload;
        if (encrypted) {
            unsigned char key[32] = {1}, iv[12] = {2};
            size_t cipher_len = 0;
            CHECK(encrypt_stream_block(key, iv, 7, "test-session", payload,
                                       payload_len, cipher, &cipher_len));
            CHECK(cipher_len == payload_len + 16);
            CHECK(decrypt_stream_block(key, iv, 7, "test-session", cipher,
                                       cipher_len, decrypted, payload_len));
            wire = decrypted;
        }
        if (ok) CHECK(tasfa_decompress_to(wire, payload_len, plain, n, type));
        else memcpy(plain, wire, n);
        CHECK(memcmp(plain, input, n) == 0);
    }
    free(plain); free(decrypted); free(cipher); free(compressed);
}
static void test_sample_positions(void) {
    const size_t n = 1024 * 1024, window = 16384;
    const size_t offsets[] = {0, (n - window) / 2, n - window};
    unsigned char *input = malloc(n);
    CHECK(input);
    /* A promising middle or tail must prevent a prefix-only rejection. */
    for (size_t i = 0; i < 3; i++) {
        noise(input, n);
        memset(input + offsets[i], 'a', window);
        roundtrip(input, n, 7, TASFA_COMPRESS_BROTLI, false);
        CHECK(calls[1] == i + 1 && calls[0] == 1 && calls[2] == 0);
        CHECK(largest[0] == n && largest[1] == window);
    }
    /* Deliberate false negative: useful compression outside sampled windows.
     * Prove that a real full codec WOULD save bytes; document rather than hide
     * the trade-off inherent in this policy. */
    memset(input, 'a', n);
    for (size_t i = 0; i < 3; i++) noise(input + offsets[i], window);
    unsigned char *full = NULL;
    size_t full_len = 0;
    CHECK(tasfa_brotli_compress_alloc(input, n, &full, &full_len));
    CHECK(full_len + 1024 < n);
    free(full);
    roundtrip(input, n, 7, TASFA_COMPRESS_NONE, true);
    CHECK(calls[1] == 3 && calls[0] == 0 && calls[2] == 0);
    /* Actual already-compressed bytes, not merely a fake file signature. */
    noise(input, n);
    CHECK(tasfa_gzip_compress_alloc(input, n, &full, &full_len));
    roundtrip(full, full_len, 7, TASFA_COMPRESS_NONE, true);
    roundtrip(full, full_len, 4, TASFA_COMPRESS_NONE, true);
    free(full);
    free(input);
}

int main(void) {
    size_t sizes[] = {131072, 1, 1023, 1024, 1025, 4097, 131071, 131073,
                      8 * 1024 * 1024, 32 * 1024 * 1024, 77777};
    for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
        size_t n = sizes[s];
        unsigned char *input = malloc(n);
        CHECK(input);
        noise(input, n);
        for (unsigned caps = 0; caps < 8; caps++) {
            roundtrip(input, n, caps, TASFA_COMPRESS_NONE, n >= 131072);
        }
        static const char text[] = "TASFA transfer regression: repeated UTF-8 text, line 42.\n";
        for (size_t i = 0; i < n; i++) input[i] = (unsigned char)text[i % (sizeof(text) - 1)];
        for (unsigned caps = 0; caps < 8; caps++) {
            int expected = !caps || n <= 1025 ? TASFA_COMPRESS_NONE :
                (caps & 1) ? TASFA_COMPRESS_BROTLI :
                (caps & 2) ? TASFA_COMPRESS_ZSTD : TASFA_COMPRESS_GZIP;
            roundtrip(input, n, caps, expected, false);
        }
        free(input);
    }
    test_sample_positions();
    puts("TASFA production compression/crypto regression tests passed");
    return 0;
}

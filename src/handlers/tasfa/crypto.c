#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "tasfa_internal.h"

static void derive_stream_iv(unsigned char out[12], const unsigned char seed[12], int chunk_index) {
    memcpy(out, seed, 12);
    out[8] ^= (unsigned char)((chunk_index >> 24) & 0xff);
    out[9] ^= (unsigned char)((chunk_index >> 16) & 0xff);
    out[10] ^= (unsigned char)((chunk_index >> 8) & 0xff);
    out[11] ^= (unsigned char)(chunk_index & 0xff);
}

static int build_stream_aad(unsigned char *out, size_t out_len, const char *upload_id, int chunk_index) {
    if (!out || !out_len) return -1;
    return snprintf((char *)out, out_len, "%s:%d", upload_id ? upload_id : "", chunk_index);
}

bool encrypt_stream_block(const unsigned char *key, const unsigned char *iv_seed, int chunk_index,
                          const char *session_id,
                          const unsigned char *plaintext, size_t plaintext_len,
                          unsigned char *ciphertext, size_t *ciphertext_len_out) {
    if (!key || !iv_seed || !plaintext || !ciphertext || !ciphertext_len_out) return false;
    unsigned char iv[12];
    unsigned char aad[512];
    int aad_len = build_stream_aad(aad, sizeof(aad), session_id, chunk_index);
    if (aad_len <= 0) return false;
    derive_stream_iv(iv, iv_seed, chunk_index);
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return false;
    bool ok = false;
    int out_len = 0;
    int final_len = 0;
    if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1) goto cleanup;
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, sizeof(iv), NULL) != 1) goto cleanup;
    if (EVP_EncryptInit_ex(ctx, NULL, NULL, key, iv) != 1) goto cleanup;
    if (EVP_EncryptUpdate(ctx, NULL, &out_len, aad, aad_len) != 1) goto cleanup;
    if (EVP_EncryptUpdate(ctx, ciphertext, &out_len, plaintext, (int)plaintext_len) != 1) goto cleanup;
    if (EVP_EncryptFinal_ex(ctx, ciphertext + out_len, &final_len) != 1) goto cleanup;
    unsigned char tag[16];
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag) != 1) goto cleanup;
    memcpy(ciphertext + out_len + final_len, tag, 16);
    *ciphertext_len_out = (size_t)(out_len + final_len + 16);
    ok = true;
cleanup:
    EVP_CIPHER_CTX_free(ctx);
    return ok;
}

bool decrypt_stream_block(const unsigned char *key, const unsigned char *iv_seed, int chunk_index,
                          const char *upload_id,
                          const unsigned char *ciphertext, size_t ciphertext_len,
                          unsigned char *plaintext, size_t plaintext_len) {
    if (!key || !iv_seed || !ciphertext || ciphertext_len < 16 || !plaintext) return false;
    if (plaintext_len + 16 != ciphertext_len) return false;
    unsigned char iv[12];
    unsigned char aad[512];
    int aad_len = build_stream_aad(aad, sizeof(aad), upload_id, chunk_index);
    if (aad_len <= 0) return false;
    derive_stream_iv(iv, iv_seed, chunk_index);
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return false;
    bool ok = false;
    int out_len = 0;
    int final_len = 0;
    const unsigned char *tag = ciphertext + plaintext_len;
    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1) goto cleanup;
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, sizeof(iv), NULL) != 1) goto cleanup;
    if (EVP_DecryptInit_ex(ctx, NULL, NULL, key, iv) != 1) goto cleanup;
    if (EVP_DecryptUpdate(ctx, NULL, &out_len, aad, aad_len) != 1) goto cleanup;
    if (EVP_DecryptUpdate(ctx, plaintext, &out_len, ciphertext, (int)plaintext_len) != 1) goto cleanup;
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, (void *)tag) != 1) goto cleanup;
    if (EVP_DecryptFinal_ex(ctx, plaintext + out_len, &final_len) != 1) goto cleanup;
    ok = (size_t)(out_len + final_len) == plaintext_len;
cleanup:
    EVP_CIPHER_CTX_free(ctx);
    return ok;
}

static __thread unsigned char *g_tasfa_decrypt_buf = NULL;
static __thread size_t g_tasfa_decrypt_buf_size = 0;

unsigned char *ensure_decrypt_buf(size_t need) {
    if (g_tasfa_decrypt_buf_size >= need) return g_tasfa_decrypt_buf;
    if (g_tasfa_decrypt_buf) free(g_tasfa_decrypt_buf);
    g_tasfa_decrypt_buf = (unsigned char *)malloc(need);
    g_tasfa_decrypt_buf_size = need;
    return g_tasfa_decrypt_buf;
}

static __thread unsigned char *g_tasfa_inflate_buf = NULL;
static __thread size_t g_tasfa_inflate_buf_size = 0;

unsigned char *ensure_inflate_buf(size_t need) {
    if (g_tasfa_inflate_buf_size >= need) return g_tasfa_inflate_buf;
    if (g_tasfa_inflate_buf) free(g_tasfa_inflate_buf);
    g_tasfa_inflate_buf = (unsigned char *)malloc(need);
    g_tasfa_inflate_buf_size = need;
    return g_tasfa_inflate_buf;
}

static __thread char *g_tasfa_read_buf = NULL;
static __thread size_t g_tasfa_read_buf_size = 0;

char *ensure_read_buf(size_t need) {
    if (g_tasfa_read_buf_size >= need) return g_tasfa_read_buf;
    if (g_tasfa_read_buf) free(g_tasfa_read_buf);
    g_tasfa_read_buf = (char *)malloc(need);
    g_tasfa_read_buf_size = need;
    return g_tasfa_read_buf;
}

bool tasfa_gzip_compress_alloc(const unsigned char *input, size_t input_len,
                               unsigned char **out, size_t *out_len) {
    if (!input || input_len == 0 || !out || !out_len) return false;
    *out = NULL;
    *out_len = 0;

    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    if (deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        return false;
    }

    size_t cap = deflateBound(&zs, (uLong)input_len);
    unsigned char *buf = (unsigned char *)cwist_alloc(cap);
    if (!buf) {
        deflateEnd(&zs);
        return false;
    }

    zs.next_in = (Bytef *)input;
    zs.avail_in = (uInt)input_len;
    zs.next_out = buf;
    zs.avail_out = (uInt)cap;
    int rc = deflate(&zs, Z_FINISH);
    if (rc != Z_STREAM_END) {
        cwist_free(buf);
        deflateEnd(&zs);
        return false;
    }

    *out_len = cap - zs.avail_out;
    *out = buf;
    deflateEnd(&zs);
    return true;
}

bool tasfa_gzip_decompress_to(const unsigned char *input, size_t input_len,
                              unsigned char *out, size_t expected_len) {
    if (!input || input_len == 0 || !out) return false;

    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    if (inflateInit2(&zs, 15 + 16) != Z_OK) return false;

    zs.next_in = (Bytef *)input;
    zs.avail_in = (uInt)input_len;
    zs.next_out = out;
    zs.avail_out = (uInt)expected_len;
    int rc = inflate(&zs, Z_FINISH);
    bool ok = (rc == Z_STREAM_END && zs.total_out == expected_len);
    inflateEnd(&zs);
    return ok;
}

/**
 * @file tasfa_crypto_wasm.c
 * @brief Host side of the sandboxed TASFA crypto module.
 *
 * Spawns `wasmtime run <module>` per operation and exchanges one framed
 * request/response (wasm/tasfa_crypto_protocol.h) over the child's
 * stdin/stdout.  The module computes AES-256-GCM with the same IV derivation
 * and AAD layout as src/handlers/tasfa/crypto.c; parity is enforced by
 * wasm/test_parity.py against OpenSSL EVP.
 */

#define _POSIX_C_SOURCE 200809L

#include "tasfa_crypto_wasm.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../../wasm/tasfa_crypto_protocol.h"

#define TASFA_WASM_MAX_IO (64u * 1024u * 1024u) /* generous chunk ceiling */

static char g_module_path[1024];
static char g_wasmtime_bin[1024] = "wasmtime";
static int g_checked = 0;
static bool g_available = false;

static void detect_once(void) {
    if (g_checked)
        return;
    g_checked = 1;
    const char *mod = getenv("TASFA_CRYPTO_WASM");
    if (mod && *mod && access(mod, R_OK) == 0) {
        snprintf(g_module_path, sizeof(g_module_path), "%s", mod);
        const char *wt = getenv("TASFA_WASMTIME");
        if (wt && *wt)
            snprintf(g_wasmtime_bin, sizeof(g_wasmtime_bin), "%s", wt);
        g_available = true;
    }
}

bool tasfa_wasm_crypto_available(void) {
    detect_once();
    return g_available;
}

static bool write_full(int fd, const uint8_t *buf, size_t len) {
    while (len > 0) {
        ssize_t n = write(fd, buf, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        buf += n;
        len -= (size_t)n;
    }
    return true;
}

static bool read_full(int fd, uint8_t *buf, size_t len) {
    while (len > 0) {
        ssize_t n = read(fd, buf, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (n == 0)
            return false;
        buf += n;
        len -= (size_t)n;
    }
    return true;
}

/**
 * Run one request.  `req` is the framed protocol buffer; on success `resp`
 * (malloc'ed, *resp_len bytes) holds the framed response.
 */
static bool run_module(const uint8_t *req, size_t req_len, uint8_t **resp, size_t *resp_len) {
    int in_pipe[2] = {-1, -1}, out_pipe[2] = {-1, -1};
    if (pipe(in_pipe) != 0 || pipe(out_pipe) != 0)
        goto fail_pipes;

    pid_t pid = fork();
    if (pid < 0)
        goto fail_pipes;
    if (pid == 0) {
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        close(in_pipe[0]);
        close(in_pipe[1]);
        close(out_pipe[0]);
        close(out_pipe[1]);
        execlp(g_wasmtime_bin, g_wasmtime_bin, "run", g_module_path, (char *)NULL);
        _exit(127);
    }

    close(in_pipe[0]);
    in_pipe[0] = -1;
    close(out_pipe[1]);
    out_pipe[1] = -1;

    bool ok = false;
    uint8_t *buf = NULL;
    if (write_full(in_pipe[1], req, req_len)) {
        /* Signal EOF so the module proceeds; then read the response.  Short
         * writes are fine: the module reads stdin until EOF. */
        close(in_pipe[1]);
        in_pipe[1] = -1;

        uint8_t hdr[5];
        if (read_full(out_pipe[0], hdr, sizeof(hdr))) {
            size_t out_len = fb_be32_read(hdr + 1);
            if (out_len <= TASFA_WASM_MAX_IO) {
                buf = (uint8_t *)malloc(5 + out_len);
                if (buf) {
                    memcpy(buf, hdr, 5);
                    if (out_len == 0 || read_full(out_pipe[0], buf + 5, out_len)) {
                        *resp = buf;
                        *resp_len = 5 + out_len;
                        ok = true;
                    } else {
                        free(buf);
                    }
                }
            }
        }
    }

    if (in_pipe[1] >= 0)
        close(in_pipe[1]);
    close(out_pipe[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    if (!ok)
        return false;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        free(*resp);
        *resp = NULL;
        return false;
    }
    return true;

fail_pipes:
    if (in_pipe[0] >= 0)
        close(in_pipe[0]);
    if (in_pipe[1] >= 0)
        close(in_pipe[1]);
    if (out_pipe[0] >= 0)
        close(out_pipe[0]);
    if (out_pipe[1] >= 0)
        close(out_pipe[1]);
    return false;
}

static size_t build_request(uint8_t op, const unsigned char *key, const unsigned char *iv_seed,
                            int chunk_index, const char *sid, const unsigned char *data,
                            size_t data_len, uint8_t **req_out) {
    size_t sid_len = sid ? strlen(sid) : 0;
    if (sid_len > TASFA_CRYPTO_MAX_SID_LEN)
        return 0;
    size_t fixed = 1 + TASFA_CRYPTO_KEY_LEN + TASFA_CRYPTO_IV_SEED_LEN + 4 + 2;
    size_t total = fixed + sid_len + 4 + data_len;
    uint8_t *req = (uint8_t *)malloc(total);
    if (!req)
        return 0;
    req[0] = op;
    memcpy(req + 1, key, TASFA_CRYPTO_KEY_LEN);
    memcpy(req + 33, iv_seed, TASFA_CRYPTO_IV_SEED_LEN);
    fb_be32_write(req + 45, (uint32_t)chunk_index);
    fb_be16_write(req + 49, (uint16_t)sid_len);
    if (sid_len)
        memcpy(req + fixed, sid, sid_len);
    fb_be32_write(req + fixed + sid_len, (uint32_t)data_len);
    if (data_len)
        memcpy(req + fixed + sid_len + 4, data, data_len);
    *req_out = req;
    return total;
}

bool tasfa_wasm_encrypt_block(const unsigned char *key, const unsigned char *iv_seed,
                              int chunk_index, const char *session_id,
                              const unsigned char *plaintext, size_t plaintext_len,
                              unsigned char *ciphertext, size_t *ciphertext_len_out) {
    if (!key || !iv_seed || !ciphertext || !ciphertext_len_out)
        return false;
    if (plaintext_len > 0 && !plaintext)
        return false;

    uint8_t *req = NULL;
    size_t req_len = build_request(TASFA_CRYPTO_OP_ENCRYPT, key, iv_seed, chunk_index, session_id,
                                   plaintext, plaintext_len, &req);
    if (req_len == 0)
        return false;
    uint8_t *resp = NULL;
    size_t resp_len = 0;
    bool ok = run_module(req, req_len, &resp, &resp_len);
    free(req);
    if (!ok)
        return false;
    if (resp[0] != 0) {
        free(resp);
        return false;
    }
    size_t out_len = fb_be32_read(resp + 1);
    if (out_len != plaintext_len + TASFA_CRYPTO_TAG_LEN) {
        free(resp);
        return false;
    }
    memcpy(ciphertext, resp + 5, out_len);
    *ciphertext_len_out = out_len;
    free(resp);
    return true;
}

bool tasfa_wasm_decrypt_block(const unsigned char *key, const unsigned char *iv_seed,
                              int chunk_index, const char *upload_id,
                              const unsigned char *ciphertext, size_t ciphertext_len,
                              unsigned char *plaintext, size_t plaintext_len) {
    if (!key || !iv_seed || !ciphertext || ciphertext_len < TASFA_CRYPTO_TAG_LEN)
        return false;
    if (plaintext_len + TASFA_CRYPTO_TAG_LEN != ciphertext_len)
        return false;
    if (plaintext_len > 0 && !plaintext)
        return false;

    uint8_t *req = NULL;
    size_t req_len = build_request(TASFA_CRYPTO_OP_DECRYPT, key, iv_seed, chunk_index, upload_id,
                                   ciphertext, ciphertext_len, &req);
    if (req_len == 0)
        return false;
    uint8_t *resp = NULL;
    size_t resp_len = 0;
    bool ok = run_module(req, req_len, &resp, &resp_len);
    free(req);
    if (!ok)
        return false;
    if (resp[0] != 0) {
        free(resp);
        return false; /* auth failure or module-level error */
    }
    size_t out_len = fb_be32_read(resp + 1);
    if (out_len != plaintext_len) {
        free(resp);
        return false;
    }
    if (out_len)
        memcpy(plaintext, resp + 5, out_len);
    free(resp);
    return true;
}

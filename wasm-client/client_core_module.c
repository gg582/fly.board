/**
 * @file client_core_module.c
 * @brief CWIST WASM/WASI client compute kernel for fly.board.
 *
 * Exposes zero-copy TypedArray views via CWIST v4's <cwist/wasm/typedarray.h>
 * when compiled with Emscripten, and compiles to a standalone WASI/WASM module
 * with zero runtime imports so it can also be instantiated synchronously via
 * WebAssembly.Instance(new WebAssembly.Module(bytes)) in both the browser and
 * Node.js VM sandboxes.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "client_core.h"

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
#include <cwist/wasm/typedarray.h>
#define FB_EXPORT EMSCRIPTEN_KEEPALIVE
#elif defined(__wasm__)
#define FB_EXPORT __attribute__((visibility("default"))) __attribute__((used))
#else
#define FB_EXPORT
#endif

static fb_gf_state_t g_gf;
static double g_samples[16];
static int32_t g_out_i32[16];
static double g_out_f64[8];
/* Text buffers start at 128 KiB and grow on demand (fb_cc_text_reserve) so
 * whole posts are never truncated.  JS must re-read the views after a reserve
 * because growth may move the buffers and detach the old memory.buffer. */
#define FB_CC_TEXT_INITIAL_CAP 131072
static char g_text_in_static[FB_CC_TEXT_INITIAL_CAP];
static char g_text_out_static[FB_CC_TEXT_INITIAL_CAP];
static char *g_text_in = g_text_in_static;
static char *g_text_out = g_text_out_static;
static size_t g_text_cap = FB_CC_TEXT_INITIAL_CAP;
static uint8_t g_chunk_in[65536];
static uint8_t g_chunk_p0[65536];
static uint8_t g_chunk_p1[65536];

#if defined(__EMSCRIPTEN__)
CWIST_WASM_INSTALL_VIEWS()
CWIST_WASM_EXPOSE_U8(fb_cc_gf_log, g_gf.gf_log, 256)
CWIST_WASM_EXPOSE_U8(fb_cc_gf_exp, g_gf.gf_exp, 510)
CWIST_WASM_EXPOSE_F64(fb_cc_samples, g_samples, 16)
CWIST_WASM_EXPOSE_I32(fb_cc_out_i32, g_out_i32, 16)
CWIST_WASM_EXPOSE_F64(fb_cc_out_f64, g_out_f64, 8)
CWIST_WASM_EXPOSE_U8(fb_cc_text_in, g_text_in, g_text_cap)
CWIST_WASM_EXPOSE_U8(fb_cc_text_out, g_text_out, g_text_cap)
CWIST_WASM_EXPOSE_U8(fb_cc_chunk_in, g_chunk_in, sizeof(g_chunk_in))
CWIST_WASM_EXPOSE_U8(fb_cc_chunk_p0, g_chunk_p0, sizeof(g_chunk_p0))
CWIST_WASM_EXPOSE_U8(fb_cc_chunk_p1, g_chunk_p1, sizeof(g_chunk_p1))
#else
FB_EXPORT const void *fb_cc_gf_log_ptr(void) { return g_gf.gf_log; }
FB_EXPORT size_t fb_cc_gf_log_len(void) { return 256; }
FB_EXPORT const void *fb_cc_gf_exp_ptr(void) { return g_gf.gf_exp; }
FB_EXPORT size_t fb_cc_gf_exp_len(void) { return 510; }
FB_EXPORT const void *fb_cc_samples_ptr(void) { return g_samples; }
FB_EXPORT size_t fb_cc_samples_len(void) { return 16; }
FB_EXPORT const void *fb_cc_out_i32_ptr(void) { return g_out_i32; }
FB_EXPORT size_t fb_cc_out_i32_len(void) { return 16; }
FB_EXPORT const void *fb_cc_out_f64_ptr(void) { return g_out_f64; }
FB_EXPORT size_t fb_cc_out_f64_len(void) { return 8; }
FB_EXPORT const void *fb_cc_text_in_ptr(void) { return g_text_in; }
FB_EXPORT size_t fb_cc_text_in_len(void) { return g_text_cap; }
FB_EXPORT const void *fb_cc_text_out_ptr(void) { return g_text_out; }
FB_EXPORT size_t fb_cc_text_out_len(void) { return g_text_cap; }
FB_EXPORT const void *fb_cc_chunk_in_ptr(void) { return g_chunk_in; }
FB_EXPORT size_t fb_cc_chunk_in_len(void) { return sizeof(g_chunk_in); }
FB_EXPORT const void *fb_cc_chunk_p0_ptr(void) { return g_chunk_p0; }
FB_EXPORT size_t fb_cc_chunk_p0_len(void) { return sizeof(g_chunk_p0); }
FB_EXPORT const void *fb_cc_chunk_p1_ptr(void) { return g_chunk_p1; }
FB_EXPORT size_t fb_cc_chunk_p1_len(void) { return sizeof(g_chunk_p1); }
#endif

FB_EXPORT const void *fb_cc_gf_row_a_ptr(int32_t pos) {
    fb_gf_init(&g_gf);
    if (pos < 0 || pos >= 6) pos = 0;
    return g_gf.row_a[pos];
}

FB_EXPORT const void *fb_cc_gf_row_b_ptr(int32_t pos) {
    fb_gf_init(&g_gf);
    if (pos < 0 || pos >= 6) pos = 0;
    return g_gf.row_b[pos];
}

/* Ensures both text buffers hold at least `need` bytes.  Returns the new
 * capacity, or the old one when allocation fails. */
FB_EXPORT int32_t fb_cc_text_reserve(int32_t need) {
    if (need <= 0 || (size_t)need <= g_text_cap) return (int32_t)g_text_cap;
    size_t cap = g_text_cap;
    while (cap < (size_t)need) cap *= 2;
    char *in = malloc(cap);
    char *out = malloc(cap);
    if (!in || !out) {
        free(in);
        free(out);
        return (int32_t)g_text_cap;
    }
    if (g_text_in != g_text_in_static) free(g_text_in);
    if (g_text_out != g_text_out_static) free(g_text_out);
    g_text_in = in;
    g_text_out = out;
    g_text_cap = cap;
    return (int32_t)cap;
}

FB_EXPORT int32_t fb_cc_init(void) {
#if defined(__EMSCRIPTEN__)
    cwist_wasm_install_views();
#endif
    fb_gf_init(&g_gf);
    return 0;
}

FB_EXPORT int32_t fb_cc_gf_mul(int32_t a, int32_t b) {
    return (int32_t)fb_gf_mul(&g_gf, (uint8_t)(a & 0xFF), (uint8_t)(b & 0xFF));
}

FB_EXPORT int32_t fb_cc_gf_div(int32_t a, int32_t b) {
    return (int32_t)fb_gf_div(&g_gf, (uint8_t)(a & 0xFF), (uint8_t)(b & 0xFF));
}

FB_EXPORT int32_t fb_cc_parity_chunk_count(int32_t data_chunks, int32_t is_rs2) {
    return fb_parity_chunk_count(data_chunks, is_rs2);
}

FB_EXPORT void fb_cc_parity_group_row(int32_t chunk_index, int32_t data_chunks, int32_t is_rs2) {
    int32_t g = 0, r = 0;
    fb_parity_group_row(chunk_index, data_chunks, is_rs2, &g, &r);
    g_out_i32[0] = g;
    g_out_i32[1] = r;
}

FB_EXPORT void fb_cc_rs_accumulate(int32_t slot, const uint8_t *data, int32_t data_len,
                                   uint8_t *p0, uint8_t *p1) {
    fb_rs_accumulate_slice(&g_gf, slot, data, data_len, p0, p1);
}

FB_EXPORT void fb_cc_rs_accumulate_static(int32_t slot, int32_t data_len) {
    if (data_len < 0) data_len = 0;
    if ((size_t)data_len > sizeof(g_chunk_in)) data_len = (int32_t)sizeof(g_chunk_in);
    fb_rs_accumulate_slice(&g_gf, slot, g_chunk_in, data_len, g_chunk_p0, g_chunk_p1);
}

FB_EXPORT void fb_cc_xor_accumulate(const uint8_t *data, int32_t data_len, uint8_t *parity) {
    fb_xor_accumulate_slice(data, data_len, parity);
}

FB_EXPORT void fb_cc_xor_accumulate_static(int32_t data_len) {
    if (data_len < 0) data_len = 0;
    if ((size_t)data_len > sizeof(g_chunk_in)) data_len = (int32_t)sizeof(g_chunk_in);
    fb_xor_accumulate_slice(g_chunk_in, data_len, g_chunk_p0);
}

FB_EXPORT void fb_cc_clear_parity_static(int32_t len) {
    if (len < 0) len = 0;
    if ((size_t)len > sizeof(g_chunk_p0)) len = (int32_t)sizeof(g_chunk_p0);
    memset(g_chunk_p0, 0, (size_t)len);
    memset(g_chunk_p1, 0, (size_t)len);
}

FB_EXPORT double fb_cc_wynn_epsilon_aitken(int32_t count) {
    if (count < 0) count = 0;
    if (count > 16) count = 16;
    return fb_wynn_epsilon_aitken(g_samples, count);
}

FB_EXPORT double fb_cc_predicted_quality(int32_t count) {
    if (count < 0) count = 0;
    if (count > 16) count = 16;
    return fb_predicted_quality(g_samples, count);
}

FB_EXPORT void fb_cc_predicted_signal(int32_t count) {
    if (count < 0) count = 0;
    if (count > 16) count = 16;
    double val = 0.75, conf = 0.25;
    fb_predicted_signal(g_samples, count, &val, &conf);
    g_out_f64[0] = val;
    g_out_f64[1] = conf;
}

FB_EXPORT double fb_cc_quality_from_metrics(double mbps, double slope,
                                            int32_t active_posts, int32_t max_parallel,
                                            double silence_ms, int32_t retry_free_streak,
                                            int32_t is_failure, int32_t is_timeout) {
    return fb_quality_from_metrics(mbps, slope, active_posts, max_parallel,
                                   silence_ms, retry_free_streak, is_failure, is_timeout);
}

FB_EXPORT int32_t fb_cc_predict_upload_rule(int32_t sample_count, int32_t retry_count,
                                            int32_t consecutive_failures,
                                            int32_t same_failure_streak,
                                            int32_t timeout_events,
                                            double progress_silence_ms,
                                            int32_t retry_free_streak,
                                            double throughput_slope) {
    if (sample_count < 0) sample_count = 0;
    if (sample_count > 16) sample_count = 16;
    double tr = 0.0, fn = 0.0, rc = 0.0, cf = 0.0;
    int32_t rule = fb_predict_upload_rule(g_samples, sample_count, retry_count,
                                          consecutive_failures, same_failure_streak,
                                          timeout_events, progress_silence_ms,
                                          retry_free_streak, throughput_slope,
                                          &tr, &fn, &rc, &cf);
    g_out_f64[0] = tr;
    g_out_f64[1] = fn;
    g_out_f64[2] = rc;
    g_out_f64[3] = cf;
    return rule;
}

FB_EXPORT void fb_cc_normalize_dl_session(int32_t chunk_count, int32_t hw_concurrency,
                                          double chunk_size, int32_t req_max_parallel,
                                          int32_t req_init_parallel, int32_t req_coalesce,
                                          double total_size, int32_t is_media_mime,
                                          int32_t fast_tier, int32_t is_mobile) {
    fb_dl_norm_out_t out;
    fb_normalize_dl_session(chunk_count, hw_concurrency, chunk_size, req_max_parallel,
                            req_init_parallel, req_coalesce, total_size, is_media_mime,
                            fast_tier, is_mobile, &out);
    g_out_i32[0] = out.max_parallel;
    g_out_i32[1] = out.target_parallel;
    g_out_i32[2] = out.coalesce_chunks;
    g_out_i32[3] = out.current_span;
    g_out_i32[4] = out.max_span;
    g_out_i32[5] = out.standard_max_parallel;
    g_out_i32[6] = out.standard_target_parallel;
    g_out_i32[7] = out.standard_coalesce_chunks;
    g_out_i32[8] = out.standard_current_span;
    g_out_i32[9] = out.standard_max_span;
    g_out_i32[10] = out.large_media;
    g_out_i32[11] = out.ultra_fast_connection;
}

FB_EXPORT void fb_cc_apply_predicted_dl_rule(int32_t large_media, int32_t ultra_fast,
                                             int32_t is_mobile, int32_t max_parallel,
                                             double predicted,
                                             int32_t target_parallel, int32_t current_span) {
    int32_t tp = target_parallel;
    int32_t cs = current_span;
    fb_apply_predicted_dl_rule(large_media, ultra_fast, is_mobile, max_parallel,
                               predicted, &tp, &cs);
    g_out_i32[0] = tp;
    g_out_i32[1] = cs;
}

FB_EXPORT void fb_cc_tune_dl_success(int32_t success_events, int32_t fast_streak,
                                     double ewma_mbps, int32_t large_media,
                                     int32_t ultra_fast, int32_t is_ultra_tier,
                                     int32_t is_mobile, int32_t current_span,
                                     int32_t max_span, int32_t target_parallel,
                                     int32_t max_parallel,
                                     int32_t std_max_parallel, int32_t std_target_parallel,
                                     int32_t std_current_span, int32_t std_max_span,
                                     double bytes, double duration_ms) {
    fb_dl_tune_success_t out;
    fb_tune_dl_success(success_events, fast_streak, ewma_mbps, large_media,
                       ultra_fast, is_ultra_tier, is_mobile, current_span,
                       max_span, target_parallel, max_parallel,
                       std_max_parallel, std_target_parallel, std_current_span, std_max_span,
                       bytes, duration_ms, &out);
    g_out_i32[0] = out.success_events;
    g_out_i32[1] = out.fast_streak;
    g_out_i32[2] = out.demote_ultra_fast;
    g_out_i32[3] = out.remember_double_chunk;
    g_out_i32[4] = out.current_span;
    g_out_i32[5] = out.target_parallel;
    g_out_f64[0] = out.ewma_mbps;
    g_out_f64[1] = out.quality_sample;
}

FB_EXPORT void fb_cc_tune_dl_failure(int32_t failure_events, int32_t large_media,
                                     int32_t is_mobile, int32_t is_timeout,
                                     int32_t current_span, int32_t target_parallel,
                                     int32_t max_parallel) {
    fb_dl_tune_failure_t out;
    fb_tune_dl_failure(failure_events, large_media, is_mobile, is_timeout,
                       current_span, target_parallel, max_parallel, &out);
    g_out_i32[0] = out.failure_events;
    g_out_i32[1] = out.fast_streak;
    g_out_i32[2] = out.current_span;
    g_out_i32[3] = out.target_parallel;
    g_out_i32[4] = out.remember_half_chunk;
    g_out_f64[0] = out.quality_sample;
}

FB_EXPORT int32_t fb_cc_upload_aimd_ramp(int32_t target_parallel, int32_t max_parallel,
                                         int32_t in_fast_recovery) {
    return fb_upload_aimd_ramp(target_parallel, max_parallel, in_fast_recovery);
}

FB_EXPORT int32_t fb_cc_normalize_operator_runs(int32_t in_len) {
    if (in_len < 0) in_len = 0;
    if ((size_t)in_len >= g_text_cap) in_len = (int32_t)(g_text_cap - 1);
    return (int32_t)fb_normalize_operator_runs(g_text_in, (size_t)in_len,
                                               g_text_out, g_text_cap);
}

FB_EXPORT int32_t fb_cc_normalize_tikz_operators(int32_t in_len) {
    if (in_len < 0) in_len = 0;
    if ((size_t)in_len >= g_text_cap) in_len = (int32_t)(g_text_cap - 1);
    return (int32_t)fb_normalize_tikz_operators(g_text_in, (size_t)in_len,
                                                g_text_out, g_text_cap);
}

FB_EXPORT void fb_cc_count_words_and_reading_time(int32_t in_len) {
    if (in_len < 0) in_len = 0;
    if ((size_t)in_len >= g_text_cap) in_len = (int32_t)(g_text_cap - 1);
    int32_t w = 0, m = 0;
    fb_count_words_and_reading_time(g_text_in, (size_t)in_len, &w, &m);
    g_out_i32[0] = w;
    g_out_i32[1] = m;
}

FB_EXPORT int32_t fb_cc_detect_source_language(int32_t in_len) {
    if (in_len < 0) in_len = 0;
    if ((size_t)in_len >= g_text_cap) in_len = (int32_t)(g_text_cap - 1);
    return fb_detect_source_language(g_text_in, (size_t)in_len);
}

FB_EXPORT int32_t fb_cc_is_invalid_translation_text(int32_t in_len) {
    if (in_len < 0) in_len = 0;
    if ((size_t)in_len >= g_text_cap) in_len = (int32_t)(g_text_cap - 1);
    return fb_is_invalid_translation_text(g_text_in, (size_t)in_len);
}

#!/usr/bin/env python3
"""Build public/js/client-core-wasm.js and update WASM-backed client scripts."""

import base64
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WASM_PATH = os.path.join(ROOT, "build-wasm", "client_core.wasm")


def make_inline_loader_js(b64_wasm):
    return f"""    /* --- CWIST WASM/WASI Client Core Kernel (src/utils/client_core.h) --- */
    var __fbWasmCore = (function() {{
        var g = typeof window !== 'undefined' ? window : (typeof globalThis !== 'undefined' ? globalThis : this);
        if (g && g.__fbClientCoreWasm) return g.__fbClientCoreWasm;
        var b64 = '{b64_wasm}';
        var chars = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
        var lookup = new Uint8Array(128);
        for (var i = 0; i < chars.length; i++) lookup[chars.charCodeAt(i)] = i;
        var len = b64.length;
        var pad = b64.charCodeAt(len - 1) === 61 ? (b64.charCodeAt(len - 2) === 61 ? 2 : 1) : 0;
        var bytes = new Uint8Array((len * 3 >> 2) - pad);
        var p = 0;
        for (var j = 0; j < len; j += 4) {{
            var n = (lookup[b64.charCodeAt(j)] << 18) |
                    (lookup[b64.charCodeAt(j + 1)] << 12) |
                    (lookup[b64.charCodeAt(j + 2)] << 6) |
                    lookup[b64.charCodeAt(j + 3)];
            bytes[p++] = (n >> 16) & 255;
            if (p < bytes.length) bytes[p++] = (n >> 8) & 255;
            if (p < bytes.length) bytes[p++] = n & 255;
        }}
        var inst = new WebAssembly.Instance(new WebAssembly.Module(bytes), {{}});
        var exp = inst.exports;
        if (exp._initialize) exp._initialize();
        exp.fb_cc_init();
        var cwistView = {{
            u8: function(ptr, n) {{ return new Uint8Array(exp.memory.buffer, ptr, n); }},
            i32: function(ptr, n) {{ return new Int32Array(exp.memory.buffer, ptr, n); }},
            f64: function(ptr, n) {{ return new Float64Array(exp.memory.buffer, ptr, n); }}
        }};
        var core = {{
            exports: exp,
            cwistView: cwistView,
            samplesView: function() {{ return cwistView.f64(exp.fb_cc_samples_ptr(), exp.fb_cc_samples_len()); }},
            outI32: function() {{ return cwistView.i32(exp.fb_cc_out_i32_ptr(), exp.fb_cc_out_i32_len()); }},
            outF64: function() {{ return cwistView.f64(exp.fb_cc_out_f64_ptr(), exp.fb_cc_out_f64_len()); }},
            textIn: function() {{ return cwistView.u8(exp.fb_cc_text_in_ptr(), exp.fb_cc_text_in_len()); }},
            textOut: function() {{ return cwistView.u8(exp.fb_cc_text_out_ptr(), exp.fb_cc_text_out_len()); }},
            chunkIn: function() {{ return cwistView.u8(exp.fb_cc_chunk_in_ptr(), exp.fb_cc_chunk_in_len()); }},
            chunkP0: function() {{ return cwistView.u8(exp.fb_cc_chunk_p0_ptr(), exp.fb_cc_chunk_p0_len()); }},
            chunkP1: function() {{ return cwistView.u8(exp.fb_cc_chunk_p1_ptr(), exp.fb_cc_chunk_p1_len()); }},
            /* Strings cross into WASM as UTF-8.  A string with lone surrogates
               (valid in JS, not in UTF-8) goes as WTF-8 instead so the WASM
               result round-trips it unchanged, exactly like the JS original. */
            lastWtf8: false,
            writeUtf8: function(str) {{
                var s = String(str || '');
                var wtf = typeof TextEncoder === 'undefined' ||
                    /[\\ud800-\\udbff](?![\\udc00-\\udfff])|(?:^|[^\\ud800-\\udbff])[\\udc00-\\udfff]/.test(s);
                core.lastWtf8 = wtf;
                if (!wtf) {{
                    var encoded = new TextEncoder().encode(s);
                    /* Grow before taking the view: growth detaches old views. */
                    exp.fb_cc_text_reserve(encoded.length + 1);
                    var dst = core.textIn();
                    dst.set(encoded, 0);
                    dst[encoded.length] = 0;
                    return encoded.length;
                }}
                exp.fb_cc_text_reserve(s.length * 3 + 1);
                dst = core.textIn();
                var p = 0;
                for (var i = 0; i < s.length; i++) {{
                    var c = s.charCodeAt(i);
                    var c2 = i + 1 < s.length ? s.charCodeAt(i + 1) : 0;
                    if (c < 0x80) {{
                        dst[p++] = c;
                    }} else if (c < 0x800) {{
                        dst[p++] = 0xC0 | (c >> 6);
                        dst[p++] = 0x80 | (c & 0x3F);
                    }} else if (c >= 0xD800 && c <= 0xDBFF && c2 >= 0xDC00 && c2 <= 0xDFFF) {{
                        var cp = 0x10000 + (((c & 0x3FF) << 10) | (c2 & 0x3FF));
                        i++;
                        dst[p++] = 0xF0 | (cp >> 18);
                        dst[p++] = 0x80 | ((cp >> 12) & 0x3F);
                        dst[p++] = 0x80 | ((cp >> 6) & 0x3F);
                        dst[p++] = 0x80 | (cp & 0x3F);
                    }} else {{
                        dst[p++] = 0xE0 | (c >> 12);
                        dst[p++] = 0x80 | ((c >> 6) & 0x3F);
                        dst[p++] = 0x80 | (c & 0x3F);
                    }}
                }}
                dst[p] = 0;
                return p;
            }},
            readUtf8: function(len) {{
                var src = core.textOut().subarray(0, len);
                if (!core.lastWtf8) {{
                    return new TextDecoder().decode(src);
                }}
                var out = '';
                var i = 0;
                while (i < len) {{
                    var b0 = src[i++];
                    if (b0 < 0x80) {{
                        out += String.fromCharCode(b0);
                    }} else if ((b0 & 0xE0) === 0xC0) {{
                        out += String.fromCharCode(((b0 & 0x1F) << 6) | (src[i++] & 0x3F));
                    }} else if ((b0 & 0xF0) === 0xE0) {{
                        out += String.fromCharCode(((b0 & 0x0F) << 12) | ((src[i++] & 0x3F) << 6) | (src[i++] & 0x3F));
                    }} else {{
                        var cp = ((b0 & 0x07) << 18) | ((src[i++] & 0x3F) << 12) | ((src[i++] & 0x3F) << 6) | (src[i++] & 0x3F);
                        cp -= 0x10000;
                        out += String.fromCharCode(0xD800 + (cp >> 10), 0xDC00 + (cp & 0x3FF));
                    }}
                }}
                return out;
            }},
            loadSamples: function(samples) {{
                if (!samples || !samples.length) return 0;
                var sv = core.samplesView();
                var n = Math.min(samples.length, 16);
                var start = samples.length - n;
                for (var i = 0; i < n; i++) sv[i] = Number(samples[start + i] || 0);
                return n;
            }}
        }};
        if (g) g.__fbClientCoreWasm = core;
        return core;
    }})();"""


def patch_editor_js(loader):
    src_path = os.path.join(ROOT, "tests", "js_baseline", "editor.js")
    dst_path = os.path.join(ROOT, "public", "js", "editor.js")
    with open(src_path, "r", encoding="utf-8") as fp:
        code = fp.read()

    # 1. Replace GF(2^8) + RS(6+2) FEC section
    old_rs = """    /* --- GF(2^8) arithmetic (poly 0x11d) for RS(6+2) FEC --- */
    var tasfaGfLog = null;
    var tasfaGfExp = null;
    /* Per-group-position multiply tables: tasfaGfRowTables[pos] = {a, b} where
       a = x, b = x^2 for x = pos + 1; each table maps a byte to its product. */
    var tasfaGfRowTables = null;

    function ensureTasfaGfTables() {
        if (tasfaGfExp) return;
        var log = new Uint8Array(256);
        var exp = new Uint8Array(510);
        var x = 1;
        for (var i = 0; i < 255; i++) {
            exp[i] = x;
            log[x] = i;
            x <<= 1;
            if (x & 0x100) x ^= 0x11d;
        }
        for (var j = 255; j < 510; j++) exp[j] = exp[j - 255];
        tasfaGfLog = log;
        tasfaGfExp = exp;
        var tables = [];
        for (var pos = 0; pos < 6; pos++) {
            var xv = pos + 1;
            var a = gfMul(xv, 1);
            var b = gfMul(xv, xv);
            var ta = new Uint8Array(256);
            var tb = new Uint8Array(256);
            for (var v = 0; v < 256; v++) {
                ta[v] = gfMul(a, v);
                tb[v] = gfMul(b, v);
            }
            tables.push({ a: ta, b: tb });
        }
        tasfaGfRowTables = tables;
    }

    function gfMul(a, b) {
        if (!a || !b) return 0;
        return tasfaGfExp[tasfaGfLog[a] + tasfaGfLog[b]];
    }

    function gfDiv(a, b) {
        if (!a) return 0;
        return tasfaGfExp[tasfaGfLog[a] + 255 - tasfaGfLog[b]];
    }

    function tasfaParityChunkCount(dataChunks, fecMode) {
        var groups = Math.ceil(dataChunks / 6);
        return fecMode === 'rs2' ? 2 * groups : groups;
    }

    /* RS(6+2): two parity chunks per group of up to 6 data chunks.
       Parity chunk global index for (group g, row k) is dataChunks + 2*g + k. */
    async function generateRsParityBuffers(file, dataChunks, chunkSize, groupIndex) {
        ensureTasfaGfTables();
        var groupStart = groupIndex * 6;
        var groupEnd = Math.min(groupStart + 6, dataChunks);
        var p0 = new Uint8Array(chunkSize);
        var p1 = new Uint8Array(chunkSize);
        for (var ci = groupStart; ci < groupEnd; ci++) {
            var start = ci * chunkSize;
            var end = Math.min(start + chunkSize, file.size);
            var data = new Uint8Array(await file.slice(start, end).arrayBuffer());
            var row = tasfaGfRowTables[ci - groupStart];
            var ta = row.a;
            var tb = row.b;
            for (var i = 0; i < data.length; i++) {
                var v = data[i];
                if (v) {
                    p0[i] ^= ta[v];
                    p1[i] ^= tb[v];
                }
            }
        }
        return [p0.buffer, p1.buffer];
    }

    /* Returns { group, row } for a parity chunk global index. */
    function tasfaParityGroupRow(asset, chunkIndex, dataChunks) {
        var off = chunkIndex - dataChunks;
        if (asset.fecMode === 'rs2') return { group: Math.floor(off / 2), row: off % 2 };
        return { group: off, row: 0 };
    }

    async function generateParityChunkBuffer(asset, file, dataChunks, chunkSize, chunkIndex) {
        var pr = tasfaParityGroupRow(asset, chunkIndex, dataChunks);
        if (asset.fecMode === 'rs2') {
            var bufs = await generateRsParityBuffers(file, dataChunks, chunkSize, pr.group);
            return bufs[pr.row];
        }
        return generateParityBuffer(file, dataChunks, chunkSize, pr.group);
    }"""

    new_rs = loader + """

    /* --- GF(2^8) arithmetic (poly 0x11d) for RS(6+2) FEC via CWIST WASM --- */
    var tasfaGfLog = null;
    var tasfaGfExp = null;
    var tasfaGfRowTables = null;

    function ensureTasfaGfTables() {
        if (tasfaGfExp) return;
        var exp = __fbWasmCore.exports;
        exp.fb_cc_init();
        tasfaGfLog = __fbWasmCore.cwistView.u8(exp.fb_cc_gf_log_ptr(), exp.fb_cc_gf_log_len());
        tasfaGfExp = __fbWasmCore.cwistView.u8(exp.fb_cc_gf_exp_ptr(), exp.fb_cc_gf_exp_len());
        var tables = [];
        for (var pos = 0; pos < 6; pos++) {
            tables.push({
                a: __fbWasmCore.cwistView.u8(exp.fb_cc_gf_row_a_ptr(pos), 256),
                b: __fbWasmCore.cwistView.u8(exp.fb_cc_gf_row_b_ptr(pos), 256)
            });
        }
        tasfaGfRowTables = tables;
    }

    function gfMul(a, b) {
        return __fbWasmCore.exports.fb_cc_gf_mul(a | 0, b | 0);
    }

    function gfDiv(a, b) {
        return __fbWasmCore.exports.fb_cc_gf_div(a | 0, b | 0);
    }

    function tasfaParityChunkCount(dataChunks, fecMode) {
        return __fbWasmCore.exports.fb_cc_parity_chunk_count(dataChunks | 0, fecMode === 'rs2' ? 1 : 0);
    }

    async function generateRsParityBuffers(file, dataChunks, chunkSize, groupIndex) {
        ensureTasfaGfTables();
        var exp = __fbWasmCore.exports;
        var groupStart = groupIndex * 6;
        var groupEnd = Math.min(groupStart + 6, dataChunks);
        var p0 = new Uint8Array(chunkSize);
        var p1 = new Uint8Array(chunkSize);
        var blockCap = exp.fb_cc_chunk_in_len();
        for (var ci = groupStart; ci < groupEnd; ci++) {
            var start = ci * chunkSize;
            var end = Math.min(start + chunkSize, file.size);
            var data = new Uint8Array(await file.slice(start, end).arrayBuffer());
            /* Take views after the await: WASM memory growth detaches them. */
            var wIn = __fbWasmCore.chunkIn();
            var wP0 = __fbWasmCore.chunkP0();
            var wP1 = __fbWasmCore.chunkP1();
            for (var off = 0; off < data.length; off += blockCap) {
                var blkLen = Math.min(blockCap, data.length - off);
                wIn.set(data.subarray(off, off + blkLen), 0);
                wP0.set(p0.subarray(off, off + blkLen), 0);
                wP1.set(p1.subarray(off, off + blkLen), 0);
                exp.fb_cc_rs_accumulate_static(ci - groupStart, blkLen);
                p0.set(wP0.subarray(0, blkLen), off);
                p1.set(wP1.subarray(0, blkLen), off);
            }
        }
        return [p0.buffer, p1.buffer];
    }

    function tasfaParityGroupRow(asset, chunkIndex, dataChunks) {
        __fbWasmCore.exports.fb_cc_parity_group_row(chunkIndex | 0, dataChunks | 0, asset && asset.fecMode === 'rs2' ? 1 : 0);
        var out = __fbWasmCore.outI32();
        return { group: out[0], row: out[1] };
    }

    async function generateParityChunkBuffer(asset, file, dataChunks, chunkSize, chunkIndex) {
        var pr = tasfaParityGroupRow(asset, chunkIndex, dataChunks);
        if (asset.fecMode === 'rs2') {
            var bufs = await generateRsParityBuffers(file, dataChunks, chunkSize, pr.group);
            return bufs[pr.row];
        }
        var exp = __fbWasmCore.exports;
        var groupStart = pr.group * 6;
        var groupEnd = Math.min(groupStart + 6, dataChunks);
        var parity = new Uint8Array(chunkSize);
        var blockCap = exp.fb_cc_chunk_in_len();
        for (var ci = groupStart; ci < groupEnd; ci++) {
            var start = ci * chunkSize;
            var end = Math.min(start + chunkSize, file.size);
            var data = new Uint8Array(await file.slice(start, end).arrayBuffer());
            /* Take views after the await: WASM memory growth detaches them. */
            var wIn = __fbWasmCore.chunkIn();
            var wP0 = __fbWasmCore.chunkP0();
            for (var off = 0; off < data.length; off += blockCap) {
                var blkLen = Math.min(blockCap, data.length - off);
                wIn.set(data.subarray(off, off + blkLen), 0);
                wP0.set(parity.subarray(off, off + blkLen), 0);
                exp.fb_cc_xor_accumulate_static(blkLen);
                parity.set(wP0.subarray(0, blkLen), off);
            }
        }
        return parity.buffer;
    }"""

    assert old_rs in code, "old_rs not found in editor.js"
    code = code.replace(old_rs, new_rs, 1)

    # 2. Replace normalizeOperatorRuns & normalizeTikZOperators
    old_norm = """    /* Collapse dragged-out operators in protected LaTeX and TikZ source.
     * Unicode minus and ASCII hyphens can be mixed by pasted text. */
    function normalizeOperatorRuns(source) {
        return String(source || '')
            .replace(/={2,}/g, '=')
            .replace(/[\\u2212-]{2,}/g, '-');
    }

    /* TikZ path syntax gives meaning to operator runs that math normalizes
     * away: '--' is the segment connector. Collapsing hyphen runs destroys
     * valid TikZ paths like '-- (1,0)'. TikZ source only gets the Unicode minus
     * unified and '=' runs collapsed. */
    function normalizeTikZOperators(source) {
        return String(source || '')
            .replace(/−/g, '-')
            .replace(/={2,}/g, '=');
    }"""

    new_norm = """    function normalizeOperatorRuns(source) {
        var n = __fbWasmCore.writeUtf8(source);
        var outLen = __fbWasmCore.exports.fb_cc_normalize_operator_runs(n);
        return __fbWasmCore.readUtf8(outLen);
    }

    function normalizeTikZOperators(source) {
        var n = __fbWasmCore.writeUtf8(source);
        var outLen = __fbWasmCore.exports.fb_cc_normalize_tikz_operators(n);
        return __fbWasmCore.readUtf8(outLen);
    }"""

    assert old_norm in code, "old_norm not found in editor.js"
    code = code.replace(old_norm, new_norm, 1)

    # 3. Replace wynnEpsilonAitken, pushTasfaQualityFromMetrics, predictedTasfaQuality, predictedTasfaSignal, predictedTasfaOutlook, hasPredictableTasfaErrorPattern, predictUploadRule
    old_predict = """    function wynnEpsilonAitken(samples) {
        if (!samples || samples.length < 3) return samples && samples.length ? samples[samples.length - 1] : 0.75;
        var s0 = Number(samples[samples.length - 3]);
        var s1 = Number(samples[samples.length - 2]);
        var s2 = Number(samples[samples.length - 1]);
        var denom = s2 - (2 * s1) + s0;
        if (!Number.isFinite(denom) || Math.abs(denom) < 1e-6) return s2;
        var accelerated = s0 - (((s1 - s0) * (s1 - s0)) / denom);
        return Number.isFinite(accelerated) ? accelerated : s2;
    }

    function pushTasfaQuality(asset, value) {
        var stats = ensureTasfaStats(asset);
        stats.qualitySamples.push(clampNumber(value, 0, 1));
        if (stats.qualitySamples.length > 12) stats.qualitySamples.shift();
    }

    function pushTasfaQualityFromMetrics(asset, metrics) {
        var stats = ensureTasfaStats(asset);
        metrics = metrics || {};
        var mbps = Number(metrics.mbps || stats.ewmaMbps || 0);
        var slope = Number(stats.throughputSlope || 0);
        var inflight = Math.max(1, Number(asset.activeChunkPosts || 1));
        var maxParallel = Math.max(1, Number(asset.maxParallel || inflight));
        var silenceMs = Number(stats.progressSilenceMs || 0);
        var retryFree = Math.min(Number(stats.retryFreeStreak || 0), 8);
        var score = 0.35 + clampNumber(mbps / 40, 0, 0.35);
        score += clampNumber(slope / 25, -0.12, 0.12);
        score += clampNumber(inflight / maxParallel, 0, 1) * 0.08;
        score += retryFree * 0.015;
        if (silenceMs > 10000) score -= clampNumber((silenceMs - 10000) / 60000, 0, 0.18);
        if (metrics.failure) score -= metrics.timeout ? 0.24 : 0.12;
        pushTasfaQuality(asset, score);
    }

    function predictedTasfaQuality(asset) {
        var stats = ensureTasfaStats(asset);
        var value = wynnEpsilonAitken(stats.qualitySamples);
        return clampNumber(value, 0, 1);
    }

    function predictedTasfaSignal(asset) {
        var stats = ensureTasfaStats(asset);
        var samples = stats.qualitySamples || [];
        var value = predictedTasfaQuality(asset);
        if (samples.length < 3) return { value: value, confidence: 0.25 };
        var recent = samples.slice(-4);
        var mean = recent.reduce(function(sum, v) { return sum + Number(v || 0); }, 0) / recent.length;
        var variance = recent.reduce(function(sum, v) {
            var d = Number(v || 0) - mean;
            return sum + (d * d);
        }, 0) / recent.length;
        var confidence = clampNumber(1 - (variance * 6), 0.1, 1);
        return { value: value, confidence: confidence };
    }

    function predictedTasfaOutlook(asset, chunkIndex) {
        var stats = ensureTasfaStats(asset);
        var signal = predictedTasfaSignal(asset);
        var retryCount = asset.retryCounts ? Number(asset.retryCounts[chunkIndex] || 0) : 0;
        var timeoutRisk = clampNumber((stats.timeoutEvents || 0) / 8, 0, 0.5);
        timeoutRisk += clampNumber((stats.progressSilenceMs || 0) / 90000, 0, 0.3);
        timeoutRisk += signal.value < 0.35 ? (0.35 - signal.value) : 0;
        var fallbackNeed = clampNumber((retryCount / 24) + ((stats.sameFailureStreak || 0) / 24) + (1 - signal.value) * 0.25, 0, 1);
        var recoveryChance = clampNumber(signal.value + ((stats.retryFreeStreak || 0) * 0.04) + ((stats.throughputSlope || 0) > 0 ? 0.12 : 0), 0, 1);
        return {
            timeoutRisk: clampNumber(timeoutRisk, 0, 1),
            fallbackNeed: fallbackNeed,
            recoveryChance: recoveryChance,
            confidence: signal.confidence
        };
    }

    function hasPredictableTasfaErrorPattern(asset, chunkIndex) {
        var stats = ensureTasfaStats(asset);
        var retryCount = asset.retryCounts ? Number(asset.retryCounts[chunkIndex] || 0) : 0;
        return retryCount >= 10 || (stats.consecutiveFailures || 0) >= 10 || (stats.sameFailureStreak || 0) >= 10 || (stats.timeoutEvents || 0) >= 8;
    }

    function predictUploadRule(asset, chunkIndex) {
        var retryCount = asset.retryCounts ? Number(asset.retryCounts[chunkIndex] || 0) : 0;
        var signal = predictedTasfaSignal(asset);
        var predicted = signal.value;
        var outlook = predictedTasfaOutlook(asset, chunkIndex);
        var predictable = hasPredictableTasfaErrorPattern(asset, chunkIndex);
        if (retryCount >= 20 || (predictable && outlook.fallbackNeed >= 0.85 && predicted < 0.2 && signal.confidence >= 0.6)) return 'fallback';
        if (predictable && retryCount >= 10) return 'guarded';
        if (predictable && outlook.timeoutRisk >= 0.55 && predicted < 0.3 && signal.confidence >= 0.75) return 'guarded';
        return 'normal';
    }"""

    new_predict = """    function wynnEpsilonAitken(samples) {
        var n = __fbWasmCore.loadSamples(samples);
        return __fbWasmCore.exports.fb_cc_wynn_epsilon_aitken(n);
    }

    function pushTasfaQuality(asset, value) {
        var stats = ensureTasfaStats(asset);
        stats.qualitySamples.push(clampNumber(value, 0, 1));
        if (stats.qualitySamples.length > 12) stats.qualitySamples.shift();
    }

    function pushTasfaQualityFromMetrics(asset, metrics) {
        var stats = ensureTasfaStats(asset);
        metrics = metrics || {};
        var score = __fbWasmCore.exports.fb_cc_quality_from_metrics(
            Number(metrics.mbps || stats.ewmaMbps || 0),
            Number(stats.throughputSlope || 0),
            Number(asset.activeChunkPosts || 1) | 0,
            Number(asset.maxParallel || asset.activeChunkPosts || 1) | 0,
            Number(stats.progressSilenceMs || 0),
            Number(stats.retryFreeStreak || 0) | 0,
            metrics.failure ? 1 : 0,
            metrics.timeout ? 1 : 0
        );
        pushTasfaQuality(asset, score);
    }

    function predictedTasfaQuality(asset) {
        var stats = ensureTasfaStats(asset);
        var n = __fbWasmCore.loadSamples(stats.qualitySamples);
        return __fbWasmCore.exports.fb_cc_predicted_quality(n);
    }

    function predictedTasfaSignal(asset) {
        var stats = ensureTasfaStats(asset);
        var n = __fbWasmCore.loadSamples(stats.qualitySamples);
        __fbWasmCore.exports.fb_cc_predicted_signal(n);
        var out = __fbWasmCore.outF64();
        return { value: out[0], confidence: out[1] };
    }

    function predictedTasfaOutlook(asset, chunkIndex) {
        var stats = ensureTasfaStats(asset);
        var retryCount = asset.retryCounts ? Number(asset.retryCounts[chunkIndex] || 0) : 0;
        var n = __fbWasmCore.loadSamples(stats.qualitySamples);
        __fbWasmCore.exports.fb_cc_predict_upload_rule(
            n, retryCount | 0, Number(stats.consecutiveFailures || 0) | 0,
            Number(stats.sameFailureStreak || 0) | 0, Number(stats.timeoutEvents || 0) | 0,
            Number(stats.progressSilenceMs || 0), Number(stats.retryFreeStreak || 0) | 0,
            Number(stats.throughputSlope || 0)
        );
        var out = __fbWasmCore.outF64();
        return {
            timeoutRisk: out[0],
            fallbackNeed: out[1],
            recoveryChance: out[2],
            confidence: out[3]
        };
    }

    function hasPredictableTasfaErrorPattern(asset, chunkIndex) {
        var stats = ensureTasfaStats(asset);
        var retryCount = asset.retryCounts ? Number(asset.retryCounts[chunkIndex] || 0) : 0;
        return retryCount >= 10 || (stats.consecutiveFailures || 0) >= 10 || (stats.sameFailureStreak || 0) >= 10 || (stats.timeoutEvents || 0) >= 8;
    }

    function predictUploadRule(asset, chunkIndex) {
        var stats = ensureTasfaStats(asset);
        var retryCount = asset.retryCounts ? Number(asset.retryCounts[chunkIndex] || 0) : 0;
        var n = __fbWasmCore.loadSamples(stats.qualitySamples);
        var code = __fbWasmCore.exports.fb_cc_predict_upload_rule(
            n, retryCount | 0, Number(stats.consecutiveFailures || 0) | 0,
            Number(stats.sameFailureStreak || 0) | 0, Number(stats.timeoutEvents || 0) | 0,
            Number(stats.progressSilenceMs || 0), Number(stats.retryFreeStreak || 0) | 0,
            Number(stats.throughputSlope || 0)
        );
        return code === 2 ? 'fallback' : (code === 1 ? 'guarded' : 'normal');
    }"""

    assert old_predict in code, "old_predict not found in editor.js"
    code = code.replace(old_predict, new_predict, 1)

    # 4. Replace recordTasfaSuccess AIMD ramp with WASM kernel call
    old_ramp = """    function recordTasfaSuccess(asset, bytes, durationMs) {
        maybeTuneUploadChunkHint(asset, true, durationMs, bytes);
        updateBdpGovernor(asset, bytes, durationMs);
        if (asset.maxParallel && (asset.targetParallel || 1) < asset.maxParallel) {
            var stats = ensureTasfaStats(asset);
            // Ramp up on every success after aggressive restart, taper gently
            var mult = Date.now() < (stats.fastRecoveryUntil || 0) ? 1.3 : 1.15;
            // Preserve multiplicative growth, but always advance at low worker counts.
            asset.targetParallel = Math.min(asset.maxParallel, Math.max((asset.targetParallel || 1) + 1, Math.round((asset.targetParallel || 1) * mult)));
            tasfaTrace(asset, 'success-ramp', { durationMs: Math.round(durationMs || 0), mbps: bytes && durationMs ? (((bytes * 8) / durationMs / 1000).toFixed(2)) : '0' });
            scheduleUploadRenegotiate(asset, false);
        }
    }"""

    new_ramp = """    function recordTasfaSuccess(asset, bytes, durationMs) {
        maybeTuneUploadChunkHint(asset, true, durationMs, bytes);
        updateBdpGovernor(asset, bytes, durationMs);
        if (asset.maxParallel && (asset.targetParallel || 1) < asset.maxParallel) {
            var stats = ensureTasfaStats(asset);
            var inFast = Date.now() < (stats.fastRecoveryUntil || 0) ? 1 : 0;
            asset.targetParallel = __fbWasmCore.exports.fb_cc_upload_aimd_ramp(
                (asset.targetParallel || 1) | 0, asset.maxParallel | 0, inFast
            );
            tasfaTrace(asset, 'success-ramp', { durationMs: Math.round(durationMs || 0), mbps: bytes && durationMs ? (((bytes * 8) / durationMs / 1000).toFixed(2)) : '0' });
            scheduleUploadRenegotiate(asset, false);
        }
    }"""

    assert old_ramp in code, "old_ramp not found in editor.js"
    code = code.replace(old_ramp, new_ramp, 1)

    # 5. Replace updateMetrics word count & reading time with WASM kernel call
    old_metrics = """    function updateMetrics() {
        if (!ta) return;
        var words = ta.value.trim() ? ta.value.trim().split(/\\s+/).length : 0;
        var minutes = words ? Math.max(1, Math.ceil(words / 220)) : 0;
        if (wordCount) wordCount.textContent = words + ' words';
        if (readingTime) readingTime.textContent = minutes + ' min read';
    }"""

    new_metrics = """    function updateMetrics() {
        if (!ta) return;
        var n = __fbWasmCore.writeUtf8(ta.value);
        __fbWasmCore.exports.fb_cc_count_words_and_reading_time(n);
        var out = __fbWasmCore.outI32();
        var words = out[0];
        var minutes = out[1];
        if (wordCount) wordCount.textContent = words + ' words';
        if (readingTime) readingTime.textContent = minutes + ' min read';
    }"""

    assert old_metrics in code, "old_metrics not found in editor.js"
    code = code.replace(old_metrics, new_metrics, 1)

    # 6. Replace generateParityBuffer XOR parity with WASM kernel call
    old_xor = """    async function generateParityBuffer(file, dataChunks, chunkSize, groupIndex) {
        var groupStart = groupIndex * 6;
        var groupEnd = Math.min(groupStart + 6, dataChunks);
        var parity = new Uint8Array(chunkSize);
        for (var ci = groupStart; ci < groupEnd; ci++) {
            var start = ci * chunkSize;
            var end = Math.min(start + chunkSize, file.size);
            var data = new Uint8Array(await file.slice(start, end).arrayBuffer());
            for (var i = 0; i < data.length; i++) {
                parity[i] ^= data[i];
            }
        }
        return parity.buffer;
    }"""

    new_xor = """    async function generateParityBuffer(file, dataChunks, chunkSize, groupIndex) {
        var exp = __fbWasmCore.exports;
        var groupStart = groupIndex * 6;
        var groupEnd = Math.min(groupStart + 6, dataChunks);
        var parity = new Uint8Array(chunkSize);
        var blockCap = exp.fb_cc_chunk_in_len();
        for (var ci = groupStart; ci < groupEnd; ci++) {
            var start = ci * chunkSize;
            var end = Math.min(start + chunkSize, file.size);
            var data = new Uint8Array(await file.slice(start, end).arrayBuffer());
            /* Take views after the await: WASM memory growth detaches them. */
            var wIn = __fbWasmCore.chunkIn();
            var wP0 = __fbWasmCore.chunkP0();
            for (var off = 0; off < data.length; off += blockCap) {
                var blkLen = Math.min(blockCap, data.length - off);
                wIn.set(data.subarray(off, off + blkLen), 0);
                wP0.set(parity.subarray(off, off + blkLen), 0);
                exp.fb_cc_xor_accumulate_static(blkLen);
                parity.set(wP0.subarray(0, blkLen), off);
            }
        }
        return parity.buffer;
    }"""

    assert old_xor in code, "old_xor not found in editor.js"
    code = code.replace(old_xor, new_xor, 1)

    with open(dst_path, "w", encoding="utf-8") as fp:
        fp.write(code)
    print(f"Patched {dst_path}")


def patch_tasfa_download_js(loader):
    src_path = os.path.join(ROOT, "tests", "js_baseline", "tasfa-download.js")
    dst_path = os.path.join(ROOT, "public", "js", "tasfa-download.js")
    with open(src_path, "r", encoding="utf-8") as fp:
        code = fp.read()

    old_dl_governor = """    function wynnEpsilonAitken(samples) {
        if (!samples || samples.length < 3) return samples && samples.length ? samples[samples.length - 1] : 0.75;
        var s0 = Number(samples[samples.length - 3]);
        var s1 = Number(samples[samples.length - 2]);
        var s2 = Number(samples[samples.length - 1]);
        var denom = s2 - (2 * s1) + s0;
        if (!Number.isFinite(denom) || Math.abs(denom) < 1e-6) return s2;
        var accelerated = s0 - (((s1 - s0) * (s1 - s0)) / denom);
        return Number.isFinite(accelerated) ? accelerated : s2;
    }

    function pushDownloadQuality(session, value) {
        session.qualitySamples = session.qualitySamples || [];
        session.qualitySamples.push(clampNumber(value, 0, 1));
        if (session.qualitySamples.length > 12) session.qualitySamples.shift();
    }

    function predictedDownloadQuality(session) {
        return clampNumber(wynnEpsilonAitken(session.qualitySamples || []), 0, 1);
    }

    function applyPredictedDownloadRule(session) {
        var ultraFastMedia = !!(session && session.largeMedia && session.ultraFastConnection);
        if (session && session.largeMedia) {
            var mediaFloor = ultraFastMedia ? Math.min(session.maxParallel || 1, isLikelyMobile() ? 3 : 4) : Math.min(session.maxParallel || 1, isLikelyMobile() ? 8 : 10);
            if ((session.targetParallel || 1) < mediaFloor) session.targetParallel = mediaFloor;
            if (ultraFastMedia) session.currentSpan = Math.min(session.currentSpan || 1, 1);
            else if ((session.currentSpan || 1) < 2) session.currentSpan = 2;
        }
        var predicted = predictedDownloadQuality(session);
        if (predicted < 0.25) {
            var lowFloor = Math.min(session.maxParallel || 1, isLikelyMobile() ? 2 : 4);
            if (session && session.largeMedia) lowFloor = ultraFastMedia ? Math.min(session.maxParallel || 1, isLikelyMobile() ? 2 : 3) : Math.min(session.maxParallel || 1, isLikelyMobile() ? 6 : 8);
            session.currentSpan = Math.max(1, Math.min(session.currentSpan || 1, 2));
            session.targetParallel = Math.max(lowFloor, Math.min(session.targetParallel || 1, Math.ceil((session.maxParallel || 1) / 3)));
        } else if (predicted < 0.45) {
            var guardedFloor = Math.min(session.maxParallel || 1, isLikelyMobile() ? 3 : 6);
            if (session && session.largeMedia) guardedFloor = ultraFastMedia ? Math.min(session.maxParallel || 1, isLikelyMobile() ? 3 : 4) : Math.min(session.maxParallel || 1, isLikelyMobile() ? 8 : 10);
            session.currentSpan = Math.max(1, Math.min(session.currentSpan || 1, 2));
            session.targetParallel = Math.max(guardedFloor, Math.min(session.targetParallel || 1, Math.ceil((session.maxParallel || 1) / 2)));
        }
    }

    function tuneDownloadSuccess(session, bytes, durationMs) {
        session.successEvents = (session.successEvents || 0) + 1;
        session.fastStreak = (session.fastStreak || 0) + 1;
        var mbps = durationMs > 0 ? ((bytes * 8) / durationMs / 1000) : 0;
        session.ewmaMbps = session.ewmaMbps ? (session.ewmaMbps * 0.75 + mbps * 0.25) : mbps;
        var demoteMbps = session.fastLinkTier === 'ultra' ? 250 : 150;
        if (session.largeMedia && session.ultraFastConnection && session.successEvents >= 2 && session.ewmaMbps > 0 && session.ewmaMbps < demoteMbps) {
            demoteUltraFastSession(session);
        }
        pushDownloadQuality(session, clampNumber(mbps / 25, 0.1, 1));
        if (!(session.largeMedia && session.ultraFastConnection) && session.fastStreak >= 6 && (session.ewmaMbps >= 25 || durationMs < 10000)) {
            rememberDownloadChunkSize(preferredDownloadChunkSize() * 2);
            session.fastStreak = 0;
        }
        if (session.largeMedia && session.ultraFastConnection) {
            session.currentSpan = Math.min(session.currentSpan || 1, 1);
            session.targetParallel = Math.min(session.targetParallel || 1, isLikelyMobile() ? 3 : 4);
        } else if (session.currentSpan < session.maxSpan && session.successEvents % 2 === 0) {
            // Integer rounding must not trap slow-start or recovering sessions at 1 or 2.
            session.currentSpan = Math.min(session.maxSpan, Math.max(session.currentSpan + 1, Math.round(session.currentSpan * 1.2)));
        }
        if (session.targetParallel < session.maxParallel && session.successEvents % 3 === 0) {
            session.targetParallel = Math.min(session.maxParallel, Math.max(session.targetParallel + 1, Math.round(session.targetParallel * 1.2)));
        }
    }

    function tuneDownloadFailure(session, kind) {
        session.failureEvents = (session.failureEvents || 0) + 1;
        session.fastStreak = 0;
        var largeMedia = !!(session && session.largeMedia);
        var repeatedFailure = session.failureEvents >= (largeMedia ? 4 : 1);
        if (repeatedFailure) {
            session.currentSpan = Math.max(1, Math.round(session.currentSpan * (largeMedia ? 0.9 : 0.8)));
        }
        pushDownloadQuality(session, kind === 'timeout' ? (largeMedia ? 0.18 : 0.05) : (largeMedia ? 0.25 : 0.15));
        if (!largeMedia || session.failureEvents % 3 === 0) {
            rememberDownloadChunkSize(preferredDownloadChunkSize() / 2);
        }
        var floor = Math.min(session.maxParallel || 1, isLikelyMobile() ? (largeMedia ? 6 : 2) : (largeMedia ? 8 : 4));
        if (repeatedFailure && (kind === 'timeout' || session.failureEvents % 4 === 0) && session.targetParallel > floor) {
            session.targetParallel = Math.max(floor, Math.round(session.targetParallel * (largeMedia ? 0.9 : 0.8)));
        }
    }"""

    new_dl_governor = loader + """

    function wynnEpsilonAitken(samples) {
        var n = __fbWasmCore.loadSamples(samples);
        return __fbWasmCore.exports.fb_cc_wynn_epsilon_aitken(n);
    }

    function pushDownloadQuality(session, value) {
        session.qualitySamples = session.qualitySamples || [];
        session.qualitySamples.push(clampNumber(value, 0, 1));
        if (session.qualitySamples.length > 12) session.qualitySamples.shift();
    }

    function predictedDownloadQuality(session) {
        var n = __fbWasmCore.loadSamples(session.qualitySamples || []);
        return __fbWasmCore.exports.fb_cc_predicted_quality(n);
    }

    function applyPredictedDownloadRule(session) {
        if (!session) return;
        var predicted = predictedDownloadQuality(session);
        __fbWasmCore.exports.fb_cc_apply_predicted_dl_rule(
            session.largeMedia ? 1 : 0,
            session.ultraFastConnection ? 1 : 0,
            isLikelyMobile() ? 1 : 0,
            (session.maxParallel || 1) | 0,
            predicted,
            (session.targetParallel || 1) | 0,
            (session.currentSpan || 1) | 0
        );
        var out = __fbWasmCore.outI32();
        session.targetParallel = out[0];
        session.currentSpan = out[1];
    }

    function tuneDownloadSuccess(session, bytes, durationMs) {
        __fbWasmCore.exports.fb_cc_tune_dl_success(
            (session.successEvents || 0) | 0,
            (session.fastStreak || 0) | 0,
            Number(session.ewmaMbps || 0),
            session.largeMedia ? 1 : 0,
            session.ultraFastConnection ? 1 : 0,
            session.fastLinkTier === 'ultra' ? 1 : 0,
            isLikelyMobile() ? 1 : 0,
            (session.currentSpan || 0) | 0,
            (session.maxSpan || 0) | 0,
            (session.targetParallel || 0) | 0,
            (session.maxParallel || 0) | 0,
            (session.standardMaxParallel || 0) | 0,
            (session.standardTargetParallel || 0) | 0,
            (session.standardCurrentSpan || 0) | 0,
            (session.standardMaxSpan || 0) | 0,
            Number(bytes || 0),
            Number(durationMs || 0)
        );
        var oi = __fbWasmCore.outI32();
        var of = __fbWasmCore.outF64();
        session.successEvents = oi[0];
        session.fastStreak = oi[1];
        session.ewmaMbps = of[0];
        if (oi[2]) {
            demoteUltraFastSession(session);
        }
        pushDownloadQuality(session, of[1]);
        if (oi[3]) {
            rememberDownloadChunkSize(preferredDownloadChunkSize() * 2);
        }
        session.currentSpan = oi[4];
        session.targetParallel = oi[5];
    }

    function tuneDownloadFailure(session, kind) {
        __fbWasmCore.exports.fb_cc_tune_dl_failure(
            (session.failureEvents || 0) | 0,
            session.largeMedia ? 1 : 0,
            isLikelyMobile() ? 1 : 0,
            kind === 'timeout' ? 1 : 0,
            (session.currentSpan || 0) | 0,
            (session.targetParallel || 0) | 0,
            (session.maxParallel || 1) | 0
        );
        var oi = __fbWasmCore.outI32();
        var of = __fbWasmCore.outF64();
        session.failureEvents = oi[0];
        session.fastStreak = oi[1];
        session.currentSpan = oi[2];
        session.targetParallel = oi[3];
        pushDownloadQuality(session, of[0]);
        if (oi[4]) {
            rememberDownloadChunkSize(preferredDownloadChunkSize() / 2);
        }
    }"""

    assert old_dl_governor in code, "old_dl_governor not found in tasfa-download.js"
    code = code.replace(old_dl_governor, new_dl_governor, 1)

    old_norm_dl = """    function normalizeDownloadSession(session) {
        if (!session) return null;
        var chunkCount = Math.max(1, Number(session.chunk_count) || 1);
        var hw = Math.max(1, Number(navigator.hardwareConcurrency) || 4);
        var chunkSize = Math.max(1, Number(session.chunk_size) || 1);
        var maxParallel = Math.max(1, Math.min(Number(session.max_parallel_chunks) || hw * 3, chunkCount));
        var initialParallel = Math.max(1, Math.min(Number(session.initial_parallel_chunks) || maxParallel, maxParallel));
        var coalesce = Math.max(1, Math.min(Number(session.coalesce_chunks) || 8, 64));
        var maxSpan = Math.max(1, Math.min(64, Math.floor(DOWNLOAD_REQUEST_BYTES_MAX / chunkSize) || 1));
        var standardMaxParallel = maxParallel;
        var standardInitialParallel = initialParallel;
        var standardCoalesce = coalesce;
        var standardMaxSpan = maxSpan;
        var mimeType = session.mime_type || 'application/octet-stream';
        var totalSize = Math.max(0, Number(session.total_size) || 0);
        var largeMedia = /^(video|audio)\\//i.test(mimeType) && totalSize >= 64 * 1024 * 1024;
        var tier = fastLinkTier();
        var ultraFastConnection = !!tier;
        if (largeMedia && ultraFastConnection) {
            maxParallel = Math.min(maxParallel, isLikelyMobile() ? 3 : 4);
            initialParallel = Math.min(initialParallel, maxParallel);
            coalesce = 1;
            maxSpan = 1;
        }
        return {
            sessionId: session.session_id,
            sessionToken: session.session_token,
            streamKeyHex: session.stream_key_hex,
            streamIvSeedHex: session.stream_iv_seed_hex,
            streamMode: session.stream_mode,
            supportsProgressiveStreaming: session.supports_progressive_streaming !== false,
            chunkSize: chunkSize,
            chunkCount: chunkCount,
            totalSize: totalSize,
            mimeType: mimeType,
            filename: session.filename || 'download',
            maxParallel: maxParallel,
            targetParallel: initialParallel,
            coalesceChunks: Math.min(coalesce, maxSpan),
            currentSpan: Math.min(coalesce, maxSpan),
            maxSpan: maxSpan,
            successEvents: 0,
            failureEvents: 0,
            fastStreak: 0,
            ewmaMbps: 0,
            qualitySamples: [],
            largeMedia: largeMedia,
            ultraFastConnection: ultraFastConnection,
            fastLinkTier: tier,
            standardMaxParallel: standardMaxParallel,
            standardTargetParallel: standardInitialParallel,
            standardCoalesceChunks: Math.min(standardCoalesce, standardMaxSpan),
            standardCurrentSpan: Math.min(standardCoalesce, standardMaxSpan),
            standardMaxSpan: standardMaxSpan
        };
    }"""

    new_norm_dl = """    function normalizeDownloadSession(session) {
        if (!session) return null;
        var chunkCount = Math.max(1, Number(session.chunk_count) || 1);
        var hw = Math.max(1, Number(navigator.hardwareConcurrency) || 4);
        var chunkSize = Math.max(1, Number(session.chunk_size) || 1);
        var mimeType = session.mime_type || 'application/octet-stream';
        var totalSize = Math.max(0, Number(session.total_size) || 0);
        var isMediaMime = /^(video|audio)\\//i.test(mimeType) ? 1 : 0;
        var tier = fastLinkTier();
        var tierCode = tier === 'ultra' ? 2 : (tier === 'fast' ? 1 : 0);
        __fbWasmCore.exports.fb_cc_normalize_dl_session(
            chunkCount | 0,
            hw | 0,
            chunkSize,
            (Number(session.max_parallel_chunks) || 0) | 0,
            (Number(session.initial_parallel_chunks) || 0) | 0,
            (Number(session.coalesce_chunks) || 0) | 0,
            totalSize,
            isMediaMime,
            tierCode,
            isLikelyMobile() ? 1 : 0
        );
        var oi = __fbWasmCore.outI32();
        return {
            sessionId: session.session_id,
            sessionToken: session.session_token,
            streamKeyHex: session.stream_key_hex,
            streamIvSeedHex: session.stream_iv_seed_hex,
            streamMode: session.stream_mode,
            supportsProgressiveStreaming: session.supports_progressive_streaming !== false,
            chunkSize: chunkSize,
            chunkCount: chunkCount,
            totalSize: totalSize,
            mimeType: mimeType,
            filename: session.filename || 'download',
            maxParallel: oi[0],
            targetParallel: oi[1],
            coalesceChunks: oi[2],
            currentSpan: oi[3],
            maxSpan: oi[4],
            successEvents: 0,
            failureEvents: 0,
            fastStreak: 0,
            ewmaMbps: 0,
            qualitySamples: [],
            largeMedia: !!oi[10],
            ultraFastConnection: !!oi[11],
            fastLinkTier: tier,
            standardMaxParallel: oi[5],
            standardTargetParallel: oi[6],
            standardCoalesceChunks: oi[7],
            standardCurrentSpan: oi[8],
            standardMaxSpan: oi[9]
        };
    }"""

    assert old_norm_dl in code, "old_norm_dl not found in tasfa-download.js"
    code = code.replace(old_norm_dl, new_norm_dl, 1)

    with open(dst_path, "w", encoding="utf-8") as fp:
        fp.write(code)
    print(f"Patched {dst_path}")


def patch_post_translate_js(loader):
    src_path = os.path.join(ROOT, "tests", "js_baseline", "post-translate.js")
    dst_path = os.path.join(ROOT, "public", "js", "post-translate.js")
    with open(src_path, "r", encoding="utf-8") as fp:
        code = fp.read()

    old_lang = """    function detectSourceLanguage(text) {
        if (/[\\uac00-\\ud7a3]/.test(text)) return 'ko';
        if (/[\\u3040-\\u30ff]/.test(text)) return 'ja';
        if (/[\\u4e00-\\u9fff]/.test(text)) return 'zh';
        if (/[\\u0400-\\u04ff]/.test(text)) return 'ru';
        if (/[\\u0600-\\u06ff]/.test(text)) return 'ar';
        if (/[\\u0900-\\u097f]/.test(text)) return 'hi';
        return 'en';
    }"""

    new_lang = loader + """

    var __fbLangCodes = ['en', 'ko', 'ja', 'zh', 'ru', 'ar', 'hi'];

    function detectSourceLanguage(text) {
        var n = __fbWasmCore.writeUtf8(text);
        var idx = __fbWasmCore.exports.fb_cc_detect_source_language(n);
        return __fbLangCodes[idx] || 'en';
    }"""

    assert old_lang in code, "old_lang not found in post-translate.js"
    code = code.replace(old_lang, new_lang, 1)

    old_invalid = """    function isInvalidTranslationText(text) {
        if (!text || typeof text !== 'string') return true;
        var upper = text.toUpperCase();
        if (upper.indexOf('INVALID LANGUAGE PAIR') !== -1) return true;
        if (upper.indexOf('INVALID TARGET LANGUAGE') !== -1) return true;
        if (upper.indexOf('INVALID SOURCE LANGUAGE') !== -1) return true;
        if (upper.indexOf('MYMEMORY WARNING') !== -1) return true;
        if (upper.indexOf('QUERY LENGTH LIMIT') !== -1) return true;
        if (upper.indexOf('NO QUERY SPECIFIED') !== -1) return true;
        if (upper.indexOf('PLEASE SELECT') !== -1) return true;
        if (upper.indexOf('INVALID ') === 0) return true;
        return false;
    }"""

    new_invalid = """    function isInvalidTranslationText(text) {
        if (!text || typeof text !== 'string') return true;
        /* Uppercase in JS so non-ASCII folds (e.g. U+0131, U+017F) match exactly. */
        var n = __fbWasmCore.writeUtf8(text.toUpperCase());
        return __fbWasmCore.exports.fb_cc_is_invalid_translation_text(n) !== 0;
    }"""

    assert old_invalid in code, "old_invalid not found in post-translate.js"
    code = code.replace(old_invalid, new_invalid, 1)

    with open(dst_path, "w", encoding="utf-8") as fp:
        fp.write(code)
    print(f"Patched {dst_path}")


def main():
    if not os.path.exists(WASM_PATH):
        sys.exit(f"Missing {WASM_PATH}")
    with open(WASM_PATH, "rb") as fp:
        b64 = base64.b64encode(fp.read()).decode("ascii")
    loader = make_inline_loader_js(b64)
    patch_editor_js(loader)
    patch_tasfa_download_js(loader)
    patch_post_translate_js(loader)


if __name__ == "__main__":
    main()

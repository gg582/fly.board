'use strict';

// Run: node --test tests/test_wasm_js_baseline_parity.cjs
// Verifies 100% functional equivalence between the archived pure-JS baseline
// in tests/js_baseline/ and the shipped CWIST WASM-backed scripts in public/js/.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const test = require('node:test');

const ROOT = path.join(__dirname, '..');

// Objects returned from vm sandboxes carry their own realm's prototypes, so
// compare them as plain data.
const plain = (v) => JSON.parse(JSON.stringify(v));
const BASELINE_DIR = path.join(ROOT, 'tests', 'js_baseline');
const SHIPPED_DIR = path.join(ROOT, 'public', 'js');

function loadEditorEnv(dir, mobile = false) {
    const wordCountEl = { textContent: '' };
    const readingTimeEl = { textContent: '' };
    const taEl = { value: '', closest: () => null, addEventListener() {} };
    const sandbox = {
        window: {},
        document: {
            readyState: 'complete',
            getElementById: (id) => {
                if (id === 'md-editor') return taEl;
                if (id === 'editor-word-count') return wordCountEl;
                if (id === 'editor-reading-time') return readingTimeEl;
                return null;
            },
            querySelectorAll: () => [],
        },
        navigator: { userAgent: mobile ? 'Android' : '', hardwareConcurrency: 4 },
        localStorage: { getItem: () => null, setItem() {} },
        setTimeout: () => 1,
        clearTimeout() {},
        console,
        TextEncoder,
        TextDecoder,
        WebAssembly,
    };
    const source = fs.readFileSync(path.join(dir, 'editor.js'), 'utf8');
    const anchor = "    var draftKey = 'flyboard:draft:' + location.pathname;";
    assert.ok(source.includes(anchor), `anchor must exist in ${dir}/editor.js`);
    const exportsExpr = `{
        ensureTasfaGfTables, gfMul, gfDiv, tasfaParityChunkCount,
        generateRsParityBuffers, tasfaParityGroupRow, generateParityChunkBuffer,
        normalizeOperatorRuns, normalizeTikZOperators,
        ensureTasfaStats, wynnEpsilonAitken, pushTasfaQuality,
        pushTasfaQualityFromMetrics, predictedTasfaQuality,
        predictedTasfaSignal, predictedTasfaOutlook,
        hasPredictableTasfaErrorPattern, predictUploadRule,
        recordTasfaSuccess, updateMetrics,
        setEditorValue: function(v) { ta.value = v; updateMetrics(); return { words: wordCount.textContent, time: readingTime.textContent }; }
    }`;
    const replacement = `window.api = ${exportsExpr}; return;`;
    vm.runInNewContext(source.replace(anchor, replacement), sandbox);
    return sandbox.window.api;
}

function loadDownloadEnv(dir, mobile = false, tierConn = null) {
    const store = new Map();
    const sandbox = {
        window: {},
        document: { readyState: 'complete', getElementById: () => null, querySelectorAll: () => [] },
        navigator: {
            userAgent: mobile ? 'iPhone' : '',
            hardwareConcurrency: 6,
            connection: tierConn || undefined,
        },
        localStorage: {
            getItem: (k) => store.get(k) || null,
            setItem: (k, v) => store.set(k, String(v)),
        },
        setTimeout: () => 1,
        clearTimeout() {},
        console,
        TextEncoder,
        TextDecoder,
        WebAssembly,
    };
    const source = fs.readFileSync(path.join(dir, 'tasfa-download.js'), 'utf8');
    const anchor = '    function init() {';
    assert.ok(source.includes(anchor), `anchor must exist in ${dir}/tasfa-download.js`);
    const exportsExpr = `{
        wynnEpsilonAitken, pushDownloadQuality, predictedDownloadQuality,
        applyPredictedDownloadRule, tuneDownloadSuccess, tuneDownloadFailure,
        normalizeDownloadSession, preferredDownloadChunkSize
    }`;
    const replacement = `${anchor} window.api = ${exportsExpr}; return;`;
    vm.runInNewContext(source.replace(anchor, replacement), sandbox);
    return sandbox.window.api;
}

function loadTranslateEnv(dir) {
    const button = { setAttribute() {}, addEventListener() {}, dataset: {} };
    const select = { options: [], addEventListener() {}, value: 'en' };
    const sourceEl = { childNodes: [] };
    const status = { textContent: '' };
    const ids = {
        'post-translate': button,
        'post-translate-target': select,
        'translation-msg': status,
    };
    const sandbox = {
        window: { fetch: true },
        document: {
            getElementById: (id) => ids[id] || null,
            querySelector: (sel) => (sel === 'article .markdown-body' ? sourceEl : null),
        },
        Node: { ELEMENT_NODE: 1 },
        AbortController,
        setTimeout: () => 1,
        clearTimeout() {},
        console,
        TextEncoder,
        TextDecoder,
        WebAssembly,
    };
    const source = fs.readFileSync(path.join(dir, 'post-translate.js'), 'utf8');
    const anchor = '    var clientTranslateCache = {};';
    assert.ok(source.includes(anchor), `anchor must exist in ${dir}/post-translate.js`);
    const replacement = `${anchor} window.api = { detectSourceLanguage, isInvalidTranslationText }; return;`;
    vm.runInNewContext(source.replace(anchor, replacement), sandbox);
    return sandbox.window.api;
}

test('100% parity: GF(2^8) multiplication, division, and row tables across all 65,536 byte pairs', () => {
    const base = loadEditorEnv(BASELINE_DIR);
    const wasm = loadEditorEnv(SHIPPED_DIR);
    base.ensureTasfaGfTables();
    wasm.ensureTasfaGfTables();

    for (let a = 0; a < 256; a++) {
        for (let b = 0; b < 256; b++) {
            assert.equal(wasm.gfMul(a, b), base.gfMul(a, b), `gfMul(${a}, ${b})`);
            if (b !== 0) {
                assert.equal(wasm.gfDiv(a, b), base.gfDiv(a, b), `gfDiv(${a}, ${b})`);
            }
        }
    }
});

test('100% parity: RS(6+2) and XOR1 parity chunk counts, group/row mapping, and buffer encoding', async () => {
    const base = loadEditorEnv(BASELINE_DIR);
    const wasm = loadEditorEnv(SHIPPED_DIR);

    for (let d = 1; d <= 64; d++) {
        for (const mode of ['rs2', 'xor1']) {
            const bCount = base.tasfaParityChunkCount(d, mode);
            const wCount = wasm.tasfaParityChunkCount(d, mode);
            assert.equal(wCount, bCount, `parityCount d=${d} mode=${mode}`);
            for (let ci = d; ci < d + bCount; ci++) {
                assert.deepEqual(
                    plain(wasm.tasfaParityGroupRow({ fecMode: mode }, ci, d)),
                    plain(base.tasfaParityGroupRow({ fecMode: mode }, ci, d)),
                    `groupRow d=${d} ci=${ci} mode=${mode}`
                );
            }
        }
    }

    // Test both small chunks and multi-block (>64KB) chunks with partial tail
    for (const [chunkSize, dataChunks, tailBytes] of [
        [2048, 6, 2048],
        [4096, 4, 513],
        [70000, 3, 12345],
        [200000, 7, 1],
        [65536, 6, 65536],
    ]) {
        const totalSize = (dataChunks - 1) * chunkSize + tailBytes;
        const raw = new Uint8Array(totalSize);
        let s = 0x12345678;
        for (let i = 0; i < totalSize; i++) {
            s = (Math.imul(s, 1664525) + 1013904223) >>> 0;
            raw[i] = (s >>> 24) & 0xff;
        }
        const blob = new Blob([raw.buffer]);
        const bRs = await base.generateRsParityBuffers(blob, dataChunks, chunkSize, 0);
        const wRs = await wasm.generateRsParityBuffers(blob, dataChunks, chunkSize, 0);
        assert.deepEqual(new Uint8Array(wRs[0]), new Uint8Array(bRs[0]), `RS P0 size=${chunkSize}`);
        assert.deepEqual(new Uint8Array(wRs[1]), new Uint8Array(bRs[1]), `RS P1 size=${chunkSize}`);

        // The baseline's xor1 path is broken: generateParityChunkBuffer calls
        // generateParityBuffer, which is declared inside runSimpleChunkUpload
        // and so throws ReferenceError. Pin that, and check the WASM path
        // against a direct XOR of the group instead.
        await assert.rejects(
            base.generateParityChunkBuffer({ fecMode: 'xor1' }, blob, dataChunks, chunkSize, dataChunks),
            (err) => err.name === 'ReferenceError'
        );
        const expectXor = new Uint8Array(chunkSize);
        for (let ci = 0; ci < Math.min(6, dataChunks); ci++) {
            const data = raw.subarray(ci * chunkSize, Math.min((ci + 1) * chunkSize, totalSize));
            for (let i = 0; i < data.length; i++) expectXor[i] ^= data[i];
        }
        const wXor = await wasm.generateParityChunkBuffer({ fecMode: 'xor1' }, blob, dataChunks, chunkSize, dataChunks);
        assert.deepEqual(new Uint8Array(wXor), expectXor, `XOR parity size=${chunkSize}`);
    }
});

test('100% parity: Editor LaTeX/TikZ operator normalization and word/reading-time metrics', () => {
    const base = loadEditorEnv(BASELINE_DIR);
    const wasm = loadEditorEnv(SHIPPED_DIR);

    const samples = [
        '',
        'a == b === c ==== d',
        'x -- y --- z \u2212\u2212 w -\u2212- q - single',
        '\\draw (0,0) -- (1,0) === (2,0) \u2212 (3,0) \u2212\u2212 (4,0);',
        '한국어 수식 $a === b \u2212\u2212 c$ 그리고 단일 - 기호와 \u2212 기호',
        Array.from({ length: 500 }, (_, i) => `word${i}`).join('   \n\t '),
        // Every ECMAScript WhiteSpace/LineTerminator that /\s/ and trim() honour.
        '\u3000가\u00a0나\u1680다\u2000라\u200a마\u2028바\u2029사\u202f아\u205f자\ufeff차\u3000',
        'zero\u200bwidth\u180espace are not whitespace',
        'emoji 😀😀 and lone \ud800 surrogate --- x',
        // Larger than the initial 128 KiB WASM text buffer: must not truncate.
        Array.from({ length: 40000 }, (_, i) => `단어${i} a==b c\u2212\u2212d`).join(' '),
    ];

    for (const sample of samples) {
        assert.equal(wasm.normalizeOperatorRuns(sample), base.normalizeOperatorRuns(sample), `opRuns: ${sample.slice(0, 30)}`);
        assert.equal(wasm.normalizeTikZOperators(sample), base.normalizeTikZOperators(sample), `tikzOps: ${sample.slice(0, 30)}`);
        assert.deepEqual(plain(wasm.setEditorValue(sample)), plain(base.setEditorValue(sample)), `metrics: ${sample.slice(0, 30)}`);
    }
});

test('100% parity: Upload Wynn-Epsilon Aitken, signal/outlook prediction, and AIMD ramp', () => {
    const base = loadEditorEnv(BASELINE_DIR);
    const wasm = loadEditorEnv(SHIPPED_DIR);

    const seriesList = [
        [],
        [0.4],
        [0.4, 0.5],
        [0.2, 0.35, 0.48],
        [0.9, 0.85, 0.82, 0.8, 0.79, 0.785],
        [0.5, 0.5, 0.5, 0.5],
        [0.12, 0.18, 0.15, 0.14, 0.13, 0.125, 0.122],
    ];

    for (const seq of seriesList) {
        assert.ok(Math.abs(wasm.wynnEpsilonAitken(seq) - base.wynnEpsilonAitken(seq)) < 1e-12);
    }

    for (const recovery of [false, true]) {
        for (const initP of [1, 2, 3, 5, 12, 20]) {
            const bAsset = { targetParallel: initP, maxParallel: 24, chunkSize: 16 * 1024 * 1024, retryCounts: [0, 5, 12, 22] };
            const wAsset = { targetParallel: initP, maxParallel: 24, chunkSize: 16 * 1024 * 1024, retryCounts: [0, 5, 12, 22] };
            if (recovery) {
                base.ensureTasfaStats(bAsset).fastRecoveryUntil = Date.now() + 60000;
                wasm.ensureTasfaStats(wAsset).fastRecoveryUntil = Date.now() + 60000;
            }
            for (let step = 0; step < 18; step++) {
                const bytes = (step + 1) * 524288;
                const dur = 80 + (step % 5) * 35;
                base.recordTasfaSuccess(bAsset, bytes, dur);
                wasm.recordTasfaSuccess(wAsset, bytes, dur);
                assert.equal(wAsset.targetParallel, bAsset.targetParallel, `targetParallel step=${step}`);
                assert.ok(Math.abs( wasm.predictedTasfaQuality(wAsset) - base.predictedTasfaQuality(bAsset) ) < 1e-12);
                const bSig = base.predictedTasfaSignal(bAsset);
                const wSig = wasm.predictedTasfaSignal(wAsset);
                assert.ok(Math.abs(wSig.value - bSig.value) < 1e-12);
                assert.ok(Math.abs(wSig.confidence - bSig.confidence) < 1e-12);
                for (let ci = 0; ci < 4; ci++) {
                    const bOut = base.predictedTasfaOutlook(bAsset, ci);
                    const wOut = wasm.predictedTasfaOutlook(wAsset, ci);
                    assert.ok(Math.abs(wOut.timeoutRisk - bOut.timeoutRisk) < 1e-12);
                    assert.ok(Math.abs(wOut.fallbackNeed - bOut.fallbackNeed) < 1e-12);
                    assert.ok(Math.abs(wOut.recoveryChance - bOut.recoveryChance) < 1e-12);
                    assert.ok(Math.abs(wOut.confidence - bOut.confidence) < 1e-12);
                    assert.equal(wasm.predictUploadRule(wAsset, ci), base.predictUploadRule(bAsset, ci));
                }
            }
        }
    }
});

test('100% parity: Download session normalization, AIMD success/failure tuning, and predicted rules', () => {
    for (const mobile of [false, true]) {
        for (const conn of [
            null,
            { effectiveType: '4g', downlink: 600, rtt: 100, saveData: false },
            { effectiveType: '4g', downlink: 950, rtt: 80, saveData: false },
        ]) {
            const base = loadDownloadEnv(BASELINE_DIR, mobile, conn);
            const wasm = loadDownloadEnv(SHIPPED_DIR, mobile, conn);

            const configs = [
                { chunk_count: 128, chunk_size: 1024 * 1024, total_size: 128 * 1024 * 1024, initial_parallel_chunks: 1, max_parallel_chunks: 8, coalesce_chunks: 1 },
                { chunk_count: 64, chunk_size: 4 * 1024 * 1024, total_size: 256 * 1024 * 1024, initial_parallel_chunks: 6, max_parallel_chunks: 12, coalesce_chunks: 4, mime_type: 'video/mp4' },
                { chunk_count: 1, chunk_size: 128 * 1024 * 1024, total_size: 128 * 1024 * 1024, initial_parallel_chunks: 1, max_parallel_chunks: 8 },
            ];

            for (const cfg of configs) {
                const bSess = base.normalizeDownloadSession(cfg);
                const wSess = wasm.normalizeDownloadSession(cfg);
                assert.equal(JSON.stringify(wSess), JSON.stringify(bSess), `normalizeDownloadSession mobile=${mobile}`);

                for (let i = 0; i < 40; i++) {
                    if (i % 9 === 4) {
                        const kind = i % 2 === 0 ? 'timeout' : 'network';
                        base.tuneDownloadFailure(bSess, kind);
                        wasm.tuneDownloadFailure(wSess, kind);
                    } else {
                        const bytes = (1 + (i % 4)) * 1024 * 1024;
                        const dur = 50 + (i % 7) * 40;
                        base.tuneDownloadSuccess(bSess, bytes, dur);
                        wasm.tuneDownloadSuccess(wSess, bytes, dur);
                    }
                    base.applyPredictedDownloadRule(bSess);
                    wasm.applyPredictedDownloadRule(wSess);

                    assert.equal(wSess.currentSpan, bSess.currentSpan, `currentSpan step=${i}`);
                    assert.equal(wSess.targetParallel, bSess.targetParallel, `targetParallel step=${i}`);
                    assert.equal(wSess.maxParallel, bSess.maxParallel, `maxParallel step=${i}`);
                    assert.equal(wSess.ultraFastConnection, bSess.ultraFastConnection, `ultraFast step=${i}`);
                    assert.equal(wasm.preferredDownloadChunkSize(), base.preferredDownloadChunkSize(), `chunkSize step=${i}`);
                }
            }
        }
    }
});

test('100% parity: Post translation language detection and invalid response filtering', () => {
    const base = loadTranslateEnv(BASELINE_DIR);
    const wasm = loadTranslateEnv(SHIPPED_DIR);

    const texts = [
        '',
        'Pure English technical post with ASCII symbols 12345!@#$%',
        '한국어 게시글 텍스트와 English 혼용',
        '日本語のテキストと漢字の混在テスト',
        '纯中文测试文本，不包含假名或韩文',
        'Текст на русском языке для проверки детектора',
        'نص باللغة العربية لاختبار كاشف اللغة',
        'हिन्दी भाषा में परीक्षण पाठ',
        'a'.repeat(300000) + ' 한',
    ];
    for (const t of texts) {
        assert.equal(wasm.detectSourceLanguage(t), base.detectSourceLanguage(t), `lang: ${t}`);
    }

    const responses = [
        '',
        null,
        'Valid translated sentence.',
        'INVALID LANGUAGE PAIR SPECIFIED. EXAMPLE: LANGPAIR=EN|IT',
        'mymemory warning: you used all available free translations',
        'Query length limit exceeded',
        'NO QUERY SPECIFIED',
        'Please select two distinct languages',
        'invalid target language',
        'Invalid something else at start',
        'Contains invalid word in the middle but valid overall',
        // Non-ASCII letters that toUpperCase() folds into ASCII.
        '\u0131nvalid target language',
        'mymemory warning: \u017fome text',
        'plea\u017fe \u017felect two languages',
        'x'.repeat(200000) + ' QUERY LENGTH LIMIT',
    ];
    for (const r of responses) {
        assert.equal(wasm.isInvalidTranslationText(r), base.isInvalidTranslationText(r), `invalidFilter: ${r}`);
    }
});

test('100% parity: seeded fuzz over text helpers (operators, whitespace, scripts, surrogates)', () => {
    const baseEd = loadEditorEnv(BASELINE_DIR);
    const wasmEd = loadEditorEnv(SHIPPED_DIR);
    const baseTr = loadTranslateEnv(BASELINE_DIR);
    const wasmTr = loadTranslateEnv(SHIPPED_DIR);
    const pool = [
        'a', 'Z', '=', '-', '−', ' ', '\t', '\n', ' ', '　', ' ', '​', '﻿',
        '가', 'あ', 'ア', '中', 'Ж', 'ع', 'ह', '😀', '\ud800', '\udc00', 'ı', 'ſ', 'ß',
        'invalid ', 'INVALID LANGUAGE PAIR', 'please select', '\\draw', '$', '0',
    ];
    let seed = 0xC0FFEE;
    const rnd = (n) => {
        seed = (Math.imul(seed, 1103515245) + 12345) >>> 0;
        return seed % n;
    };
    for (let iter = 0; iter < 3000; iter++) {
        let s = '';
        const len = rnd(40);
        for (let i = 0; i < len; i++) s += pool[rnd(pool.length)];
        const tag = JSON.stringify(s);
        assert.equal(wasmEd.normalizeOperatorRuns(s), baseEd.normalizeOperatorRuns(s), `opRuns ${tag}`);
        assert.equal(wasmEd.normalizeTikZOperators(s), baseEd.normalizeTikZOperators(s), `tikzOps ${tag}`);
        assert.deepEqual(plain(wasmEd.setEditorValue(s)), plain(baseEd.setEditorValue(s)), `metrics ${tag}`);
        assert.equal(wasmTr.detectSourceLanguage(s), baseTr.detectSourceLanguage(s), `lang ${tag}`);
        assert.equal(wasmTr.isInvalidTranslationText(s), baseTr.isInvalidTranslationText(s), `invalid ${tag}`);
    }
});

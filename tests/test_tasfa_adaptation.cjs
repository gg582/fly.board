'use strict';

// Run: node --test tests/test_tasfa_adaptation.cjs
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const test = require('node:test');

function policy(upload = false, mobile = false) {
    const sandbox = {
        window: {},
        document: { readyState: 'complete', getElementById: () => null, querySelectorAll: () => [] },
        navigator: { userAgent: mobile ? 'Android' : '', hardwareConcurrency: 4 },
        localStorage: { getItem: () => null, setItem() {} },
        setTimeout: () => 1, clearTimeout() {}, console,
    };
    const name = upload ? 'editor.js' : 'tasfa-download.js';
    const source = fs.readFileSync(path.join(__dirname, '../public/js', name), 'utf8');
    // Expose production closure-local entrypoints only in the VM. Constants and
    // function bodies stay intact; unrelated DOM initialization is skipped.
    const anchor = upload ? "    var draftKey = 'flyboard:draft:' + location.pathname;" : '    function init() {';
    assert.ok(source.includes(anchor), 'test hook must match the shipped script');
    const exports = upload ? '{ recordTasfaSuccess, ensureTasfaStats }' :
        '{ normalizeDownloadSession, tuneDownloadSuccess, tuneDownloadFailure, applyPredictedDownloadRule }';
    const replacement = `${upload ? '' : anchor} window.policy = ${exports}; return;`;
    vm.runInNewContext(source.replace(anchor, replacement), sandbox);
    return sandbox.window.policy;
}

function session(p, initial = 1, extra = {}) {
    return p.normalizeDownloadSession({
        chunk_count: 128, chunk_size: 1024 * 1024, total_size: 128 * 1024 * 1024,
        initial_parallel_chunks: initial, max_parallel_chunks: 8, coalesce_chunks: initial,
        ...extra,
    });
}
const success = (p, state) => p.tuneDownloadSuccess(state, 1024 * 1024, 100);

for (const initial of [1, 2]) {
    test(`download span grows from ${initial} on the second success`, () => {
        const p = policy(); const state = session(p, initial);
        success(p, state);
        assert.equal(state.currentSpan, initial);
        success(p, state);
        assert.ok(state.currentSpan > initial);
    });
    test(`download concurrency grows from ${initial} on the third success`, () => {
        const p = policy(); const state = session(p, initial);
        success(p, state); success(p, state);
        assert.equal(state.targetParallel, initial);
        success(p, state);
        assert.ok(state.targetParallel > initial);
    });
}

test('download recovers from guarded reduction without changing session chunk size', () => {
    const p = policy(); const state = session(p, 8);
    p.tuneDownloadFailure(state, 'timeout');
    p.applyPredictedDownloadRule(state);
    assert.ok(state.currentSpan <= 2);
    assert.ok(state.targetParallel < state.maxParallel);
    for (let i = 0; i < 120; i++) {
        success(p, state); p.applyPredictedDownloadRule(state);
        assert.ok(state.currentSpan <= state.maxSpan);
        assert.ok(state.targetParallel <= state.maxParallel);
    }
    assert.equal(state.currentSpan, state.maxSpan);
    assert.equal(state.targetParallel, state.maxParallel);
    assert.equal(state.chunkSize, 1024 * 1024);
});

test('download preserves larger multiplicative steps and negotiated limits', () => {
    const p = policy(); const state = session(p, 10, { max_parallel_chunks: 12 });
    state.successEvents = 5; success(p, state);
    assert.equal(state.currentSpan, 12);
    assert.equal(state.targetParallel, 12);
    state.currentSpan = state.maxSpan - 1;
    state.successEvents = 5; success(p, state);
    assert.equal(state.currentSpan, state.maxSpan);
    assert.equal(state.targetParallel, state.maxParallel);
});

test('single-chunk downloads retain one worker and spans respect the request byte budget', () => {
    const p = policy(); const state = session(p, 1, { chunk_count: 1, chunk_size: 128 * 1024 * 1024 });
    for (let i = 0; i < 12; i++) success(p, state);
    assert.equal(state.targetParallel, 1);
    assert.equal(state.currentSpan, 1);
});

for (const mobile of [false, true]) {
    test(`fast media retains one-chunk spans and its concurrency cap, mobile=${mobile}`, () => {
        const p = policy(false, mobile); const state = session(p, 8, { mime_type: 'video/mp4' });
        state.ultraFastConnection = true; state.fastLinkTier = 'ultra';
        state.maxParallel = state.targetParallel = mobile ? 3 : 4;
        state.currentSpan = state.maxSpan = 1;
        for (let i = 0; i < 12; i++) p.tuneDownloadSuccess(state, 64 * 1024 * 1024, 100);
        assert.equal(state.currentSpan, 1);
        assert.equal(state.targetParallel, mobile ? 3 : 4);
    });
}

for (const recovery of [false, true]) {
    for (const initial of [1, 2, 3]) {
        test(`upload grows from ${initial}, fast recovery=${recovery}`, () => {
            const p = policy(true);
            const asset = { targetParallel: initial, maxParallel: 8, chunkSize: 32 * 1024 * 1024 };
            if (recovery) p.ensureTasfaStats(asset).fastRecoveryUntil = Date.now() + 60000;
            p.recordTasfaSuccess(asset, 1024 * 1024, 100);
            assert.ok(asset.targetParallel > initial);
            for (let i = 0; i < 20; i++) {
                p.recordTasfaSuccess(asset, 1024 * 1024, 100);
                assert.ok(asset.targetParallel <= asset.maxParallel);
            }
            assert.equal(asset.targetParallel, asset.maxParallel);
            assert.equal(asset.chunkSize, 32 * 1024 * 1024);
        });
    }
}

test('upload preserves larger multiplicative steps and a single-worker limit', () => {
    const p = policy(true); const asset = { targetParallel: 20, maxParallel: 30 };
    p.recordTasfaSuccess(asset, 1024 * 1024, 100);
    assert.equal(asset.targetParallel, 23);
    const single = { targetParallel: 1, maxParallel: 1 };
    p.recordTasfaSuccess(single, 1024 * 1024, 100);
    assert.equal(single.targetParallel, 1);
});

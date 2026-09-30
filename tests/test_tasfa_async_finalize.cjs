'use strict';

// Run: node --test tests/test_tasfa_async_finalize.cjs
const assert = require('node:assert/strict');
const crypto = require('node:crypto');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const test = require('node:test');

// Same VM extraction pattern as the other editor.js tests: cut the script at
// the draftKey anchor and export the async-finalize helpers (function
// declarations hoist, so definitions later in the closure are reachable).
function uploadHelpers() {
    const sandbox = {
        window: {},
        document: { readyState: 'complete', getElementById: () => null, querySelectorAll: () => [] },
        navigator: { userAgent: '', hardwareConcurrency: 4 },
        localStorage: { getItem: () => null, setItem() {} },
        setTimeout: () => 1, clearTimeout() {}, console,
    };
    const source = fs.readFileSync(path.join(__dirname, '../public/js/editor.js'), 'utf8');
    const anchor = "    var draftKey = 'flyboard:draft:' + location.pathname;";
    assert.ok(source.includes(anchor), 'test hook must match the shipped script');
    const replacement = 'window.h = { tasfaCompleteRequestBody, tasfaPreviewsTerminal }; return;';
    vm.runInNewContext(source.replace(anchor, replacement), sandbox);
    return sandbox.window.h;
}

test('complete request opts in to async_finalize', () => {
    const h = uploadHelpers();
    const asset = { uploadId: 'abc123', uploadToken: 'tok456' };
    const body = h.tasfaCompleteRequestBody(asset);
    assert.equal(body.upload_id, 'abc123');
    assert.equal(body.upload_token, 'tok456');
    assert.equal(body.async_finalize, '1');
});

test('previews state predicate treats done/failed/skipped as terminal', () => {
    const h = uploadHelpers();
    assert.equal(h.tasfaPreviewsTerminal('done'), true);
    assert.equal(h.tasfaPreviewsTerminal('failed'), true);
    assert.equal(h.tasfaPreviewsTerminal('skipped'), true);
    assert.equal(h.tasfaPreviewsTerminal('pending'), false);
    assert.equal(h.tasfaPreviewsTerminal(undefined), false);
    assert.equal(h.tasfaPreviewsTerminal(''), false);
});

// Reference model of the server's rolling hash: chunks noted out of order,
// ctx consumes the contiguous received prefix. Must equal the one-shot digest.
function rollingDigest(chunks, chunkSize, totalSize, arrivalOrder) {
    const received = new Array(chunks.length).fill(false);
    const hash = crypto.createHash('sha256');
    let next = 0;
    for (const idx of arrivalOrder) {
        received[idx] = true;
        while (next < chunks.length && received[next]) {
            hash.update(chunks[next]);
            next++;
        }
    }
    // finalize(): consume any remaining contiguous tail
    while (next < chunks.length && received[next]) {
        hash.update(chunks[next]);
        next++;
    }
    const consumed = chunks.slice(0, next).reduce((n, c, i) =>
        n + Math.min(chunkSize, totalSize - i * chunkSize), 0);
    return consumed === totalSize ? hash.digest('hex') : null;
}

test('rolling contiguous-prefix hash equals one-shot SHA-256 for any arrival order', () => {
    const chunkSize = 64 * 1024;
    const dataChunks = 9; // last chunk partial
    const totalSize = (dataChunks - 1) * chunkSize + 12345;
    const whole = crypto.randomBytes(totalSize);
    const chunks = [];
    for (let i = 0; i < dataChunks; i++) {
        chunks.push(whole.subarray(i * chunkSize, Math.min((i + 1) * chunkSize, totalSize)));
    }
    const oneShot = crypto.createHash('sha256').update(whole).digest('hex');

    // Sequential arrival
    assert.equal(rollingDigest(chunks, chunkSize, totalSize, chunks.map((_, i) => i)), oneShot);
    // Reverse arrival
    assert.equal(rollingDigest(chunks, chunkSize, totalSize, chunks.map((_, i) => i).reverse()), oneShot);
    // Random arrival orders
    for (let trial = 0; trial < 20; trial++) {
        const order = chunks.map((_, i) => i);
        for (let i = order.length - 1; i > 0; i--) {
            const j = Math.floor(Math.random() * (i + 1));
            [order[i], order[j]] = [order[j], order[i]];
        }
        assert.equal(rollingDigest(chunks, chunkSize, totalSize, order), oneShot);
    }
    // Incomplete prefix must report unusable (server falls back to full re-read)
    assert.equal(rollingDigest(chunks.slice(0, 4), chunkSize, totalSize, [0, 1, 2, 3]), null);
});

test('incremental SHA-256 across random splits matches one-shot', () => {
    for (const total of [1, 63, 64, 65, 4096, 65536, 65537, 1 << 20]) {
        const whole = crypto.randomBytes(total);
        const oneShot = crypto.createHash('sha256').update(whole).digest('hex');
        for (let splitCount = 1; splitCount <= 8; splitCount++) {
            const hash = crypto.createHash('sha256');
            let offset = 0;
            for (let s = 0; s < splitCount; s++) {
                const remain = whole.length - offset;
                const partsLeft = splitCount - s;
                const len = s === splitCount - 1 ? remain : Math.floor(remain / partsLeft * (0.5 + Math.random()));
                hash.update(whole.subarray(offset, offset + len));
                offset += len;
            }
            assert.equal(offset, whole.length);
            assert.equal(hash.digest('hex'), oneShot, `total=${total} splits=${splitCount}`);
        }
    }
});

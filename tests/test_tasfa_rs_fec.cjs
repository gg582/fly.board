'use strict';

// Run: node --test tests/test_tasfa_rs_fec.cjs
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const test = require('node:test');

// Same VM extraction pattern as test_tasfa_adaptation.cjs: cut the script at
// the draftKey anchor and export the RS FEC helpers defined before it.
function rsHelpers() {
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
    const replacement = 'window.rs = { gfMul, gfDiv, ensureTasfaGfTables, generateRsParityBuffers, tasfaParityChunkCount }; return;';
    vm.runInNewContext(source.replace(anchor, replacement), sandbox);
    return sandbox.window.rs;
}

// Independent reference GF(2^8) implementation (log/exp rebuilt inline).
function referenceGf() {
    const exp = new Uint8Array(510);
    const log = new Uint8Array(256);
    let x = 1;
    for (let i = 0; i < 255; i++) {
        exp[i] = x;
        log[x] = i;
        x <<= 1;
        if (x & 0x100) x ^= 0x11d;
    }
    for (let i = 255; i < 510; i++) exp[i] = exp[i - 255];
    return {
        mul: (a, b) => (a && b ? exp[log[a] + log[b]] : 0),
        div: (a, b) => (a ? exp[log[a] + 255 - log[b]] : 0),
    };
}

function makeChunks(count, chunkSize, seed) {
    // Deterministic pseudo-random chunks with plenty of zero bytes to exercise
    // the zero-skip path in both encoder and recovery.
    const chunks = [];
    let s = seed >>> 0;
    for (let c = 0; c < count; c++) {
        const buf = new Uint8Array(chunkSize);
        for (let i = 0; i < chunkSize; i++) {
            s = (s * 1664525 + 1013904223) >>> 0;
            const r = (s >>> 24) & 0xff;
            buf[i] = r < 128 ? r : 0;
        }
        chunks.push(buf);
    }
    return chunks;
}

function referenceParities(gf, chunks) {
    const chunkSize = chunks[0].length;
    const p0 = new Uint8Array(chunkSize);
    const p1 = new Uint8Array(chunkSize);
    chunks.forEach((data, pos) => {
        const xv = pos + 1;
        const a = xv;
        const b = gf.mul(xv, xv);
        for (let i = 0; i < data.length; i++) {
            const v = data[i];
            if (v) {
                p0[i] ^= gf.mul(a, v);
                p1[i] ^= gf.mul(b, v);
            }
        }
    });
    return [p0, p1];
}

// Recover erased positions (1 or 2) from present chunks + parity rows,
// using only the RS decode formulas (mirrors perform_rs_recovery).
function recover(gf, present, erased, p0, p1) {
    const chunkSize = p0.length;
    const r0 = new Uint8Array(p0);
    const r1 = new Uint8Array(p1);
    for (const [pos, data] of present) {
        const xv = pos + 1;
        const a = xv;
        const b = gf.mul(xv, xv);
        for (let i = 0; i < chunkSize; i++) {
            const v = data[i];
            if (v) {
                r0[i] ^= gf.mul(a, v);
                r1[i] ^= gf.mul(b, v);
            }
        }
    }
    const out = {};
    if (erased.length === 1) {
        const u = erased[0];
        const aU = u + 1;
        const du = new Uint8Array(chunkSize);
        for (let i = 0; i < chunkSize; i++) du[i] = gf.div(r0[i], aU);
        out[u] = du;
    } else {
        const [u, v] = erased;
        const aU = u + 1, bU = gf.mul(aU, aU);
        const aV = v + 1, bV = gf.mul(aV, aV);
        const det = gf.mul(aU, bV) ^ gf.mul(aV, bU);
        const du = new Uint8Array(chunkSize);
        const dv = new Uint8Array(chunkSize);
        for (let i = 0; i < chunkSize; i++) {
            const P0 = r0[i], P1 = r1[i];
            du[i] = gf.div(gf.mul(P0, bV) ^ gf.mul(P1, aV), det);
            dv[i] = gf.div(gf.mul(aU, P1) ^ gf.mul(bU, P0), det);
        }
        out[u] = du;
        out[v] = dv;
    }
    return out;
}

test('gfMul matches the reference implementation', () => {
    const rs = rsHelpers();
    rs.ensureTasfaGfTables();
    const gf = referenceGf();
    for (const [a, b] of [[0, 0], [0, 7], [5, 0], [1, 1], [2, 3], [7, 11], [13, 247], [255, 255], [128, 3]]) {
        assert.equal(rs.gfMul(a, b), gf.mul(a, b), `gfMul(${a},${b})`);
        assert.equal(rs.gfDiv(a, b || 1), gf.div(a, b || 1), `gfDiv(${a},${b || 1})`);
    }
    // Generator sanity: 2 is a primitive element under 0x11d (255 distinct powers).
    const seen = new Set();
    let x = 1;
    for (let i = 0; i < 255; i++) {
        seen.add(x);
        x = rs.gfMul(x, 2);
    }
    assert.equal(seen.size, 255);
    assert.equal(x, 1);
});

test('chunk-count formulas: rs2 doubles legacy parity count', () => {
    const rs = rsHelpers();
    for (const d of [1, 5, 6, 7, 12, 13, 100]) {
        const legacy = Math.ceil(d / 6);
        assert.equal(rs.tasfaParityChunkCount(d, 'xor1'), legacy, `xor1 d=${d}`);
        assert.equal(rs.tasfaParityChunkCount(d, 'rs2'), 2 * legacy, `rs2 d=${d}`);
        assert.equal(d + rs.tasfaParityChunkCount(d, 'rs2'), d + 2 * legacy, `rs2 total d=${d}`);
    }
    // Mode derivation parity between client and server:
    // chunk_count == data + 2*ceil(d/6)  <=>  rs2
    for (const d of [1, 6, 7, 13]) {
        const rs2Total = d + 2 * Math.ceil(d / 6);
        assert.equal(rs2Total === d + rs.tasfaParityChunkCount(d, 'rs2'), true);
    }
});

test('generateRsParityBuffers matches the reference parities', async () => {
    const rs = rsHelpers();
    const gf = referenceGf();
    const chunkSize = 4096;
    const chunks = makeChunks(6, chunkSize, 42);
    const file = new Blob(chunks.map((c) => c.buffer));
    const bufs = await rs.generateRsParityBuffers(file, 6, chunkSize, 0);
    const [refP0, refP1] = referenceParities(gf, chunks);
    assert.deepEqual(new Uint8Array(bufs[0]), refP0);
    assert.deepEqual(new Uint8Array(bufs[1]), refP1);
});

test('recovers any single erasure per group', async () => {
    const rs = rsHelpers();
    const gf = referenceGf();
    const chunkSize = 2048;
    const chunks = makeChunks(6, chunkSize, 7);
    const [p0, p1] = referenceParities(gf, chunks);
    for (const erased of [0, 1, 2, 3, 4, 5]) {
        const present = chunks.map((data, pos) => [pos, data]).filter(([pos]) => pos !== erased);
        const out = recover(gf, present, [erased], p0, p1);
        assert.deepEqual(out[erased], chunks[erased], `erased=${erased}`);
    }
});

test('recovers any pair of erasures per group', async () => {
    const rs = rsHelpers();
    const gf = referenceGf();
    const chunkSize = 2048;
    const chunks = makeChunks(6, chunkSize, 99);
    const [p0, p1] = referenceParities(gf, chunks);
    for (let u = 0; u < 6; u++) {
        for (let v = u + 1; v < 6; v++) {
            const present = chunks.map((data, pos) => [pos, data]).filter(([pos]) => pos !== u && pos !== v);
            const out = recover(gf, present, [u, v], p0, p1);
            assert.deepEqual(out[u], chunks[u], `pair (${u},${v}) first`);
            assert.deepEqual(out[v], chunks[v], `pair (${u},${v}) second`);
        }
    }
});

test('recovers erasures in a partial group with a short last chunk', async () => {
    const rs = rsHelpers();
    const gf = referenceGf();
    const chunkSize = 1024;
    const chunks = makeChunks(3, chunkSize, 5);
    // 4 data chunks; the last one is a partial slice of the blob.
    const partial = makeChunks(1, 300, 11)[0];
    const all = chunks.concat([partial]);
    const blob = new Blob(all.map((c) => (c.length === chunkSize ? c.buffer : c.slice(0, 300).buffer)));
    const total = 3 * chunkSize + 300;
    assert.equal(blob.size, total);
    const bufs = await rs.generateRsParityBuffers(blob, 4, chunkSize, 0);
    const [refP0, refP1] = referenceParities(gf, all);
    assert.deepEqual(new Uint8Array(bufs[0]), refP0);
    assert.deepEqual(new Uint8Array(bufs[1]), refP1);
    // Erase the partial last chunk plus a full one, recover both.
    const present = all.map((data, pos) => [pos, data]).filter(([pos]) => pos !== 1 && pos !== 3);
    const out = recover(gf, present, [1, 3], refP0, refP1);
    assert.deepEqual(out[1], all[1]);
    assert.deepEqual(out[3].slice(0, all[3].length), all[3]);
});

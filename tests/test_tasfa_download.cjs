'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const vm = require('node:vm');
const { createHash } = require('node:crypto');

// A deterministic transport fixture: 100 ms/request + 32 MiB/s per stream.
// This measures scheduler latency, NOT real WAN or server throughput.
async function transfer(initial, maxParallel, maxSpan) {
    const chunkSize = 1024 * 1024;
    const payload = Buffer.alloc(31 * chunkSize + 123);
    for (let i = 0; i < payload.length; i++) payload[i] = (i * 31 + (i >>> 12)) & 255;
    let now = 1000000;
    const started = now;
    let seq = 0;
    const timers = new Map();
    const schedule = (fn, delay) => {
        const id = ++seq;
        timers.set(id, { at: now + delay, fn });
        return id;
    };
    const requests = [];
    const progress = [];
    let active = 0;
    let peakActive = 0;
    let completed = 0;
    let settled = false;
    let result;
    let failure;
    class FakeDate extends Date { static now() { return now; } }
    class XHR {
        open(method, url) { assert.equal(method, 'GET'); this.url = new URL(url, 'https://example.test'); }
        setRequestHeader() {}
        getResponseHeader() { return null; }
        abort() { throw new Error('unexpected download timeout'); }
        send() {
            const index = Number(this.url.pathname.split('/').pop());
            const span = Number(this.url.searchParams.get('span') || 1);
            assert.equal(this.url.searchParams.get('session_id'), 'fixture-session');
            assert.equal(this.url.searchParams.get('session_token'), 'fixture-only');
            assert.ok(span >= 1 && span <= maxSpan);
            const start = index * chunkSize;
            const end = Math.min(start + span * chunkSize, payload.length);
            assert.ok(end > start);
            requests.push({ index, span, at: now });
            active++;
            peakActive = Math.max(peakActive, active);
            assert.ok(active <= maxParallel);
            schedule(() => {
                active--;
                this.status = 200;
                this.response = payload.buffer.slice(payload.byteOffset + start, payload.byteOffset + end);
                this.onload();
            }, 100 + (end - start) / (32 * 1024 * 1024) * 1000);
        }
    }
    const window = { location: { origin: 'https://example.test' }, addEventListener() {} };
    const sandbox = {
        window, document: { readyState: 'complete', querySelectorAll: () => [] },
        navigator: { userAgent: '', hardwareConcurrency: 4 },
        localStorage: { getItem: () => null, setItem() {} },
        caches: { open: async () => ({ match: async () => null, put: async () => {} }) },
        XMLHttpRequest: XHR, URL, Blob, Response, Date: FakeDate,
        setTimeout: schedule, clearTimeout: (id) => timers.delete(id),
        fetch: async (url, options) => {
            if (url === '/file/download/complete') {
                assert.equal(options.method, 'POST');
                assert.equal(options.body, 'session_id=fixture-session&session_token=fixture-only');
                completed++;
                return { ok: true };
            }
            assert.ok(url.startsWith('/file/download/1/handshake?'));
            return { ok: true, status: 200, json: async () => ({
                session_id: 'fixture-session', session_token: 'fixture-only',
                chunk_count: Math.ceil(payload.length / chunkSize), chunk_size: chunkSize,
                total_size: payload.length, filename: 'fixture.bin',
                initial_parallel_chunks: initial, max_parallel_chunks: maxParallel,
                coalesce_chunks: 1,
            }) };
        },
    };
    // Execute the entire unmodified production script and its public entrypoint.
    vm.runInNewContext(fs.readFileSync(path.join(__dirname, '../public/js/tasfa-download.js'), 'utf8'), sandbox);
    window.fetchBlobViaTasfa('/file/download/1', { silent: true, onProgress: (pct) => progress.push(pct) })
        .then((value) => { result = value; settled = true; }, (error) => { failure = error; settled = true; });
    for (let steps = 0; !settled && steps < 10000; steps++) {
        // Let all production Promise continuations run before advancing virtual time.
        await new Promise(setImmediate);
        if (settled) break;
        const entry = [...timers].sort((a, b) => a[1].at - b[1].at || a[0] - b[0])[0];
        assert.ok(entry, 'download must make progress');
        timers.delete(entry[0]);
        now = entry[1].at;
        entry[1].fn();
    }
    assert.ok(settled, 'bounded scheduler must finish');
    if (failure) throw failure;
    assert.equal(completed, 1);
    assert.equal(result.filename, 'fixture.bin');
    assert.equal(result.blob.size, payload.length);
    const digest = (data) => createHash('sha256').update(data).digest('hex');
    assert.equal(digest(Buffer.from(await result.blob.arrayBuffer())), digest(payload));
    assert.equal(progress.at(-1), 100);
    assert.ok(progress.every((pct, i) => pct >= 0 && pct <= 100 && (i === 0 || pct >= progress[i - 1])));
    const coverage = Array(Math.ceil(payload.length / chunkSize)).fill(0);
    for (const { index, span } of requests) {
        for (let i = index; i < index + span; i++) coverage[i]++;
    }
    assert.ok(coverage.every((count) => count === 1), 'no missing or duplicate chunks');
    assert.equal(active, 0);
    return { virtualMs: now - started, requests: requests.length, peakActive };
}

test('public download ramps up from one worker and one-chunk requests', async (t) => {
    const stats = await transfer(1, 8, 64);
    t.diagnostic(JSON.stringify(stats));
    assert.ok(stats.peakActive > 1, 'healthy transfers should leave single-worker slow start');
    assert.ok(stats.requests < 32, 'healthy transfers should coalesce chunks after slow start');
    assert.ok(stats.virtualMs < 2000, 'avoid the >4-second serialized fixture baseline');
});

test('public download respects a server limit of one worker', async () => {
    const stats = await transfer(1, 1, 64);
    assert.equal(stats.peakActive, 1);
});

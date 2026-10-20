'use strict';

// Run: node --test tests/test_tagmap_hit_parity.cjs
// Checks the CWIST WASM tag-map hit test (fb_tagmap_hit_test) against the
// archived pure-JS hitTestNode in tests/js_baseline/tagmap.js.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const test = require('node:test');

const ROOT = path.join(__dirname, '..');
const createTagmapModule = require(path.join(ROOT, 'public', 'js', 'tagmap-kernel.js'));

function extractFunction(source, name) {
    const start = source.indexOf(`    function ${name}(`);
    assert.ok(start >= 0, `baseline must define ${name}`);
    const end = source.indexOf('\n    }\n', start);
    // One-line helpers end on their own line.
    const lineEnd = source.indexOf('\n', start);
    return source.slice(start, source.slice(start, lineEnd).trim().endsWith('}') ? lineEnd : end + 6);
}

function loadBaselineHitTest() {
    const source = fs.readFileSync(path.join(ROOT, 'tests', 'js_baseline', 'tagmap.js'), 'utf8');
    const body = ['worldToScreenX', 'worldToScreenY', 'hitTestNode']
        .map((name) => extractFunction(source, name)).join('\n');
    const ctx = { graph: null, posX: null, posY: null, posR: null, cam: null, viewW: 0, viewH: 0 };
    vm.runInNewContext(`${body}\nthis.hitTestNode = hitTestNode;`, ctx);
    return ctx;
}

test('100% parity: tag map hit test matches the JS baseline', async () => {
    const mod = await createTagmapModule();
    const base = loadBaselineHitTest();
    let seed = 0x5eed;
    let hits = 0;
    const rnd = (n) => {
        seed = (Math.imul(seed, 1103515245) + 12345) >>> 0;
        return seed % n;
    };

    for (const n of [0, 1, 7, 60, 240]) {
        const nodes = Array.from({ length: n }, (_, i) => ({ id: i + 1, n: 1 + rnd(40), board_id: 1 + rnd(4) }));
        const edges = [];
        for (let i = 0; i < n * 2 && n > 1; i++) {
            const a = rnd(n);
            const b = rnd(n);
            if (a !== b) edges.push({ a, b, w: 1 + rnd(5) });
        }
        mod._fb_tagmap_init();
        const nPtr = mod._malloc(Math.max(4, n * 12));
        const ePtr = mod._malloc(Math.max(4, edges.length * 12));
        const nView = mod.cwistView.i32(nPtr, n * 3);
        nodes.forEach((node, i) => nView.set([node.id, node.n, node.board_id], i * 3));
        const eView = mod.cwistView.i32(ePtr, edges.length * 3);
        edges.forEach((e, i) => eView.set([e.a, e.b, e.w], i * 3));
        mod._fb_tagmap_layout(nPtr, n, ePtr, edges.length, 130);
        mod._free(nPtr);
        mod._free(ePtr);

        base.graph = { nodes };
        base.posX = mod.cwistView.f64(mod._fb_tagmap_x_ptr(), mod._fb_tagmap_x_len());
        base.posY = mod.cwistView.f64(mod._fb_tagmap_y_ptr(), mod._fb_tagmap_y_len());
        base.posR = mod.cwistView.f64(mod._fb_tagmap_r_ptr(), mod._fb_tagmap_r_len());

        for (const z of [0.05, 0.3, 1, 2.5, 9]) {
            base.cam = { x: (rnd(400) - 200), y: (rnd(400) - 200), z };
            base.viewW = 280 + rnd(1400);
            base.viewH = 200 + rnd(900);
            for (let k = 0; k < 400; k++) {
                // Aim most probes at nodes so hits (and near-ties) are exercised.
                let sx;
                let sy;
                if (n && k % 4) {
                    const i = rnd(n);
                    sx = (base.posX[i] - base.cam.x) * z + base.viewW * 0.5 + (rnd(60) - 30) / 3;
                    sy = (base.posY[i] - base.cam.y) * z + base.viewH * 0.5 + (rnd(60) - 30) / 3;
                } else {
                    sx = rnd(base.viewW);
                    sy = rnd(base.viewH);
                }
                const got = mod._fb_tagmap_hit_test(sx, sy, base.cam.x, base.cam.y, z, base.viewW, base.viewH);
                assert.equal(got, base.hitTestNode(sx, sy), `n=${n} z=${z} probe=(${sx},${sy})`);
                if (got >= 0) hits++;
            }
        }
    }
    assert.ok(hits > 1000, `probes must exercise real hits (got ${hits})`);
});

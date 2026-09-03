/* WebGPU high-quality image enhancement for high-DPI displays.
 *
 * When the physical pixels an <img> occupies exceed the resolution of the
 * candidate the browser picked from srcset, the browser's built-in bilinear
 * upscale leaves the image soft.  If WebGPU is available we re-scale the
 * image ourselves with a separable Lanczos-3 filter on the GPU and clamp the
 * result to the 2x2 source neighborhood (ringing/halo suppression), then
 * swap the pixels back into the same <img> via a blob URL so layout and the
 * theme toggle keep working.  Any failure leaves the original untouched. */
(function () {
'use strict';
if (!('gpu' in navigator)) return;
var DPR = window.devicePixelRatio || 1;
if (DPR <= 1) return;

var MAX_SCALE = 3;          /* never invent more than 3x the pixels */
var MAX_PIXELS = 16000000;  /* cap GPU/readback work (~16 MP) */

var deviceP = null;
function getDevice() {
    if (!deviceP) {
        deviceP = navigator.gpu.requestAdapter().then(function (adapter) {
            if (!adapter) throw new Error('no adapter');
            return adapter.requestDevice();
        });
        deviceP.catch(function () { deviceP = null; });
    }
    return deviceP;
}

var WGSL = [
    '@group(0) @binding(0) var srcTex: texture_2d<f32>;',
    '@group(0) @binding(1) var dstTex: texture_storage_2d<rgba8unorm, write>;',
    '@group(0) @binding(2) var<uniform> p: vec4f; /* srcW srcH dstW dstH */',
    '',
    'fn lanczos3(x: f32) -> f32 {',
    '    let ax = abs(x);',
    '    if (ax >= 3.0) { return 0.0; }',
    '    if (ax < 1e-4) { return 1.0; }',
    '    let px = 3.14159265 * x;',
    '    let q  = px / 3.0;',
    '    return (sin(px) / px) * (sin(q) / q);',
    '}',
    '',
    'fn tap(pos: vec2i, maxp: vec2i) -> vec4f {',
    '    return textureLoad(srcTex, clamp(pos, vec2i(0, 0), maxp), 0);',
    '}',
    '',
    '@compute @workgroup_size(8, 8)',
    'fn hpass(@builtin(global_invocation_id) id: vec3u) {',
    '    let dw = u32(p.z); let dh = u32(p.w);',
    '    if (id.x >= dw || id.y >= dh) { return; }',
    '    let sw = i32(p.x); let sh = i32(p.y);',
    '    let scale = p.x / p.z; /* src px per dst px (<1 when upscaling) */',
    '    let center = (f32(id.x) + 0.5) * scale - 0.5;',
    '    let base = i32(floor(center));',
    '    var acc = vec4f(0.0);',
    '    var wsum = 0.0;',
    '    let y = i32(id.y);',
    '    for (var k = -2; k <= 3; k++) {',
    '        let si = base + k;',
    '        let w = lanczos3(f32(si) - center);',
    '        acc += tap(vec2i(si, y), vec2i(sw - 1, sh - 1)) * w;',
    '        wsum += w;',
    '    }',
    '    var col = acc / wsum;',
    '    /* Ringing suppression: clamp to the min/max of the 2x2 source',
    '     * neighborhood so lobes cannot overshoot into halos. */',
    '    let x0 = clamp(base, 0, sw - 1); let x1 = clamp(base + 1, 0, sw - 1);',
    '    let c00 = tap(vec2i(x0, y), vec2i(sw - 1, sh - 1));',
    '    let c10 = tap(vec2i(x1, y), vec2i(sw - 1, sh - 1));',
    '    col = clamp(col, min(c00, c10), max(c00, c10));',
    '    textureStore(dstTex, vec2i(i32(id.x), i32(id.y)), col);',
    '}',
    '',
    '@compute @workgroup_size(8, 8)',
    'fn vpass(@builtin(global_invocation_id) id: vec3u) {',
    '    let dw = u32(p.z); let dh = u32(p.w);',
    '    if (id.x >= dw || id.y >= dh) { return; }',
    '    let sw = i32(p.x); let sh = i32(p.y);',
    '    let scale = p.y / p.w;',
    '    let center = (f32(id.y) + 0.5) * scale - 0.5;',
    '    let base = i32(floor(center));',
    '    var acc = vec4f(0.0);',
    '    var wsum = 0.0;',
    '    let x = i32(id.x);',
    '    for (var k = -2; k <= 3; k++) {',
    '        let si = base + k;',
    '        let w = lanczos3(f32(si) - center);',
    '        acc += tap(vec2i(x, si), vec2i(sw - 1, sh - 1)) * w;',
    '        wsum += w;',
    '    }',
    '    var col = acc / wsum;',
    '    let y0 = clamp(base, 0, sh - 1); let y1 = clamp(base + 1, 0, sh - 1);',
    '    let c00 = tap(vec2i(x, y0), vec2i(sw - 1, sh - 1));',
    '    let c01 = tap(vec2i(x, y1), vec2i(sw - 1, sh - 1));',
    '    col = clamp(col, min(c00, c01), max(c00, c01));',
    '    textureStore(dstTex, vec2i(i32(id.x), i32(id.y)), col);',
    '}'
].join('\n');

var pipelineP = null;
function getPipelines(device) {
    if (!pipelineP) {
        pipelineP = (function () {
            var module = device.createShaderModule({ code: WGSL });
            var layout = 'auto';
            return Promise.all([
                device.createComputePipeline({ layout: layout, compute: { module: module, entryPoint: 'hpass' } }),
                device.createComputePipeline({ layout: layout, compute: { module: module, entryPoint: 'vpass' } })
            ]);
        })();
    }
    return pipelineP;
}

function ceilDiv(a, b) { return Math.ceil(a / b); }

async function upscaleLanczos(img, targetW, targetH) {
    var device = await getDevice();
    var pipes = await getPipelines(device);

    var bitmap = await createImageBitmap(img);
    var sw = bitmap.width, sh = bitmap.height;
    var tw = targetW, th = targetH;

    var srcTex = device.createTexture({
        size: [sw, sh], format: 'rgba8unorm',
        usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST | GPUTextureUsage.RENDER_ATTACHMENT
    });
    device.queue.copyExternalImageToTexture({ source: bitmap }, { texture: srcTex }, [sw, sh]);
    bitmap.close();

    var midTex = device.createTexture({
        size: [tw, sh], format: 'rgba8unorm',
        usage: GPUTextureUsage.STORAGE_BINDING | GPUTextureUsage.TEXTURE_BINDING
    });
    var dstTex = device.createTexture({
        size: [tw, th], format: 'rgba8unorm',
        usage: GPUTextureUsage.STORAGE_BINDING | GPUTextureUsage.COPY_SRC
    });

    function uniforms(a, b, c, d) {
        var buf = device.createBuffer({ size: 16, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST });
        device.queue.writeBuffer(buf, 0, new Float32Array([a, b, c, d]));
        return buf;
    }

    var encoder = device.createCommandEncoder();

    var bg1 = device.createBindGroup({
        layout: pipes[0].getBindGroupLayout(0),
        entries: [
            { binding: 0, resource: srcTex.createView() },
            { binding: 1, resource: midTex.createView() },
            { binding: 2, resource: { buffer: uniforms(sw, sh, tw, sh) } }
        ]
    });
    var bg2 = device.createBindGroup({
        layout: pipes[1].getBindGroupLayout(0),
        entries: [
            { binding: 0, resource: midTex.createView() },
            { binding: 1, resource: dstTex.createView() },
            { binding: 2, resource: { buffer: uniforms(tw, sh, tw, th) } }
        ]
    });

    var pass = encoder.beginComputePass();
    pass.setPipeline(pipes[0]);
    pass.setBindGroup(0, bg1);
    pass.dispatchWorkgroups(ceilDiv(tw, 8), ceilDiv(sh, 8));
    pass.setPipeline(pipes[1]);
    pass.setBindGroup(0, bg2);
    pass.dispatchWorkgroups(ceilDiv(tw, 8), ceilDiv(th, 8));
    pass.end();

    var bytesPerRow = Math.ceil(tw * 4 / 256) * 256;
    var readback = device.createBuffer({
        size: bytesPerRow * th,
        usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ
    });
    encoder.copyTextureToBuffer({ texture: dstTex }, { buffer: readback, bytesPerRow: bytesPerRow }, [tw, th]);
    device.queue.submit([encoder.finish()]);

    await readback.mapAsync(GPUMapMode.READ);
    var mapped = new Uint8Array(readback.getMappedRange());
    var pixels = new Uint8ClampedArray(tw * th * 4);
    for (var row = 0; row < th; row++) {
        pixels.set(mapped.subarray(row * bytesPerRow, row * bytesPerRow + tw * 4), row * tw * 4);
    }
    readback.unmap();
    srcTex.destroy(); midTex.destroy(); dstTex.destroy(); readback.destroy();

    var canvas = document.createElement('canvas');
    canvas.width = tw; canvas.height = th;
    canvas.getContext('2d').putImageData(new ImageData(pixels, tw, th), 0, 0);
    return new Promise(function (resolve, reject) {
        canvas.toBlob(function (blob) {
            if (blob) resolve(blob); else reject(new Error('toBlob failed'));
        }, 'image/png');
    });
}

function targetSizeFor(img) {
    var rect = img.getBoundingClientRect();
    var needW = Math.round(rect.width * DPR);
    var needH = Math.round(rect.height * DPR);
    if (needW <= 0 || needH <= 0) return null;
    var nw = img.naturalWidth, nh = img.naturalHeight;
    if (nw <= 0 || nh <= 0) return null;
    if (nw >= needW * 0.95 && nh >= needH * 0.95) return null; /* already sharp */
    /* object-fit: cover means only one axis limits; scale to cover both */
    var scale = Math.min(needW / nw, needH / nh);
    if (img.classList.contains('hero-bg')) scale = Math.max(needW / nw, needH / nh);
    scale = Math.min(scale, MAX_SCALE);
    if (scale <= 1.05) return null;
    var tw = Math.round(nw * scale), th = Math.round(nh * scale);
    while (tw * th > MAX_PIXELS) { tw = Math.floor(tw * 0.9); th = Math.floor(th * 0.9); }
    if (tw <= nw || th <= nh) return null;
    return [tw, th];
}

var seen = new WeakMap(); /* img -> last source URL we enhanced (or tried) */

async function enhance(img) {
    if (!img || !img.complete || img.naturalWidth <= 0) return;
    var src = img.currentSrc || img.src;
    if (!src || seen.get(img) === src) return;
    var target = targetSizeFor(img);
    if (!target) { seen.set(img, src); return; }
    seen.set(img, src);
    try {
        var blob = await upscaleLanczos(img, target[0], target[1]);
        /* Bail if the theme toggle already moved on to another source. */
        if ((img.currentSrc || img.src) !== src) return;
        var url = URL.createObjectURL(blob);
        var old = img.dataset.wgpuUrl;
        img.removeAttribute('srcset'); /* keep the browser from downgrading us */
        img.src = url;
        img.dataset.wgpuUrl = url;
        if (old) URL.revokeObjectURL(old);
    } catch (e) {
        /* WebGPU unavailable/failed: the browser's own scaling remains. */
    }
}

function enhanceAll() {
    document.querySelectorAll('img.hero-bg, img.hero-logo').forEach(function (img) {
        if (img.complete) enhance(img);
        else img.addEventListener('load', function () { enhance(img); }, { once: true });
    });
}

/* Re-run when the theme toggle swaps src (and srcset), or on resize. */
var scheduled = false;
function schedule() {
    if (scheduled) return;
    scheduled = true;
    setTimeout(function () { scheduled = false; enhanceAll(); }, 150);
}

var mo = new MutationObserver(function (muts) {
    for (var i = 0; i < muts.length; i++) {
        var t = muts[i].target;
        if (t && t.tagName === 'IMG' && t.src && t.src !== t.dataset.wgpuUrl) { schedule(); return; }
    }
});

function start() {
    enhanceAll();
    mo.observe(document.documentElement, { subtree: true, attributes: true, attributeFilter: ['src', 'srcset'] });
    window.addEventListener('resize', schedule);
}

if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', start);
else start();
})();

/**
 * @file invert.js
 * @brief Browser loader for the image-inversion WASM module.
 *
 * Usage:
 *   import { createImageInverter } from './invert.js';
 *   const invert = await createImageInverter('/assets/js/invert.wasm');
 *   // rgba: Uint8Array/Uint8ClampedArray of w*h*4 pixels, alpha preserved
 *   const inverted = invert(rgba, width * height, { oklch: true });
 *
 * The math is the production image_invert_core.h compiled to WASM, so the
 * output matches the server's pre-baked dark variants.  Returns null when
 * WebAssembly is unavailable.
 */

export async function createImageInverter(wasmUrl, { glueUrl } = {}) {
    if (typeof WebAssembly === 'undefined')
        return null;
    glueUrl = glueUrl || wasmUrl.replace(/\.wasm$/, '.js');

    const createInvertModule = (await import(/* @vite-ignore */ glueUrl)).default;
    const mod = await createInvertModule({
        locateFile: (path) => (path.endsWith('.wasm') ? wasmUrl : path),
    });
    if (typeof mod._fb_invert_rgba !== 'function')
        return null;

    return function invertRgba(rgba, pixelCount, { oklch = false } = {}) {
        if (!rgba || pixelCount <= 0)
            return null;
        const inPtr = mod._malloc(rgba.length);
        mod.writeArrayToMemory(rgba, inPtr);
        const outPtr = mod._fb_invert_rgba(inPtr, pixelCount, oklch ? 1 : 0);
        mod._free(inPtr);
        if (!outPtr)
            return null;
        const out = new Uint8Array(mod.HEAPU8.buffer, outPtr, pixelCount * 4).slice();
        mod._fb_invert_free(outPtr);
        return out;
    };
}

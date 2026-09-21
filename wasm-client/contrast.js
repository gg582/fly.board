/**
 * @file contrast.js
 * @brief Browser loader for the contrast-sampling WASM module.
 *
 * Usage:
 *   import { createContrastSampler } from './contrast.js';
 *   const sample = await createContrastSampler('/assets/js/contrast.wasm');
 *   // rgb: Uint8Array of w*h*3, row-major
 *   const [L_left, L_center, L_right] = sample(rgb, width, height);
 *
 * Values are CIE L* of the left/center/right thirds of the top 60% of the
 * image, identical to the server's text-contrast heuristic.  Returns null
 * when WebAssembly is unavailable.
 */

export async function createContrastSampler(wasmUrl, { glueUrl } = {}) {
    if (typeof WebAssembly === 'undefined')
        return null;
    glueUrl = glueUrl || wasmUrl.replace(/\.wasm$/, '.js');

    const createContrastModule = (await import(/* @vite-ignore */ glueUrl)).default;
    const mod = await createContrastModule({
        locateFile: (path) => (path.endsWith('.wasm') ? wasmUrl : path),
    });
    if (typeof mod._fb_contrast_sample !== 'function')
        return null;

    return function sampleContrast(rgb, width, height) {
        if (!rgb || width < 1 || height < 1)
            return null;
        const inPtr = mod._malloc(rgb.length);
        mod.writeArrayToMemory(rgb, inPtr);
        const outPtr = mod._malloc(24);
        const rc = mod._fb_contrast_sample(inPtr, width, height, outPtr);
        mod._free(inPtr);
        if (rc !== 0) {
            mod._free(outPtr);
            return null;
        }
        const L = new Float64Array(mod.HEAPU8.buffer, outPtr, 3).slice();
        mod._free(outPtr);
        return [L[0], L[1], L[2]];
    };
}

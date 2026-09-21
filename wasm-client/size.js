/**
 * @file size.js
 * @brief Browser loader for the image-size probing WASM module.
 *
 * Usage:
 *   import { createImageSizeProbe } from './size.js';
 *   const probe = await createImageSizeProbe('/assets/js/size.wasm');
 *   const dims = probe(await file.arrayBuffer());  // { width, height } | null
 *
 * Reads dimensions from the in-memory bytes via stb_info_from_memory, the
 * same stb build the server uses.  Returns null when WebAssembly is
 * unavailable or the bytes are not a recognized image.
 */

export async function createImageSizeProbe(wasmUrl, { glueUrl } = {}) {
    if (typeof WebAssembly === 'undefined')
        return null;
    glueUrl = glueUrl || wasmUrl.replace(/\.wasm$/, '.js');

    const createSizeModule = (await import(/* @vite-ignore */ glueUrl)).default;
    const mod = await createSizeModule({
        locateFile: (path) => (path.endsWith('.wasm') ? wasmUrl : path),
    });
    if (typeof mod._fb_image_size !== 'function')
        return null;

    return function probeSize(bytes) {
        if (!bytes || bytes.byteLength === 0)
            return null;
        const data = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
        const inPtr = mod._malloc(data.length);
        mod.writeArrayToMemory(data, inPtr);
        const outPtr = mod._malloc(8);
        const rc = mod._fb_image_size(inPtr, data.length, outPtr, outPtr + 4);
        mod._free(inPtr);
        if (rc !== 0) {
            mod._free(outPtr);
            return null;
        }
        const v = new Int32Array(mod.HEAPU8.buffer, outPtr, 2).slice();
        mod._free(outPtr);
        return { width: v[0], height: v[1] };
    };
}

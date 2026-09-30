
const fs = require('fs');
const createContrastModule = require(process.argv[2]);
(async () => {
    const mod = await createContrastModule();
    const input = fs.readFileSync(process.argv[3]);
    const w = input.readInt32BE(0), h = input.readInt32BE(4);
    const px = input.length - 8;
    const inPtr = mod._malloc(px);
    mod.writeArrayToMemory(input.subarray(8), inPtr);
    const outPtr = mod._malloc(24);
    const rc = mod._fb_contrast_sample(inPtr, w, h, outPtr);
    mod._free(inPtr);
    if (rc !== 0) { console.error('sample failed'); process.exit(2); }
    const L = new Float64Array(mod.HEAPU8.buffer, outPtr, 3);
    const buf = Buffer.alloc(24);
    buf.writeDoubleLE(L[0], 0); buf.writeDoubleLE(L[1], 8); buf.writeDoubleLE(L[2], 16);
    process.stdout.write(buf);
    mod._free(outPtr);
})();

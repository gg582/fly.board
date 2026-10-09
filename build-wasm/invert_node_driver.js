
const fs = require('fs');
const createInvertModule = require(process.argv[2]);
(async () => {
    const mod = await createInvertModule();
    const input = fs.readFileSync(process.argv[3]);
    const mode = input[0];
    const px = input.length - 1;
    const inPtr = mod._malloc(px);
    mod.writeArrayToMemory(input.subarray(1), inPtr);
    const outPtr = mod._fb_invert_rgba(inPtr, px / 4, mode);
    mod._free(inPtr);
    if (!outPtr) { console.error('invert failed'); process.exit(2); }
    const out = new Uint8Array(mod.HEAPU8.buffer, outPtr, px);
    process.stdout.write(Buffer.from(out));
    mod._fb_invert_free(outPtr);
})();

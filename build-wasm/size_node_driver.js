
const fs = require('fs');
const createSizeModule = require(process.argv[2]);
(async () => {
    const mod = await createSizeModule();
    const input = fs.readFileSync(process.argv[3]);
    const inPtr = mod._malloc(input.length);
    mod.writeArrayToMemory(input, inPtr);
    const outPtr = mod._malloc(8);
    const rc = mod._fb_image_size(inPtr, input.length, outPtr, outPtr + 4);
    mod._free(inPtr);
    if (rc !== 0) { console.error('probe failed'); process.exit(3); }
    const v = new Int32Array(mod.HEAPU8.buffer, outPtr, 2);
    const buf = Buffer.alloc(8);
    buf.writeInt32BE(v[0], 0); buf.writeInt32BE(v[1], 4);
    process.stdout.write(buf);
    mod._free(outPtr);
})();

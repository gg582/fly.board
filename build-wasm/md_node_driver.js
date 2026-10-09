
const fs = require('fs');
const createMdModule = require(process.argv[2]);
(async () => {
    const mod = await createMdModule();
    const fixture = fs.readFileSync(process.argv[3], 'utf8');
    const inPtr = mod._malloc(fixture.length + 1);
    mod.stringToUTF8(fixture, inPtr, fixture.length + 1);
    const outPtr = mod._fb_md_render(inPtr);
    mod._free(inPtr);
    if (!outPtr) { console.error('render failed'); process.exit(2); }
    process.stdout.write(mod.UTF8ToString(outPtr));
    mod._fb_md_free(outPtr);
})();

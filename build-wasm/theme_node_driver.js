
const fs = require('fs');
const createThemeModule = require(process.argv[2]);
(async () => {
    const mod = await createThemeModule();
    const input = fs.readFileSync(process.argv[3]);
    const mode = input[0];
    const overrides = input.slice(1).toString('utf8');
    const oPtr = overrides ? mod._malloc(overrides.length + 1) : 0;
    if (oPtr) mod.stringToUTF8(overrides, oPtr, overrides.length + 1);
    let outPtr;
    if (mode === 2) outPtr = mod._fb_theme_all_json(oPtr);
    else outPtr = mod._fb_theme_css(mode, oPtr);
    if (oPtr) mod._free(oPtr);
    if (!outPtr) { console.error('theme build failed'); process.exit(2); }
    process.stdout.write(mod.UTF8ToString(outPtr));
    mod._fb_theme_free(outPtr);
})();

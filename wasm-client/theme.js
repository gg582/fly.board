/**
 * @file theme.js
 * @brief Browser loader for the theme-generation WASM module.
 *
 * Usage:
 *   import { createThemeGenerator } from './theme.js';
 *   const theme = await createThemeGenerator('/assets/js/theme.wasm');
 *   styleEl.textContent = theme.css(false, { accent: '#AA3333', roundness: 0.5 });
 *
 * The module compiles the production src/render/theme pipeline, so the
 * output matches the server's theme_build_css.  Overrides accepted: accent,
 * roundness, bg_full_light, bg_full_dark, use_special_modes.  Returns null
 * when WebAssembly is unavailable.
 */

export async function createThemeGenerator(wasmUrl, { glueUrl } = {}) {
    if (typeof WebAssembly === 'undefined')
        return null;
    glueUrl = glueUrl || wasmUrl.replace(/\.wasm$/, '.js');

    const createThemeModule = (await import(/* @vite-ignore */ glueUrl)).default;
    const mod = await createThemeModule({
        locateFile: (path) => (path.endsWith('.wasm') ? wasmUrl : path),
    });
    if (typeof mod._fb_theme_css !== 'function')
        return null;

    const withOverrides = (fn) => (darkMode, overrides) => {
        const json = overrides ? JSON.stringify(overrides) : '';
        const oPtr = json ? mod._malloc(json.length + 1) : 0;
        if (oPtr)
            mod.stringToUTF8(json, oPtr, json.length + 1);
        const outPtr = fn(darkMode ? 1 : 0, oPtr);
        if (oPtr)
            mod._free(oPtr);
        if (!outPtr)
            return '';
        const out = mod.UTF8ToString(outPtr);
        mod._fb_theme_free(outPtr);
        return out;
    };

    return {
        css: withOverrides(mod._fb_theme_css),
        json: withOverrides(mod._fb_theme_json),
        allJson: (overrides) => {
            const json = overrides ? JSON.stringify(overrides) : '';
            const oPtr = json ? mod._malloc(json.length + 1) : 0;
            if (oPtr)
                mod.stringToUTF8(json, oPtr, json.length + 1);
            const outPtr = mod._fb_theme_all_json(oPtr);
            if (oPtr)
                mod._free(oPtr);
            if (!outPtr)
                return '';
            const out = mod.UTF8ToString(outPtr);
            mod._fb_theme_free(outPtr);
            return out;
        },
    };
}

/**
 * @file md_render.js
 * @brief Browser loader for the markdown WASM module.
 *
 * Usage:
 *   import { createMarkdownRenderer } from './md_render.js';
 *   const render = await createMarkdownRenderer('/assets/js/md_render.wasm');
 *   container.innerHTML = render(markdownSource);
 *
 * The module is the production render_markdown_to_html pipeline compiled to
 * WASM (see wasm-client/markdown_module.c); output is byte-identical to the
 * server except for the width/height attributes only the server can inject.
 *
 * Falls back to null when WebAssembly is unavailable so callers can keep
 * their server-rendered HTML.
 */

export async function createMarkdownRenderer(wasmUrl, { glueUrl } = {}) {
    if (typeof WebAssembly === 'undefined')
        return null;
    glueUrl = glueUrl || wasmUrl.replace(/\.wasm$/, '.js');

    const createMdModule = (await import(/* @vite-ignore */ glueUrl)).default;
    const mod = await createMdModule({
        locateFile: (path) => (path.endsWith('.wasm') ? wasmUrl : path),
    });
    if (typeof mod._fb_md_render !== 'function')
        return null;

    return function renderMarkdown(md) {
        if (!md)
            return '';
        const inPtr = mod._malloc(md.length + 1);
        mod.stringToUTF8(md, inPtr, md.length + 1);
        const outPtr = mod._fb_md_render(inPtr);
        mod._free(inPtr);
        if (!outPtr)
            return '';
        const html = mod.UTF8ToString(outPtr);
        mod._fb_md_free(outPtr);
        return html;
    };
}

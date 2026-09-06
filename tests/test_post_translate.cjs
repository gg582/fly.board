'use strict';

// Run with: node --test tests/test_post_translate.cjs
// Exercise the shipped script without a server, network, or model download.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const vm = require('node:vm');

const root = path.join(__dirname, '..');
const renderer = fs.readFileSync(path.join(root, 'src/render/render_post.c'), 'utf8');
const script = fs.readFileSync(path.join(root, 'public/js/post-translate.js'), 'utf8');
const selectHtml = renderer.match(/<select id='post-translate-target'[^]*?<\/select>/)[0];
const targets = [...selectHtml.matchAll(/<option value='([^']+)'>([^<]+)<\/option>/g)]
    .map((match) => ({ value: match[1], label: match[2] }));

function element(tag, text = '') {
    // These fixtures contain plain text. Keep both views synchronized like a DOM
    // element, so a missing restore write cannot pass on stale innerHTML.
    let content = text;
    return {
        nodeType: 1, tagName: tag.toUpperCase(),
        get textContent() { return content; },
        set textContent(value) { content = value; },
        get innerHTML() { return content; },
        set innerHTML(value) { content = value; },
        childNodes: [], style: {}, dataset: {}, attributes: {}, listeners: {},
        classList: { contains: () => false },
        querySelectorAll: () => [],
        cloneNode() { return element(tag, this.textContent); },
        setAttribute(name, value) { this.attributes[name] = value; },
        addEventListener(name, listener) { this.listeners[name] = listener; },
    };
}

function page(text, fetchImpl) {
    const paragraph = element('p', text);
    const source = element('div');
    source.childNodes = [paragraph];
    const button = element('button');
    const select = element('select');
    select.options = targets.map((t) => ({ value: t.value, text: t.label, disabled: false, hidden: false }));
    const status = element('span');
    const requests = [];
    const ids = {
        'post-translate': button,
        'post-translate-target': select,
        'translation-msg': status,
    };
    vm.runInNewContext(script, {
        document: {
            getElementById: (id) => ids[id],
            querySelector: (selector) => selector === 'article .markdown-body' ? source : null,
        },
        window: { fetch: true },
        Node: { ELEMENT_NODE: 1 },
        AbortController,
        // Do not wait for UI message/timeout timers in unit tests.
        setTimeout: () => 1,
        clearTimeout: () => {},
        console: { warn: () => {}, error: () => {} },
        fetch: async (url, options) => {
            const body = JSON.parse(options.body);
            requests.push({ url, body });
            return fetchImpl(body);
        },
    });
    return { paragraph, button, select, status, requests };
}

const success = () => ({
    ok: true,
    json: async () => ({ ok: true, parts: ['軟體開發與網路安全。'] }),
});

test('language picker exposes distinct Simplified and Traditional Chinese targets', () => {
    assert.equal(targets.find((target) => target.value === 'zh')?.label, 'Chinese (Simplified)');
    assert.equal(targets.find((target) => target.value === 'zh-TW')?.label, 'Chinese (Traditional)');
    assert.equal(new Set(targets.map((target) => target.value)).size, targets.length);
});

for (const [source, text] of [
    ['en', 'Software development and network security.'],
    ['ko', '소프트웨어 개발과 네트워크 보안.'],
    ['zh', '软件开发与网络安全。'],
]) {
    test(`${source} to Traditional Chinese reaches the API and can restore the original`, async () => {
        const p = page(text, success);
        p.select.value = 'zh-TW';
        await p.button.listeners.click();
        assert.deepEqual(p.requests, [{
            url: '/api/translate', body: { source, target: 'zh-TW', chunks: [text] },
        }]);
        assert.equal(p.paragraph.textContent, '軟體開發與網路安全。');
        assert.equal(p.button.dataset.translationState, 'rendered');
        assert.equal(p.button.attributes['aria-pressed'], 'true');
        await p.button.listeners.click();
        assert.equal(p.paragraph.innerHTML, text);
        assert.equal(p.button.dataset.translationState, 'idle');
        assert.equal(p.button.attributes['aria-pressed'], 'false');
        assert.equal(p.requests.length, 1);
    });
}

test('changing from Traditional to Simplified Chinese restores the original before translating', async () => {
    const text = 'Software development and network security.';
    const p = page(text, success);
    p.select.value = 'zh-TW';
    await p.button.listeners.click();
    p.select.value = 'zh';
    p.select.listeners.change();
    assert.equal(p.paragraph.innerHTML, text);
    assert.equal(p.button.dataset.translationState, 'idle');
    assert.equal(p.button.attributes['aria-pressed'], 'false');
    await p.button.listeners.click();
    assert.equal(p.requests.length, 2);
    assert.deepEqual(p.requests[1].body, { source: 'en', target: 'zh', chunks: [text] });
});

test('provider and on-device fallback failure restores the original and reports an error', async () => {
    const text = 'Software development and network security.';
    const p = page(text, () => ({
        ok: false,
        json: async () => ({ ok: false, error: 'translation upstream unavailable' }),
    }));
    // The sandbox deliberately cannot import a remote WASM translator.
    p.select.value = 'zh-TW';
    await p.button.listeners.click();
    assert.equal(p.paragraph.innerHTML, text);
    assert.equal(p.paragraph.style.opacity, '1');
    assert.equal(p.button.dataset.translationState, 'error');
    assert.equal(p.button.disabled, false);
    assert.equal(p.select.disabled, false);
    assert.match(p.status.textContent, /Translation is temporarily unavailable/);
});

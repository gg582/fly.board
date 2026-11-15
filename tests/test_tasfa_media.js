'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');

class Element {
    constructor(tagName) {
        this.tagName = tagName.toUpperCase();
        this.style = {};
        this.dataset = {};
        this.attributes = {};
        this.children = [];
        this.parentNode = null;
        this.className = '';
        this.src = '';
    }
    setAttribute(name, value) {
        this.attributes[name] = String(value);
        if (name === 'src') this.src = String(value);
    }
    getAttribute(name) {
        return this.attributes[name] === undefined ? null : this.attributes[name];
    }
    removeAttribute(name) { delete this.attributes[name]; }
    appendChild(child) {
        this.children.push(child);
        child.parentNode = this;
        return child;
    }
    insertBefore(child) { return this.appendChild(child); }
    replaceChild(next, previous) {
        const index = this.children.indexOf(previous);
        if (index >= 0) this.children[index] = next;
        next.parentNode = this;
        previous.parentNode = null;
        return next;
    }
    addEventListener() {}
    querySelector(selector) {
        if (selector === '.tasfa-media-loader') {
            return this.children.find(child => child.className === selector.slice(1)) || null;
        }
        return null;
    }
    closest() { return null; }
}

function contextFor(fetchImpl) {
    const document = {
        readyState: 'complete',
        documentElement: {},
        createElement: tag => new Element(tag),
        querySelectorAll: () => [],
        addEventListener: () => {}
    };
    const window = {
        TASFA_MEDIA_HANDSHAKE_TIMEOUT_MS: 40,
        AbortController,
        Blob,
        Response,
        URL,
        TextEncoder,
        TextDecoder,
        Uint8Array,
        Int32Array,
        Float64Array,
        setTimeout,
        clearTimeout,
        setInterval,
        clearInterval,
        fetch: fetchImpl,
        navigator: { hardwareConcurrency: 4, userAgent: 'test' },
        localStorage: { getItem: () => null, setItem: () => {} },
        caches: { open: async () => ({ match: async () => null, put: async () => {} }) },
        addEventListener: () => {},
        __fbClientCoreWasm: {
            exports: { fb_cc_normalize_dl_session: () => {} },
            outI32: () => new Array(12).fill(1)
        }
    };
    return { window, document };
}

function install(fetchImpl) {
    const { window, document } = contextFor(fetchImpl);
    vm.runInNewContext(fs.readFileSync(process.env.TASFA_MEDIA_SOURCE || 'public/js/tasfa-download.js', 'utf8'), {
        window,
        document,
        fetch: fetchImpl,
        navigator: window.navigator,
        localStorage: window.localStorage,
        caches: window.caches,
        AbortController,
        Blob,
        Response,
        URL,
        TextEncoder,
        TextDecoder,
        Uint8Array,
        Int32Array,
        Float64Array,
        setTimeout,
        clearTimeout,
        setInterval,
        clearInterval,
        console
    });
    return window;
}

function mediaElement() {
    const el = new Element('img');
    el.setAttribute('src', '/file/download/253');
    const parent = new Element('div');
    parent.appendChild(el);
    return el;
}

const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
const findByTag = (root, tagName) => {
    if (!root) return null;
    if (root.tagName === tagName) return root;
    for (const child of root.children || []) {
        const found = findByTag(child, tagName);
        if (found) return found;
    }
    return null;
};

(async () => {
    let handshakes = 0;
    let heads = 0;
    const fatalWindow = install(async (url, options = {}) => {
        if (url.includes('/handshake')) {
            handshakes++;
            return { ok: false, status: 404, json: async () => ({}) };
        }
        if (options.method === 'HEAD') {
            heads++;
            return { ok: true, status: 200, headers: { get: () => 'video/mp4' } };
        }
        throw new Error('unexpected fetch: ' + url);
    });
    fatalWindow.upgradeTasfaMedia(mediaElement());
    await delay(80);
    assert.equal(handshakes, 1, 'fatal handshake must not enter exponential retries');
    assert.equal(heads, 1, 'native fallback must probe the media MIME once');

    const timeoutWindow = install(async (url, options = {}) => {
        if (url.includes('/handshake')) return new Promise(() => {});
        if (options.method === 'HEAD') {
            return { ok: true, status: 200, headers: { get: () => 'video/mp4' } };
        }
        throw new Error('unexpected fetch: ' + url);
    });
    const timeoutElement = mediaElement();
    const timeoutParent = timeoutElement.parentNode;
    timeoutWindow.upgradeTasfaMedia(timeoutElement);
    await delay(120);
    assert.equal(timeoutParent.tagName, 'DIV');
    const native = findByTag(timeoutParent, 'VIDEO');
    assert(native, 'timed-out handshake must produce a native video player');
    assert.equal(native.src, '/file/download/253?w=896&h=896');

    let successHeads = 0;
    const successWindow = install(async (url, options = {}) => {
        if (url.includes('/handshake')) {
            return {
                ok: true,
                status: 200,
                json: async () => ({
                    session_id: 'session-id', session_token: 'session-token',
                    chunk_count: 2, chunk_size: 50, total_size: 100,
                    mime_type: 'video/mp4', filename: 'movie.mp4'
                })
            };
        }
        if (options.method === 'HEAD') {
            successHeads++;
            return { ok: true, status: 200, headers: { get: () => 'video/mp4' } };
        }
        throw new Error('unexpected fetch: ' + url);
    });
    const successElement = mediaElement();
    const successParent = successElement.parentNode;
    successWindow.upgradeTasfaMedia(successElement);
    await delay(60);
    const sessionPlayer = findByTag(successParent, 'VIDEO');
    assert(sessionPlayer, 'successful handshake must produce a player');
    assert.match(sessionPlayer.src, /session_id=session-id/);
    assert.match(sessionPlayer.src, /session_token=session-token/);
    assert.equal(successHeads, 0, 'successful handshake must not invoke fallback HEAD');

    console.log('TASFA media fatal, timeout, and successful handshake tests passed');
})().catch(error => {
    console.error(error);
    process.exitCode = 1;
});

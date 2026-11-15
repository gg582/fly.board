'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const { MessageChannel } = require('node:worker_threads');

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
        if (name.startsWith('data-')) {
            const key = name.slice(5).replace(/-([a-z])/g, (_, letter) => letter.toUpperCase());
            this.dataset[key] = String(value);
        }
    }
    getAttribute(name) {
        return this.attributes[name] === undefined ? null : this.attributes[name];
    }
    removeAttribute(name) {
        delete this.attributes[name];
        if (name === 'src') this.src = '';
        if (name.startsWith('data-')) {
            const key = name.slice(5).replace(/-([a-z])/g, (_, letter) => letter.toUpperCase());
            delete this.dataset[key];
        }
    }
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
    play() { return Promise.resolve(); }
    load() {}
    querySelector(selector) {
        if (selector === '.tasfa-media-loader') {
            return this.children.find(child => child.className === selector.slice(1)) || null;
        }
        return null;
    }
    closest() { return null; }
}

function testSetTimeout(callback, milliseconds, ...args) {
    const timer = setTimeout(callback, milliseconds, ...args);
    if (milliseconds >= 5000 && typeof timer.unref === 'function') timer.unref();
    return timer;
}

function serviceWorkerStub(streamChunks) {
    return {
        controller: {
            postMessage(message, transfer) {
                if (message.type === 'TASFA_STREAM_OPEN') {
                    transfer[0].postMessage({ type: 'TASFA_STREAM_READY', streamId: message.streamId });
                    transfer[0].close();
                } else if (message.type === 'TASFA_STREAM_CHUNK') {
                    streamChunks.push(new Uint8Array(message.chunk));
                }
            }
        }
    };
}

function contextFor(fetchImpl, options) {
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
        setTimeout: testSetTimeout,
        clearTimeout,
        setInterval,
        clearInterval,
        fetch: fetchImpl,
        location: { origin: 'https://blog.test' },
        navigator: {
            hardwareConcurrency: 4,
            userAgent: 'test',
            serviceWorker: options.serviceWorker
        },
        localStorage: { getItem: () => null, setItem: () => {} },
        caches: { open: async () => ({ match: async () => null, put: async () => {} }) },
        addEventListener: () => {},
        __fbClientCoreWasm: {
            loadSamples: samples => samples.length,
            exports: {
                fb_cc_normalize_dl_session: () => {},
                fb_cc_predicted_quality: () => 0.5,
                fb_cc_apply_predicted_dl_rule: () => {},
                fb_cc_tune_dl_success: () => {},
                fb_cc_tune_dl_failure: () => {}
            },
            outI32: () => new Array(12).fill(1),
            outF64: () => new Array(2).fill(0.5)
        }
    };
    return { window, document };
}

function install(fetchImpl, options = {}) {
    const { window, document } = contextFor(fetchImpl, options);
    const testConsole = { log: console.log, error: console.error, warn: () => {} };
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
        XMLHttpRequest: options.XMLHttpRequest,
        MessageChannel,
        setTimeout: testSetTimeout,
        clearTimeout,
        setInterval,
        clearInterval,
        console: testConsole
    });
    return window;
}

function mediaElement(tagName = 'img') {
    const el = new Element(tagName);
    el.setAttribute('src', '/file/download/253');
    const parent = new Element('div');
    parent.appendChild(el);
    return el;
}

const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
async function waitFor(predicate, description) {
    const deadline = Date.now() + 1500;
    while (!predicate()) {
        if (Date.now() > deadline) throw new Error('timed out waiting for ' + description);
        await delay(5);
    }
}
const findByTag = (root, tagName) => {
    if (!root) return null;
    if (root.tagName === tagName) return root;
    for (const child of root.children || []) {
        const found = findByTag(child, tagName);
        if (found) return found;
    }
    return null;
};

function handshakeResponse() {
    return {
        ok: true,
        status: 200,
        json: async () => ({
            session_id: 'session-id', session_token: 'session-token',
            chunk_count: 2, chunk_size: 50, total_size: 100,
            mime_type: 'video/mp4', filename: 'movie.mp4',
            supports_progressive_streaming: true
        })
    };
}

function chunkXhrStub(chunkUrls, failureStatus = 0) {
    return class {
        open(method, url) { this.method = method; this.url = url; }
        setRequestHeader() {}
        getResponseHeader() { return null; }
        send() {
            chunkUrls.push(this.url);
            setTimeout(() => {
                const match = this.url.match(/\/chunk\/(\d+)/);
                this.status = failureStatus || (match ? 200 : 404);
                this.response = this.status === 200 && match ? new Uint8Array(50).fill(Number(match[1]) + 1).buffer : null;
                this.onload();
            }, 0);
        }
        abort() { if (this.onabort) this.onabort(); }
    };
}

(async () => {
    let handshakes = 0;
    let heads = 0;
    const fatalWindow = install(async (url, options = {}) => {
        if (options.method === 'HEAD') heads++;
        if (url.includes('/handshake')) {
            handshakes++;
            return { ok: false, status: 404, json: async () => ({}) };
        }
        throw new Error('unexpected fetch: ' + url);
    });
    const fatalElement = mediaElement();
    fatalWindow.upgradeTasfaMedia(fatalElement);
    await waitFor(() => fatalElement.dataset.tasfaError === '1', 'fatal handshake error');
    assert.equal(handshakes, 1, 'fatal handshake must not enter exponential retries');
    assert.equal(heads, 0, 'fatal handshake must not issue a native HEAD request');
    assert.equal(findByTag(fatalElement.parentNode, 'VIDEO'), null, 'fatal handshake must not create a native player');

    const timeoutWindow = install(async (url, options = {}) => {
        if (options.method === 'HEAD') heads++;
        if (url.includes('/handshake')) return new Promise(() => {});
        throw new Error('unexpected fetch: ' + url);
    });
    const timeoutElement = mediaElement();
    timeoutWindow.upgradeTasfaMedia(timeoutElement);
    await waitFor(() => timeoutElement.dataset.tasfaError === '1', 'handshake timeout error');
    assert.equal(heads, 0, 'handshake timeout must not issue a native HEAD request');
    assert.equal(findByTag(timeoutElement.parentNode, 'VIDEO'), null, 'handshake timeout must fail without native playback');

    const chunkUrls = [];
    const streamChunks = [];
    let successHeads = 0;
    let completed = 0;
    const successWindow = install(async (url, options = {}) => {
        if (options.method === 'HEAD') successHeads++;
        if (url.includes('/handshake')) return handshakeResponse();
        if (url === '/file/download/complete') {
            completed++;
            return { ok: true, status: 200 };
        }
        throw new Error('unexpected non-TASFA fetch: ' + url);
    }, {
        XMLHttpRequest: chunkXhrStub(chunkUrls),
        serviceWorker: serviceWorkerStub(streamChunks)
    });
    const successElement = mediaElement();
    const successParent = successElement.parentNode;
    successWindow.upgradeTasfaMedia(successElement);
    await waitFor(() => {
        const player = findByTag(successParent, 'VIDEO');
        return player && player.src.startsWith('/__tasfa_stream__/');
    }, 'TASFA progressive player');
    const sessionPlayer = findByTag(successParent, 'VIDEO');
    assert(!sessionPlayer.src.includes('session_id='), 'playback URL must not expose a native session URL');
    assert.equal(successHeads, 0, 'successful playback must not invoke native HEAD');
    await waitFor(() => completed === 1, 'TASFA completion');
    assert.equal(chunkUrls.length, 2, 'media bytes must arrive through TASFA chunks');
    assert(chunkUrls.every(url => url.includes('/chunk/') && url.includes('session_token=session-token')));
    assert.equal(streamChunks.length, 2, 'decrypted media chunks must feed the Service Worker stream');
    assert.deepEqual(Array.from(streamChunks[0].slice(0, 2)), [1, 1]);
    assert.deepEqual(Array.from(streamChunks[1].slice(0, 2)), [2, 2]);

    const videoChunkUrls = [];
    const videoStreamChunks = [];
    const videoWindow = install(async url => {
        if (url.includes('/handshake')) return handshakeResponse();
        if (url === '/file/download/complete') return { ok: true, status: 200 };
        throw new Error('unexpected non-TASFA fetch: ' + url);
    }, {
        XMLHttpRequest: chunkXhrStub(videoChunkUrls),
        serviceWorker: serviceWorkerStub(videoStreamChunks)
    });
    const video = mediaElement('video');
    videoWindow.upgradeTasfaMedia(video);
    await waitFor(() => video.src.startsWith('/__tasfa_stream__/'), 'native element TASFA upgrade');
    assert(!video.src.includes('/file/download/'), 'video element must not retain its direct source');
    assert.equal(videoChunkUrls.length, 2, 'native video elements must also use TASFA chunks');

    const noSwChunkUrls = [];
    const noSwWindow = install(async url => {
        if (url.includes('/handshake')) return handshakeResponse();
        throw new Error('unexpected non-TASFA fetch: ' + url);
    }, { XMLHttpRequest: chunkXhrStub(noSwChunkUrls) });
    const noSwElement = mediaElement();
    const noSwParent = noSwElement.parentNode;
    noSwWindow.upgradeTasfaMedia(noSwElement);
    await waitFor(() => {
        const player = findByTag(noSwParent, 'VIDEO');
        return player && player.src.startsWith('blob:');
    }, 'protected playback without a Service Worker');
    assert.equal(noSwChunkUrls.length, 2, 'missing Service Worker must retain protected TASFA chunk transfer');
    assert.equal(noSwElement.dataset.tasfaError, undefined, 'protected blob playback must not be marked as an error');

    const animationChunkUrls = [];
    const animationStreamChunks = [];
    const animationWindow = install(async url => {
        if (url.includes('/handshake')) return handshakeResponse();
        if (url === '/file/download/complete') return { ok: true, status: 200 };
        throw new Error('unexpected non-TASFA fetch: ' + url);
    }, {
        XMLHttpRequest: chunkXhrStub(animationChunkUrls),
        serviceWorker: serviceWorkerStub(animationStreamChunks)
    });
    const animation = mediaElement();
    const animationParent = animation.parentNode;
    animation.setAttribute('data-tasfa-animation-url', '/file/preview/253');
    const animationRoot = new Element('div');
    animationRoot.querySelectorAll = selector => selector === 'img[data-tasfa-animation-url]' ? [animation] : [];
    animationWindow.initMarkdownAffordances(animationRoot);
    await waitFor(() => {
        const player = findByTag(animationParent, 'VIDEO');
        return player && player.src.startsWith('/__tasfa_stream__/');
    }, 'animated TASFA preview');
    const animationPlayer = findByTag(animationParent, 'VIDEO');
    assert.equal(animationPlayer.autoplay, true);
    assert.equal(animationPlayer.loop, true);
    assert.equal(animationPlayer.muted, true);
    assert.equal(animationChunkUrls.length, 2, 'animated previews must use TASFA chunks instead of a Range probe');

    const fatalChunkUrls = [];
    const fatalChunkWindow = install(async url => {
        if (url.includes('/handshake')) return handshakeResponse();
        throw new Error('unexpected non-TASFA fetch: ' + url);
    }, {
        XMLHttpRequest: chunkXhrStub(fatalChunkUrls, 404),
        serviceWorker: serviceWorkerStub([])
    });
    const fatalChunkElement = mediaElement();
    fatalChunkWindow.upgradeTasfaMedia(fatalChunkElement);
    await waitFor(() => fatalChunkElement.dataset.tasfaError === '1', 'fatal chunk error');
    assert(fatalChunkUrls.length > 0, 'fatal chunk test must exercise the chunk endpoint');
    assert(fatalChunkUrls.every(url => url.includes('/chunk/')));
    assert.equal(findByTag(fatalChunkElement.parentNode, 'VIDEO'), null, 'fatal chunks must not enable native playback');

    const silentSwChunkUrls = [];
    let silentPort = null;
    const silentServiceWorker = {
        controller: {
            postMessage(message, transfer) {
                if (message.type === 'TASFA_STREAM_OPEN') silentPort = transfer[0];
                if (message.type === 'TASFA_STREAM_CLOSE' && silentPort) {
                    silentPort.close();
                    silentPort = null;
                }
            }
        }
    };
    const silentSwWindow = install(async url => {
        if (url.includes('/handshake')) return handshakeResponse();
        throw new Error('unexpected non-TASFA fetch: ' + url);
    }, {
        XMLHttpRequest: chunkXhrStub(silentSwChunkUrls),
        serviceWorker: silentServiceWorker
    });
    const silentSwElement = mediaElement();
    const silentSwParent = silentSwElement.parentNode;
    silentSwWindow.upgradeTasfaMedia(silentSwElement);
    await waitFor(() => {
        const player = findByTag(silentSwParent, 'VIDEO');
        return player && (player.src.startsWith('blob:') || player.src.startsWith('/__tasfa_media__/'));
    }, 'protected playback after a Service Worker timeout');
    assert.equal(silentSwChunkUrls.length, 2, 'an unacknowledged stream must retain protected TASFA chunk transfer');
    assert(!findByTag(silentSwParent, 'VIDEO').src.includes('/file/download/'));
    assert.equal(silentSwElement.dataset.tasfaError, undefined);

    console.log('TASFA media tests passed without native playback fallback');
})().catch(error => {
    console.error(error);
    process.exitCode = 1;
});

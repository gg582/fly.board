'use strict';

const assert = require('node:assert/strict');
const {startRealtime} = require('../public/js/realtime.js');

class Events {
    constructor() { this.listeners = {}; }
    addEventListener(name, fn) { (this.listeners[name] ||= []).push(fn); }
    emit(name, event) { (this.listeners[name] || []).forEach(fn => fn(event)); }
}

class Source extends Events {
    static instances = [];
    constructor(url) {
        super();
        this.url = url;
        this.closed = false;
        Source.instances.push(this);
    }
    close() { this.closed = true; }
}

const store = new Map();
const timers = new Map();
let timerId = 0;
const beacons = [];
const fetches = [];
const listeners = {};
const win = {
    EventSource: Source,
    URLSearchParams,
    Uint8Array,
    sessionStorage: {
        getItem: key => store.get(key) || null,
        setItem: (key, value) => store.set(key, value)
    },
    crypto: {getRandomValues: bytes => bytes.fill(7)},
    navigator: {sendBeacon: (path, body) => beacons.push([path, body.toString()])},
    fetch: (url, options = {}) => {
        fetches.push([url, options]);
        if (url.startsWith('/realtime/comments/')) {
            return Promise.resolve({ok: true, text: () => Promise.resolve('<div>new comment</div>')});
        }
        return Promise.resolve({ok: true});
    },
    setTimeout: fn => { timers.set(++timerId, fn); return timerId; },
    clearTimeout: id => timers.delete(id),
    setInterval: fn => { timers.set(++timerId, fn); return timerId; },
    clearInterval: id => timers.delete(id),
    addEventListener: (name, fn) => { (listeners[name] ||= []).push(fn); },
    removeEventListener: () => {}
};

const commentList = {innerHTML: ''};
const state = {hidden: true, textContent: ''};
const typingState = {hidden: true, textContent: ''};
const root = new Events();
root.getAttribute = name => ({'data-realtime-post': '12', 'data-latest-comment': '8', 'data-typing': '1'})[name];
root.removeEventListener = () => {};
const doc = {
    getElementById: id => id === 'comments-list' ? commentList : id === 'realtime-viewers' ? state : typingState
};

(async () => {
    const rt = startRealtime(root, {window: win, document: doc});
    assert(rt);
    assert.match(Source.instances[0].url, /after=8/);
    assert.match(Source.instances[0].url, /session=07070707070707070707070707070707/);
    assert.equal(fetches[0][0], '/realtime/presence');

    rt.source.emit('state', {data: JSON.stringify({viewers: '2-5 viewers', typing: 1})});
    assert.equal(state.textContent, '2-5 viewers');
    rt.source.emit('comment', {data: '{"comment_id":9}', lastEventId: '9'});
    await new Promise(resolve => setImmediate(resolve));
    await new Promise(resolve => setImmediate(resolve));
    assert.equal(commentList.innerHTML, '<div>new comment</div>');

    root.emit('input', {target: {tagName: 'TEXTAREA', name: 'content', value: 'draft'}});
    assert(timers.size > 1);
    for (const [id, fn] of [...timers]) {
        timers.delete(id);
        fn();
    }
    assert(fetches.some(([url]) => url === '/realtime/typing'), JSON.stringify({fetches, timers: [...timers]}));
    for (const fn of listeners.pagehide) fn();
    assert.equal(beacons.length, 2);
    assert.match(beacons[0][1], /active=0/);
    rt.stop();
    assert(rt.source.closed);
    console.log('realtime browser reconnect/event tests passed');
})().catch(error => { console.error(error); process.exitCode = 1; });

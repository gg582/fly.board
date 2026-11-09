'use strict';
// In-page comment submit: swaps #comments-list from the JSON fragment, shows
// errors from the status body, and falls back to a native submit otherwise.
const assert = require('assert');
const {bind} = require('../public/js/comments.js');

function makeEnv(fetchImpl) {
    const listeners = {};
    const list = {innerHTML: 'old'};
    const nodes = {};
    const doc = {
        addEventListener: (t, fn) => { listeners[t] = fn; },
        removeEventListener: () => {},
        getElementById: id => id === 'comments-list' ? list : nodes[id] || null,
        createElement: () => ({setAttribute() {}, style: {}, hidden: false, textContent: ''})
    };
    const nativeSubmits = [];
    const win = {
        fetch: fetchImpl,
        URLSearchParams: class { constructor(fd) { this.fd = fd; } toString() { return 'content=x'; } },
        FormData: class { constructor(f) { this.f = f; } },
        HTMLFormElement: {prototype: {submit() { nativeSubmits.push(this); }}},
        history: {replaceState(_, __, hash) { win.hash = hash; }}
    };
    return {win, doc, list, nodes, listeners, nativeSubmits};
}

function makeForm(action) {
    const textarea = {value: 'hello'};
    const button = {disabled: false};
    const children = {};
    return {
        dataset: {}, textarea, button,
        getAttribute: n => n === 'action' ? action : null,
        matches: sel => sel.indexOf('action="' + action + '"') !== -1,
        querySelector: sel => sel.indexOf('textarea') === 0 ? textarea
            : sel.indexOf('button') === 0 ? button
            : sel === '.comment-status' ? children.status || null : null,
        appendChild: el => { children.status = el; },
        children
    };
}

function response(status, type, body) {
    return Promise.resolve({
        ok: status >= 200 && status < 300, status,
        headers: {get: () => type},
        json: () => Promise.resolve(JSON.parse(body)),
        text: () => Promise.resolve(body)
    });
}

const tick = () => new Promise(r => setTimeout(r, 0));

(async () => {
    // Success: list swapped, textarea cleared, anchor set, header sent.
    let sent;
    let env = makeEnv((url, opts) => { sent = {url, opts}; return response(200, 'application/json; charset=utf-8',
        JSON.stringify({id: 7, pending: false, html: "<div id='comment-7'>new</div>"})); });
    env.nodes['comment-7'] = {scrollIntoView() {}};
    bind({window: env.win, document: env.doc});
    let form = makeForm('/comment/new');
    let prevented = false;
    env.listeners.submit({target: form, defaultPrevented: false, preventDefault() { prevented = true; }});
    assert.ok(prevented, 'submit is taken over');
    assert.equal(form.button.disabled, true, 'button disabled while sending');
    await tick(); await tick(); await tick();
    assert.equal(sent.url, '/comment/new');
    assert.equal(sent.opts.headers['X-Fly-Comment'], 'fragment');
    assert.equal(env.list.innerHTML, "<div id='comment-7'>new</div>");
    assert.equal(form.textarea.value, '', 'textarea cleared');
    assert.equal(form.button.disabled, false);
    assert.equal(env.win.hash, '#comment-7');

    // Error: message shown, list untouched, text kept.
    env = makeEnv(() => response(429, 'text/plain', 'Too many submissions'));
    bind({window: env.win, document: env.doc});
    form = makeForm('/comment/new');
    env.listeners.submit({target: form, defaultPrevented: false, preventDefault() {}});
    await tick(); await tick(); await tick();
    assert.equal(env.list.innerHTML, 'old');
    assert.equal(form.textarea.value, 'hello');
    assert.ok(form.children.status.textContent.indexOf('Too many') === 0, 'error shown');

    // Non-JSON success (login page after redirect): native submit.
    env = makeEnv(() => response(200, 'text/html', '<html>login</html>'));
    bind({window: env.win, document: env.doc});
    form = makeForm('/comment/new');
    env.listeners.submit({target: form, defaultPrevented: false, preventDefault() {}});
    await tick(); await tick(); await tick();
    assert.equal(env.nativeSubmits.length, 1, 'falls back to native submit');

    // Other forms and cancelled submits are left alone.
    let called = false;
    env = makeEnv(() => { called = true; return response(200, 'application/json', '{}'); });
    bind({window: env.win, document: env.doc});
    env.listeners.submit({target: makeForm('/report'), defaultPrevented: false, preventDefault() {}});
    env.listeners.submit({target: makeForm('/comment/new'), defaultPrevented: true, preventDefault() {}});
    await tick();
    assert.equal(called, false, 'unrelated or cancelled submits are not intercepted');

    console.log('comments.js in-page submit tests passed');
})().catch(e => { console.error(e); process.exit(1); });

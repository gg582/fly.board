/* In-page comment submit and edit. The form posts with fetch and asks for
 * the re-rendered thread (X-Fly-Comment: fragment); only #comments-list is
 * swapped, so a comment no longer reloads the whole post page, its scripts
 * and its math. Without JavaScript, or when the answer is not the JSON
 * fragment (a login redirect, an old server), the form submits natively. */
(function (global) {
    'use strict';

    /* Comment HTML comes from the post body's renderer; this finishes it the
     * way the post page does. A server-rendered page already loaded
     * highlight.js / KaTeX / katex-render.js / copy.js when its HTML (comments
     * included) needed them. A thread swapped in later may need one the page
     * never loaded, so fetch exactly what render_page.c would have emitted,
     * then run the same entry points. */
    var HL = 'https://cdnjs.cloudflare.com/ajax/libs/highlight.js/11.12.0/';
    var HL_LANGS = ['fortran', 'prolog', 'erlang', 'elixir', 'elm', 'haskell', 'ocaml', 'scheme', 'lisp',
        'clojure', 'vhdl', 'verilog', 'matlab', 'r', 'perl', 'groovy', 'nix', 'x86asm', 'llvm',
        'brainfuck', 'freedesktop'];
    var KATEX = 'https://cdn.jsdelivr.net/npm/katex@0.16.9/dist/';
    var KATEX_RENDER = '/assets/js/katex-render.js?v=11';
    var loads = {};

    function loadScript(doc, src) {
        if (loads[src]) return loads[src];
        loads[src] = new Promise(function (resolve, reject) {
            var s = doc.createElement('script');
            s.src = src;
            s.async = false;
            s.onload = function () { resolve(); };
            s.onerror = function () { delete loads[src]; reject(new Error(src)); };
            doc.head.appendChild(s);
        });
        return loads[src];
    }

    function loadCss(doc, href, id) {
        if ((id && doc.getElementById(id)) || doc.querySelector('link[href="' + href + '"]')) return;
        var l = doc.createElement('link');
        l.rel = 'stylesheet';
        l.href = href;
        if (id) l.id = id;
        doc.head.appendChild(l);
        return l;
    }

    function needsHighlight(list) {
        return !!list.querySelector('code[class*="language-"]');
    }

    function needsKatex(list) {
        return !!list.querySelector('.math-block, .math-inline, .tikz-block, code.language-tikz');
    }

    function ensureHighlight(win, doc) {
        if (win.hljs) return Promise.resolve();
        var dark = doc.documentElement.getAttribute('data-theme-base') === 'dark';
        var link = loadCss(doc, HL + 'styles/github' + (dark ? '-dark' : '') + '.min.css', 'hl-theme');
        if (link) link.setAttribute('data-active', dark ? 'dark' : 'light');
        return loadScript(doc, HL + 'highlight.min.js').then(function () {
            return Promise.all(HL_LANGS.map(function (lang) {
                return loadScript(doc, HL + 'languages/' + lang + '.min.js').catch(function () {});
            }));
        });
    }

    function ensureKatex(win, doc) {
        if (typeof win.__renderBlogMath === 'function') return Promise.resolve(false);
        loadCss(doc, KATEX + 'katex.min.css');
        var core = win.katex ? Promise.resolve() : loadScript(doc, KATEX + 'katex.min.js');
        /* katex-render.js typesets document.body itself once it loads. */
        return core.then(function () { return loadScript(doc, KATEX_RENDER); }).then(function () { return true; });
    }

    function ensureCopy(win, doc) {
        if (typeof win.__initCopyButtons === 'function') return Promise.resolve(false);
        return loadScript(doc, '/assets/js/copy.js?v=2').then(function () { return true; });
    }

    function typeset(win, doc, list) {
        if (!list || !list.querySelector) return Promise.resolve();
        var steps = Promise.resolve();
        if (needsHighlight(list)) {
            steps = steps.then(function () { return ensureHighlight(win, doc); }).then(function () {
                Array.prototype.forEach.call(list.querySelectorAll('pre code[class*="language-"]:not([data-highlighted])'),
                    function (el) { try { win.hljs.highlightElement(el); } catch (_) {} });
            });
        }
        if (needsKatex(list)) {
            steps = steps.then(function () { return ensureKatex(win, doc); }).then(function (fresh) {
                if (!fresh && typeof win.__renderBlogMath === 'function') {
                    try { win.__renderBlogMath(list); } catch (_) {}
                }
            });
        }
        if (list.querySelector('.markdown-body pre')) {
            steps = steps.then(function () { return ensureCopy(win, doc); }).then(function (fresh) {
                if (!fresh && typeof win.__initCopyButtons === 'function') win.__initCopyButtons();
            });
        }
        return steps.catch(function () { /* a CDN miss leaves readable source */ });
    }

    function status(doc, form, text, isError) {
        var el = form.querySelector('.comment-status');
        if (!el) {
            el = doc.createElement('div');
            el.className = 'comment-status';
            el.setAttribute('role', 'status');
            el.setAttribute('aria-live', 'polite');
            form.appendChild(el);
        }
        el.textContent = text || '';
        el.style.color = isError ? 'var(--danger, #c0392b)' : 'var(--muted)';
        el.hidden = !text;
    }

    function bind(env) {
        var win = env.window;
        var doc = env.document;
        if (!win.fetch || !win.URLSearchParams || !win.FormData || typeof Promise === 'undefined') return null;

        function onSubmit(event) {
            var form = event.target;
            if (event.defaultPrevented || !form || !form.matches ||
                !form.matches('form[action="/comment/new"], form[action="/comment/edit"]')) return;
            var list = doc.getElementById('comments-list');
            if (!list || form.dataset.sending === '1') {
                if (form.dataset.sending === '1') event.preventDefault();
                return;
            }
            event.preventDefault();
            var button = form.querySelector('button[type="submit"], button:not([type])');
            form.dataset.sending = '1';
            if (button) button.disabled = true;
            status(doc, form, 'Posting...', false);
            var body = new win.URLSearchParams(new win.FormData(form));

            function done() {
                form.dataset.sending = '';
                if (button) button.disabled = false;
            }
            function fallback() {
                done();
                /* HTMLFormElement.submit skips submit listeners: no loop. */
                win.HTMLFormElement.prototype.submit.call(form);
            }

            win.fetch(form.getAttribute('action'), {
                method: 'POST',
                credentials: 'same-origin',
                cache: 'no-store',
                headers: {
                    'Content-Type': 'application/x-www-form-urlencoded;charset=UTF-8',
                    'Accept': 'application/json',
                    'X-Fly-Comment': 'fragment'
                },
                body: body.toString()
            }).then(function (response) {
                var type = response.headers.get('Content-Type') || '';
                if (response.ok && type.indexOf('application/json') !== 0) {
                    fallback();
                    return null;
                }
                if (!response.ok) {
                    return response.text().then(function (text) {
                        done();
                        status(doc, form, (text || 'The comment could not be posted.').slice(0, 300), true);
                        return null;
                    });
                }
                return response.json();
            }).then(function (data) {
                if (!data) return;
                done();
                list.innerHTML = data.html || '';
                typeset(win, doc, list);
                var textarea = form.querySelector('textarea[name="content"]');
                if (textarea && form.getAttribute('action') === '/comment/new') textarea.value = '';
                if (data.pending) {
                    status(doc, form, 'Thanks - your comment is waiting for review.', false);
                    return;
                }
                status(doc, form, '', false);
                var node = data.id ? doc.getElementById('comment-' + data.id) : null;
                if (node) {
                    if (win.history && win.history.replaceState)
                        win.history.replaceState(null, '', '#comment-' + data.id);
                    if (node.scrollIntoView) node.scrollIntoView({block: 'nearest'});
                }
            }).catch(function () {
                /* Network trouble: let the browser try the classic way. */
                fallback();
            });
        }

        doc.addEventListener('submit', onSubmit);
        /* realtime.js swaps the thread too and finishes it the same way. */
        win.__flyTypesetComments = function (list) { return typeset(win, doc, list); };
        return {stop: function () { doc.removeEventListener('submit', onSubmit); }};
    }

    if (typeof module !== 'undefined' && module.exports) {
        module.exports = {bind: bind};
    } else if (global.document) {
        bind({window: global, document: global.document});
    }
})(typeof window !== 'undefined' ? window : globalThis);

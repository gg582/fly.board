/* In-page comment submit and edit. The form posts with fetch and asks for
 * the re-rendered thread (X-Fly-Comment: fragment); only #comments-list is
 * swapped, so a comment no longer reloads the whole post page, its scripts
 * and its math. Without JavaScript, or when the answer is not the JSON
 * fragment (a login redirect, an old server), the form submits natively. */
(function (global) {
    'use strict';

    function typeset(win, list) {
        if (typeof win.__renderBlogMath === 'function') {
            try { win.__renderBlogMath(list); } catch (_) {}
        }
        if (win.hljs && list.querySelectorAll) {
            Array.prototype.forEach.call(list.querySelectorAll('pre code[class*="language-"]'), function (el) {
                try { win.hljs.highlightElement(el); } catch (_) {}
            });
        }
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
        if (!win.fetch || !win.URLSearchParams || !win.FormData) return null;

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
                typeset(win, list);
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
        return {stop: function () { doc.removeEventListener('submit', onSubmit); }};
    }

    if (typeof module !== 'undefined' && module.exports) {
        module.exports = {bind: bind};
    } else if (global.document) {
        bind({window: global, document: global.document});
    }
})(typeof window !== 'undefined' ? window : globalThis);

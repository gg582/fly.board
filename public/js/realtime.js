(function (global) {
    'use strict';

    function sessionId(win) {
        var storage = win.sessionStorage;
        var key = 'flyboard.realtime.session';
        var existing = storage.getItem(key);
        if (existing && /^[0-9a-f]{32}$/i.test(existing)) return existing;
        if (!win.crypto || !win.crypto.getRandomValues) return null;
        var bytes = new Uint8Array(16);
        win.crypto.getRandomValues(bytes);
        var id = Array.prototype.map.call(bytes, function (byte) {
            return byte.toString(16).padStart(2, '0');
        }).join('');
        storage.setItem(key, id);
        return id;
    }

    function startRealtime(root, env) {
        var win = env.window;
        var doc = env.document;
        var session = sessionId(win);
        if (!session || !win.EventSource) return null;
        var postId = root.getAttribute('data-realtime-post');
        var after = root.getAttribute('data-latest-comment') || '0';
        var typingEnabled = root.getAttribute('data-typing') === '1';
        var viewers = doc.getElementById('realtime-viewers');
        var typing = doc.getElementById('realtime-typing');
        var stopped = false;
        var refreshing = false;
        var refreshAgain = false;
        var lastTyping = 0;
        var typingTimer = 0;

        function refreshComments() {
            if (refreshing) {
                refreshAgain = true;
                return;
            }
            refreshing = true;
            win.fetch('/realtime/comments/' + encodeURIComponent(postId), {
                credentials: 'same-origin',
                cache: 'no-store'
            }).then(function (response) {
                if (!response.ok) throw new Error('comment refresh failed');
                return response.text();
            }).then(function (html) {
                if (stopped) return;
                var list = doc.getElementById('comments-list');
                list.innerHTML = html;
                /* Same finishing as the post body (comments.js loads what is missing). */
                if (typeof win.__flyTypesetComments === 'function') win.__flyTypesetComments(list);
            }).catch(function () {
                /* The server-rendered comments remain usable; the next event retries. */
            }).then(function () {
                refreshing = false;
                if (refreshAgain) {
                    refreshAgain = false;
                    refreshComments();
                }
            });
        }

        var source = new win.EventSource('/realtime/events?post_id=' + encodeURIComponent(postId) +
            '&session=' + session + '&after=' + encodeURIComponent(after));
        source.addEventListener('state', function (event) {
            var state;
            try { state = JSON.parse(event.data); } catch (_) { return; }
            if (viewers && typeof state.viewers === 'string') {
                viewers.textContent = state.viewers;
                viewers.hidden = false;
            }
            if (typing) typing.hidden = !(typingEnabled && Number(state.typing) > 0);
        });
        source.addEventListener('comment', refreshComments);

        function update(path, active, beacon) {
            var body = new win.URLSearchParams();
            body.set('post_id', postId);
            body.set('session', session);
            body.set('active', active ? '1' : '0');
            if (beacon && win.navigator.sendBeacon) {
                win.navigator.sendBeacon(path, body);
                return;
            }
            win.fetch(path, {
                method: 'POST',
                credentials: 'same-origin',
                cache: 'no-store',
                headers: {'Content-Type': 'application/x-www-form-urlencoded;charset=UTF-8'},
                body: body.toString(),
                keepalive: true
            }).catch(function () {});
        }

        function heartbeat() {
            if (!stopped) update('/realtime/presence', true, false);
        }

        function stopTyping() {
            lastTyping = 0;
            if (typingTimer) {
                win.clearTimeout(typingTimer);
                typingTimer = 0;
            }
            if (typingEnabled) update('/realtime/typing', false, false);
        }

        function onInput(event) {
            if (!typingEnabled || event.target.tagName !== 'TEXTAREA' || event.target.name !== 'content') return;
            if (!event.target.value.trim()) {
                stopTyping();
                return;
            }
            var now = Date.now();
            var wait = Math.max(0, 2600 - (now - lastTyping));
            if (typingTimer) return;
            typingTimer = win.setTimeout(function () {
                typingTimer = 0;
                lastTyping = Date.now();
                update('/realtime/typing', true, false);
            }, wait);
        }

        function onSubmit(event) {
            if (event.target.matches('form[action="/comment/new"]')) stopTyping();
        }

        function onPageHide() {
            update('/realtime/presence', false, true);
            if (typingEnabled) update('/realtime/typing', false, true);
        }

        function onPageShow(event) {
            if (event.persisted) heartbeat();
        }

        root.addEventListener('input', onInput);
        root.addEventListener('submit', onSubmit);
        win.addEventListener('pagehide', onPageHide);
        win.addEventListener('pageshow', onPageShow);
        heartbeat();
        var heartbeatTimer = win.setInterval(heartbeat, 15000);

        return {
            source: source,
            stop: function () {
                stopped = true;
                source.close();
                win.clearInterval(heartbeatTimer);
                if (typingTimer) win.clearTimeout(typingTimer);
                root.removeEventListener('input', onInput);
                root.removeEventListener('submit', onSubmit);
                win.removeEventListener('pagehide', onPageHide);
                win.removeEventListener('pageshow', onPageShow);
            }
        };
    }

    if (typeof module !== 'undefined' && module.exports) {
        module.exports = {startRealtime: startRealtime};
    } else {
        global.addEventListener('DOMContentLoaded', function () {
            var root = global.document.querySelector('[data-realtime-post]');
            if (root) startRealtime(root, {window: global, document: global.document});
        });
    }
})(typeof window !== 'undefined' ? window : globalThis);

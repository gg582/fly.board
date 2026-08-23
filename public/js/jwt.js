/* jwt.js — client-side JWT lifetime manager for fly.board
 *
 * Reads the JS-readable `jwt_access` cookie set at login and injects the
 * token as `Authorization: Bearer <token>` on every fetch() and
 * XMLHttpRequest. This gives the backend a second way to identify the
 * logged-in user when the HttpOnly session cookie is lost on long-lived
 * connections (e.g. during multipart uploads or editor autosave).
 *
 * This file intentionally does NOT store the token in localStorage/sessionStorage
 * and does NOT read the HttpOnly session cookie.
 */
(function () {
    'use strict';

    function getCookie(name) {
        if (typeof document === 'undefined' || !document.cookie) return null;
        var prefix = name + '=';
        var cookies = document.cookie.split(';');
        for (var i = 0; i < cookies.length; i++) {
            var c = cookies[i].replace(/^\s+/, '');
            if (c.indexOf(prefix) === 0) {
                return decodeURIComponent(c.substring(prefix.length));
            }
        }
        return null;
    }

    function currentJwt() {
        return getCookie('jwt_access');
    }

    function isSameOrigin(input) {
        try {
            var url = input instanceof Request ? input.url : String(input);
            return new URL(url, window.location.href).origin === window.location.origin;
        } catch (e) {
            return false;
        }
    }

    /* A document navigation cannot be decorated by this fetch wrapper. Keep
     * the access token only in the controlling service worker's memory so it
     * can add the same bearer credential for navigations from this tab when a
     * broken/reused connection omits Cookie. Nothing is persisted to Cache or
     * storage, and the token is scoped to the sending client. */
    function syncServiceWorkerToken() {
        if (!navigator.serviceWorker) return;
        var message = { type: 'AUTH_TOKEN', token: currentJwt() || '' };
        function send(worker) {
            if (worker) worker.postMessage(message);
        }
        send(navigator.serviceWorker.controller);
        navigator.serviceWorker.ready.then(function(registration) {
            send(registration.active);
        }).catch(function() {});
    }

    if (navigator.serviceWorker) {
        syncServiceWorkerToken();
        navigator.serviceWorker.addEventListener('controllerchange', syncServiceWorkerToken);
        window.addEventListener('pageshow', syncServiceWorkerToken);
    }

    function clearServiceWorkerToken() {
        if (!navigator.serviceWorker) return;
        var message = { type: 'AUTH_TOKEN', token: '' };
        function send(worker) {
            if (worker) worker.postMessage(message);
        }
        send(navigator.serviceWorker.controller);
        navigator.serviceWorker.ready.then(function(registration) {
            send(registration.active);
        }).catch(function() {});
    }

    /* Logout navigations bypass the service worker, so the SW would keep the
     * stale token and re-authenticate the post-logout redirect via its
     * bearer fallback.  Clear it the moment a logout action starts. */
    function isLogoutAction(el) {
        var target = el && el.closest ? el.closest('a[href*="/logout"], form[action*="/logout"]') : null;
        return !!target;
    }
    document.addEventListener('click', function(e) {
        if (isLogoutAction(e.target)) clearServiceWorkerToken();
    }, true);
    document.addEventListener('submit', function(e) {
        if (isLogoutAction(e.target)) clearServiceWorkerToken();
    }, true);

    function needsAuthHeader(headers) {
        if (!headers) return true;
        if (headers instanceof Headers) {
            return !headers.has('Authorization');
        }
        if (Array.isArray(headers)) {
            for (var i = 0; i < headers.length; i++) {
                if (headers[i] && String(headers[i][0]).toLowerCase() === 'authorization') {
                    return false;
                }
            }
            return true;
        }
        if (typeof headers === 'object') {
            for (var k in headers) {
                if (Object.prototype.hasOwnProperty.call(headers, k) &&
                    String(k).toLowerCase() === 'authorization') {
                    return false;
                }
            }
        }
        return true;
    }

    function addAuthHeader(headers, token) {
        if (!token || !needsAuthHeader(headers)) return headers;
        var value = 'Bearer ' + token;
        if (headers instanceof Headers) {
            headers.set('Authorization', value);
            return headers;
        }
        if (Array.isArray(headers)) {
            headers.push(['Authorization', value]);
            return headers;
        }
        if (typeof headers === 'object' && headers !== null) {
            headers.Authorization = value;
            return headers;
        }
        return { 'Authorization': value };
    }

    /* Wrap fetch() */
    var origFetch = window.fetch;
    window.fetch = function (input, init) {
        init = init || {};
        var token = currentJwt();
        if (token && isSameOrigin(input)) {
            if (input instanceof Request) {
                /* Rebuild an existing Request after adding the header. Some
                 * engines ignore init.headers when input is already a Request. */
                var headers = new Headers(input.headers);
                if (!headers.has('Authorization')) {
                    headers.set('Authorization', 'Bearer ' + token);
                    init.headers = headers;
                    input = new Request(input, init);
                    init = undefined;
                }
            } else {
                init.headers = addAuthHeader(init.headers, token);
            }
        }
        return origFetch.call(this, input, init);
    };

    /* Wrap XMLHttpRequest so multipart uploads/autosave keep auth */
    var origOpen = XMLHttpRequest.prototype.open;
    var origSetRequestHeader = XMLHttpRequest.prototype.setRequestHeader;
    var origSend = XMLHttpRequest.prototype.send;

    XMLHttpRequest.prototype.open = function (method, url) {
        this.__jwt_sent__ = false;
        this.__jwt_headers__ = {};
        this.__jwt_same_origin__ = isSameOrigin(url);
        return origOpen.apply(this, arguments);
    };

    XMLHttpRequest.prototype.setRequestHeader = function (name, value) {
        this.__jwt_headers__ = this.__jwt_headers__ || {};
        this.__jwt_headers__[String(name).toLowerCase()] = true;
        return origSetRequestHeader.apply(this, arguments);
    };

    XMLHttpRequest.prototype.send = function (body) {
        var token = currentJwt();
        if (token && this.__jwt_same_origin__ && !this.__jwt_sent__ &&
            (!this.__jwt_headers__ || !this.__jwt_headers__['authorization'])) {
            origSetRequestHeader.call(this, 'Authorization', 'Bearer ' + token);
            this.__jwt_sent__ = true;
        }
        return origSend.apply(this, arguments);
    };
})();

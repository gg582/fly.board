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
        if (token) {
            if (input instanceof Request) {
                /* Request.headers is immutable in some browsers; build a new
                 * Headers object from it and override via init. */
                var headers = new Headers(input.headers);
                if (!headers.has('Authorization')) {
                    headers.set('Authorization', 'Bearer ' + token);
                    init.headers = headers;
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

    XMLHttpRequest.prototype.open = function () {
        this.__jwt_sent__ = false;
        this.__jwt_headers__ = this.__jwt_headers__ || {};
        return origOpen.apply(this, arguments);
    };

    XMLHttpRequest.prototype.setRequestHeader = function (name, value) {
        this.__jwt_headers__ = this.__jwt_headers__ || {};
        this.__jwt_headers__[String(name).toLowerCase()] = true;
        return origSetRequestHeader.apply(this, arguments);
    };

    XMLHttpRequest.prototype.send = function (body) {
        var token = currentJwt();
        if (token && !this.__jwt_sent__ &&
            (!this.__jwt_headers__ || !this.__jwt_headers__['authorization'])) {
            origSetRequestHeader.call(this, 'Authorization', 'Bearer ' + token);
            this.__jwt_sent__ = true;
        }
        return origSend.apply(this, arguments);
    };
})();

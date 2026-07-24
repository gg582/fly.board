var LOGO_CACHE = 'logo-cache-v4';
var TASFA_MEDIA_CACHE = 'tasfa-media-cache-v1';
var STATIC_CACHE = 'fly-static-v4';
var CDN_CACHE = 'fly-cdn-v2';
var PRECACHE = 'fly-precache-v2';
var NAVIGATION_CACHE = 'fly-navigation-v1';
var LOGO_MAX_AGE = 3600000; // 1 hour in milliseconds
var STATIC_MAX_AGE = 7 * 24 * 60 * 60 * 1000; // 7 days (immutable hashed assets)
var CDN_MAX_AGE = 30 * 24 * 60 * 60 * 1000; // 30 days (versioned CDN URLs)
var NAVIGATION_MAX_AGE = 60 * 1000; // 60 seconds stale-while-revalidate for HTML pages
var tasfaSessions = {};

/* Assets to prefetch on SW install so repeat navigations never hit the network
   for the shell JS/CSS, fonts, and the theme config. */
var PRECACHE_URLS = [
    '/',
    '/assets/js/jwt.js',
    '/assets/js/layout.js',
    '/assets/js/katex-render.js',
    '/assets/js/tasfa-download.js',
    '/assets/css/google-fonts.css',
    '/assets/css/pretendard.css',
    '/assets/css/d2coding.css',
    '/themes.json'
];

/* Progressive video streaming streams managed by the Service Worker */
var tasfaStreams = {};

function tasfaSessionKeys(rawUrl) {
    var keys = [];
    try {
        var parsed = new URL(rawUrl, self.location.origin);
        keys.push(parsed.origin + parsed.pathname);
        keys.push(parsed.pathname);
    } catch (e) {
        if (rawUrl) keys.push(String(rawUrl).split(/[?#]/)[0]);
    }
    if (rawUrl) keys.push(rawUrl);
    return keys;
}

function rememberTasfaSession(rawUrl, session) {
    tasfaSessionKeys(rawUrl).forEach(function(key) {
        if (key) tasfaSessions[key] = session;
    });
}

function lookupTasfaSession(rawUrl) {
    var keys = tasfaSessionKeys(rawUrl);
    for (var i = 0; i < keys.length; i++) {
        if (tasfaSessions[keys[i]]) return tasfaSessions[keys[i]];
    }
    return null;
}

function waitForSession(rawUrl, timeoutMs) {
    return new Promise(function(resolve) {
        var start = Date.now();
        function check() {
            var session = lookupTasfaSession(rawUrl);
            if (session) {
                resolve(session);
                return;
            }
            if (Date.now() - start > timeoutMs) {
                resolve(null);
                return;
            }
            setTimeout(check, 50);
        }
        check();
    });
}

self.addEventListener('message', function(event) {
    if (!event.data) return;

    if (event.data.type === 'TASFA_SESSION') {
        rememberTasfaSession(event.data.url, {
            sessionId: event.data.sessionId,
            sessionToken: event.data.sessionToken,
            ultraFastConnection: !!event.data.ultraFastConnection
        });
    } else if (event.data.type === 'TASFA_STREAM_OPEN') {
        /* Client opens a progressive video stream. Create a ReadableStream
           that will be consumed by the browser's media element. */
        var streamId = event.data.streamId;
        var streamController = null;
        var stream = new ReadableStream({
            start: function(controller) {
                streamController = controller;
            }
        });
        tasfaStreams[streamId] = {
            controller: streamController,
            stream: stream,
            headers: event.data.headers || {}
        };
        if (event.ports && event.ports[0]) {
            event.ports[0].postMessage({ type: 'TASFA_STREAM_READY', streamId: streamId });
        }
    } else if (event.data.type === 'TASFA_STREAM_CHUNK') {
        /* Push a decrypted chunk into the progressive stream. */
        var entry = tasfaStreams[event.data.streamId];
        if (entry && entry.controller) {
            var chunk = event.data.chunk;
            if (chunk instanceof ArrayBuffer) {
                entry.controller.enqueue(new Uint8Array(chunk));
            } else if (ArrayBuffer.isView(chunk)) {
                entry.controller.enqueue(new Uint8Array(chunk.buffer, chunk.byteOffset, chunk.byteLength));
            }
        }
    } else if (event.data.type === 'TASFA_STREAM_CLOSE') {
        /* Close the progressive stream when all chunks have been fed. */
        var entry = tasfaStreams[event.data.streamId];
        if (entry && entry.controller) {
            try { entry.controller.close(); } catch (e) {}
        }
        /* Delay deletion so that any late range/re-buffer requests from the
           media element do not immediately hit "Stream not found". */
        var closeStreamId = event.data.streamId;
        setTimeout(function() {
            delete tasfaStreams[closeStreamId];
        }, 60000);
    }
});

self.addEventListener('install', function(e) {
    e.waitUntil(
        caches.open(PRECACHE).then(function(cache) {
            return cache.addAll(PRECACHE_URLS);
        }).then(function() {
            return self.skipWaiting();
        })
    );
});

self.addEventListener('activate', function(e) {
    var allowedCaches = [LOGO_CACHE, TASFA_MEDIA_CACHE, STATIC_CACHE, CDN_CACHE, PRECACHE, NAVIGATION_CACHE];
    e.waitUntil(
        caches.keys().then(function(keys) {
            return Promise.all(keys.map(function(key) {
                if (allowedCaches.indexOf(key) === -1) {
                    return caches.delete(key);
                }
            }));
        }).then(function() {
            return self.clients.claim();
        })
    );
});

function emptyPngResponse(status, statusText) {
    var png = new Uint8Array([0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A,0x00,0x00,0x00,0x0D,0x49,0x48,0x44,0x52,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01,0x08,0x06,0x00,0x00,0x00,0x1F,0x15,0xC4,0x89,0x00,0x00,0x00,0x0A,0x49,0x44,0x41,0x54,0x78,0x9C,0x63,0x60,0x00,0x00,0x00,0x02,0x00,0x01,0x73,0x75,0x01,0x18,0x00,0x00,0x00,0x00,0x49,0x45,0x4E,0x44,0xAE,0x42,0x60,0x82]);
    return new Response(png, {
        status: status || 503,
        statusText: statusText || 'Service Unavailable',
        headers: { 'Content-Type': 'image/png', 'Cache-Control': 'no-store' }
    });
}

function isLogoUrl(url) {
    return url.startsWith(self.location.origin + '/assets/img/');
}

function handleRangeRequest(request, response) {
    var rangeHeader = request.headers.get('Range');
    if (!rangeHeader) {
        return response;
    }

    return response.blob().then(function(blob) {
        var total = blob.size;

        if (!response.ok) {
            return new Response(blob, {
                status: response.status,
                statusText: response.statusText,
                headers: response.headers
            });
        }

        var match = rangeHeader.trim().match(/^bytes=(\d+)-(\d+)?$/);
        if (!match) {
            var suffixMatch = rangeHeader.trim().match(/^bytes=-(\d+)$/);
            if (suffixMatch) {
                var suffixLen = parseInt(suffixMatch[1], 10);
                if (suffixLen <= 0) {
                    return new Response('', {
                        status: 416,
                        headers: { 'Content-Range': 'bytes */' + total }
                    });
                }
                var suffixStart = Math.max(0, total - suffixLen);
                var suffixBlob = blob.slice(suffixStart, total);
                var suffixHeaders = new Headers(response.headers);
                suffixHeaders.set('Content-Range', 'bytes ' + suffixStart + '-' + (total - 1) + '/' + total);
                suffixHeaders.set('Content-Length', suffixBlob.size.toString());
                suffixHeaders.set('Accept-Ranges', 'bytes');
                return new Response(suffixBlob, {
                    status: 206,
                    statusText: 'Partial Content',
                    headers: suffixHeaders
                });
            }
            /* Multi-range or unsupported syntax: return the full resource
               rather than fabricating a malformed 206 response. */
            return new Response(blob, {
                status: 200,
                headers: response.headers
            });
        }

        var start = parseInt(match[1], 10);
        var end = match[2] ? parseInt(match[2], 10) : total - 1;
        if (end >= total) {
            end = total - 1;
        }

        if (start >= total) {
            return new Response('', {
                status: 416,
                headers: {
                    'Content-Range': 'bytes */' + total
                }
            });
        }

        var slicedBlob = blob.slice(start, end + 1);
        var headers = new Headers(response.headers);
        headers.set('Content-Range', 'bytes ' + start + '-' + end + '/' + total);
        headers.set('Content-Length', slicedBlob.size.toString());
        headers.set('Accept-Ranges', 'bytes');

        return new Response(slicedBlob, {
            status: 206,
            statusText: 'Partial Content',
            headers: headers
        });
    });
}

function isFirefox() {
    try {
        return /Firefox\//i.test(self.navigator.userAgent || '');
    } catch (e) {
        return false;
    }
}

function isCdnUrl(url) {
    return url.startsWith('https://fonts.googleapis.com') ||
           url.startsWith('https://fonts.gstatic.com') ||
           url.startsWith('https://cdn.jsdelivr.net') ||
           url.startsWith('https://cdnjs.cloudflare.com') ||
           url.startsWith('https://cdn.plyr.io');
}

function shouldSkipCaching(response) {
    if (!response || !response.ok) return true;
    var cc = response.headers.get('Cache-Control') || '';
    if (cc.indexOf('no-store') !== -1) return true;
    if (cc.indexOf('no-cache') !== -1) return true;
    if (cc.indexOf('must-revalidate') !== -1) return true;
    if (cc.indexOf('private') !== -1) return true;
    return false;
}

function fetchAndCache(request, cacheName, maxAge, fetchOptions) {
    fetchOptions = fetchOptions || {};
    return fetchWithRetry(request, fetchOptions).then(function(response) {
        if (!shouldSkipCaching(response)) {
            var cloned = response.clone();
            caches.open(cacheName).then(function(cache) {
                var headers = new Headers(cloned.headers);
                headers.set('x-sw-fetched', Date.now().toString());
                headers.set('x-sw-max-age', maxAge.toString());
                // Remove encoding/length that may mismatch the stored blob.
                headers.delete('content-encoding');
                cloned.blob().then(function(blob) {
                    headers.set('content-length', blob.size.toString());
                    cache.put(request, new Response(blob, {
                        status: cloned.status,
                        statusText: cloned.statusText,
                        headers: headers
                    }));
                });
            });
        }
        return response;
    });
}

function cachedOrFetch(request, cacheName, maxAge, fetchOptions) {
    return caches.open(cacheName).then(function(cache) {
        return cache.match(request).then(function(cachedResponse) {
            var now = Date.now();
            if (cachedResponse) {
                var fetched = cachedResponse.headers.get('x-sw-fetched');
                var age = fetched ? (now - parseInt(fetched, 10)) : Infinity;
                if (age < maxAge) {
                    return cachedResponse;
                }
            }
            return fetchAndCache(request, cacheName, maxAge, fetchOptions).catch(function(err) {
                if (cachedResponse) return cachedResponse;
                throw err;
            });
        });
    });
}

function fetchWithRetry(request, options) {
    options = options || {};
    var maxRetries = options.maxRetries || 12;
    // KR<->NG worst-case: measured RTT up to 4000 ms; TCP+TLS reconnect ~12 s.
    // First retry must land after at least one full reconnect cycle, so
    // baseline delay is 5 s.  Subsequent retries back off up to 90 s.
    var baseDelay = options.baseDelay || 5000;
    var maxDelay = options.maxDelay || 90000;
    var retries = 0;
    var triedFirefoxFallback = false;

    function delay() {
        var exp = Math.min(retries, 4); // 5s * 2^4 = 80s, clamped to 90s ceiling
        var d = Math.min(baseDelay * Math.pow(2, exp), maxDelay);
        // ±2000 ms jitter: wide enough to desynchronise clients on the same
        // KR-> [US West] -> EU -> NG path that all drop at once.
        d += Math.floor(Math.random() * 2000);
        return new Promise(function(resolve) { setTimeout(resolve, d); });
    }

    function doFetch(req) {
        return fetch(req, { credentials: 'same-origin' }).then(function(response) {
            // Only retry on server-side errors. 4xx client errors should not be retried.
            if (response.status >= 500 && retries < maxRetries) {
                retries++;
                return delay().then(attempt);
            }
            return response;
        });
    }

    function attempt() {
        return doFetch(request).catch(function(err) {
            // Firefox HTTP/2 and HTTP/3 connections occasionally fail with
            // NS_ERROR_NET_RESET / NS_ERROR_NET_TIMEOUT / NS_ERROR_PARTIAL_TRANSFER.
            // A single cache-busting, no-store retry forces a fresh connection
            // and often recovers without waiting through the full backoff loop.
            if (!triedFirefoxFallback && options.firefoxFallback && isFirefox()) {
                triedFirefoxFallback = true;
                try {
                    var url = new URL(request.url);
                    url.searchParams.set('_ffcb', Date.now().toString());
                    var ffReq = new Request(url.href, {
                        method: request.method,
                        headers: request.headers,
                        mode: request.mode,
                        credentials: request.credentials,
                        cache: 'no-store'
                    });
                    return doFetch(ffReq).catch(function() {
                        // Fallback also failed; continue normal retry loop.
                        if (retries < maxRetries) {
                            retries++;
                            return delay().then(attempt);
                        }
                        throw err;
                    });
                } catch (e) {}
            }
            // Network errors (ERR_CONNECTION_RESET, ERR_CONNECTION_REFUSED,
            // ERR_NETWORK_CHANGED, etc.) — always retry with backoff.
            if (retries < maxRetries) {
                retries++;
                return delay().then(attempt);
            }
            throw err;
        });
    }

    return attempt();
}

self.addEventListener('fetch', function(event) {
    try {
        var url = event.request.url;
        // Don't intercept cross-origin requests (multi-port)
        if (new URL(url).origin !== self.location.origin) return;

        // Bypass SW interception for direct chunk transfers, handshakes, uploads,
        // and requests with query credentials to prevent Firefox connection pooling/partial-transfer bugs.
        if (url.includes('/chunk/') ||
            url.includes('/handshake') ||
            url.includes('/file/upload') ||
            url.includes('tasfa_fallback=1') ||
            url.includes('session_id=')) {
            return;
        }

        if (url.indexOf(self.location.origin + '/file/download/') === 0 && event.request.method === 'GET') {
            event.respondWith(
                waitForSession(url, 2000).then(function(session) {
                    if (session) {
                        var headers = new Headers(event.request.headers);
                        headers.set('X-TASFA-Session-ID', session.sessionId);
                        headers.set('X-TASFA-Session-Token', session.sessionToken);
                        return fetch(new Request(event.request, { headers: headers })).then(function(response) {
                            if (event.request.headers.get('Range')) {
                                if (response.status === 206) return response;
                                if (session.ultraFastConnection) return response;
                                return handleRangeRequest(event.request, response);
                            }
                            return response;
                        });
                    }
                    return fetch(event.request);
                })
            );
            return;
        }

    if (url.indexOf(self.location.origin + '/__tasfa_media__/') === 0 && event.request.method === 'GET') {
        event.respondWith(
            caches.open(TASFA_MEDIA_CACHE).then(function(cache) {
                return cache.match(event.request).then(function(response) {
                    if (!response) {
                        return fetch(event.request, { credentials: 'same-origin' });
                    }
                    return handleRangeRequest(event.request, response);
                });
            })
        );
        return;
    }

    /* Progressive TASFA video stream: the client feeds chunks through
       postMessage and the media element reads from this URL.
       This stream is strictly sequential; do not advertise byte ranges,
       because the ReadableStream is always fed from byte 0. Firefox in
       particular treats a Content-Length/range mismatch as a partial
       transfer error (NS_ERROR_PARTIAL_TRANSFER). */
    /* Navigation requests (full page HTML): stale-while-revalidate.  Serve the
     * cached page immediately so transitions feel instant, then refresh the
     * cache entry in the background for the next navigation.  A short TTL keeps
     * dynamic content reasonably fresh. */
    if (event.request.mode === 'navigate' && event.request.method === 'GET') {
        var refPath = '';
        try { refPath = new URL(event.request.referrer || '').pathname; } catch (e) {}
        var authPaths = ['/login', '/register', '/logout', '/account', '/admin'];
        var fromAuthPage = authPaths.some(function(p) {
            return refPath === p || refPath.indexOf(p + '/') === 0;
        });
        if (fromAuthPage) return;
        event.respondWith(
            caches.open(NAVIGATION_CACHE).then(function(cache) {
                return cache.match(event.request).then(function(cachedResponse) {
                    var now = Date.now();
                    var isFresh = false;
                    if (cachedResponse) {
                        var fetched = cachedResponse.headers.get('x-sw-fetched');
                        isFresh = fetched && (now - parseInt(fetched, 10)) < NAVIGATION_MAX_AGE;
                    }
                    var networkFetch = fetchWithRetry(event.request, { maxRetries: 3, baseDelay: 150, firefoxFallback: true }).then(function(networkResponse) {
                        if (!shouldSkipCaching(networkResponse)) {
                            var cloned = networkResponse.clone();
                            caches.open(NAVIGATION_CACHE).then(function(c) {
                                var headers = new Headers(cloned.headers);
                                headers.set('x-sw-fetched', Date.now().toString());
                                headers.delete('content-encoding');
                                cloned.blob().then(function(blob) {
                                    headers.set('content-length', blob.size.toString());
                                    c.put(event.request, new Response(blob, {
                                        status: cloned.status,
                                        statusText: cloned.statusText,
                                        headers: headers
                                    }));
                                });
                            });
                        }
                        return networkResponse;
                    }).catch(function(err) {
                        if (cachedResponse) return cachedResponse;
                        throw err;
                    });

                    if (isFresh && cachedResponse) {
                        // Use cache now, refresh in background.
                        networkFetch.catch(function(){});
                        return cachedResponse;
                    }
                    // Otherwise wait for network; fall back to stale cache on failure.
                    return networkFetch;
                });
            })
        );
        return;
    }

    /* Cache CDN fonts, styles and scripts aggressively. High-RTT links suffer
     * most on these cross-origin resources because they are fetched on every
     * cold navigation. */
    if (isCdnUrl(url) && event.request.method === 'GET' && !event.request.headers.get('Range')) {
        event.respondWith(
            cachedOrFetch(event.request, CDN_CACHE, CDN_MAX_AGE).catch(function(err) {
                if (typeof console !== 'undefined' && console.warn) console.warn('[SW] CDN fetch failed:', url, err);
                throw err;
            })
        );
        return;
    }

    if (url.indexOf(self.location.origin + '/__tasfa_stream__/') === 0 && event.request.method === 'GET') {
        var pathParts = new URL(url).pathname.split('/');
        var streamId = pathParts[pathParts.length - 1];
        var entry = tasfaStreams[streamId];
        if (entry) {
            var headers = new Headers();
            headers.set('Content-Type', entry.headers.contentType || 'application/octet-stream');
            var totalLength = Number(entry.headers.contentLength || 0);
            var rangeHeader = event.request.headers.get('Range');
            var status = 200;

            if (rangeHeader && totalLength > 0) {
                var match = rangeHeader.trim().match(/^bytes=(\d+)-(\d+)?$/);
                if (match) {
                    var start = parseInt(match[1], 10);
                    if (start === 0) {
                        /* First-byte range request: serve as 206 so the player
                           knows range requests are honoured for this URL. */
                        status = 206;
                        headers.set('Content-Range', 'bytes 0-' + (totalLength - 1) + '/' + totalLength);
                        headers.set('Content-Length', entry.headers.contentLength);
                        headers.set('Accept-Ranges', 'bytes');
                    }
                    /* start > 0 is not supported by a sequential stream.
                       Fall back to 200 with no Content-Length so Firefox does
                       not see a promised length mismatch. */
                }
                /* Suffix or multi-range: same no-length fallback. */
            } else if (totalLength > 0) {
                headers.set('Content-Length', entry.headers.contentLength);
            }

            event.respondWith(new Response(entry.stream, {
                status: status,
                headers: headers
            }));
        } else {
            event.respondWith(new Response('', {
                status: 204,
                headers: {
                    'Cache-Control': 'no-store'
                }
            }));
        }
        return;
    }

    function withoutCsp(response) {
        var headers = new Headers(response.headers);
        headers.delete('content-security-policy');
        return new Response(response.body, {
            status: response.status,
            statusText: response.statusText,
            headers: headers
        });
    }

    // Logo and favicon caching (cache-first with 24h TTL)
    if ((event.request.destination === 'image' || url.includes('favicon')) && (isLogoUrl(url) || url.includes('favicon'))) {
        event.respondWith(
            caches.open(LOGO_CACHE).then(function(cache) {
                return cache.match(event.request).then(function(cachedResponse) {
                    var now = Date.now();
                    if (cachedResponse && cachedResponse.ok) {
                        var fetched = cachedResponse.headers.get('x-sw-fetched');
                        if (fetched && (now - parseInt(fetched, 10)) < LOGO_MAX_AGE) {
                            return withoutCsp(cachedResponse);
                        }
                    }
                    return fetchWithRetry(event.request, { maxRetries: 3, baseDelay: 150, firefoxFallback: true }).then(function(networkResponse) {
                        if (!networkResponse.ok) {
                            return cachedResponse && cachedResponse.ok ? withoutCsp(cachedResponse) : networkResponse;
                        }
                        // Use blob() for Firefox compatibility when reconstructing Response.
                        // IMPORTANT: strip Content-Length from the stored/served headers so
                        // Firefox does not see a mismatch between the declared length (which
                        // may reflect the compressed network body) and the decompressed blob.
                        // A mismatch triggers NS_ERROR_NET_PARTIAL_TRANSFER in Firefox.
                        return networkResponse.blob().then(function(blob) {
                            var storeHeaders = new Headers(networkResponse.headers);
                            storeHeaders.set('x-sw-fetched', now.toString());
                            storeHeaders.delete('content-length');  /* avoid FF partial-transfer */
                            storeHeaders.delete('content-encoding'); /* blob is already decoded */
                            var modified = new Response(blob, {
                                status: networkResponse.status,
                                statusText: networkResponse.statusText,
                                headers: storeHeaders
                            });
                            cache.put(event.request, modified);
                            /* Serve response: set Content-Length to actual blob byte size
                               so the browser never receives fewer bytes than advertised. */
                            var serveHeaders = new Headers(networkResponse.headers);
                            serveHeaders.delete('content-security-policy');
                            serveHeaders.delete('content-encoding');
                            serveHeaders.set('content-length', String(blob.size));
                            return new Response(blob, {
                                status: networkResponse.status,
                                statusText: networkResponse.statusText,
                                headers: serveHeaders
                            });
                        });
                    }).catch(function(err) {
                        if (cachedResponse && cachedResponse.ok) {
                            return withoutCsp(cachedResponse);
                        }
                        // Firefox logs MIME/decoding mismatches for text/plain
                        // image responses. Return a valid 1x1 transparent PNG so
                        // the browser shows a clean broken-image placeholder.
                        return emptyPngResponse(503, 'Service Unavailable');
                    });
                });
            })
        );
        return;
    }

    var isTasfa = url.includes('/tasfa/') || url.includes('/file/upload') || url.includes('/file/download');
    /* Direct image assets (e.g. thumbnails, profile pictures, static images)
       are not TASFA endpoints, but under heavy photo posts they suffer from
       the same transient connection resets. Intercept all /assets/ images so
       fetchWithRetry can recover short read/connection errors. */
    var isDirectImageAsset = event.request.destination === 'image' && url.includes('/assets/');
    /* Static JS/CSS under /assets/ (e.g. tasfa-download.js, copy.js, katex-render.js)
       and the themes.json config fetch share the same high-RTT failure mode:
       the first TCP attempt races against a server-side idle timeout and gets
       refused.  Intercept them so fetchWithRetry can transparently recover. */
    var isStaticScript = (event.request.destination === 'script' || event.request.destination === 'style') && url.includes('/assets/');
    var isThemesFetch = url === self.location.origin + '/themes.json';
    if ((!isTasfa && !isDirectImageAsset && !isStaticScript && !isThemesFetch) || event.request.method !== 'GET') return;
    if (event.request.headers.get('Range') || event.request.destination === 'video' || event.request.destination === 'audio') return;
    /* Cache same-origin static assets. On high-RTT links this avoids a full
     * round trip for JS/CSS/images that rarely change. Stale responses are
     * still served from cache when the network is down. */
    event.respondWith(
        cachedOrFetch(event.request, STATIC_CACHE, STATIC_MAX_AGE, { maxRetries: 5, baseDelay: 200, firefoxFallback: true }).catch(function(err) {
            if (isDirectImageAsset) {
                /* For image assets, returning JSON causes Firefox to report a
                   decoding/error mismatch. Return a valid 1x1 transparent PNG so
                   the browser shows a clean broken-image placeholder. */
                return emptyPngResponse(503, 'Service Unavailable');
            }
            if (isStaticScript) {
                /* Return an empty script body so the browser does not block on a
                   parse error, and so that layout.js error-event retry logic can
                   still fire a second attempt if needed. */
                return new Response('/* SW: fetch failed after retries */', {
                    status: 503,
                    headers: {'Content-Type': 'application/javascript; charset=utf-8', 'Cache-Control': 'no-store'}
                });
            }
            return new Response(JSON.stringify({ok:false, error:'network', retry:true}), {
                status: 503,
                headers: {'Content-Type':'application/json'}
            });
        })
    );
    } catch (e) {
        // Never let a service-worker exception break a page request.
        if (typeof console !== 'undefined' && console.error) console.error('[SW] fetch handler error:', e);
    }
});

var LOGO_CACHE = 'logo-cache-v2';
var TASFA_MEDIA_CACHE = 'tasfa-media-cache-v1';
var LOGO_MAX_AGE = 3600000; // 1 hour in milliseconds
var tasfaSessions = {};

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
    self.skipWaiting();
});

self.addEventListener('activate', function(e) {
    e.waitUntil(self.clients.claim());
});

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

function fetchWithRetry(request, retries) {
    retries = retries || 0;
    return fetch(request, { credentials: 'same-origin' }).then(function(response) {
        if ((response.status >= 500 || !response.ok) && retries < 3) {
            return new Promise(function(resolve) { setTimeout(resolve, 500); }).then(function() {
                return fetchWithRetry(request, retries + 1);
            });
        }
        return response;
    }).catch(function(err) {
        if (retries < 3) {
            return new Promise(function(resolve) { setTimeout(resolve, 500); }).then(function() {
                return fetchWithRetry(request, retries + 1);
            });
        }
        throw err;
    });
}

self.addEventListener('fetch', function(event) {
    try {
        var url = event.request.url;
        // Don't intercept cross-origin requests (multi-port)
        if (new URL(url).origin !== self.location.origin) return;

        if (url.indexOf(self.location.origin + '/file/download/') === 0 && event.request.method === 'GET') {
        var session = lookupTasfaSession(url);
        if (session) {
            var headers = new Headers(event.request.headers);
            headers.set('X-TASFA-Session-ID', session.sessionId);
            headers.set('X-TASFA-Session-Token', session.sessionToken);
            event.respondWith(
                fetch(new Request(url, { headers: headers, credentials: 'same-origin' })).then(function(response) {
                    if (event.request.headers.get('Range')) {
                        if (response.status === 206) return response;
                        if (session.ultraFastConnection) return response;
                        return handleRangeRequest(event.request, response);
                    }
                    return response;
                })
            );
            return;
        }
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
                    return fetch(event.request, { credentials: 'same-origin' }).then(function(networkResponse) {
                        if (!networkResponse.ok) {
                            return cachedResponse && cachedResponse.ok ? withoutCsp(cachedResponse) : networkResponse;
                        }
                        // Use blob() for Firefox compatibility when reconstructing Response
                        return networkResponse.blob().then(function(blob) {
                            var headers = new Headers(networkResponse.headers);
                            headers.set('x-sw-fetched', now.toString());
                            var modified = new Response(blob, {
                                status: networkResponse.status,
                                statusText: networkResponse.statusText,
                                headers: headers
                            });
                            cache.put(event.request, modified);
                            return withoutCsp(new Response(blob, {
                                status: networkResponse.status,
                                statusText: networkResponse.statusText,
                                headers: networkResponse.headers
                            }));
                        });
                    }).catch(function(err) {
                        if (cachedResponse && cachedResponse.ok) {
                            return withoutCsp(cachedResponse);
                        }
                        // Do not let respondWith reject on network errors
                        return new Response('', {
                            status: 404,
                            statusText: 'Not Found',
                            headers: { 'Content-Type': 'text/plain' }
                        });
                    });
                });
            })
        );
        return;
    }

    var isTasfa = url.includes('/tasfa/') || url.includes('/file/upload') || url.includes('/file/download');
    if (!isTasfa || event.request.method !== 'GET') return;
    if (event.request.headers.get('Range') || event.request.destination === 'video' || event.request.destination === 'audio') return;
    var promise = fetchWithRetry(event.request).catch(function(err) {
        return new Response(JSON.stringify({ok:false, error:'network', retry:true}), {
            status: 503,
            headers: {'Content-Type':'application/json'}
        });
    });
    event.respondWith(promise);
    } catch (e) {
        // Never let a service-worker exception break a page request.
        if (typeof console !== 'undefined' && console.error) console.error('[SW] fetch handler error:', e);
    }
});

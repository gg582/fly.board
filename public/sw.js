var LOGO_CACHE = 'logo-cache-v1';
var TASFA_MEDIA_CACHE = 'tasfa-media-cache-v1';
var LOGO_MAX_AGE = 86400000; // 86400 seconds in milliseconds

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
        var match = rangeHeader.trim().match(/^bytes=(\d+)-(\d+)?$/);
        if (!match) {
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

self.addEventListener('fetch', function(event) {
    var url = event.request.url;
    // Don't intercept cross-origin requests (multi-port)
    if (new URL(url).origin !== self.location.origin) return;

    if (url.indexOf(self.location.origin + '/__tasfa_media__/') === 0 && event.request.method === 'GET') {
        event.respondWith(
            caches.open(TASFA_MEDIA_CACHE).then(function(cache) {
                return cache.match(event.request).then(function(response) {
                    if (!response) {
                        return fetch(event.request);
                    }
                    return handleRangeRequest(event.request, response);
                });
            })
        );
        return;
    }

    // Logo / asset image / favicon caching: cache-first with 86400s expiration
    if ((event.request.destination === 'image' || url.includes('favicon')) && (isLogoUrl(url) || url.includes('favicon'))) {
        event.respondWith(
            caches.open(LOGO_CACHE).then(function(cache) {
                return cache.match(event.request).then(function(response) {
                    var now = Date.now();
                    if (response && response.ok) {
                        var fetched = response.headers.get('x-sw-fetched');
                        if (fetched && (now - parseInt(fetched, 10)) < LOGO_MAX_AGE) {
                            return response;
                        }
                    }
                    return fetch(event.request).then(function(networkResponse) {
                        if (networkResponse.ok) {
                            var clone = networkResponse.clone();
                            var headers = new Headers(clone.headers);
                            headers.set('x-sw-fetched', now.toString());
                            var modified = new Response(clone.body, {
                                status: clone.status,
                                statusText: clone.statusText,
                                headers: headers
                            });
                            cache.put(event.request, modified);
                        } else if (response && response.ok) {
                            return response; // Use expired but valid cache if network returns non-200
                        }
                        return networkResponse;
                    }).catch(function(err) {
                        if (response && response.ok) {
                            return response;
                        }
                        throw err;
                    });
                });
            })
        );
        return;
    }

    var isTasfa = url.includes('/tasfa/') || url.includes('/file/upload') || url.includes('/file/download');
    if (!isTasfa || event.request.method !== 'GET') return;
    if (event.request.headers.get('Range') || event.request.destination === 'video' || event.request.destination === 'audio') return;
    var promise = fetch(event.request).catch(function(err) {
        return new Response(JSON.stringify({ok:false, error:'network', retry:true}), {
            status: 503,
            headers: {'Content-Type':'application/json'}
        });
    });
    event.respondWith(promise);
});

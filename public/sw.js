var LOGO_CACHE = 'logo-cache-v1';
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

self.addEventListener('fetch', function(event) {
    var url = event.request.url;
    // Don't intercept cross-origin requests (multi-port)
    if (new URL(url).origin !== self.location.origin) return;

    // Logo / asset image caching: cache-first with 86400s expiration
    if (event.request.destination === 'image' && isLogoUrl(url)) {
        event.respondWith(
            caches.open(LOGO_CACHE).then(function(cache) {
                return cache.match(event.request).then(function(response) {
                    var now = Date.now();
                    if (response) {
                        var fetched = response.headers.get('x-sw-fetched');
                        if (fetched && (now - parseInt(fetched, 10)) < LOGO_MAX_AGE) {
                            return response;
                        }
                    }
                    return fetch(event.request).then(function(networkResponse) {
                        var clone = networkResponse.clone();
                        var headers = new Headers(clone.headers);
                        headers.set('x-sw-fetched', now.toString());
                        var modified = new Response(clone.body, {
                            status: clone.status,
                            statusText: clone.statusText,
                            headers: headers
                        });
                        cache.put(event.request, modified);
                        return networkResponse;
                    });
                });
            })
        );
        return;
    }

    var isTasfa = url.includes('/tasfa/') || url.includes('/file/upload') || url.includes('/file/download');
    if (!isTasfa || event.request.method !== 'GET') return;
    var promise = fetch(event.request).catch(function(err) {
        return new Response(JSON.stringify({ok:false, error:'network', retry:true}), {
            status: 503,
            headers: {'Content-Type':'application/json'}
        });
    });
    event.respondWith(promise);
});

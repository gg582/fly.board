(function() {
    var cache = new Map();

    function normalizeUrl(url) {
        if (!url) return '';
        if (url.indexOf('blob:') === 0) return '';
        try {
            var parsed = new URL(url, window.location.origin);
            return parsed.pathname;
        } catch (e) {
            var path = url.split(/[?#]/)[0];
            if (path && path.indexOf('/') !== 0 && path.indexOf('http') !== 0) {
                path = '/' + path;
            }
            return path;
        }
    }

    function handshakeUrl(baseUrl) {
        var path = normalizeUrl(baseUrl);
        if (!path) return null;
        if (path.indexOf('/file/download/') === 0) return path + '/handshake';
        if (path.indexOf('/assets/img/') === 0) return '/assets/tasfa/img/' + encodeURIComponent(path.slice('/assets/img/'.length)) + '/handshake';
        if (path.indexOf('/assets/uploads/') === 0) return '/assets/tasfa/uploads/' + encodeURIComponent(path.slice('/assets/uploads/'.length)) + '/handshake';
        return null;
    }

    function chunkUrl(baseUrl, sessionId, sessionToken, chunkIndex) {
        var path = normalizeUrl(baseUrl);
        if (!path) return null;
        if (path.indexOf('/file/download/') === 0) {
            return path + '/chunk/' + String(chunkIndex) + '?session_id=' + encodeURIComponent(sessionId) + '&session_token=' + encodeURIComponent(sessionToken);
        } else if (path.indexOf('/assets/img/') === 0) {
            return '/assets/tasfa/img/' + encodeURIComponent(path.slice('/assets/img/'.length)) + '/chunk/' + String(chunkIndex) + '?session_id=' + encodeURIComponent(sessionId) + '&session_token=' + encodeURIComponent(sessionToken);
        } else if (path.indexOf('/assets/uploads/') === 0) {
            return '/assets/tasfa/uploads/' + encodeURIComponent(path.slice('/assets/uploads/'.length)) + '/chunk/' + String(chunkIndex) + '?session_id=' + encodeURIComponent(sessionId) + '&session_token=' + encodeURIComponent(sessionToken);
        }
        return null;
    }

    function downloadHandshakeParams() {
        return {
            chunk_size: '1048576',
            link_effective_type: '4g',
            link_downlink_mbps: '10.0',
            link_rtt_ms: '50',
            link_retry_events: '0',
            link_timeout_events: '0',
            link_save_data: '0'
        };
    }

    function appendQuery(url, params) {
        var parts = [];
        Object.keys(params).forEach(function(key) {
            parts.push(encodeURIComponent(key) + '=' + encodeURIComponent(params[key]));
        });
        return url + (url.indexOf('?') === -1 ? '?' : '&') + parts.join('&');
    }

    async function fetchDownloadSession(baseUrl) {
        var hsUrl = handshakeUrl(baseUrl);
        if (!hsUrl) throw new Error('unsupported base url');
        hsUrl = appendQuery(hsUrl, downloadHandshakeParams());
        var response = await fetch(hsUrl, { credentials: 'same-origin' });
        if (!response.ok) throw new Error('handshake status ' + response.status);
        return response.json();
    }

    async function fetchChunk(baseUrl, session, chunkIndex) {
        var url = chunkUrl(baseUrl, session.session_id, session.session_token, chunkIndex);
        var response = await fetch(url, { credentials: 'same-origin' });
        if (!response.ok) throw new Error('chunk status ' + response.status);
        var buffer = await response.arrayBuffer();
        return new Uint8Array(buffer);
    }

    async function notifyDownloadComplete(sessionId, sessionToken) {
        try {
            await fetch('/file/download/complete', {
                method: 'POST',
                credentials: 'same-origin',
                headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
                body: 'session_id=' + encodeURIComponent(sessionId) + '&session_token=' + encodeURIComponent(sessionToken)
            });
        } catch (e) {}
    }

    async function fetchBlobViaTasfa(baseUrl) {
        if (cache.has(baseUrl)) {
            return cache.get(baseUrl);
        }
        var promise = (async function() {
            var session = await fetchDownloadSession(baseUrl);
            var totalSize = parseInt(session.total_size, 10) || 0;
            var chunkSize = parseInt(session.chunk_size, 10) || 1048576;
            var chunkCount = parseInt(session.chunk_count, 10) || 1;
            
            var allBytes = new Uint8Array(totalSize);
            for (var i = 0; i < chunkCount; i++) {
                var chunkData = await fetchChunk(baseUrl, session, i);
                allBytes.set(chunkData, i * chunkSize);
            }

            await notifyDownloadComplete(session.session_id, session.session_token);
            return {
                blob: new Blob([allBytes.buffer], { type: session.mime_type || 'application/octet-stream' }),
                filename: session.filename || 'download'
            };
        })();
        cache.set(baseUrl, promise);
        return promise;
    }

    function triggerDownload(baseUrl) {
        return fetchBlobViaTasfa(baseUrl).then(function(result) {
            var objectUrl = URL.createObjectURL(result.blob);
            var a = document.createElement('a');
            a.href = objectUrl;
            a.download = result.filename || 'download';
            document.body.appendChild(a);
            a.click();
            a.remove();
            setTimeout(function() { URL.revokeObjectURL(objectUrl); }, 1000);
        });
    }

    function upgradeMediaElement(el) {
        if (el.getAttribute('data-tasfa-skip') === '1') return;
        var baseUrl = el.getAttribute('data-tasfa-download') || el.getAttribute('src') || '';
        if (!baseUrl) return;
        if (el.dataset.tasfaLoaded === '1') return;
        el.dataset.tasfaLoaded = '1';
        el.setAttribute('data-tasfa-download', baseUrl);

        fetchBlobViaTasfa(baseUrl).then(function(result) {
            var objectUrl = URL.createObjectURL(result.blob);
            el.src = objectUrl;
            if (el.tagName === 'VIDEO' || el.tagName === 'AUDIO') {
                el.controls = true;
                el.load();
            }
        }).catch(function(err) {
            console.error('TASFA media load failed:', baseUrl, err);
        });
    }

    function upgradeDownloadLink(el) {
        var baseUrl = el.getAttribute('data-tasfa-download-link') || el.getAttribute('href') || '';
        if (!baseUrl) return;
        if (!el.getAttribute('data-tasfa-download-link')) el.setAttribute('data-tasfa-download-link', baseUrl);
        el.addEventListener('click', function(event) {
            event.preventDefault();
            triggerDownload(baseUrl).catch(function() {
                el.setAttribute('data-tasfa-error', '1');
            });
        });
    }

    function upgradeWithin(root) {
        if (!root || !root.querySelectorAll) return;
        
        root.querySelectorAll('img[data-tasfa-download], img[src^="/assets/img/"], img[src^="/assets/uploads/"]').forEach(upgradeMediaElement);
        root.querySelectorAll('video[src^="/file/download/"], video[src^="/assets/uploads/"], video[data-tasfa-download]').forEach(upgradeMediaElement);
        root.querySelectorAll('audio[src^="/file/download/"], audio[src^="/assets/uploads/"], audio[data-tasfa-download]').forEach(upgradeMediaElement);
        root.querySelectorAll('a[data-tasfa-download-link], a[href^="/file/download/"]').forEach(upgradeDownloadLink);
    }

    function init() {
        window.fetchBlobViaTasfa = fetchBlobViaTasfa;
        window.openTasfaDownload = triggerDownload;
        window.initMarkdownAffordances = function(root) {
            upgradeWithin(root && root.querySelectorAll ? root : document);
        };
        upgradeWithin(document);
        if (window.MutationObserver) {
            new MutationObserver(function(mutations) {
                mutations.forEach(function(mutation) {
                    mutation.addedNodes.forEach(function(node) {
                        if (node && node.nodeType === 1) {
                            upgradeWithin(node);
                        }
                    });
                });
            }).observe(document.documentElement, { childList: true, subtree: true });
        }
    }

    if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', init);
    else init();
})();

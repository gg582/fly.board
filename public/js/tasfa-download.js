(function() {
    var cache = new Map();

    async function fetchBlobViaTasfa(baseUrl) {
        if (cache.has(baseUrl)) {
            return cache.get(baseUrl);
        }
        var promise = (async function() {
            var response = await fetch(baseUrl, { credentials: 'same-origin' });
            if (!response.ok) throw new Error('Fetch failed: ' + response.status);
            var blob = await response.blob();
            var filename = baseUrl.substring(baseUrl.lastIndexOf('/') + 1) || 'download';
            return { blob: blob, filename: filename };
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

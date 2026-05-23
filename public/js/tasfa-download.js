(function() {
    var cache = new Map();
    var downloadStates = {};
    var SPACER_GIF = "data:image/gif;base64,R0lGODlhAQABAIAAAAAAAP///yH5BAEAAAAALAAAAAABAAEAAAIBRAA7";
    var CACHE_NAME = 'tasfa-small-files-v1';
    var SMALL_FILE_THRESHOLD = 5 * 1024 * 1024; // 5MB

    async function getCachedBlob(baseUrl) {
        try {
            var c = await caches.open(CACHE_NAME);
            var resp = await c.match(baseUrl);
            if (resp) return resp.blob();
        } catch (e) {}
        return null;
    }

    async function putCachedBlob(baseUrl, blob, mimeType) {
        try {
            var c = await caches.open(CACHE_NAME);
            await c.put(baseUrl, new Response(blob, {
                headers: {'Content-Type': mimeType || 'application/octet-stream'}
            }));
        } catch (e) {}
    }

    async function notifyDownloadComplete(sessionId, sessionToken) {
        try {
            await fetch('/file/download/complete', {
                method: 'POST',
                credentials: 'same-origin',
                headers: {
                    'Content-Type': 'application/x-www-form-urlencoded',
                    'X-Requested-With': 'XMLHttpRequest'
                },
                body: 'session_id=' + encodeURIComponent(sessionId) + '&session_token=' + encodeURIComponent(sessionToken)
            });
        } catch (e) {
            // best-effort: ignore completion errors
        }
    }

    function handshakeUrl(baseUrl) {
        if (baseUrl.indexOf('/file/download/') === 0) return baseUrl + '/handshake';
        if (baseUrl.indexOf('/assets/img/') === 0) return '/assets/tasfa/img/' + encodeURIComponent(baseUrl.slice('/assets/img/'.length)) + '/handshake';
        if (baseUrl.indexOf('/assets/uploads/') === 0) return '/assets/tasfa/uploads/' + encodeURIComponent(baseUrl.slice('/assets/uploads/'.length)) + '/handshake';
        if (baseUrl.indexOf('/assets/profile/') === 0) return '/assets/tasfa/profile/' + encodeURIComponent(baseUrl.slice('/assets/profile/'.length)) + '/handshake';
        return null;
    }

    function chunkUrl(baseUrl, sessionId, sessionToken, chunkIndex, span) {
        var url = null;
        if (baseUrl.indexOf('/file/download/') === 0) {
            url = baseUrl + '/chunk/' + String(chunkIndex) + '?session_id=' + encodeURIComponent(sessionId) + '&session_token=' + encodeURIComponent(sessionToken);
        } else if (baseUrl.indexOf('/assets/img/') === 0) {
            url = '/assets/tasfa/img/' + encodeURIComponent(baseUrl.slice('/assets/img/'.length)) + '/chunk/' + String(chunkIndex) + '?session_id=' + encodeURIComponent(sessionId) + '&session_token=' + encodeURIComponent(sessionToken);
        } else if (baseUrl.indexOf('/assets/uploads/') === 0) {
            url = '/assets/tasfa/uploads/' + encodeURIComponent(baseUrl.slice('/assets/uploads/'.length)) + '/chunk/' + String(chunkIndex) + '?session_id=' + encodeURIComponent(sessionId) + '&session_token=' + encodeURIComponent(sessionToken);
        } else if (baseUrl.indexOf('/assets/profile/') === 0) {
            url = '/assets/tasfa/profile/' + encodeURIComponent(baseUrl.slice('/assets/profile/'.length)) + '/chunk/' + String(chunkIndex) + '?session_id=' + encodeURIComponent(sessionId) + '&session_token=' + encodeURIComponent(sessionToken);
        }
        if (url && span > 1) url += '&span=' + String(span);
        return url;
    }

    async function fetchJson(url, retries) {
        retries = retries || 0;
        var response = await fetch(url, {
            credentials: 'same-origin',
            headers: {
                'Accept': 'application/json',
                'X-Requested-With': 'XMLHttpRequest'
            }
        });
        if (response.status === 429 && retries < 10) {
            var data = await response.json().catch(function() { return {}; });
            var delay = (data.retry_after || 3) * 1000;
            await new Promise(function(r) { setTimeout(r, delay); });
            return fetchJson(url, retries + 1);
        }
        if (!response.ok) throw new Error('handshake:' + response.status);
        return response.json();
    }

    function normalizeDownloadSession(session) {
        if (!session) return null;
        var chunkCount = Math.max(1, Number(session.chunk_count) || 1);
        var hw = Math.max(1, Number(navigator.hardwareConcurrency) || 4);
        return {
            sessionId: session.session_id,
            sessionToken: session.session_token,
            chunkSize: Math.max(1, Number(session.chunk_size) || 1),
            chunkCount: chunkCount,
            totalSize: Math.max(0, Number(session.total_size) || 0),
            mimeType: session.mime_type || 'application/octet-stream',
            filename: session.filename || 'download',
            maxParallel: Math.max(1, Math.min(Number(session.max_parallel_chunks) || hw * 2, chunkCount)),
            coalesceChunks: Math.max(1, Math.min(Number(session.coalesce_chunks) || 4, 64))
        };
    }

    async function fetchDownloadSession(baseUrl) {
        var hsUrl = handshakeUrl(baseUrl);
        if (!hsUrl) throw new Error('unsupported base url');
        var session = normalizeDownloadSession(await fetchJson(hsUrl));
        if (!session || !session.sessionId || !session.sessionToken) {
            throw new Error((session && session.error) || 'invalid handshake');
        }
        return session;
    }

    function chunkByteSize(session, chunkIndex) {
        if (chunkIndex === session.chunkCount - 1) {
            var rem = session.totalSize - (chunkIndex * session.chunkSize);
            return Math.max(0, rem);
        }
        return Math.min(session.chunkSize, session.totalSize - (chunkIndex * session.chunkSize));
    }

    function _dlId(baseUrl) {
        return 'tasfa-dl-' + baseUrl.replace(/[^a-z0-9]/gi, '_');
    }

    function createDownloadProgress(baseUrl, filename) {
        var id = _dlId(baseUrl);
        if (document.getElementById(id)) return;
        var wrap = document.createElement('div');
        wrap.id = id;
        wrap.className = 'tasfa-download-progress';
        var safeName = (filename || 'Downloading...').replace(/[<>'"]/g, '');
        wrap.innerHTML = '<div class="tasfa-download-progress-text"><span class="tasfa-dl-name">' + safeName + '</span><span class="tasfa-dl-pct">0%</span></div><div class="tasfa-download-progress-bar"><div class="tasfa-download-progress-inner" style="width:0%"></div></div>';
        document.body.appendChild(wrap);
    }

    function updateDownloadProgress(baseUrl, percent, filename) {
        var el = document.getElementById(_dlId(baseUrl));
        if (!el) return;
        var pctEl = el.querySelector('.tasfa-dl-pct');
        var barEl = el.querySelector('.tasfa-download-progress-inner');
        var nameEl = el.querySelector('.tasfa-dl-name');
        if (pctEl) pctEl.textContent = percent + '%';
        if (barEl) barEl.style.width = percent + '%';
        if (nameEl && filename) nameEl.textContent = filename.replace(/[<>'"]/g, '');
    }

    function removeDownloadProgress(baseUrl) {
        var el = document.getElementById(_dlId(baseUrl));
        if (el) el.remove();
    }

    function fetchChunk(baseUrl, session, allBytes, chunkIndex, span, retries) {
        retries = retries || 0;
        return new Promise(function(resolve, reject) {
            var url = chunkUrl(baseUrl, session.sessionId, session.sessionToken, chunkIndex, span);
            var xhr = new XMLHttpRequest();
            xhr.open('GET', url, true);
            xhr.responseType = 'arraybuffer';
            xhr.setRequestHeader('X-Requested-With', 'XMLHttpRequest');
            xhr.timeout = 120000;
            xhr.onload = function() {
                if (xhr.status === 429 && retries < 10) {
                    var delay = 3000;
                    try {
                        var resp = JSON.parse(xhr.responseText);
                        if (resp.retry_after) delay = resp.retry_after * 1000;
                    } catch(e) {}
                    setTimeout(function() {
                        fetchChunk(baseUrl, session, allBytes, chunkIndex, span, retries + 1).then(resolve).catch(reject);
                    }, delay);
                    return;
                }
                if (xhr.status < 200 || xhr.status >= 300) {
                    if (retries < 3) {
                        setTimeout(function() {
                            fetchChunk(baseUrl, session, allBytes, chunkIndex, span, retries + 1).then(resolve).catch(reject);
                        }, 2000 * (retries + 1));
                        return;
                    }
                    reject(new Error('chunk:' + xhr.status));
                    return;
                }
                var buffer = xhr.response;
                if (!buffer) { reject(new Error('empty response')); return; }
                var data = new Uint8Array(buffer);
                var offset = 0;
                var totalReceived = 0;
                for (var i = 0; i < span; i++) {
                    var idx = chunkIndex + i;
                    if (idx >= session.chunkCount) break;
                    var size = chunkByteSize(session, idx);
                    if (offset + size > data.byteLength) {
                        size = data.byteLength - offset;
                    }
                    if (size > 0) {
                        allBytes.set(data.subarray(offset, offset + size), idx * session.chunkSize);
                    }
                    offset += size;
                    totalReceived += size;
                }
                resolve(totalReceived);
            };
            xhr.onerror = function() {
                if (retries < 5) {
                    setTimeout(function() {
                        fetchChunk(baseUrl, session, allBytes, chunkIndex, span, retries + 1).then(resolve).catch(reject);
                    }, 2000 * (retries + 1));
                    return;
                }
                reject(new Error('network'));
            };
            xhr.ontimeout = function() {
                if (retries < 5) {
                    setTimeout(function() {
                        fetchChunk(baseUrl, session, allBytes, chunkIndex, span, retries + 1).then(resolve).catch(reject);
                    }, 2000 * (retries + 1));
                    return;
                }
                reject(new Error('timeout'));
            };
            xhr.send();
        });
    }

    async function fetchBlobViaTasfa(baseUrl, options) {
        if (cache.has(baseUrl)) {
            var cached = cache.get(baseUrl);
            if (cached && typeof cached.then === 'function') return cached;
        }

        var silent = options && options.silent;

        var promise = (async function() {
            var session = await fetchDownloadSession(baseUrl);

            if (session.totalSize <= SMALL_FILE_THRESHOLD) {
                var cachedBlob = await getCachedBlob(baseUrl);
                if (cachedBlob) {
                    await notifyDownloadComplete(session.sessionId, session.sessionToken);
                    return {blob: cachedBlob, filename: session.filename};
                }
            }

            var allBytes = new Uint8Array(session.totalSize);
            var sharedState = {
                chunkCount: session.chunkCount,
                totalSize: session.totalSize,
                nextChunk: 0,
                completedChunks: 0,
                downloadedBytes: 0,
                failed: null
            };
            downloadStates[baseUrl] = {
                sharedState: sharedState,
                filename: session.filename
            };

            var pending = [];
            var bitmap = new Array(session.chunkCount).fill(0);
            var SPAN = session.coalesceChunks || 4;
            for (var i = 0; i < session.chunkCount; i += SPAN) pending.push(i);

            async function worker() {
                while (pending.length > 0 && !sharedState.failed) {
                    var idx = pending.pop();
                    if (bitmap[idx]) continue;
                    try {
                        var actualSpan = Math.min(SPAN, session.chunkCount - idx);
                        var received = await fetchChunk(baseUrl, session, allBytes, idx, actualSpan);
                        for (var i = 0; i < actualSpan; i++) {
                            var doneIdx = idx + i;
                            if (doneIdx < session.chunkCount && !bitmap[doneIdx]) {
                                bitmap[doneIdx] = 1;
                                sharedState.completedChunks += 1;
                                sharedState.downloadedBytes += chunkByteSize(session, doneIdx);
                            }
                        }
                    } catch (e) {
                        sharedState.failed = e || new Error('download failed');
                    }
                }
            }

            var workers = [];
            for (var i = 0; i < session.maxParallel; i++) {
                workers.push(worker());
            }
            await Promise.all(workers);

            if (sharedState.failed) throw sharedState.failed;

            var result = {
                blob: new Blob([allBytes.buffer], { type: session.mimeType }),
                filename: session.filename
            };

            if (session.totalSize <= SMALL_FILE_THRESHOLD) {
                await putCachedBlob(baseUrl, result.blob, session.mimeType);
            }

            await notifyDownloadComplete(session.sessionId, session.sessionToken);

            return result;
        })();

        if (!silent) {
            createDownloadProgress(baseUrl);
            var progressInterval = setInterval(function() {
                var state = downloadStates[baseUrl];
                if (!state) {
                    clearInterval(progressInterval);
                    return;
                }
                var ss = state.sharedState;
                var pct = ss.totalSize > 0 ? Math.round((ss.downloadedBytes / ss.totalSize) * 100) : 0;
                updateDownloadProgress(baseUrl, Math.min(100, pct), state.filename);
                if (ss.completedChunks >= ss.chunkCount || ss.failed) {
                    clearInterval(progressInterval);
                }
            }, 150);
        }

        cache.set(baseUrl, promise);
        try {
            var result = await promise;
            if (!silent) {
                updateDownloadProgress(baseUrl, 100, result.filename);
                setTimeout(function() { removeDownloadProgress(baseUrl); delete downloadStates[baseUrl]; }, 800);
            }
            return result;
        } catch (error) {
            if (!silent) {
                removeDownloadProgress(baseUrl);
                delete downloadStates[baseUrl];
            }
            cache.delete(baseUrl);
            throw error;
        }
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

    function setMediaSource(el, baseUrl) {
        if (!baseUrl || el.dataset.tasfaLoaded === '1') return;
        el.dataset.tasfaLoaded = '1';
        fetchBlobViaTasfa(baseUrl, { silent: true }).then(function(result) {
            var objectUrl = URL.createObjectURL(result.blob);
            if (el.tagName === 'IMG') el.src = objectUrl;
            else el.src = objectUrl;
            if (el.tagName === 'VIDEO' || el.tagName === 'AUDIO') el.load();
        }).catch(function(err) {
            console.error('TASFA media load failed:', baseUrl, err);
            el.setAttribute('data-tasfa-error', '1');
            el.dataset.tasfaLoaded = '';
        });
    }

    var _mediaObserver = null;
    function observeMediaLoad(el, baseUrl) {
        if (!window.IntersectionObserver) {
            setMediaSource(el, baseUrl);
            return;
        }
        if (!_mediaObserver) {
            _mediaObserver = new IntersectionObserver(function(entries) {
                entries.forEach(function(entry) {
                    if (entry.isIntersecting) {
                        var target = entry.target;
                        var url = target.getAttribute('data-tasfa-download') || target.getAttribute('src') || '';
                        if (url) setMediaSource(target, url);
                        _mediaObserver.unobserve(target);
                    }
                });
            }, { rootMargin: '200px' });
        }
        _mediaObserver.observe(el);
    }

    function upgradeMediaElement(el) {
        var baseUrl = el.getAttribute('data-tasfa-download') || el.getAttribute('src') || '';
        if (!/^\/(file\/download|assets\/img|assets\/uploads|assets\/profile)\//.test(baseUrl)) return;
        if (!el.getAttribute('data-tasfa-download')) el.setAttribute('data-tasfa-download', baseUrl);
        if (el.tagName === 'IMG') el.src = SPACER_GIF;
        else el.removeAttribute('src');
        observeMediaLoad(el, baseUrl);
    }

    function upgradeDownloadLink(el) {
        var baseUrl = el.getAttribute('data-tasfa-download-link') || el.getAttribute('href') || '';
        if (!/^\/file\/download\//.test(baseUrl)) return;
        if (!el.getAttribute('data-tasfa-download-link')) el.setAttribute('data-tasfa-download-link', baseUrl);
        el.addEventListener('click', function(event) {
            event.preventDefault();
            triggerDownload(baseUrl).catch(function() {
                el.setAttribute('data-tasfa-error', '1');
            });
        });
    }

    function wrapMediaForDownload(el) {
        if (el.closest('.media-download-wrap')) return;
        var wrap = document.createElement('div');
        wrap.className = 'media-download-wrap';
        el.parentNode.insertBefore(wrap, el);
        wrap.appendChild(el);

        var btn = document.createElement('button');
        btn.type = 'button';
        btn.className = 'media-download-btn';
        btn.textContent = 'Download';
        btn.addEventListener('click', function(e) {
            e.preventDefault();
            e.stopPropagation();
            var url = el.getAttribute('data-tasfa-download') || el.getAttribute('src') || '';
            if (url && window.openTasfaDownload) {
                window.openTasfaDownload(url);
            }
        });
        wrap.appendChild(btn);
    }

    function upgradeWithin(root) {
        if (!root || !root.querySelectorAll) return;
        root.querySelectorAll('.markdown-body img[src^="blob:"], .markdown-body video[src^="blob:"], .markdown-body audio[src^="blob:"]').forEach(function(el) { el.remove(); });
        root.querySelectorAll('img[data-tasfa-download], img[src^="/assets/img/"], img[src^="/assets/profile/"]').forEach(upgradeMediaElement);
        root.querySelectorAll('video[src^="/file/download/"], video[data-tasfa-download], audio[src^="/file/download/"], audio[data-tasfa-download]').forEach(upgradeMediaElement);
        root.querySelectorAll('a[data-tasfa-download-link], a[href^="/file/download/"]').forEach(upgradeDownloadLink);
        root.querySelectorAll('.markdown-body img, .markdown-body video, .markdown-body audio').forEach(wrapMediaForDownload);
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

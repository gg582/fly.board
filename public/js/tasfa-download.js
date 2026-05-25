(function() {
    var cache = new Map();
    var downloadStates = {};
    var SPACER_GIF = "data:image/gif;base64,R0lGODlhAQABAIAAAAAAAP///yH5BAEAAAAALAAAAAABAAEAAAIBRAA7";
    var CACHE_NAME = 'tasfa-small-files-v1';
    var SMALL_FILE_THRESHOLD = 5 * 1024 * 1024; // 5MB
    var DOWNLOAD_CHUNK_STORE = 'tasfa_download_chunk_size_v3';
    var DOWNLOAD_CHUNK_MIN = 512 * 1024;
    var DOWNLOAD_CHUNK_DEFAULT = 2 * 1024 * 1024;
    var DOWNLOAD_CHUNK_MOBILE_DEFAULT = 1024 * 1024;
    var DOWNLOAD_CHUNK_MAX = 16 * 1024 * 1024;
    var DOWNLOAD_CHUNK_STEP_UP = 512 * 1024;
    var DOWNLOAD_CHUNK_STEP_DOWN = 256 * 1024;
    var DOWNLOAD_REQUEST_BYTES_MAX = 64 * 1024 * 1024;
    var DOWNLOAD_CONNECT_TIMEOUT_MS = 3000;

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

    function clampNumber(value, minValue, maxValue) {
        value = Number(value);
        if (!Number.isFinite(value)) value = minValue;
        return Math.max(minValue, Math.min(maxValue, value));
    }

    function isLikelyMobile() {
        return /Mobi|Android|iPhone|iPad/i.test(navigator.userAgent || '');
    }

    function getConnectionSnapshot() {
        var c = navigator.connection || navigator.mozConnection || navigator.webkitConnection || {};
        return {
            effectiveType: c.effectiveType || '',
            downlink: Number(c.downlink || 0),
            rtt: Number(c.rtt || 0),
            saveData: c.saveData ? '1' : '0'
        };
    }

    function preferredDownloadChunkSize() {
        var saved = 0;
        try { saved = Number(localStorage.getItem(DOWNLOAD_CHUNK_STORE) || 0); } catch (e) {}
        var base = saved || (isLikelyMobile() ? DOWNLOAD_CHUNK_MOBILE_DEFAULT : DOWNLOAD_CHUNK_DEFAULT);
        base = Math.round(base / DOWNLOAD_CHUNK_STEP_DOWN) * DOWNLOAD_CHUNK_STEP_DOWN;
        return clampNumber(base, DOWNLOAD_CHUNK_MIN, DOWNLOAD_CHUNK_MAX);
    }

    function rememberDownloadChunkSize(value) {
        var next = Math.round(clampNumber(value, DOWNLOAD_CHUNK_MIN, DOWNLOAD_CHUNK_MAX) / DOWNLOAD_CHUNK_STEP_DOWN) * DOWNLOAD_CHUNK_STEP_DOWN;
        try { localStorage.setItem(DOWNLOAD_CHUNK_STORE, String(next)); } catch (e) {}
        return next;
    }

    function appendQuery(url, params) {
        var parts = [];
        Object.keys(params).forEach(function(key) {
            var value = params[key];
            if (value === undefined || value === null || value === '') return;
            parts.push(encodeURIComponent(key) + '=' + encodeURIComponent(String(value)));
        });
        if (!parts.length) return url;
        return url + (url.indexOf('?') === -1 ? '?' : '&') + parts.join('&');
    }

    function downloadHandshakeParams() {
        var conn = getConnectionSnapshot();
        return {
            chunk_size: String(preferredDownloadChunkSize()),
            link_effective_type: conn.effectiveType,
            link_downlink_mbps: conn.downlink > 0 ? conn.downlink.toFixed(2) : '',
            link_rtt_ms: conn.rtt > 0 ? String(Math.round(conn.rtt)) : '',
            link_retry_events: '0',
            link_timeout_events: '0',
            link_save_data: conn.saveData
        };
    }

    function wynnEpsilonAitken(samples) {
        if (!samples || samples.length < 3) return samples && samples.length ? samples[samples.length - 1] : 0.75;
        var s0 = Number(samples[samples.length - 3]);
        var s1 = Number(samples[samples.length - 2]);
        var s2 = Number(samples[samples.length - 1]);
        var denom = s2 - (2 * s1) + s0;
        if (!Number.isFinite(denom) || Math.abs(denom) < 1e-6) return s2;
        var accelerated = s0 - (((s1 - s0) * (s1 - s0)) / denom);
        return Number.isFinite(accelerated) ? accelerated : s2;
    }

    function pushDownloadQuality(session, value) {
        session.qualitySamples = session.qualitySamples || [];
        session.qualitySamples.push(clampNumber(value, 0, 1));
        if (session.qualitySamples.length > 12) session.qualitySamples.shift();
    }

    function predictedDownloadQuality(session) {
        return clampNumber(wynnEpsilonAitken(session.qualitySamples || []), 0, 1);
    }

    function applyPredictedDownloadRule(session) {
        var predicted = predictedDownloadQuality(session);
        if (predicted < 0.25) {
            var lowFloor = Math.min(session.maxParallel || 1, isLikelyMobile() ? 2 : 4);
            session.currentSpan = Math.max(1, Math.min(session.currentSpan || 1, 2));
            session.targetParallel = Math.max(lowFloor, Math.min(session.targetParallel || 1, Math.ceil((session.maxParallel || 1) / 3)));
        } else if (predicted < 0.45) {
            var guardedFloor = Math.min(session.maxParallel || 1, isLikelyMobile() ? 3 : 6);
            session.currentSpan = Math.max(1, Math.min(session.currentSpan || 1, 2));
            session.targetParallel = Math.max(guardedFloor, Math.min(session.targetParallel || 1, Math.ceil((session.maxParallel || 1) / 2)));
        }
    }

    function tuneDownloadSuccess(session, bytes, durationMs) {
        session.successEvents = (session.successEvents || 0) + 1;
        session.fastStreak = (session.fastStreak || 0) + 1;
        var mbps = durationMs > 0 ? ((bytes * 8) / durationMs / 1000) : 0;
        session.ewmaMbps = session.ewmaMbps ? (session.ewmaMbps * 0.75 + mbps * 0.25) : mbps;
        pushDownloadQuality(session, clampNumber(mbps / 25, 0.1, 1));
        if (session.fastStreak >= 6 && (session.ewmaMbps >= 25 || durationMs < 10000)) {
            rememberDownloadChunkSize(preferredDownloadChunkSize() + DOWNLOAD_CHUNK_STEP_UP);
            session.fastStreak = 0;
        }
        if (session.currentSpan < session.maxSpan && session.successEvents % 2 === 0) {
            session.currentSpan += 1;
        }
        if (session.targetParallel < session.maxParallel && session.successEvents % 3 === 0) {
            session.targetParallel += 1;
        }
    }

    function tuneDownloadFailure(session, kind) {
        session.failureEvents = (session.failureEvents || 0) + 1;
        session.fastStreak = 0;
        session.currentSpan = 1;
        pushDownloadQuality(session, kind === 'timeout' ? 0.05 : 0.15);
        rememberDownloadChunkSize(preferredDownloadChunkSize() - DOWNLOAD_CHUNK_STEP_DOWN);
        if (session.currentSpan > 1) session.currentSpan -= 1;
        var floor = Math.min(session.maxParallel || 1, isLikelyMobile() ? 2 : 4);
        if ((kind === 'timeout' || session.failureEvents % 4 === 0) && session.targetParallel > floor) {
            session.targetParallel -= 1;
        }
    }

    async function notifyDownloadComplete(sessionId, sessionToken) {
        var controller = window.AbortController ? new AbortController() : null;
        var timeoutId = null;
        try {
            if (controller) {
                timeoutId = setTimeout(function() { controller.abort(); }, DOWNLOAD_CONNECT_TIMEOUT_MS);
            }
            await fetch('/file/download/complete', {
                method: 'POST',
                credentials: 'same-origin',
                headers: {
                    'Content-Type': 'application/x-www-form-urlencoded',
                    'X-Requested-With': 'XMLHttpRequest'
                },
                body: 'session_id=' + encodeURIComponent(sessionId) + '&session_token=' + encodeURIComponent(sessionToken),
                signal: controller ? controller.signal : undefined
            });
        } catch (e) {
            // best-effort: ignore completion errors
        } finally {
            if (timeoutId) clearTimeout(timeoutId);
        }
    }

    function handshakeUrl(baseUrl) {
        if (baseUrl.indexOf('/file/download/') === 0) return baseUrl + '/handshake';
        if (baseUrl.indexOf('/assets/img/') === 0) return '/assets/tasfa/img/' + encodeURIComponent(baseUrl.slice('/assets/img/'.length)) + '/handshake';
        if (baseUrl.indexOf('/assets/uploads/') === 0) return '/assets/tasfa/uploads/' + encodeURIComponent(baseUrl.slice('/assets/uploads/'.length)) + '/handshake';
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
        }
        if (url && span > 1) url += '&span=' + String(span);
        return url;
    }

    async function fetchJson(url, retries) {
        retries = retries || 0;
        var controller = window.AbortController ? new AbortController() : null;
        var timeoutId = null;
        var response;
        try {
            if (controller) {
                timeoutId = setTimeout(function() { controller.abort(); }, DOWNLOAD_CONNECT_TIMEOUT_MS);
            }
            response = await fetch(url, {
                credentials: 'same-origin',
                headers: {
                    'Accept': 'application/json',
                    'X-Requested-With': 'XMLHttpRequest'
                },
                signal: controller ? controller.signal : undefined
            });
        } finally {
            if (timeoutId) clearTimeout(timeoutId);
        }
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
        var chunkSize = Math.max(1, Number(session.chunk_size) || 1);
        var maxParallel = Math.max(1, Math.min(Number(session.max_parallel_chunks) || hw * 3, chunkCount));
        var initialParallel = Math.max(1, Math.min(Number(session.initial_parallel_chunks) || maxParallel, maxParallel));
        var coalesce = Math.max(1, Math.min(Number(session.coalesce_chunks) || 8, 64));
        var maxSpan = Math.max(1, Math.min(64, Math.floor(DOWNLOAD_REQUEST_BYTES_MAX / chunkSize) || 1));
        return {
            sessionId: session.session_id,
            sessionToken: session.session_token,
            chunkSize: chunkSize,
            chunkCount: chunkCount,
            totalSize: Math.max(0, Number(session.total_size) || 0),
            mimeType: session.mime_type || 'application/octet-stream',
            filename: session.filename || 'download',
            maxParallel: maxParallel,
            targetParallel: initialParallel,
            coalesceChunks: Math.min(coalesce, maxSpan),
            currentSpan: Math.min(coalesce, maxSpan),
            maxSpan: maxSpan,
            successEvents: 0,
            failureEvents: 0,
            fastStreak: 0,
            ewmaMbps: 0,
            qualitySamples: []
        };
    }

    async function fetchDownloadSession(baseUrl) {
        var hsUrl = handshakeUrl(baseUrl);
        if (!hsUrl) throw new Error('unsupported base url');
        hsUrl = appendQuery(hsUrl, downloadHandshakeParams());
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

    function armXhrIdleTimeout(xhr, timeoutMs) {
        var timer = null;
        function clear() {
            if (timer) {
                clearTimeout(timer);
                timer = null;
            }
        }
        function arm() {
            clear();
            timer = setTimeout(function() {
                try { xhr._tasfaIdleTimeout = true; } catch (e) {}
                try { xhr.abort(); } catch (e) {}
            }, timeoutMs);
        }
        arm();
        return { arm: arm, clear: clear };
    }

    function fetchChunk(baseUrl, session, allBytes, chunkIndex, span, retries) {
        retries = retries || 0;
        return new Promise(function(resolve, reject) {
            var startedAt = Date.now();
            var url = chunkUrl(baseUrl, session.sessionId, session.sessionToken, chunkIndex, span);
            var xhr = new XMLHttpRequest();
            xhr.open('GET', url, true);
            xhr.responseType = 'arraybuffer';
            xhr.setRequestHeader('X-Requested-With', 'XMLHttpRequest');
            var expectedBytes = 0;
            for (var e = 0; e < span; e++) {
                if (chunkIndex + e >= session.chunkCount) break;
                expectedBytes += chunkByteSize(session, chunkIndex + e);
            }
            var watchdog = armXhrIdleTimeout(xhr, DOWNLOAD_CONNECT_TIMEOUT_MS);
            xhr.onprogress = function() {
                watchdog.arm();
            };
            xhr.onload = function() {
                watchdog.clear();
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
                    reject(new Error('chunk:' + xhr.status));
                    return;
                }
                var buffer = xhr.response;
                if (!buffer) { reject(new Error('empty response')); return; }
                var data = new Uint8Array(buffer);
                if (data.byteLength !== expectedBytes) {
                    reject(new Error('short response'));
                    return;
                }
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
                resolve({ bytes: totalReceived, durationMs: Date.now() - startedAt });
            };
            xhr.onerror = function() {
                watchdog.clear();
                reject(new Error('network'));
            };
            xhr.ontimeout = function() {
                watchdog.clear();
                reject(new Error('timeout'));
            };
            xhr.onabort = function() {
                watchdog.clear();
                if (xhr._tasfaIdleTimeout) reject(new Error('timeout'));
                else reject(new Error('abort'));
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
        var onProgress = options && options.onProgress;

        var promise = (async function() {
            var session = await fetchDownloadSession(baseUrl);

            if (session.totalSize <= SMALL_FILE_THRESHOLD) {
                var cachedBlob = await getCachedBlob(baseUrl);
                if (cachedBlob) {
                    await notifyDownloadComplete(session.sessionId, session.sessionToken);
                    if (onProgress) onProgress(100);
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
            var inflight = new Array(session.chunkCount).fill(0);
            var retryCounts = new Array(session.chunkCount).fill(0);
            var activeFetches = 0;
            for (var i = 0; i < session.chunkCount; i++) pending.push(i);

            function claimSpan() {
                while (pending.length > 0) {
                    var idx = pending.shift();
                    if (bitmap[idx] || inflight[idx]) continue;
                    applyPredictedDownloadRule(session);
                    var span = Math.min(session.currentSpan || 1, session.chunkCount - idx);
                    while (span > 1 && (bitmap[idx + span - 1] || inflight[idx + span - 1])) span -= 1;
                    for (var j = 0; j < span; j++) inflight[idx + j] = 1;
                    return { idx: idx, span: span };
                }
                return null;
            }

            function releaseSpan(idx, span) {
                for (var j = 0; j < span; j++) {
                    if (idx + j < inflight.length) inflight[idx + j] = 0;
                }
            }

            async function worker() {
                while (!sharedState.failed) {
                    if (activeFetches >= (session.targetParallel || 1)) {
                        await new Promise(function(r) { setTimeout(r, 20); });
                        continue;
                    }
                    var claim = claimSpan();
                    if (!claim) break;
                    activeFetches += 1;
                    try {
                        var received = await fetchChunk(baseUrl, session, allBytes, claim.idx, claim.span);
                        releaseSpan(claim.idx, claim.span);
                        activeFetches = Math.max(0, activeFetches - 1);
                        tuneDownloadSuccess(session, received.bytes || 0, received.durationMs || 0);
                        for (var i = 0; i < claim.span; i++) {
                            var doneIdx = claim.idx + i;
                            if (doneIdx < session.chunkCount && !bitmap[doneIdx]) {
                                bitmap[doneIdx] = 1;
                                sharedState.completedChunks += 1;
                                sharedState.downloadedBytes += chunkByteSize(session, doneIdx);
                                if (onProgress) {
                                    var pct = Math.round((sharedState.downloadedBytes / sharedState.totalSize) * 100);
                                    onProgress(pct);
                                }
                            }
                        }
                    } catch (e) {
                        releaseSpan(claim.idx, claim.span);
                        activeFetches = Math.max(0, activeFetches - 1);
                        var msg = e && e.message ? e.message : 'network';
                        tuneDownloadFailure(session, msg.indexOf('timeout') !== -1 ? 'timeout' : 'network');
                        var exhausted = false;
                        for (var k = 0; k < claim.span; k++) {
                            var ci = claim.idx + k;
                            if (ci >= session.chunkCount || bitmap[ci]) continue;
                            retryCounts[ci] = (retryCounts[ci] || 0) + 1;
                            if (retryCounts[ci] > 80) exhausted = true;
                            pending.push(ci);
                        }
                        if (exhausted) sharedState.failed = e || new Error('download failed');
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
            if (onProgress) onProgress(100);

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
        if (el.getAttribute('data-tasfa-skip') === '1') return;
        el.dataset.tasfaLoaded = '1';
        
        var wrap = el.closest('.media-loading-wrap');
        var progressInner = wrap ? wrap.querySelector('.media-loading-progress-inner') : null;

        fetchBlobViaTasfa(baseUrl, { 
            silent: true,
            onProgress: function(pct) {
                if (progressInner) progressInner.style.width = pct + '%';
            }
        }).then(function(result) {
            var objectUrl = URL.createObjectURL(result.blob);
            
            if (el.tagName === 'IMG' && result.blob.type && result.blob.type.indexOf('video/') === 0) {
                var video = document.createElement('video');
                video.setAttribute('data-tasfa-download', baseUrl);
                video.setAttribute('data-tasfa-loaded', '1');
                video.src = objectUrl;
                video.style.maxWidth = '100%';
                video.style.height = 'auto';
                video.style.display = 'block';
                video.controls = true;
                
                var poster = el.getAttribute('poster');
                if (poster) video.setAttribute('poster', poster);
                
                el.parentNode.replaceChild(video, el);
                upgradeVideoElement(video);
                return;
            }
            
            if (el.tagName === 'IMG') el.src = objectUrl;
            else el.src = objectUrl;
            if (el.tagName === 'VIDEO' || el.tagName === 'AUDIO') {
                el.load();
                if (el.tagName === 'VIDEO' && isLikelyMobile()) {
                    el.controls = true;
                    el.play().catch(function(){});
                }
            }
            if (wrap) {
                setTimeout(function() {
                    var p = wrap.querySelector('.media-loading-progress');
                    if (p) p.style.opacity = '0';
                }, 500);
            }
        }).catch(function(err) {
            console.error('TASFA media load failed:', baseUrl, err);
            el.setAttribute('data-tasfa-error', '1');
            el.dataset.tasfaLoaded = '';
            if (wrap) {
                var btn = document.createElement('button');
                btn.className = 'media-load-btn';
                btn.textContent = 'Retry Load';
                btn.onclick = function() {
                    btn.remove();
                    setMediaSource(el, baseUrl);
                };
                wrap.appendChild(btn);
            }
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
        if (el.getAttribute('data-tasfa-skip') === '1') return;
        var baseUrl = el.getAttribute('data-tasfa-download') || el.getAttribute('src') || '';
        if (!/^\/(file\/download|assets\/img|assets\/uploads)\//.test(baseUrl)) return;
        
        if (el.closest('.media-loading-wrap')) return;

        var wrap = document.createElement('div');
        wrap.className = 'media-loading-wrap';
        el.parentNode.insertBefore(wrap, el);
        wrap.appendChild(el);
        
        var progress = document.createElement('div');
        progress.className = 'media-loading-progress';
        progress.innerHTML = '<div class="media-loading-progress-inner"></div>';
        wrap.appendChild(progress);

        if (el.tagName === 'VIDEO') el.src = '';
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
        if (el.closest('.media-download-wrap') || el.closest('.media-loading-wrap')) return;
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

    function upgradeVideoElement(el) {
        if (el.getAttribute('data-tasfa-skip') === '1') return;
        var baseUrl = el.getAttribute('data-tasfa-download') || el.getAttribute('src') || '';
        if (!/^\/(file\/download|assets\/uploads)\//.test(baseUrl)) return;
        
        var isLoaded = el.getAttribute('data-tasfa-loaded') === '1';
        if (!el.getAttribute('data-tasfa-download')) el.setAttribute('data-tasfa-download', baseUrl);
        if (el.closest('.tasfa-video-shell')) {
            if (isLoaded && el.src && el.src.indexOf('blob:') === 0) {
                var ov = el.parentNode.querySelector('.tasfa-video-overlay');
                if (ov) ov.style.display = 'none';
            }
            return;
        }

        var wrap = document.createElement('div');
        wrap.className = 'tasfa-video-shell';
        wrap.style.position = 'relative';
        wrap.style.width = '100%';
        wrap.style.background = '#000';
        wrap.style.borderRadius = '8px';
        wrap.style.overflow = 'hidden';
        wrap.style.minHeight = '180px';
        wrap.style.aspectRatio = el.style.aspectRatio || '16 / 9';
        el.parentNode.insertBefore(wrap, el);
        wrap.appendChild(el);

        el.style.width = '100%';
        el.style.height = '100%';
        el.style.display = 'block';
        el.style.objectFit = 'contain';

        el.setAttribute('controls', 'controls');
        el.setAttribute('preload', 'none');
        el.setAttribute('playsinline', 'playsinline');
        
        if (!isLoaded) {
            el.src = '';
            el.removeAttribute('src');
            try { el.load(); } catch(e) {}
        }

        var overlay = document.createElement('div');
        overlay.className = 'tasfa-video-overlay';
        overlay.style.position = 'absolute';
        overlay.style.inset = '0';
        overlay.style.display = 'flex';
        overlay.style.flexDirection = 'column';
        overlay.style.alignItems = 'center';
        overlay.style.justifyContent = 'center';
        overlay.style.gap = '10px';
        overlay.style.padding = '16px';
        var poster = el.getAttribute('poster');
        if (poster) {
            overlay.style.backgroundImage = 'linear-gradient(180deg, rgba(0,0,0,0.36), rgba(0,0,0,0.62)), url("' + poster + '")';
            overlay.style.backgroundSize = 'cover';
            overlay.style.backgroundPosition = 'center';
        } else {
            overlay.style.background = 'linear-gradient(180deg, rgba(0,0,0,0.36), rgba(0,0,0,0.62))';
        }
        overlay.style.color = '#fff';
        overlay.style.zIndex = '2';
        if (el.getAttribute('data-tasfa-loaded') === '1') {
            overlay.style.display = 'none';
        }

        var status = document.createElement('div');
        status.className = 'tasfa-video-status';
        status.textContent = 'Ready to load';
        status.style.fontSize = '14px';
        status.style.fontWeight = '500';
        status.style.opacity = '0.95';
        status.style.textShadow = '0 1px 4px rgba(0,0,0,0.6)';
        status.style.fontFamily = 'inherit';

        var btn = document.createElement('button');
        btn.type = 'button';
        btn.className = 'tasfa-video-load-btn btn';
        btn.textContent = 'Play Video';
        btn.style.border = '0';
        btn.style.borderRadius = '20px';
        btn.style.padding = '10px 22px';
        btn.style.background = 'var(--accent, #0066cc)';
        btn.style.color = '#ffffff';
        btn.style.cursor = 'pointer';
        btn.style.fontWeight = '600';
        btn.style.fontSize = '14px';
        btn.style.transition = 'transform 0.2s ease, filter 0.2s ease';
        btn.style.boxShadow = '0 4px 12px rgba(0,0,0,0.3)';
        btn.addEventListener('mouseenter', function() {
            btn.style.transform = 'scale(1.05)';
            btn.style.filter = 'brightness(1.1)';
        });
        btn.addEventListener('mouseleave', function() {
            btn.style.transform = 'scale(1)';
            btn.style.filter = 'none';
        });

        var progressContainer = document.createElement('div');
        progressContainer.className = 'tasfa-video-progress';
        progressContainer.style.width = '80%';
        progressContainer.style.maxWidth = '300px';
        progressContainer.style.height = '6px';
        progressContainer.style.background = 'rgba(255,255,255,0.2)';
        progressContainer.style.borderRadius = '3px';
        progressContainer.style.marginTop = '10px';
        progressContainer.style.overflow = 'hidden';
        progressContainer.style.display = 'none';

        var progressBar = document.createElement('div');
        progressBar.style.width = '0%';
        progressBar.style.height = '100%';
        progressBar.style.background = 'var(--accent, #0066cc)';
        progressBar.style.transition = 'width 0.2s linear';
        progressContainer.appendChild(progressBar);

        function setLoadingState(active, label) {
            wrap.setAttribute('data-tasfa-loading', active ? '1' : '0');
            // Keep overlay visible but hide the button and show the progress bar
            btn.style.display = active ? 'none' : 'block';
            progressContainer.style.display = active ? 'block' : 'none';
            if (!active) overlay.style.display = 'flex';
            status.textContent = label || (active ? 'Loading video...' : 'Ready to load');
        }

        btn.addEventListener('click', function(event) {
            event.preventDefault();
            event.stopPropagation();
            if (wrap.getAttribute('data-tasfa-loading') === '1') return;
            if (el.getAttribute('data-tasfa-loaded') === '1') {
                try { el.play(); } catch (e) {}
                return;
            }
            setLoadingState(true, 'Loading video... 0%');
            progressBar.style.width = '0%';
            
            var progressInterval = setInterval(function() {
                var state = downloadStates[baseUrl];
                if (state && state.sharedState) {
                    var ss = state.sharedState;
                    var pct = ss.totalSize > 0 ? Math.round((ss.downloadedBytes / ss.totalSize) * 100) : 0;
                    progressBar.style.width = pct + '%';
                    status.textContent = 'Loading video... ' + Math.min(100, pct) + '%';
                }
            }, 150);

            fetchBlobViaTasfa(baseUrl, { silent: true }).then(function(result) {
                clearInterval(progressInterval);
                var objectUrl = URL.createObjectURL(result.blob);
                el.src = objectUrl;
                el.setAttribute('preload', 'auto');
                el.load();
                el.setAttribute('data-tasfa-loaded', '1');
                overlay.style.display = 'none';

                // Video Sane/Damage Playback Recovery Algorithm
                var lastSaneTime = 0;
                var recoveryAttempts = 0;
                el.addEventListener('timeupdate', function() {
                    if (el.currentTime > 0 && !el.paused && el.readyState >= 2) {
                        lastSaneTime = el.currentTime;
                    }
                });

                el.addEventListener('error', handleVideoDamage);
                el.addEventListener('stalled', handleVideoDamage);

                function handleVideoDamage(e) {
                    // Detect potential frame/data corruption stalling
                    if (recoveryAttempts < 3) {
                        recoveryAttempts++;
                        var failedTime = el.currentTime || lastSaneTime;
                        status.style.display = 'block';
                        status.textContent = 'Correcting playback...';
                        
                        // Calculate matching chunk index for failed timestamp (estimating chunk duration ~5 seconds)
                        var chunkIndex = Math.max(0, Math.floor(failedTime / 5));
                        
                        // Attempt partial chunk re-fetch silently
                        fetchDownloadSession(baseUrl).then(function(session) {
                            var testBytes = new Uint8Array(session.chunkSize);
                            return fetchChunk(baseUrl, session, testBytes, chunkIndex, 1);
                        }).then(function() {
                            // Recovered chunk, resume at last sane time
                            el.currentTime = lastSaneTime;
                            el.play().catch(function(){});
                            status.style.display = 'none';
                        }).catch(function() {
                            // Fallback to Sane starting point
                            el.currentTime = lastSaneTime;
                            el.play().catch(function(){});
                            status.style.display = 'none';
                        });
                    } else {
                        // Hard reset to last sane timestamp
                        el.currentTime = lastSaneTime;
                        el.play().catch(function(){});
                    }
                }

                try {
                    var p = el.play();
                    if (p && p.catch) p.catch(function(){});
                } catch (e) {}
            }).catch(function(err) {
                clearInterval(progressInterval);
                console.error('TASFA video load failed:', baseUrl, err);
                setLoadingState(false, 'Video load failed');
                el.setAttribute('data-tasfa-error', '1');
            });
        });

        overlay.appendChild(btn);
        overlay.appendChild(progressContainer);
        overlay.appendChild(status);
        wrap.appendChild(overlay);
    }

    function upgradeWithin(root) {
        if (!root || !root.querySelectorAll) return;
        root.querySelectorAll('.markdown-body img[src^="blob:"], .markdown-body video[src^="blob:"], .markdown-body audio[src^="blob:"]').forEach(function(el) { el.remove(); });
        
        // Upgrade TASFA media first
        root.querySelectorAll('img[data-tasfa-download], img[src^="/assets/img/"], img[src^="/assets/uploads/"]').forEach(upgradeMediaElement);
        root.querySelectorAll('video[src^="/file/download/"], video[src^="/assets/uploads/"], video[data-tasfa-download]').forEach(upgradeVideoElement);
        root.querySelectorAll('audio[src^="/file/download/"], audio[src^="/assets/uploads/"], audio[data-tasfa-download]').forEach(upgradeMediaElement);
        
        // Upgrade links
        root.querySelectorAll('a[data-tasfa-download-link], a[href^="/file/download/"]').forEach(upgradeDownloadLink);
        
        // Finally wrap others in markdown
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

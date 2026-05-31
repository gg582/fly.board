(function() {
    var cache = new Map();
    var downloadStates = {};
    var CACHE_NAME = 'tasfa-small-files-v1';
    var SMALL_FILE_THRESHOLD = 100 * 1024 * 1024; // 100MB
    var DOWNLOAD_CHUNK_STORE = 'tasfa_download_chunk_size_v3';
    var DOWNLOAD_CHUNK_MIN = 2 * 1024 * 1024;
    var DOWNLOAD_CHUNK_DEFAULT = 8 * 1024 * 1024;
    var DOWNLOAD_CHUNK_MOBILE_DEFAULT = 4 * 1024 * 1024;
    var DOWNLOAD_CHUNK_MAX = 32 * 1024 * 1024;
    var DOWNLOAD_CHUNK_STEP_UP = 1024 * 1024;
    var DOWNLOAD_CHUNK_STEP_DOWN = 512 * 1024;
    var DOWNLOAD_REQUEST_BYTES_MAX = 128 * 1024 * 1024;
    var DOWNLOAD_CONNECT_TIMEOUT_MS = 20000;
    /* data: URL cannot be received in TASFA environment - no placeholder used */
    var TASFA_MEDIA_CACHE = 'tasfa-media-cache-v1';
    var objectUrls = new WeakMap();
    var videoPlayerModule = null;

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
            rememberDownloadChunkSize(preferredDownloadChunkSize() * 2);
            session.fastStreak = 0;
        }
        if (session.currentSpan < session.maxSpan && session.successEvents % 2 === 0) {
            session.currentSpan = Math.min(session.maxSpan, Math.max(1, Math.round(session.currentSpan * 1.2)));
        }
        if (session.targetParallel < session.maxParallel && session.successEvents % 3 === 0) {
            session.targetParallel = Math.min(session.maxParallel, Math.max(1, Math.round(session.targetParallel * 1.2)));
        }
    }

    function tuneDownloadFailure(session, kind) {
        session.failureEvents = (session.failureEvents || 0) + 1;
        session.fastStreak = 0;
        session.currentSpan = Math.max(1, Math.round(session.currentSpan * 0.8));
        pushDownloadQuality(session, kind === 'timeout' ? 0.05 : 0.15);
        rememberDownloadChunkSize(preferredDownloadChunkSize() / 2);
        var floor = Math.min(session.maxParallel || 1, isLikelyMobile() ? 2 : 4);
        if ((kind === 'timeout' || session.failureEvents % 4 === 0) && session.targetParallel > floor) {
            session.targetParallel = Math.max(floor, Math.round(session.targetParallel * 0.8));
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

    function chunkUrl(baseUrl, sessionId, sessionToken, chunkIndex, span) {
        var path = normalizeUrl(baseUrl);
        if (!path) return null;
        var url = null;
        if (path.indexOf('/file/download/') === 0) {
            url = path + '/chunk/' + String(chunkIndex) + '?session_id=' + encodeURIComponent(sessionId) + '&session_token=' + encodeURIComponent(sessionToken);
        } else if (path.indexOf('/assets/img/') === 0) {
            url = '/assets/tasfa/img/' + encodeURIComponent(path.slice('/assets/img/'.length)) + '/chunk/' + String(chunkIndex) + '?session_id=' + encodeURIComponent(sessionId) + '&session_token=' + encodeURIComponent(sessionToken);
        } else if (path.indexOf('/assets/uploads/') === 0) {
            url = '/assets/tasfa/uploads/' + encodeURIComponent(path.slice('/assets/uploads/'.length)) + '/chunk/' + String(chunkIndex) + '?session_id=' + encodeURIComponent(sessionId) + '&session_token=' + encodeURIComponent(sessionToken);
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
        } catch (e) {
            if (retries < 5) {
                await new Promise(function(r) { setTimeout(r, 1000); });
                return fetchJson(url, retries + 1);
            }
            throw e;
        } finally {
            if (timeoutId) clearTimeout(timeoutId);
        }
        if (response.status === 429 && retries < 10) {
            var data = await response.json().catch(function() { return {}; });
            var delay = (data.retry_after || 3) * 1000;
            await new Promise(function(r) { setTimeout(r, delay); });
            return fetchJson(url, retries + 1);
        }
        if ((response.status >= 500 || !response.ok) && retries < 5) {
            await new Promise(function(r) { setTimeout(r, 1000); });
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
            streamKeyHex: session.stream_key_hex,
            streamIvSeedHex: session.stream_iv_seed_hex,
            streamMode: session.stream_mode,
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
        // Register session with Service Worker for range-request support
        if (navigator.serviceWorker && navigator.serviceWorker.controller) {
            navigator.serviceWorker.controller.postMessage({
                type: 'TASFA_SESSION',
                url: baseUrl,
                sessionId: session.sessionId,
                sessionToken: session.sessionToken
            });
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

    async function decryptBuffer(session, chunkIndex, buffer) {
        if (session.streamMode !== 'aes-256-gcm' || !session.streamKeyHex || !session.streamIvSeedHex) {
            return new Uint8Array(buffer);
        }

        if (!session.cryptoKey) {
            var rawKey = new Uint8Array(session.streamKeyHex.match(/.{1,2}/g).map(function(b) { return parseInt(b, 16); }));
            session.cryptoKey = await crypto.subtle.importKey('raw', rawKey, 'AES-GCM', false, ['decrypt']);
            session.ivSeed = new Uint8Array(session.streamIvSeedHex.match(/.{1,2}/g).map(function(b) { return parseInt(b, 16); }));
        }

        var iv = new Uint8Array(session.ivSeed);
        iv[8] ^= (chunkIndex >> 24) & 0xff;
        iv[9] ^= (chunkIndex >> 16) & 0xff;
        iv[10] ^= (chunkIndex >> 8) & 0xff;
        iv[11] ^= chunkIndex & 0xff;

        var aad = new TextEncoder().encode(session.sessionId + ':' + chunkIndex);

        try {
            var plaintext = await crypto.subtle.decrypt(
                { name: 'AES-GCM', iv: iv, additionalData: aad, tagLength: 128 },
                session.cryptoKey,
                buffer
            );
            return new Uint8Array(plaintext);
        } catch (e) {
            throw new Error('decryption_failed');
        }
    }

    function fetchChunk(baseUrl, session, parts, chunkIndex, span, retries) {
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

                decryptBuffer(session, chunkIndex, buffer).then(function(data) {
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
                            parts[idx] = new Uint8Array(data.subarray(offset, offset + size));
                        }
                        offset += size;
                        totalReceived += size;
                    }
                    resolve({ bytes: totalReceived, durationMs: Date.now() - startedAt });
                }).catch(reject);
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
        var range = options && options.range;
        var handshakeOnly = options && options.handshakeOnly;

        var promise = (async function() {
            var session = await fetchDownloadSession(baseUrl);

            if (range) {
                var rangeHeaders = {
                    'X-TASFA-Session-ID': session.sessionId,
                    'X-TASFA-Session-Token': session.sessionToken,
                    'Range': 'bytes=' + range.start + '-' + range.end
                };
                var rangeResponse = await fetch(baseUrl, { headers: rangeHeaders });
                if (!rangeResponse.ok) throw new Error('range fetch failed: ' + rangeResponse.status);
                return { blob: await rangeResponse.blob(), filename: session.filename };
            }

            if (handshakeOnly) {
                return { filename: session.filename };
            }

            if (session.totalSize <= SMALL_FILE_THRESHOLD) {
                var cachedBlob = await getCachedBlob(baseUrl);
                if (cachedBlob) {
                    await notifyDownloadComplete(session.sessionId, session.sessionToken);
                    if (onProgress) onProgress(100);
                    return {blob: cachedBlob, filename: session.filename};
                }
            }

            var parts = new Array(session.chunkCount);
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
                while (!sharedState.failed && sharedState.completedChunks < session.chunkCount) {
                    if (activeFetches >= (session.targetParallel || 1)) {
                        await new Promise(function(r) { setTimeout(r, 20); });
                        continue;
                    }
                    var claim = claimSpan();
                    if (!claim) break;
                    activeFetches += 1;
                    try {
                        var received = await fetchChunk(baseUrl, session, parts, claim.idx, claim.span);
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
                        // Add a small delay before retrying to prevent rapid retry exhaustion
                        await new Promise(function(r) { setTimeout(r, 1000); });
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

            var missing = 0;
            for (var p = 0; p < session.chunkCount; p++) {
                if (!parts[p]) missing++;
            }
            if (missing > 0) throw new Error('incomplete download: ' + missing + ' chunks missing');

            var result = {
                blob: new Blob(parts, { type: session.mimeType }),
                filename: session.filename
            };

            if (session.totalSize <= SMALL_FILE_THRESHOLD) {
                await putCachedBlob(baseUrl, result.blob, session.mimeType);
            }

            await notifyDownloadComplete(session.sessionId, session.sessionToken);
            if (onProgress) onProgress(100);

            return result;
        })();

        if (!silent && !handshakeOnly) {
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

        if (!handshakeOnly) {
            cache.set(baseUrl, promise);
        }
        try {
            var result = await promise;
            if (!silent && !handshakeOnly) {
                updateDownloadProgress(baseUrl, 100, result.filename);
                setTimeout(function() { removeDownloadProgress(baseUrl); delete downloadStates[baseUrl]; }, 800);
            }
            return result;
        } catch (error) {
            if (!silent && !handshakeOnly) {
                removeDownloadProgress(baseUrl);
                delete downloadStates[baseUrl];
            }
            if (!handshakeOnly) {
                cache.delete(baseUrl);
            }
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

    function isTasfaDownloadUrl(url) {
        var path = normalizeUrl(url);
        return !!handshakeUrl(path);
    }

    function mediaBaseUrl(el) {
        var explicit = el.getAttribute('data-tasfa-download') || el.getAttribute('data-tasfa-src') || '';
        if (explicit && isTasfaDownloadUrl(explicit)) return explicit;
        var attrSrc = el.getAttribute('src') || '';
        if (attrSrc && isTasfaDownloadUrl(attrSrc)) return attrSrc;
        return '';
    }

    function stableMediaCacheUrl(baseUrl) {
        return '/__tasfa_media__/' + encodeURIComponent(baseUrl.replace(/[^a-z0-9_.-]/gi, '_')) + '-' + Date.now();
    }

    async function waitForServiceWorkerController(timeoutMs) {
        if (!navigator.serviceWorker) return false;
        if (navigator.serviceWorker.controller) return true;
        try { await navigator.serviceWorker.ready; } catch (e) {}
        if (navigator.serviceWorker.controller) return true;
        await new Promise(function(resolve) {
            var done = false;
            var timer = setTimeout(function() {
                if (done) return;
                done = true;
                navigator.serviceWorker.removeEventListener('controllerchange', onChange);
                resolve();
            }, timeoutMs || 1500);
            function onChange() {
                if (done) return;
                done = true;
                clearTimeout(timer);
                navigator.serviceWorker.removeEventListener('controllerchange', onChange);
                resolve();
            }
            navigator.serviceWorker.addEventListener('controllerchange', onChange);
        });
        return !!navigator.serviceWorker.controller;
    }

    async function createMediaPlaybackUrl(baseUrl, blob, tagName) {
        /* video/audio: blob: URL does not work in TASFA environment, so only SW cache path is used.
           Others like img allow URL.createObjectURL fallback on SW failure. */
        if (window.caches && navigator.serviceWorker && await waitForServiceWorkerController(1500)) {
            var url = stableMediaCacheUrl(baseUrl);
            try {
                var cache = await caches.open(TASFA_MEDIA_CACHE);
                await cache.put(url, new Response(blob, {
                    headers: {
                        'Content-Type': blob.type || 'application/octet-stream',
                        'Cache-Control': 'no-store'
                    }
                }));
                return url;
            } catch (e) {}
        }
        return URL.createObjectURL(blob);
    }

    function setMediaObjectUrl(el, objectUrl) {
        if (!objectUrl) return; /* If SW cache path is not obtained - do nothing */
        objectUrls.set(el, objectUrl);
        el.setAttribute('src', objectUrl);
        // Reveal the element now that the blob: (or SW cache) URL is in place.
        el.style.opacity = '1';
        if (typeof el.load === 'function') {
            try { el.load(); } catch (e) {}
        } else if (el.parentElement && typeof el.parentElement.load === 'function') {
            try { el.parentElement.load(); } catch (e) {}
        }
    }

    function updateProgressUI(el, percent) {
        /* Loading UI removed; keep function signature for compatibility */
    }

    function upgradeVideoLinkButton(el) {
        var videoLink = el.getAttribute('data-tasfa-video-link') || '';
        if (!videoLink) return;
        if (el.dataset.tasfaVideoBound === '1') return;
        el.dataset.tasfaVideoBound = '1';

        var isThumb = el.classList.contains('file-video-thumb-link');

        el.addEventListener('click', function(event) {
            event.preventDefault();

            el.disabled = true;
            var title = isThumb ? (el.closest('.file-repo-card-inner')
                ? (el.closest('.file-repo-card-inner').querySelector('h4') || {}).textContent || ''
                : '') : '';
            (videoPlayerModule || (videoPlayerModule = import('/assets/js/tasfa-video-player.js?v=2')))
                .then(function(mod) {
                    if (!mod || typeof mod.openTasfaVideoModal !== 'function') throw new Error('modal unavailable');
                    mod.openTasfaVideoModal(videoLink, title);
                })
                .catch(function() {
                    window.open(videoLink, '_blank', 'noopener,noreferrer');
                })
                .finally(function() {
                    el.disabled = false;
                });
        });
    }

    /**
     * Called when img/audio element actually points to a video or audio file.
     * Replaces the element inline with an embedded player.
     */
    function replaceWithEmbeddedPlayer(el, playUrl, isAudio) {
        el.setAttribute('data-tasfa-ready', '1');
        el.removeAttribute('data-tasfa-progress');

        function bindLoaderRemoval(mediaEl) {
            var wrap = mediaEl.closest('.tasfa-media-wrap') || mediaEl.closest('.tasfa-inline-media-wrap');
            if (!wrap) return;
            var loader = wrap.querySelector('.tasfa-media-loader');
            if (!loader) return;
            function removeLoader() {
                if (loader.parentElement) loader.remove();
                if (wrap.classList.contains('tasfa-inline-media-wrap') && wrap.parentElement) {
                    wrap.parentElement.insertBefore(mediaEl, wrap);
                    wrap.remove();
                }
            }
            mediaEl.addEventListener('loadeddata', removeLoader, { once: true });
            setTimeout(removeLoader, 5000);
        }

        var existingTag = el.tagName ? el.tagName.toLowerCase() : '';
        if (existingTag === 'video' || existingTag === 'audio') {
            el.src = playUrl;
            el.style.opacity = '1';
            bindLoaderRemoval(el);
            return;
        }

        var mediaEl = document.createElement(isAudio ? 'audio' : 'video');
        mediaEl.setAttribute('controls', '');
        mediaEl.setAttribute('playsinline', '');
        mediaEl.src = playUrl;
        mediaEl.style.maxWidth = '100%';
        mediaEl.style.display = 'block';

        if (el.className) mediaEl.className = el.className;
        if (el.style.cssText) mediaEl.style.cssText = el.style.cssText;

        if (el.parentNode) {
            el.parentNode.replaceChild(mediaEl, el);
        }

        bindLoaderRemoval(mediaEl);
    }

    function upgradeMediaElement(el) {
        if (!el || el.dataset.tasfaMediaBound === '1') return;
        if (el.getAttribute('data-tasfa-skip') === '1') return;

        var tagName = el.tagName ? el.tagName.toLowerCase() : '';

        var baseUrl = mediaBaseUrl(el);
        var posterUrl = el.getAttribute('data-tasfa-poster') || '';
        if (!baseUrl && !posterUrl) return;

        el.dataset.tasfaMediaBound = '1';
        if (baseUrl) el.setAttribute('data-tasfa-download', baseUrl);

        if (tagName === 'img') {
            var wrap = document.createElement('div');
            wrap.className = 'tasfa-image-wrap';
            if (el.parentNode) {
                el.parentNode.insertBefore(wrap, el);
                wrap.appendChild(el);
            }

            if (baseUrl) {
                var dlBtn = document.createElement('a');
                dlBtn.className = 'tasfa-image-dl-btn';
                dlBtn.textContent = 'Download';
                dlBtn.href = 'javascript:void(0);';
                dlBtn.addEventListener('click', function(event) {
                    event.preventDefault();
                    triggerDownload(baseUrl).catch(function(){});
                });
                wrap.appendChild(dlBtn);
            }

            function setImageSrc(url) {
                el.src = url;
                el.style.opacity = '1';
                el.setAttribute('data-tasfa-ready', '1');
            }

            var currentSrc = el.getAttribute('src') || '';
            var alreadyThumb = currentSrc.indexOf('/assets/uploads/') === 0;

            if (alreadyThumb) {
                var testImg = new Image();
                testImg.onload = function() {
                    setImageSrc(currentSrc);
                };
                testImg.onerror = function() {
                    setImageSrc(baseUrl || currentSrc);
                };
                testImg.src = currentSrc;
            } else if (baseUrl) {
                var thumbUrl = null;
                if (baseUrl.indexOf('/assets/uploads/') === 0) {
                    var filename = baseUrl.slice('/assets/uploads/'.length);
                    if (filename.indexOf('.thumbs/') !== 0) {
                        thumbUrl = '/assets/uploads/.thumbs/' + filename;
                    }
                }

                if (thumbUrl) {
                    var testImg = new Image();
                    testImg.onload = function() {
                        setImageSrc(thumbUrl);
                    };
                    testImg.onerror = function() {
                        setImageSrc(baseUrl);
                    };
                    testImg.src = thumbUrl;
                } else {
                    setImageSrc(baseUrl);
                }
            }

            if (posterUrl && isTasfaDownloadUrl(posterUrl)) {
                fetchBlobViaTasfa(posterUrl, { silent: true }).then(async function(result) {
                    var objectUrl = await createMediaPlaybackUrl(posterUrl, result.blob);
                    if (objectUrl) el.setAttribute('poster', objectUrl);
                }).catch(function() {});
            }

            return;
        }

        // Wrap in loader container so the user sees "Loading..." instead of empty space
        var wrap = document.createElement('div');
        wrap.className = 'tasfa-media-wrap';
        if (el.parentNode) {
            el.parentNode.insertBefore(wrap, el);
            wrap.appendChild(el);
        }

        var loader = document.createElement('div');
        loader.className = 'tasfa-media-loader';
        loader.innerHTML = '<div class="tasfa-media-loader-spinner"></div><span>Loading...</span>';
        wrap.appendChild(loader);

        if (baseUrl && isTasfaDownloadUrl(el.getAttribute('src') || '')) {
            // Swap the original src with a 1x1 transparent pixel so the browser
            // never renders a broken-image icon while TASFA downloads the file.
            el.setAttribute('src', 'data:image/gif;base64,R0lGODlhAQABAIAAAAAAAP///yH5BAEAAAAALAAAAAABAAEAAAIBRAA7');
        }

        // Keep the element invisible until the real blob: / SW-cache URL is ready.
        el.style.opacity = '0';
        el.style.transition = 'opacity 0.25s ease';

        if (posterUrl && isTasfaDownloadUrl(posterUrl)) {
            fetchBlobViaTasfa(posterUrl, { silent: true }).then(async function(result) {
                var objectUrl = await createMediaPlaybackUrl(posterUrl, result.blob);
                if (objectUrl) el.setAttribute('poster', objectUrl);
            }).catch(function() {});
        }

        if (baseUrl) {
            fetchBlobViaTasfa(baseUrl, {
                silent: true,
                onProgress: function(percent) {
                    el.setAttribute('data-tasfa-progress', String(percent));
                }
            }).then(async function(result) {
                var mimeType = result.blob.type || '';
                var filename = result.filename || '';
                var ext = filename.split('.').pop().toLowerCase();
                var isVideo = ['mp4', 'webm', 'ogg', 'mov', 'avi', 'mkv', 'flv', 'wmv', 'm4v'].indexOf(ext) !== -1 || /^video\//.test(mimeType);
                var isAudio = ['mp3', 'wav', 'm4a', 'aac', 'flac', 'wma'].indexOf(ext) !== -1 || /^audio\//.test(mimeType);

                if (isVideo || isAudio) {
                    var playUrl = await createMediaPlaybackUrl(baseUrl, result.blob, isAudio ? 'audio' : 'video');
                    if (!playUrl) playUrl = baseUrl;
                    replaceWithEmbeddedPlayer(el, playUrl, isAudio);
                    return;
                }
                var objectUrl = await createMediaPlaybackUrl(baseUrl, result.blob, tagName);
                setMediaObjectUrl(el, objectUrl);
                el.setAttribute('data-tasfa-ready', '1');
                el.removeAttribute('data-tasfa-progress');
            }).catch(function() {
                el.dataset.tasfaMediaBound = '0';
                el.setAttribute('data-tasfa-error', '1');
            });
        }
    }

    function upgradeDownloadLink(el) {
        var baseUrl = el.getAttribute('data-tasfa-download-link') || el.getAttribute('href') || '';
        var match = baseUrl.match(/\/file\/download\/\d+/);
        if (!match) return;
        var relativeUrl = match[0];
        if (el.dataset.tasfaDownloadBound === '1') return;
        el.dataset.tasfaDownloadBound = '1';
        if (!el.getAttribute('data-tasfa-download-link')) el.setAttribute('data-tasfa-download-link', relativeUrl);
        el.addEventListener('click', function(event) {
            event.preventDefault();
            triggerDownload(relativeUrl).catch(function() {
                el.setAttribute('data-tasfa-error', '1');
            });
        });
    }

    function upgradeWithin(root) {
        if (!root || !root.querySelectorAll) return;
        var downloadSelector = 'a[data-tasfa-download-link], a[href^="/file/download/"], a[href*="/file/download/"]';
        var videoSelector = 'button[data-tasfa-video-link]';
        if (root.matches) {
            if (root.matches(downloadSelector)) upgradeDownloadLink(root);
            if (root.matches(videoSelector)) upgradeVideoLinkButton(root);
        }
        root.querySelectorAll(downloadSelector).forEach(upgradeDownloadLink);
        root.querySelectorAll(videoSelector).forEach(upgradeVideoLinkButton);
    }

    function upgradeMediaWithin(root) {
        if (!root || !root.querySelectorAll) return;
        var mediaSelector = 'img[data-tasfa-download], img[src^="/file/download/"], img[src^="/assets/img/"], img[src^="/assets/uploads/"], audio[data-tasfa-download], audio[src^="/file/download/"], video[data-tasfa-download], video[src^="/file/download/"]';
        if (root.matches) {
            if (root.matches(mediaSelector)) upgradeMediaElement(root);
        }
        root.querySelectorAll(mediaSelector).forEach(upgradeMediaElement);
    }

    function init() {
        window.fetchBlobViaTasfa = fetchBlobViaTasfa;
        window.openTasfaDownload = triggerDownload;
        window.upgradeTasfaMedia = upgradeMediaElement;
        window.initMarkdownAffordances = function(root) {
            var r = root && root.querySelectorAll ? root : document;
            upgradeWithin(r);
            upgradeMediaWithin(r);
        };
        upgradeWithin(document);
        upgradeMediaWithin(document);
        if (window.MutationObserver) {
            new MutationObserver(function(mutations) {
                mutations.forEach(function(mutation) {
                    mutation.addedNodes.forEach(function(node) {
                        if (node && node.nodeType === 1) {
                            upgradeWithin(node);
                            upgradeMediaWithin(node);
                        }
                    });
                });
            }).observe(document.documentElement, { childList: true, subtree: true });
        }
        window.addEventListener('pagehide', function() {
            document.querySelectorAll('[data-tasfa-media-bound="1"]').forEach(function(el) {
                objectUrls.delete(el);
            });
        });
    }

    if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', init);
    else init();
})();

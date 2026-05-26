(function() {
    var cache = new Map();
    var downloadStates = {};
    var CACHE_NAME = 'tasfa-small-files-v1';
    var SMALL_FILE_THRESHOLD = 100 * 1024 * 1024; // 100MB
    var DOWNLOAD_CHUNK_STORE = 'tasfa_download_chunk_size_v3';
    var DOWNLOAD_CHUNK_MIN = 512 * 1024;
    var DOWNLOAD_CHUNK_DEFAULT = 2 * 1024 * 1024;
    var DOWNLOAD_CHUNK_MOBILE_DEFAULT = 1024 * 1024;
    var DOWNLOAD_CHUNK_MAX = 16 * 1024 * 1024;
    var DOWNLOAD_CHUNK_STEP_UP = 512 * 1024;
    var DOWNLOAD_CHUNK_STEP_DOWN = 256 * 1024;
    var DOWNLOAD_REQUEST_BYTES_MAX = 64 * 1024 * 1024;
    var DOWNLOAD_CONNECT_TIMEOUT_MS = 3000;
    var EMPTY_IMAGE_SRC = 'data:image/gif;base64,R0lGODlhAQABAIAAAAAAAP///ywAAAAAAQABAAACAUwAOw==';
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
                            allBytes.set(data.subarray(offset, offset + size), idx * session.chunkSize);
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
                while (!sharedState.failed && sharedState.completedChunks < session.chunkCount) {
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

    async function createMediaPlaybackUrl(baseUrl, blob) {
        var url = stableMediaCacheUrl(baseUrl);
        if (window.caches && navigator.serviceWorker && await waitForServiceWorkerController(1500)) {
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
        var previous = objectUrls.get(el);
        if (previous && previous !== objectUrl && previous.indexOf('blob:') === 0) {
            try { URL.revokeObjectURL(previous); } catch (e) {}
        }
        objectUrls.set(el, objectUrl);
        el.setAttribute('src', objectUrl);
        if (typeof el.load === 'function') {
            try { el.load(); } catch (e) {}
        } else if (el.parentElement && typeof el.parentElement.load === 'function') {
            try { el.parentElement.load(); } catch (e) {}
        }
    }

    function updateProgressUI(el, percent) {
        var wrap = el.closest('.tasfa-media-wrap') || el.closest('.tasfa-inline-media-wrap');
        if (!wrap && el.parentNode) {
            wrap = document.createElement('div');
            wrap.className = 'tasfa-inline-media-wrap';
            wrap.style.position = 'relative';
            wrap.style.display = el.style.display === 'block' ? 'block' : 'inline-block';
            wrap.style.maxWidth = '100%';
            if (el.style.width) wrap.style.width = el.style.width;
            el.parentNode.insertBefore(wrap, el);
            wrap.appendChild(el);
        }
        if (!wrap) return;
        var loader = wrap.querySelector('.tasfa-media-loader');
        if (!loader) {
            loader = document.createElement('div');
            loader.className = 'tasfa-media-loader';
            loader.innerHTML = '<div>Loading...</div><div class="tasfa-media-loader-bar"><div class="tasfa-media-loader-inner"></div></div>';
            wrap.appendChild(loader);
        }
        var inner = loader.querySelector('.tasfa-media-loader-inner');
        if (inner) inner.style.width = percent + '%';
        if (percent >= 100) {
            setTimeout(function() {
                if (loader.parentElement) loader.remove();
                if (wrap.classList.contains('tasfa-inline-media-wrap') && wrap.parentElement) {
                    wrap.parentElement.insertBefore(el, wrap);
                    wrap.remove();
                }
            }, 600);
        }
    }

    function upgradeVideoLinkButton(el) {
        var videoLink = el.getAttribute('data-tasfa-video-link') || '';
        if (!videoLink) return;
        if (el.dataset.tasfaVideoBound === '1') return;
        el.dataset.tasfaVideoBound = '1';

        var isThumb = el.classList.contains('file-video-thumb-link');

        el.addEventListener('click', function(event) {
            event.preventDefault();

            if (isThumb) {
                // File repo card thumbnail: open Plyr modal overlay
                var title = el.closest('.file-repo-card-inner')
                    ? (el.closest('.file-repo-card-inner').querySelector('h4') || {}).textContent || ''
                    : '';
                el.disabled = true;
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
            } else {
                // Post / markdown embedded video: open in new tab (no preloading)
                var win = window.open(videoLink, '_blank', 'noopener,noreferrer');
                if (win) { try { win.opener = null; } catch (e) {} }
            }
        });
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

        if (baseUrl && isTasfaDownloadUrl(el.getAttribute('src') || '')) {
            el.removeAttribute('src');
        }

        /* img용 placeholder (video/audio는 src 없애는 것으로 충분) */
        if (baseUrl && tagName === 'img' && !el.getAttribute('src')) {
            el.setAttribute('src', EMPTY_IMAGE_SRC);
        }

        /* Parallel upgrade for poster if exists */
        if (posterUrl && isTasfaDownloadUrl(posterUrl)) {
            fetchBlobViaTasfa(posterUrl, { silent: true }).then(async function(result) {
                var objectUrl = await createMediaPlaybackUrl(posterUrl, result.blob);
                el.setAttribute('poster', objectUrl);
            }).catch(function() {});
        }

        if (baseUrl) {
            fetchBlobViaTasfa(baseUrl, {
                silent: true,
                onProgress: function(percent) {
                    el.setAttribute('data-tasfa-progress', String(percent));
                    updateProgressUI(el, percent);
                }
            }).then(async function(result) {
                var objectUrl = await createMediaPlaybackUrl(baseUrl, result.blob);
                setMediaObjectUrl(el, objectUrl);
                el.setAttribute('data-tasfa-ready', '1');
                el.removeAttribute('data-tasfa-progress');
                updateProgressUI(el, 100);
            }).catch(function() {
                el.dataset.tasfaMediaBound = '0';
                el.setAttribute('data-tasfa-error', '1');
                var wrap = el.closest('.tasfa-media-wrap') || el.closest('.tasfa-inline-media-wrap');
                if (wrap) {
                    var loader = wrap.querySelector('.tasfa-media-loader');
                    if (loader) loader.remove();
                    if (wrap.classList.contains('tasfa-inline-media-wrap') && wrap.parentElement) {
                        wrap.parentElement.insertBefore(el, wrap);
                        wrap.remove();
                    }
                }
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
        var mediaSelector = 'img[src^="/file/download/"], img[src^="/assets/img/"], img[src^="/assets/uploads/"], audio[src^="/file/download/"]';
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
                var objectUrl = objectUrls.get(el);
                if (objectUrl && objectUrl.indexOf('blob:') === 0) {
                    try { URL.revokeObjectURL(objectUrl); } catch (e) {}
                }
                objectUrls.delete(el);
            });
        });
    }

    if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', init);
    else init();
})();

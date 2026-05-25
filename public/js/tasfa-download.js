(function() {
    var cache = new Map();
    var downloadStates = {};
    var SPACER_GIF = "";
    try {
        var binary = atob("R0lGODlhAQABAIAAAAAAAP///yH5BAEAAAAALAAAAAABAAEAAAIBRAA7");
        var array = new Uint8Array(binary.length);
        for (var i = 0; i < binary.length; i++) {
            array[i] = binary.charCodeAt(i);
        }
        SPACER_GIF = URL.createObjectURL(new Blob([array], { type: "image/gif" }));
    } catch (e) {
        SPACER_GIF = "data:image/gif;base64,R0lGODlhAQABAIAAAAAAAP///yH5BAEAAAAALAAAAAABAAEAAAIBRAA7";
    }
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
            var response = await fetch(baseUrl, { credentials: 'same-origin' });
            if (!response.ok) throw new Error('Fetch failed: ' + response.status);
            
            var totalSize = parseInt(response.headers.get('content-length'), 10) || 0;
            var contentType = response.headers.get('content-type') || 'application/octet-stream';
            var filename = baseUrl.substring(baseUrl.lastIndexOf('/') + 1) || 'download';
            
            var sharedState = {
                chunkCount: 1,
                totalSize: totalSize,
                nextChunk: 0,
                completedChunks: 0,
                downloadedBytes: 0,
                failed: null
            };
            downloadStates[baseUrl] = {
                sharedState: sharedState,
                filename: filename
            };

            var reader = response.body ? response.body.getReader() : null;
            var chunks = [];
            var downloadedBytes = 0;

            if (reader) {
                while (true) {
                    var { done, value } = await reader.read();
                    if (done) break;
                    chunks.push(value);
                    downloadedBytes += value.byteLength;
                    sharedState.downloadedBytes = downloadedBytes;
                    if (onProgress && totalSize > 0) {
                        onProgress(Math.round((downloadedBytes / totalSize) * 100));
                    }
                }
                sharedState.completedChunks = 1;
            } else {
                var blob = await response.blob();
                sharedState.downloadedBytes = blob.size;
                sharedState.completedChunks = 1;
                if (onProgress) onProgress(100);
                return { blob: blob, filename: filename };
            }

            var blob = new Blob(chunks, { type: contentType });
            if (onProgress) onProgress(100);
            return { blob: blob, filename: filename };
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
        var path = normalizeUrl(baseUrl);
        if (!/^\/(file\/download|assets\/img|assets\/uploads)\//.test(path)) return;
        
        if (!el.getAttribute('data-tasfa-download')) el.setAttribute('data-tasfa-download', baseUrl);
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
        if (el.closest('.media-download-wrap') || el.closest('.media-loading-wrap') || el.closest('.plyr-tasfa-wrapper') || el.closest('.plyr')) return;
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

    function formatTime(seconds) {
        if (!seconds || isNaN(seconds)) return '0:00';
        var mins = Math.floor(seconds / 60);
        var secs = Math.floor(seconds % 60);
        if (secs < 10) secs = '0' + secs;
        return mins + ':' + secs;
    }

    function loadPlyr(callback) {
        if (window.Plyr) {
            callback();
            return;
        }
        if (window._loadingPlyr) {
            window._plyrCallbacks = window._plyrCallbacks || [];
            window._plyrCallbacks.push(callback);
            return;
        }
        window._loadingPlyr = true;
        window._plyrCallbacks = [callback];

        // Load CSS
        var link = document.createElement('link');
        link.rel = 'stylesheet';
        link.href = 'https://cdn.plyr.io/3.7.8/plyr.css';
        document.head.appendChild(link);

        // Load JS
        var script = document.createElement('script');
        script.src = 'https://cdn.plyr.io/3.7.8/plyr.js';
        script.onload = function() {
            window.Plyr = Plyr;
            if (window._plyrCallbacks) {
                window._plyrCallbacks.forEach(function(cb) { cb(); });
            }
            delete window._plyrCallbacks;
            delete window._loadingPlyr;
        };
        document.head.appendChild(script);
    }

    function upgradeVideoElement(el) {
        if (el.getAttribute('data-tasfa-skip') === '1') return;
        var baseUrl = el.getAttribute('data-tasfa-download') || el.getAttribute('src') || '';
        var path = normalizeUrl(baseUrl);
        if (!/^\/(file\/download|assets\/uploads)\//.test(path)) return;
        
        var isLoaded = el.getAttribute('data-tasfa-loaded') === '1';
        if (!el.getAttribute('data-tasfa-download')) el.setAttribute('data-tasfa-download', baseUrl);
        if (el.closest('.plyr-tasfa-wrapper')) return;

        // Create player wrapper
        var wrapper = document.createElement('div');
        wrapper.className = 'plyr-tasfa-wrapper';
        wrapper.style.cssText = 'position:relative;width:100%;border-radius:12px;overflow:hidden;aspect-ratio:16/9;background:#0c0c0e;box-shadow:0 20px 40px rgba(0,0,0,0.5);margin:24px auto;';
        
        el.parentNode.insertBefore(wrapper, el);
        wrapper.appendChild(el);

        el.removeAttribute('controls');
        el.style.width = '100%';
        el.style.height = '100%';
        el.style.display = 'block';

        // Add loading/poster overlay
        var overlay = document.createElement('div');
        overlay.className = 'plyr-tasfa-loading';
        overlay.style.cssText = 'position:absolute;inset:0;display:flex;flex-direction:column;align-items:center;justify-content:center;z-index:10;background:rgba(10,10,14,0.85);backdrop-filter:blur(6px);-webkit-backdrop-filter:blur(6px);color:#fff;font-family:sans-serif;cursor:pointer;transition:opacity 0.4s ease, visibility 0.4s;';
        
        var poster = el.getAttribute('poster');
        if (poster) {
            var posterPath = normalizeUrl(poster);
            if (/^\/(file\/download|assets\/img|assets\/uploads)\//.test(posterPath)) {
                fetchBlobViaTasfa(poster, { silent: true }).then(function(result) {
                    var objectUrl = URL.createObjectURL(result.blob);
                    overlay.style.backgroundImage = 'linear-gradient(180deg, rgba(0,0,0,0.3), rgba(0,0,0,0.6)), url("' + objectUrl + '")';
                    overlay.style.backgroundSize = 'cover';
                    overlay.style.backgroundPosition = 'center';
                }).catch(function() {});
            } else {
                overlay.style.backgroundImage = 'linear-gradient(180deg, rgba(0,0,0,0.3), rgba(0,0,0,0.6)), url("' + poster + '")';
                overlay.style.backgroundSize = 'cover';
                overlay.style.backgroundPosition = 'center';
            }
        }

        // Add keyframe animation for spinner if not present
        if (!document.getElementById('tasfa-spin-style')) {
            var style = document.createElement('style');
            style.id = 'tasfa-spin-style';
            style.textContent = '@keyframes tasfa-spin { 0% { transform: rotate(0deg); } 100% { transform: rotate(360deg); } }';
            document.head.appendChild(style);
        }

        var spinner = document.createElement('div');
        spinner.style.cssText = 'width:50px;height:50px;border:4px solid rgba(255,255,255,0.1);border-top:4px solid var(--accent, #0066cc);border-radius:50%;animation:tasfa-spin 1s linear infinite;margin-bottom:16px;transition:opacity 0.3s;';
        overlay.appendChild(spinner);

        var statusText = document.createElement('div');
        statusText.style.cssText = 'font-size:16px;font-weight:500;text-shadow:0 1px 3px rgba(0,0,0,0.5);';
        statusText.textContent = 'Click to load video';
        overlay.appendChild(statusText);

        wrapper.appendChild(overlay);

        function initPlyrInstance() {
            loadPlyr(function() {
                var player = new Plyr(el, {
                    controls: ['play-large', 'play', 'progress', 'current-time', 'duration', 'mute', 'volume', 'captions', 'settings', 'pip', 'airplay', 'fullscreen'],
                    ratio: '16:9'
                });
                player.play().catch(function(){});
            });
        }

        if (isLoaded) {
            overlay.style.opacity = '0';
            overlay.style.visibility = 'hidden';
            overlay.style.pointerEvents = 'none';
            initPlyrInstance();
        } else {
            var isDownloading = false;
            var startLoad = function(event) {
                if (event) {
                    event.preventDefault();
                    event.stopPropagation();
                }
                if (isDownloading) return;
                isDownloading = true;

                spinner.style.borderColor = 'rgba(255,255,255,0.1)';
                spinner.style.borderTopColor = 'var(--accent, #0066cc)';
                statusText.textContent = 'Loading video... 0%';

                var progressInterval = setInterval(function() {
                    var state = downloadStates[baseUrl];
                    if (state && state.sharedState) {
                        var ss = state.sharedState;
                        var pct = ss.totalSize > 0 ? Math.round((ss.downloadedBytes / ss.totalSize) * 100) : 0;
                        statusText.textContent = 'Loading video... ' + Math.min(100, pct) + '%';
                    }
                }, 100);

                fetchBlobViaTasfa(baseUrl, { silent: true }).then(function(result) {
                    clearInterval(progressInterval);
                    var objectUrl = URL.createObjectURL(result.blob);
                    el.src = objectUrl;
                    el.setAttribute('preload', 'auto');
                    el.load();
                    el.setAttribute('data-tasfa-loaded', '1');
                    
                    overlay.style.opacity = '0';
                    overlay.style.visibility = 'hidden';
                    overlay.style.pointerEvents = 'none';
                    
                    initPlyrInstance();
                }).catch(function(err) {
                    clearInterval(progressInterval);
                    console.error('TASFA video load failed:', baseUrl, err);
                    isDownloading = false;
                    statusText.textContent = 'Loading failed. Click to retry.';
                    spinner.style.borderTopColor = '#ff4d4d'; // Red spinner on error
                    el.setAttribute('data-tasfa-error', '1');
                });
            };
            overlay.addEventListener('click', startLoad);
        }
    }

    function upgradeAudioElement(el) {
        if (el.getAttribute('data-tasfa-skip') === '1') return;
        var baseUrl = el.getAttribute('data-tasfa-download') || el.getAttribute('src') || '';
        var path = normalizeUrl(baseUrl);
        if (!/^\/(file\/download|assets\/uploads)\//.test(path)) return;
        
        var isLoaded = el.getAttribute('data-tasfa-loaded') === '1';
        if (!el.getAttribute('data-tasfa-download')) el.setAttribute('data-tasfa-download', baseUrl);
        if (el.closest('.tasfa-audio-container')) return;

        // Custom HTML5 Audio Player Container
        var container = document.createElement('div');
        container.className = 'tasfa-audio-container paused';
        el.parentNode.insertBefore(container, el);
        container.appendChild(el);

        el.removeAttribute('controls');
        el.setAttribute('preload', 'none');

        if (!isLoaded) {
            el.src = '';
            el.removeAttribute('src');
            try { el.load(); } catch(e) {}
        }

        // Control Panel
        var controls = document.createElement('div');
        controls.className = 'tasfa-audio-controls';

        // Play/Pause Button
        var playBtn = document.createElement('button');
        playBtn.type = 'button';
        playBtn.className = 'tasfa-audio-play-btn';
        var playSvg = '<svg viewBox="0 0 24 24"><path d="M8 5v14l11-7z"/></svg>';
        var pauseSvg = '<svg viewBox="0 0 24 24"><path d="M6 19h4V5H6v14zm8-14v14h4V5h-4z"/></svg>';
        playBtn.innerHTML = playSvg;
        controls.appendChild(playBtn);

        // Details / Progress Info
        var details = document.createElement('div');
        details.className = 'tasfa-audio-details';

        var title = document.createElement('div');
        title.className = 'tasfa-audio-title';
        var filename = baseUrl.substring(baseUrl.lastIndexOf('/') + 1);
        title.textContent = decodeURIComponent(filename) || 'Audio Track';
        details.appendChild(title);

        var progressRow = document.createElement('div');
        progressRow.className = 'tasfa-audio-progress-row';

        var seeker = document.createElement('input');
        seeker.type = 'range';
        seeker.className = 'tasfa-audio-seeker';
        seeker.min = '0';
        seeker.max = '100';
        seeker.value = '0';
        progressRow.appendChild(seeker);

        var timeDisplay = document.createElement('div');
        timeDisplay.className = 'tasfa-audio-time-display';
        timeDisplay.textContent = '0:00 / 0:00';
        progressRow.appendChild(timeDisplay);

        details.appendChild(progressRow);
        controls.appendChild(details);

        // Volume
        var volumeGroup = document.createElement('div');
        volumeGroup.className = 'tasfa-audio-volume-group';
        var muteBtn = document.createElement('button');
        muteBtn.type = 'button';
        muteBtn.className = 'tasfa-audio-control-btn';
        var volHighSvg = '<svg viewBox="0 0 24 24"><path d="M3 9v6h4l5 5V4L7 9H3zm13.5 3c0-1.77-1.02-3.29-2.5-4.03v8.05c1.48-.73 2.5-2.25 2.5-4.02zM14 3.23v2.06c2.89.86 5 3.54 5 6.71s-2.11 5.85-5 6.71v2.06c4.01-.91 7-4.49 7-8.77s-2.99-7.86-7-8.77z"/></svg>';
        var volMuteSvg = '<svg viewBox="0 0 24 24"><path d="M16.5 12c0-1.77-1.02-3.29-2.5-4.03v2.21l2.45 2.45c.03-.21.05-.42.05-.63zm2.5 0c0 .94-.2 1.82-.54 2.64l1.51 1.51C20.63 14.91 21 13.5 21 12c0-4.28-2.99-7.86-7-8.77v2.06c2.89.86 5 3.54 5 6.71zM4.27 3L3 4.27 7.73 9H3v6h4l5 5v-6.73l4.25 4.25c-.67.52-1.42.93-2.25 1.18v2.06c1.38-.31 2.63-.95 3.69-1.81L19.73 21 21 19.73l-9-9L4.27 3zM12 4L9.91 6.09 12 8.18V4z"/></svg>';
        muteBtn.innerHTML = volHighSvg;
        volumeGroup.appendChild(muteBtn);

        var volumeSlider = document.createElement('input');
        volumeSlider.type = 'range';
        volumeSlider.className = 'tasfa-audio-volume-slider';
        volumeSlider.min = '0';
        volumeSlider.max = '100';
        volumeSlider.value = '80';
        el.volume = 0.8;
        volumeGroup.appendChild(volumeSlider);
        controls.appendChild(volumeGroup);

        container.appendChild(controls);

        // State update helpers
        function updatePlayPause() {
            if (el.paused) {
                container.classList.add('paused');
                playBtn.innerHTML = playSvg;
            } else {
                container.classList.remove('paused');
                playBtn.innerHTML = pauseSvg;
            }
        }

        function initPlayerEvents() {
            volumeSlider.addEventListener('input', function() {
                var vol = volumeSlider.value / 100;
                el.volume = vol;
                el.muted = (vol === 0);
                muteBtn.innerHTML = el.muted ? volMuteSvg : volHighSvg;
            });

            muteBtn.addEventListener('click', function(e) {
                e.stopPropagation();
                el.muted = !el.muted;
                muteBtn.innerHTML = el.muted ? volMuteSvg : volHighSvg;
                volumeSlider.value = el.muted ? '0' : Math.round(el.volume * 100);
            });

            el.addEventListener('timeupdate', function() {
                if (el.duration) {
                    var pct = (el.currentTime / el.duration) * 100;
                    seeker.value = Math.round(pct);
                    timeDisplay.textContent = formatTime(el.currentTime) + ' / ' + formatTime(el.duration);
                }
            });

            el.addEventListener('loadedmetadata', function() {
                timeDisplay.textContent = formatTime(el.currentTime) + ' / ' + formatTime(el.duration);
            });

            seeker.addEventListener('input', function() {
                if (el.duration) {
                    el.currentTime = (seeker.value / 100) * el.duration;
                }
            });

            var togglePlay = function(e) {
                if (e) {
                    e.preventDefault();
                    e.stopPropagation();
                }
                if (el.paused) {
                    el.play().catch(function(){});
                } else {
                    el.pause();
                }
                updatePlayPause();
            };

            playBtn.addEventListener('click', togglePlay);
            el.addEventListener('play', updatePlayPause);
            el.addEventListener('pause', updatePlayPause);
        }

        if (isLoaded) {
            initPlayerEvents();
        } else {
            var loadAndPlay = function(e) {
                if (e) {
                    e.preventDefault();
                    e.stopPropagation();
                }
                if (container.getAttribute('data-tasfa-loading') === '1') return;

                container.setAttribute('data-tasfa-loading', '1');
                title.textContent = 'Loading audio... 0%';

                fetchBlobViaTasfa(baseUrl, { 
                    silent: true,
                    onProgress: function(pct) {
                        title.textContent = 'Loading audio... ' + pct + '%';
                    }
                }).then(function(result) {
                    var objectUrl = URL.createObjectURL(result.blob);
                    el.src = objectUrl;
                    el.setAttribute('preload', 'auto');
                    el.load();
                    el.setAttribute('data-tasfa-loaded', '1');
                    
                    container.setAttribute('data-tasfa-loading', '0');
                    title.textContent = decodeURIComponent(filename) || 'Audio Track';
                    
                    initPlayerEvents();
                    el.play().catch(function(){});
                    playBtn.removeEventListener('click', loadAndPlay);
                }).catch(function(err) {
                    console.error('TASFA audio load failed:', baseUrl, err);
                    container.setAttribute('data-tasfa-loading', '0');
                    title.textContent = 'Loading failed';
                    el.setAttribute('data-tasfa-error', '1');
                });
            };
            playBtn.addEventListener('click', loadAndPlay);
        }
    }

    function upgradeWithin(root) {
        if (!root || !root.querySelectorAll) return;
        root.querySelectorAll('.markdown-body img[src^="blob:"]:not([data-tasfa-download]), .markdown-body video[src^="blob:"]:not([data-tasfa-download]), .markdown-body audio[src^="blob:"]:not([data-tasfa-download])').forEach(function(el) { el.remove(); });
        
        // Upgrade TASFA media first
        root.querySelectorAll('img[data-tasfa-download], img[src^="/assets/img/"], img[src^="/assets/uploads/"]').forEach(upgradeMediaElement);
        root.querySelectorAll('video[src^="/file/download/"], video[src^="/assets/uploads/"], video[data-tasfa-download]').forEach(upgradeVideoElement);
        root.querySelectorAll('audio[src^="/file/download/"], audio[src^="/assets/uploads/"], audio[data-tasfa-download]').forEach(upgradeAudioElement);
        
        // Upgrade links
        root.querySelectorAll('a[data-tasfa-download-link], a[href^="/file/download/"]').forEach(upgradeDownloadLink);
        
        // Finally wrap others in markdown
        root.querySelectorAll('.markdown-body img, .markdown-body video, .markdown-body audio').forEach(wrapMediaForDownload);
    }

    function init() {
        // Inject Custom Video Player Stylesheet
        var style = document.createElement('style');
        style.textContent = `
            .tasfa-video-container {
                position: relative;
                width: 100%;
                background: #0d0d11;
                border-radius: 12px;
                overflow: hidden;
                aspect-ratio: 16 / 9;
                box-shadow: 0 20px 40px rgba(0,0,0,0.5), inset 0 0 0 1px rgba(255,255,255,0.05);
            }
            .tasfa-video-container video {
                width: 100%;
                height: 100%;
                display: block;
                object-fit: contain;
            }
            .tasfa-video-play-overlay {
                position: absolute;
                inset: 0;
                background: rgba(10, 10, 14, 0.6);
                backdrop-filter: blur(8px);
                -webkit-backdrop-filter: blur(8px);
                display: flex;
                flex-direction: column;
                align-items: center;
                justify-content: center;
                z-index: 5;
                cursor: pointer;
                transition: opacity 0.4s cubic-bezier(0.25, 1, 0.5, 1), visibility 0.4s;
            }
            .tasfa-video-play-overlay.playing {
                opacity: 0;
                visibility: hidden;
                pointer-events: none;
            }
            .tasfa-video-play-overlay-btn {
                width: 80px;
                height: 80px;
                border: none;
                border-radius: 50%;
                background: linear-gradient(135deg, var(--accent, #0066cc) 0%, var(--accent-hover, #0052a3) 100%);
                color: #fff;
                display: flex;
                align-items: center;
                justify-content: center;
                font-size: 28px;
                box-shadow: 0 10px 25px rgba(0,0,0,0.3), inset 0 1px 0 rgba(255,255,255,0.2);
                cursor: pointer;
                transition: transform 0.3s cubic-bezier(0.34, 1.56, 0.64, 1), background 0.2s ease, box-shadow 0.3s ease;
            }
            .tasfa-video-play-overlay:hover .tasfa-video-play-overlay-btn {
                transform: scale(1.12);
                box-shadow: 0 15px 35px rgba(0,0,0,0.4), inset 0 1px 0 rgba(255,255,255,0.3);
            }
            .tasfa-video-controls {
                position: absolute;
                bottom: 0;
                left: 0;
                right: 0;
                background: linear-gradient(to top, rgba(10, 10, 14, 0.95) 0%, rgba(10, 10, 14, 0.4) 70%, rgba(10, 10, 14, 0) 100%);
                backdrop-filter: blur(12px);
                -webkit-backdrop-filter: blur(12px);
                border-top: 1px solid rgba(255, 255, 255, 0.08);
                padding: 12px 16px;
                display: flex;
                flex-direction: column;
                gap: 10px;
                z-index: 6;
                opacity: 0;
                transform: translateY(10px);
                transition: opacity 0.3s cubic-bezier(0.25, 1, 0.5, 1), transform 0.3s cubic-bezier(0.25, 1, 0.5, 1);
                pointer-events: auto;
            }
            .tasfa-video-container:hover .tasfa-video-controls,
            .tasfa-video-container.paused .tasfa-video-controls {
                opacity: 1;
                transform: translateY(0);
            }
            .tasfa-video-seeker-container {
                position: relative;
                width: 100%;
                height: 6px;
                display: flex;
                align-items: center;
                cursor: pointer;
            }
            .tasfa-video-seeker-container:hover {
                height: 8px;
            }
            .tasfa-video-seeker {
                -webkit-appearance: none;
                appearance: none;
                width: 100%;
                height: 100%;
                background: rgba(255, 255, 255, 0.15);
                border-radius: 4px;
                outline: none;
                cursor: pointer;
                margin: 0;
                transition: background 0.15s ease;
            }
            .tasfa-video-seeker::-webkit-slider-thumb {
                -webkit-appearance: none;
                appearance: none;
                width: 14px;
                height: 14px;
                border-radius: 50%;
                background: #fff;
                cursor: pointer;
                transition: transform 0.15s ease;
                box-shadow: 0 2px 6px rgba(0,0,0,0.4);
            }
            .tasfa-video-seeker-container:hover .tasfa-video-seeker::-webkit-slider-thumb {
                transform: scale(1.25);
                background: var(--accent, #0066cc);
            }
            .tasfa-video-seeker::-moz-range-thumb {
                width: 14px;
                height: 14px;
                border: none;
                border-radius: 50%;
                background: #fff;
                cursor: pointer;
                transition: transform 0.15s ease;
                box-shadow: 0 2px 6px rgba(0,0,0,0.4);
            }
            .tasfa-video-seeker-container:hover .tasfa-video-seeker::-moz-range-thumb {
                transform: scale(1.25);
                background: var(--accent, #0066cc);
            }
            .tasfa-video-controls-row {
                display: flex;
                align-items: center;
                justify-content: space-between;
                width: 100%;
                color: #fff;
                font-size: 13px;
            }
            .tasfa-video-controls-group {
                display: flex;
                align-items: center;
                gap: 16px;
            }
            .tasfa-video-control-btn {
                background: none;
                border: none;
                color: #fff;
                cursor: pointer;
                display: flex;
                align-items: center;
                justify-content: center;
                padding: 6px;
                opacity: 0.8;
                transition: opacity 0.2s ease, transform 0.2s cubic-bezier(0.34, 1.56, 0.64, 1);
            }
            .tasfa-video-control-btn:hover {
                opacity: 1;
                transform: scale(1.15);
            }
            .tasfa-video-control-btn svg {
                width: 20px;
                height: 20px;
                fill: currentColor;
            }
            .tasfa-video-time-display {
                font-family: 'Outfit', 'Inter', monospace, sans-serif;
                font-size: 12px;
                opacity: 0.8;
                letter-spacing: 0.5px;
            }
            .tasfa-video-volume-group {
                display: flex;
                align-items: center;
                gap: 8px;
            }
            .tasfa-video-volume-slider {
                -webkit-appearance: none;
                appearance: none;
                width: 70px;
                height: 4px;
                background: rgba(255, 255, 255, 0.2);
                border-radius: 2px;
                outline: none;
                cursor: pointer;
                transition: background-color 0.15s;
            }
            .tasfa-video-volume-slider::-webkit-slider-thumb {
                -webkit-appearance: none;
                appearance: none;
                width: 10px;
                height: 10px;
                border-radius: 50%;
                background: #fff;
                cursor: pointer;
                box-shadow: 0 1px 3px rgba(0,0,0,0.3);
            }
            .tasfa-video-volume-slider::-moz-range-thumb {
                width: 10px;
                height: 10px;
                border: none;
                border-radius: 50%;
                background: #fff;
                cursor: pointer;
                box-shadow: 0 1px 3px rgba(0,0,0,0.3);
            }

            .tasfa-audio-container {
                width: 100%;
                background: linear-gradient(135deg, rgba(255, 255, 255, 0.08) 0%, rgba(255, 255, 255, 0.03) 100%);
                backdrop-filter: blur(16px);
                -webkit-backdrop-filter: blur(16px);
                border: 1px solid rgba(255, 255, 255, 0.1);
                border-radius: 16px;
                padding: 16px 20px;
                box-shadow: 0 10px 30px rgba(0,0,0,0.15), inset 0 1px 0 rgba(255,255,255,0.1);
                display: flex;
                flex-direction: column;
                gap: 12px;
                box-sizing: border-box;
                margin: 12px 0;
            }
            .tasfa-audio-controls {
                display: flex;
                align-items: center;
                gap: 20px;
                width: 100%;
            }
            .tasfa-audio-play-btn {
                width: 52px;
                height: 52px;
                border: none;
                border-radius: 50%;
                background: linear-gradient(135deg, var(--accent, #0066cc) 0%, var(--accent-hover, #0052a3) 100%);
                color: #fff;
                display: flex;
                align-items: center;
                justify-content: center;
                font-size: 20px;
                box-shadow: 0 4px 15px rgba(0,0,0,0.2), inset 0 1px 0 rgba(255,255,255,0.2);
                cursor: pointer;
                flex-shrink: 0;
                transition: transform 0.2s cubic-bezier(0.34, 1.56, 0.64, 1), background 0.2s;
            }
            .tasfa-audio-play-btn:hover {
                transform: scale(1.08);
            }
            .tasfa-audio-play-btn svg {
                width: 22px;
                height: 22px;
                fill: currentColor;
            }
            .tasfa-audio-details {
                display: flex;
                flex-direction: column;
                gap: 6px;
                flex-grow: 1;
                min-width: 0;
            }
            .tasfa-audio-title {
                color: #fff;
                font-size: 14px;
                font-weight: 600;
                white-space: nowrap;
                overflow: hidden;
                text-overflow: ellipsis;
                letter-spacing: 0.3px;
            }
            .tasfa-audio-progress-row {
                display: flex;
                align-items: center;
                gap: 12px;
                width: 100%;
            }
            .tasfa-audio-seeker {
                -webkit-appearance: none;
                appearance: none;
                flex-grow: 1;
                height: 5px;
                background: rgba(255, 255, 255, 0.15);
                border-radius: 3px;
                outline: none;
                cursor: pointer;
                margin: 0;
            }
            .tasfa-audio-seeker::-webkit-slider-thumb {
                -webkit-appearance: none;
                appearance: none;
                width: 12px;
                height: 12px;
                border-radius: 50%;
                background: var(--accent, #0066cc);
                cursor: pointer;
                box-shadow: 0 1px 3px rgba(0,0,0,0.3);
            }
            .tasfa-audio-seeker::-moz-range-thumb {
                width: 12px;
                height: 12px;
                border: none;
                border-radius: 50%;
                background: var(--accent, #0066cc);
                cursor: pointer;
                box-shadow: 0 1px 3px rgba(0,0,0,0.3);
            }
            .tasfa-audio-time-display {
                color: rgba(255, 255, 255, 0.6);
                font-family: 'Outfit', 'Inter', monospace, sans-serif;
                font-size: 11px;
                flex-shrink: 0;
                user-select: none;
            }
            .tasfa-audio-volume-group {
                display: flex;
                align-items: center;
                gap: 8px;
                flex-shrink: 0;
            }
            .tasfa-audio-control-btn {
                background: none;
                border: none;
                color: rgba(255, 255, 255, 0.7);
                cursor: pointer;
                display: flex;
                align-items: center;
                justify-content: center;
                padding: 4px;
                transition: color 0.2s, transform 0.1s;
            }
            .tasfa-audio-control-btn:hover {
                color: #fff;
                transform: scale(1.1);
            }
            .tasfa-audio-control-btn svg {
                width: 18px;
                height: 18px;
                fill: currentColor;
            }
            .tasfa-audio-volume-slider {
                -webkit-appearance: none;
                appearance: none;
                width: 60px;
                height: 4px;
                background: rgba(255, 255, 255, 0.15);
                border-radius: 2px;
                outline: none;
                cursor: pointer;
            }
            .tasfa-audio-volume-slider::-webkit-slider-thumb {
                -webkit-appearance: none;
                appearance: none;
                width: 8px;
                height: 8px;
                border-radius: 50%;
                background: #fff;
                cursor: pointer;
            }
            .tasfa-audio-volume-slider::-moz-range-thumb {
                width: 8px;
                height: 8px;
                border: none;
                border-radius: 50%;
                background: #fff;
                cursor: pointer;
        `;
        document.head.appendChild(style);

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

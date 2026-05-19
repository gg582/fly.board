(function() {
    var cache = new Map();
    var DOWNLOAD_FETCH_TIMEOUT_MS = 6000;
    var DOWNLOAD_RETRY_LIMIT = 5;
    var DOWNLOAD_MULTI_SESSION_CAP = 6;
    var SPACER_GIF = "data:image/gif;base64,R0lGODlhAQABAIAAAAAAAP///yH5BAEAAAAALAAAAAABAAEAAAIBRAA7";

    var TasfaDB = {
        _db: null,
        open: function() {
            if (this._db) return Promise.resolve(this._db);
            return new Promise(function(resolve, reject) {
                try {
                    var request = indexedDB.open('tasfa_cache', 1);
                    request.onupgradeneeded = function(e) {
                        var db = e.target.result;
                        if (!db.objectStoreNames.contains('chunks')) {
                            db.createObjectStore('chunks');
                        }
                    };
                    request.onsuccess = function(e) {
                        TasfaDB._db = e.target.result;
                        resolve(TasfaDB._db);
                    };
                    request.onerror = function(e) { resolve(null); };
                } catch (err) { resolve(null); }
            });
        },
        putChunk: function(key, data) {
            return this.open().then(function(db) {
                if (!db) return;
                return new Promise(function(resolve) {
                    try {
                        var tx = db.transaction('chunks', 'readwrite');
                        tx.objectStore('chunks').put(data, key);
                        tx.oncomplete = function() { resolve(); };
                        tx.onerror = function() { resolve(); };
                    } catch (err) { resolve(); }
                });
            });
        },
        getChunk: function(key) {
            return this.open().then(function(db) {
                if (!db) return null;
                return new Promise(function(resolve) {
                    try {
                        var tx = db.transaction('chunks', 'readonly');
                        var req = tx.objectStore('chunks').get(key);
                        req.onsuccess = function(e) { resolve(e.target.result); };
                        req.onerror = function() { resolve(null); };
                    } catch (err) { resolve(null); }
                });
            });
        }
    };

    function handshakeUrl(baseUrl) {
        if (baseUrl.indexOf('/file/download/') === 0) return baseUrl + '/handshake';
        if (baseUrl.indexOf('/assets/img/') === 0) return '/assets/tasfa/img/' + encodeURIComponent(baseUrl.slice('/assets/img/'.length)) + '/handshake';
        if (baseUrl.indexOf('/assets/uploads/') === 0) return '/assets/tasfa/uploads/' + encodeURIComponent(baseUrl.slice('/assets/uploads/'.length)) + '/handshake';
        return null;
    }

    function chunkUrl(baseUrl, sessionId, sessionToken, chunkIndex) {
        if (baseUrl.indexOf('/file/download/') === 0) {
            return baseUrl + '/chunk/' + String(chunkIndex) + '?session_id=' + encodeURIComponent(sessionId) + '&session_token=' + encodeURIComponent(sessionToken);
        }
        if (baseUrl.indexOf('/assets/img/') === 0) {
            return '/assets/tasfa/img/' + encodeURIComponent(baseUrl.slice('/assets/img/'.length)) + '/chunk/' + String(chunkIndex) + '?session_id=' + encodeURIComponent(sessionId) + '&session_token=' + encodeURIComponent(sessionToken);
        }
        if (baseUrl.indexOf('/assets/uploads/') === 0) {
            return '/assets/tasfa/uploads/' + encodeURIComponent(baseUrl.slice('/assets/uploads/'.length)) + '/chunk/' + String(chunkIndex) + '?session_id=' + encodeURIComponent(sessionId) + '&session_token=' + encodeURIComponent(sessionToken);
        }
        return null;
    }

    function buildLinkHintQuery(state) {
        var conn = navigator.connection || navigator.mozConnection || navigator.webkitConnection;
        var params = new URLSearchParams();
        params.set('link_stability_score', String(computeLinkStabilityScore(state, conn)));
        params.set('link_effective_type', conn && conn.effectiveType ? conn.effectiveType : '');
        params.set('link_downlink_mbps', conn && typeof conn.downlink === 'number' ? String(conn.downlink) : '');
        params.set('link_rtt_ms', String(Math.round((state && state.ewmaRttMs) || (conn && conn.rtt) || 0)));
        params.set('link_retry_events', String((state && state.retries) || 0));
        params.set('link_timeout_events', String((state && state.timeouts) || 0));
        params.set('link_save_data', conn && conn.saveData ? '1' : '0');
        return params.toString();
    }

    function computeLinkStabilityScore(state, conn) {
        var score = 55;
        var effectiveType = conn && conn.effectiveType ? conn.effectiveType : '';
        var downlink = conn && typeof conn.downlink === 'number' ? conn.downlink : 0;
        var rtt = (state && state.ewmaRttMs) || (conn && typeof conn.rtt === 'number' ? conn.rtt : 0);
        if (effectiveType === '4g') score += 24;
        else if (effectiveType === '3g') score += 10;
        else if (effectiveType === '2g' || effectiveType === 'slow-2g') score -= 10;
        if (downlink >= 30) score += 18;
        else if (downlink >= 10) score += 12;
        else if (downlink >= 3) score += 6;
        else if (downlink > 0 && downlink < 1.5) score -= 10;
        if (rtt > 0) {
            if (rtt <= 60) score += 16;
            else if (rtt <= 120) score += 8;
            else if (rtt <= 220) score += 2;
            else if (rtt <= 450) score -= 10;
            else score -= 18;
        }
        score -= ((state && state.retries) || 0) * 5;
        score -= ((state && state.timeouts) || 0) * 12;
        if (conn && conn.saveData) score -= 10;
        if (score < 10) score = 10;
        if (score > 100) score = 100;
        return score;
    }

    async function fetchJson(url) {
        var response = await fetch(url, {
            headers: {
                'Accept': 'application/json',
                'X-Requested-With': 'XMLHttpRequest'
            }
        });
        if (!response.ok) throw new Error('handshake:' + response.status);
        return response.json();
    }

    function nextRetryDelay(attempt) {
        return Math.min(400, 50 * Math.pow(1.45, Math.max(0, Number(attempt) || 0)));
    }

    function writeCoalescedBuffer(target, buffer, session, startChunk, span, baseUrl) {
        var total = new Uint8Array(buffer);
        var offset = 0;
        for (var i = 0; i < span; i += 1) {
            var chunkIndex = startChunk + i;
            var remaining = (Number(session.total_size) || 0) - (chunkIndex * Number(session.chunk_size));
            var expected = Math.min(Number(session.chunk_size), remaining);
            var chunkData = total.subarray(offset, offset + expected);
            target.set(chunkData, chunkIndex * Number(session.chunk_size));
            TasfaDB.putChunk(baseUrl + ':' + chunkIndex, chunkData.buffer.slice(chunkData.byteOffset, chunkData.byteOffset + chunkData.byteLength));
            offset += expected;
        }
    }

    function normalizeDownloadSession(session) {
        if (!session) return null;
        var chunkCount = Math.max(1, Number(session.chunk_count) || 1);
        var maxParallel = Math.max(1, Math.min(Number(session.max_parallel_chunks) || 20, Math.max(chunkCount, 1)));
        var initialParallel = Math.max(1, Math.min(Number(session.initial_parallel_chunks) || maxParallel, maxParallel));
        return {
            raw: session,
            sessionId: session.session_id,
            sessionToken: session.session_token,
            chunkSize: Math.max(1, Number(session.chunk_size) || 1),
            chunkCount: chunkCount,
            totalSize: Math.max(0, Number(session.total_size) || 0),
            mimeType: session.mime_type || 'application/octet-stream',
            filename: session.filename || 'download',
            maxParallel: maxParallel,
            initialParallel: initialParallel,
            pacingMs: Math.max(0, Number(session.dispatch_pacing_ms) || 0),
            coalesceChunks: Math.max(1, Math.min(Number(session.coalesce_chunks) || 1, 64))
        };
    }

    async function fetchDownloadSession(baseUrl, linkState) {
        var hsUrl = handshakeUrl(baseUrl);
        if (!hsUrl) throw new Error('unsupported base url');
        var session = normalizeDownloadSession(await fetchJson(hsUrl + '?' + buildLinkHintQuery(linkState || {})));
        if (!session || !session.sessionId || !session.sessionToken) {
            throw new Error((session && session.error) || 'invalid handshake');
        }
        return session;
    }

    function chooseMultiTasfaSessionCount(session) {
        var conn = navigator.connection || navigator.mozConnection || navigator.webkitConnection;
        var saveData = !!(conn && conn.saveData);
        var downlink = conn && typeof conn.downlink === 'number' ? conn.downlink : 0;
        var cores = Math.max(1, Number(navigator.hardwareConcurrency) || 1);
        var memory = Math.max(0, Number(navigator.deviceMemory) || 0);
        var chunkCount = Math.max(1, Number(session && session.chunkCount) || 1);
        var coalesce = Math.max(1, Number(session && session.coalesceChunks) || 1);
        if (saveData || chunkCount < 24 || cores < 4) return 1;

        var count = 1;
        if (downlink >= 12 && cores >= 4 && chunkCount >= 32) count = 2;
        if (downlink >= 28 && cores >= 8 && memory >= 4 && chunkCount >= 96) count = 3;
        if (downlink >= 60 && cores >= 10 && memory >= 8 && chunkCount >= 160) count = 4;

        var groupBudget = Math.max(1, Math.floor(chunkCount / Math.max(8, coalesce)));
        return Math.max(1, Math.min(DOWNLOAD_MULTI_SESSION_CAP, count, groupBudget));
    }

    async function buildDownloadLanes(baseUrl, primarySession, linkState) {
        var lanes = [{
            session: primarySession,
            windowSize: primarySession.initialParallel,
            linkState: Object.assign({}, linkState || {})
        }];
        var targetCount = chooseMultiTasfaSessionCount(primarySession);
        if (targetCount <= 1) return lanes;

        var extraRequests = [];
        for (var i = 1; i < targetCount; i += 1) {
            extraRequests.push(fetchDownloadSession(baseUrl, linkState));
        }
        var results = await Promise.allSettled(extraRequests);
        results.forEach(function(result) {
            if (result.status !== 'fulfilled') return;
            var session = result.value;
            if (!session ||
                session.chunkCount !== primarySession.chunkCount ||
                session.chunkSize !== primarySession.chunkSize ||
                session.totalSize !== primarySession.totalSize) {
                return;
            }
            lanes.push({
                session: session,
                windowSize: session.initialParallel,
                linkState: Object.assign({}, linkState || {})
            });
        });
        return lanes;
    }

    function reserveDownloadGroup(sharedState, span) {
        if (!sharedState || sharedState.nextChunk >= sharedState.chunkCount) return null;
        while (sharedState.nextChunk < sharedState.chunkCount && sharedState.bitmap[sharedState.nextChunk]) {
            sharedState.nextChunk++;
        }
        if (sharedState.nextChunk >= sharedState.chunkCount) return null;

        var startChunk = sharedState.nextChunk;
        var actualSpan = 0;
        while (actualSpan < span && (startChunk + actualSpan) < sharedState.chunkCount && !sharedState.bitmap[startChunk + actualSpan]) {
            actualSpan++;
        }
        sharedState.nextChunk += actualSpan;
        return { startChunk: startChunk, span: actualSpan };
    }

    async function fetchGroupWithRetry(baseUrl, lane, allBytes, startChunk, span, retries) {
        var lastErr = null;
        for (var i = 0; i <= retries; i += 1) {
            var controller = new AbortController();
            var timeoutLimit = document.visibilityState === 'hidden' ? 30000 : DOWNLOAD_FETCH_TIMEOUT_MS;
            var timeoutId = setTimeout(function() { controller.abort(); }, timeoutLimit);
            var startedAt = Date.now();
            try {
                var url = chunkUrl(baseUrl, lane.session.sessionId, lane.session.sessionToken, startChunk) + '&span=' + String(span);
                var response = await fetch(url, {
                    headers: { 'X-Requested-With': 'XMLHttpRequest' },
                    signal: controller.signal
                });
                clearTimeout(timeoutId);
                if (!response.ok) throw new Error('chunk:' + response.status);
                lane.linkState.ewmaRttMs = lane.linkState.ewmaRttMs > 0
                    ? ((lane.linkState.ewmaRttMs * 0.7) + ((Date.now() - startedAt) * 0.3))
                    : (Date.now() - startedAt);
                writeCoalescedBuffer(allBytes, await response.arrayBuffer(), lane.session.raw, startChunk, span, baseUrl);
                return;
            } catch (e) {
                clearTimeout(timeoutId);
                lastErr = e;
                lane.linkState.retries = Number(lane.linkState.retries || 0) + 1;
                if (e && e.name === 'AbortError') lane.linkState.timeouts = Number(lane.linkState.timeouts || 0) + 1;
                if (e && e.message === 'timeout:fetch') lane.linkState.timeouts = Number(lane.linkState.timeouts || 0) + 1;
                if (i < retries) await new Promise(function(r) { setTimeout(r, nextRetryDelay(i)); });
            }
        }
        throw lastErr;
    }

    function runDownloadLane(baseUrl, lane, sharedState, allBytes) {
        return new Promise(function(resolve, reject) {
            function finishIfDone() {
                if (sharedState.failed) return true;
                if (sharedState.completedChunks >= sharedState.chunkCount && lane.active === 0) {
                    resolve();
                    return true;
                }
                return false;
            }

            function pump() {
                if (finishIfDone()) return;
                var reservedAny = false;
                while (lane.active < lane.windowSize) {
                    var group = reserveDownloadGroup(sharedState, lane.session.coalesceChunks);
                    if (!group) break;
                    reservedAny = true;
                    lane.active += 1;
                    fetchGroupWithRetry(baseUrl, lane, allBytes, group.startChunk, group.span, DOWNLOAD_RETRY_LIMIT).then(function(currentGroup) {
                        return function() {
                            lane.active -= 1;
                            for (var i = 0; i < currentGroup.span; i++) {
                                if (!sharedState.bitmap[currentGroup.startChunk + i]) {
                                    sharedState.bitmap[currentGroup.startChunk + i] = 1;
                                    sharedState.completedChunks += 1;
                                }
                            }
                            if (lane.windowSize < lane.session.maxParallel) {
                                lane.windowSize = Math.min(
                                    lane.session.maxParallel,
                                    lane.windowSize + Math.max(12, Math.ceil(lane.windowSize * 0.2))
                                );
                            }
                            if (!finishIfDone()) {
                                if (lane.session.pacingMs > 0) setTimeout(pump, lane.session.pacingMs);
                                else pump();
                            }
                        };
                    }(group)).catch(function(error) {
                        sharedState.failed = error || new Error('download failed');
                        reject(sharedState.failed);
                    });
                    if (lane.session.pacingMs > 0) break;
                }

                if (lane.session.pacingMs > 0 && !sharedState.failed && sharedState.nextChunk < sharedState.chunkCount && lane.active < lane.windowSize) {
                    setTimeout(pump, lane.session.pacingMs);
                } else if (!reservedAny && !sharedState.failed && lane.active === 0 && sharedState.completedChunks < sharedState.chunkCount) {
                    setTimeout(pump, 16);
                } else {
                    finishIfDone();
                }
            }

            lane.active = 0;
            pump();
        });
    }

    async function fetchBlobViaTasfa(baseUrl) {
        if (cache.has(baseUrl)) {
            var cached = cache.get(baseUrl);
            if (cached && typeof cached.then === 'function') return cached;
        }

        var promise = (async function() {
            var linkState = { ewmaRttMs: 0, retries: 0, timeouts: 0 };
            var primarySession = await fetchDownloadSession(baseUrl, linkState);
            var allBytes = new Uint8Array(primarySession.totalSize);
            var sharedState = {
                chunkCount: primarySession.chunkCount,
                nextChunk: 0,
                completedChunks: 0,
                failed: null,
                bitmap: new Array(primarySession.chunkCount).fill(0)
            };

            for (var i = 0; i < primarySession.chunkCount; i++) {
                var data = await TasfaDB.getChunk(baseUrl + ':' + i);
                if (data) {
                    allBytes.set(new Uint8Array(data), i * primarySession.chunkSize);
                    sharedState.bitmap[i] = 1;
                    sharedState.completedChunks++;
                }
            }

            if (sharedState.completedChunks < sharedState.chunkCount) {
                var lanes = await buildDownloadLanes(baseUrl, primarySession, linkState);
                await Promise.all(lanes.map(function(lane) {
                    return runDownloadLane(baseUrl, lane, sharedState, allBytes);
                }));
            }

            return {
                blob: new Blob([allBytes.buffer], { type: primarySession.mimeType }),
                filename: primarySession.filename
            };
        })();

        cache.set(baseUrl, promise);
        try {
            return await promise;
        } catch (error) {
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
        fetchBlobViaTasfa(baseUrl).then(function(result) {
            var objectUrl = URL.createObjectURL(result.blob);
            if (el.tagName === 'IMG') el.src = objectUrl;
            else el.src = objectUrl;
            if (el.tagName === 'VIDEO' || el.tagName === 'AUDIO') el.load();
        }).catch(function() {
            el.setAttribute('data-tasfa-error', '1');
        });
    }

    function upgradeMediaElement(el) {
        var baseUrl = el.getAttribute('data-tasfa-download') || el.getAttribute('src') || '';
        if (!/^\/(file\/download|assets\/img|assets\/uploads)\//.test(baseUrl)) return;
        if (!el.getAttribute('data-tasfa-download')) el.setAttribute('data-tasfa-download', baseUrl);
        if (el.tagName === 'IMG') el.src = SPACER_GIF;
        else el.removeAttribute('src');
        setMediaSource(el, baseUrl);
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

    function upgradeWithin(root) {
        if (!root || !root.querySelectorAll) return;
        root.querySelectorAll('img[src^="/file/download/"], img[src^="/assets/img/"], img[src^="/assets/uploads/"], img[data-tasfa-download]').forEach(upgradeMediaElement);
        root.querySelectorAll('video[src^="/file/download/"], video[data-tasfa-download], audio[src^="/file/download/"], audio[data-tasfa-download]').forEach(upgradeMediaElement);
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

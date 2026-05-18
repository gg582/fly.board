(function() {
    var cache = new Map();
    var DOWNLOAD_FETCH_TIMEOUT_MS = 7000;
    var DOWNLOAD_RETRY_LIMIT = 5;
    var SPACER_GIF = "data:image/gif;base64,R0lGODlhAQABAIAAAAAAAP///yH5BAEAAAAALAAAAAABAAEAAAIBRAA7";

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
        return Math.min(1000, 120 * Math.pow(1.6, Math.max(0, Number(attempt) || 0)));
    }

    function writeCoalescedBuffer(target, buffer, session, startChunk, span) {
        var total = new Uint8Array(buffer);
        var offset = 0;
        for (var i = 0; i < span; i += 1) {
            var chunkIndex = startChunk + i;
            var remaining = (Number(session.total_size) || 0) - (chunkIndex * Number(session.chunk_size));
            var expected = Math.min(Number(session.chunk_size), remaining);
            target.set(total.subarray(offset, offset + expected), chunkIndex * Number(session.chunk_size));
            offset += expected;
        }
    }

    async function fetchBlobViaTasfa(baseUrl) {
        if (cache.has(baseUrl)) return cache.get(baseUrl);

        var promise = (async function() {
            var hsUrl = handshakeUrl(baseUrl);
            if (!hsUrl) throw new Error('unsupported base url');
            var linkState = { ewmaRttMs: 0, retries: 0, timeouts: 0 };
            var session = await fetchJson(hsUrl + '?' + buildLinkHintQuery(linkState));
            if (!session || session.ok === false || !session.session_id || !session.session_token) {
                throw new Error((session && session.error) || 'invalid handshake');
            }

            var chunkCount = Math.max(1, Number(session.chunk_count) || 1);
            var maxParallel = Math.max(1, Math.min(Number(session.max_parallel_chunks) || 16, chunkCount));
            var initialParallel = Math.max(1, Math.min(Number(session.initial_parallel_chunks) || maxParallel, maxParallel));
            var pacingMs = Math.max(0, Number(session.dispatch_pacing_ms) || 0);
            var coalesceChunks = Math.max(1, Math.min(Number(session.coalesce_chunks) || 1, 40));
            var allBytes = new Uint8Array(Math.max(0, Number(session.total_size) || 0));
            var nextIndex = 0;
            var windowSize = initialParallel;
            var active = 0;

            async function fetchGroupWithRetry(startChunk, span, retries) {
                var lastErr = null;
                for (var i = 0; i <= retries; i += 1) {
                    var controller = new AbortController();
                    var timeoutId = setTimeout(function() { controller.abort(); }, DOWNLOAD_FETCH_TIMEOUT_MS);
                    var startedAt = Date.now();
                    try {
                        var url = chunkUrl(baseUrl, session.session_id, session.session_token, startChunk) + '&span=' + String(span);
                        var response = await fetch(url, {
                            headers: { 'X-Requested-With': 'XMLHttpRequest' },
                            signal: controller.signal
                        });
                        clearTimeout(timeoutId);
                        if (!response.ok) throw new Error('chunk:' + response.status);
                        linkState.ewmaRttMs = linkState.ewmaRttMs > 0 ? ((linkState.ewmaRttMs * 0.75) + ((Date.now() - startedAt) * 0.25)) : (Date.now() - startedAt);
                        writeCoalescedBuffer(allBytes, await response.arrayBuffer(), session, startChunk, span);
                        return;
                    } catch (e) {
                        clearTimeout(timeoutId);
                        lastErr = e;
                        linkState.retries += 1;
                        if (e && e.name === 'AbortError') linkState.timeouts += 1;
                        if (e && e.message === 'timeout:fetch') linkState.timeouts += 1;
                        if (i < retries) await new Promise(function(r) { setTimeout(r, nextRetryDelay(i)); });
                    }
                }
                throw lastErr;
            }

            await new Promise(function(resolve, reject) {
                function schedule() {
                    if (nextIndex >= chunkCount && active === 0) {
                        resolve();
                        return;
                    }

                    while (active < windowSize && nextIndex < chunkCount) {
                        var chunkIndex = nextIndex;
                        var span = Math.min(coalesceChunks, chunkCount - chunkIndex);
                        nextIndex += span;
                        active += 1;
                        fetchGroupWithRetry(chunkIndex, span, DOWNLOAD_RETRY_LIMIT).then(function() {
                            active -= 1;
                            if (windowSize < maxParallel) windowSize = Math.min(maxParallel, windowSize + 6);
                            if (pacingMs > 0) setTimeout(schedule, pacingMs);
                            else schedule();
                        }).catch(function(error) {
                            windowSize = Math.max(1, windowSize - 2);
                            active -= 1;
                            reject(error);
                        });
                        if (pacingMs > 0) break;
                    }

                    if (active > 0 && pacingMs > 0 && nextIndex < chunkCount) {
                        setTimeout(schedule, pacingMs);
                    }
                }

                schedule();
            });
            return {
                blob: new Blob([allBytes.buffer], { type: session.mime_type || 'application/octet-stream' }),
                filename: session.filename || 'download'
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

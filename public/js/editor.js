(function() {
    function init() {
    var ta = document.getElementById('md-editor');
    var preview = document.getElementById('md-preview');
    var fileInput = document.getElementById('file-input');
    var uploadPreview = document.getElementById('upload-preview');
    var dropzone = document.getElementById('upload-dropzone');
    var fileRepoUploadButton = document.getElementById('file-repo-upload-btn');
    var form = ta ? ta.closest('form') : null;
    var isEditorMode = !!ta;
    var isFileRepoMode = !!document.getElementById('file-repo-upload-root');
    var titleInput = document.getElementById('post-title-input');
    var wordCount = document.getElementById('editor-word-count');
    var readingTime = document.getElementById('editor-reading-time');
    var syncStatus = document.getElementById('editor-sync-status');
    var summaryInput = document.getElementById('summary-input');
    var tabButtons = document.querySelectorAll('[data-editor-tab]');
    var panes = document.querySelectorAll('[data-editor-pane]');
    var toolbarButtons = document.querySelectorAll('.editor-tool');
    var submitButtons = form ? form.querySelectorAll("button[type='submit']") : [];
    var timer = null;
    var isSubmitting = false;
    var AssetRegistry = [];
    var USE_TASFA = window.BLOG_USE_TASFA !== false;
    var USE_FORMATTER = window.use_formatter === true || window.BLOG_USE_FORMATTER === true;
    var PLAIN_UPLOAD_ENDPOINT = '/api/upload';
    var UPLOAD_INIT_ENDPOINT = '/file/upload/init';
    var UPLOAD_COMPLETE_ENDPOINT = '/file/upload/complete';
    var UPLOAD_CANCEL_ENDPOINT = '/file/upload/cancel';
    var UPLOAD_STATUS_ENDPOINT = '/file/upload/status';
    var UPLOAD_RENEGOTIATE_ENDPOINT = '/file/upload/renegotiate';
    var UPLOAD_ENDPOINT = '/file/upload';
    var UPLOAD_CHUNK_SIZE = 24 * 1024 * 1024;
    var UPLOAD_DEFAULT_PARALLEL = 16;
    var TASFA_UPLOAD_CHUNK_MIN = 4 * 1024 * 1024;
    var TASFA_UPLOAD_CHUNK_MAX = 48 * 1024 * 1024;
    var TASFA_UPLOAD_CHUNK_MOBILE_MAX = 24 * 1024 * 1024;
    var TASFA_UPLOAD_CHUNK_STEP_UP = 2 * 1024 * 1024;
    var TASFA_UPLOAD_CHUNK_STEP_DOWN = 512 * 1024;
    var TASFA_UPLOAD_CHUNK_STORE = 'tasfa_upload_chunk_size_v3';
    var TASFA_TRACE_LIMIT = 160;
    var TASFA_SOFT_STALL_MS = 8000;
    var TASFA_HARD_STALL_MS = 20000;
    var TASFA_FAST_RECOVERY_MS = 3000;
    var TASFA_CONNECT_TIMEOUT_MS = 3000;
    var TASFA_FALLBACK_PREFETCH_MAX_BYTES = 64 * 1024 * 1024;
    var TASFA_MIN_SIZE_BYTES = 4 * 1024 * 1024;
    var FileUploadQueue = [];
    var FileUploadQueueSeq = 0;
    var isFileUploadRunning = false;

    function escapeHtml(value) {
        return String(value || '')
            .replace(/&/g, '&amp;')
            .replace(/</g, '&lt;')
            .replace(/>/g, '&gt;')
            .replace(/"/g, '&quot;')
            .replace(/'/g, '&#39;');
    }

    function escapeRegExp(value) {
        return value.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
    }

    function generateUUID() {
        return 'asset-' + Math.random().toString(36).slice(2) + Date.now().toString(36);
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

    function isGoodTasfaLink() {
        var conn = getConnectionSnapshot();
        if (conn.saveData === '1') return false;
        if (conn.effectiveType === '4g' && (conn.downlink >= 8 || conn.rtt <= 160 || conn.downlink <= 0)) return true;
        if (!conn.effectiveType && !isLikelyMobile()) return true;
        return conn.downlink >= 12 && (conn.rtt <= 220 || conn.rtt <= 0);
    }

    function classifyTasfaTransfer(file) {
        var name = (file && file.name ? file.name : '').toLowerCase();
        var type = (file && file.type ? file.type : '').toLowerCase();
        var size = file && file.size ? file.size : 0;
        var isArchive = /(\.zip|\.7z|\.tar|\.gz|\.bz2|\.xz|\.rar)$/.test(name) || type === 'application/zip' || type.indexOf('compressed') !== -1;
        var isVideo = type.indexOf('video/') === 0 || /\.(mp4|mov|mkv|webm|avi|m4v)$/.test(name);
        var isAudio = type.indexOf('audio/') === 0;
        var isLarge = size >= 64 * 1024 * 1024;
        var isSmall = size > TASFA_MIN_SIZE_BYTES && size <= 12 * 1024 * 1024;
        if (isLarge || isVideo || isArchive) {
            return { kind: isVideo ? 'video' : (isArchive ? 'archive' : 'large'), priority: 90, highPerformance: true, smallBatch: false };
        }
        if (isSmall) {
            return { kind: isAudio ? 'audio-small' : 'small-batch', priority: 70, highPerformance: false, smallBatch: true };
        }
        return { kind: isAudio ? 'audio' : 'standard', priority: 50, highPerformance: false, smallBatch: false };
    }

    function applyTasfaTransferProfile(asset) {
        if (!asset || !asset.transferProfile) return;
        var profile = asset.transferProfile;
        if (profile.highPerformance) {
            asset.targetParallel = Math.max(1, asset.maxParallel || asset.targetParallel || UPLOAD_DEFAULT_PARALLEL);
            asset.dispatchPacingMs = 0;
            var stats = ensureTasfaStats(asset);
            stats.fastRecoveryUntil = Date.now() + TASFA_FAST_RECOVERY_MS;
            tasfaTrace(asset, 'profile-high-performance', { kind: profile.kind });
        } else if (profile.smallBatch) {
            asset.targetParallel = Math.max(asset.targetParallel || 1, Math.min(asset.maxParallel || 1, 8));
            asset.dispatchPacingMs = 0;
            tasfaTrace(asset, 'profile-small-batch', { kind: profile.kind });
        }
    }

    function tasfaTrace(asset, event, data) {
        if (!asset) return;
        asset.tasfaTrace = asset.tasfaTrace || [];
        var item = {
            t: Date.now(),
            event: event,
            uploadId: asset.uploadId || '',
            sessionId: asset.client_uuid || '',
            profile: asset.transferProfile ? asset.transferProfile.kind : '',
            chunk: data && data.chunkIndex !== undefined ? data.chunkIndex : null,
            chunkSize: asset.chunkSize || 0,
            target: asset.targetParallel || 0,
            max: asset.maxParallel || 0,
            pacing: asset.dispatchPacingMs || 0,
            inflight: asset.activeChunkPosts || 0,
            inflightBytes: asset.inflightBytes ? asset.inflightBytes.reduce(function(sum, value) { return sum + Number(value || 0); }, 0) : 0,
            encryptedCacheBytes: asset.encryptedCacheBytes || 0,
            fallbackPrefetchBudget: asset.chunkSize ? tasfaFallbackPrefetchBudget(asset) : 0
        };
        if (data) {
            Object.keys(data).forEach(function(key) {
                if (key !== 'chunkIndex') item[key] = data[key];
            });
        }
        asset.tasfaTrace.push(item);
        if (asset.tasfaTrace.length > TASFA_TRACE_LIMIT) asset.tasfaTrace.shift();
        if (asset.client_uuid) {
            window.__tasfaUploadTraces = window.__tasfaUploadTraces || {};
            window.__tasfaUploadTraces[asset.client_uuid] = asset.tasfaTrace;
            window.__tasfaUploadSummaries = window.__tasfaUploadSummaries || {};
            window.__tasfaUploadSummaries[asset.client_uuid] = tasfaTraceSummary(asset);
        }
        if (window.BLOG_TASFA_DEBUG && window.console && console.debug) {
            console.debug('[TASFA]', item);
        }
    }

    function setTasfaStatus(asset, kind) {
        if (!asset || !asset.ui || !asset.ui.status) return;
        var messages = {
            preparing: 'Preparing upload...',
            uploading: 'Uploading...',
            active: 'Uploading...',
            waiting: 'Waiting...',
            checking: 'Keeping upload active...',
            verifying: 'Checking uploaded data...',
            finalizing: 'Saving upload...',
            retrying: 'Continuing upload...',
            resending: 'Continuing upload...',
            done: 'Uploaded'
        };
        asset.ui.status.textContent = messages[kind] || messages.uploading;
    }

    function tasfaTraceSummary(asset) {
        var trace = asset && asset.tasfaTrace ? asset.tasfaTrace : [];
        var summary = {
            uploadId: asset && asset.uploadId || '',
            sessionId: asset && asset.client_uuid || '',
            profile: asset && asset.transferProfile ? asset.transferProfile.kind : '',
            events: trace.length,
            renegotiations: 0,
            retries: 0,
            errors: 0,
            softRecoveries: 0,
            hardRecoveries: 0,
            peakInflightBytes: 0,
            peakEncryptedCacheBytes: 0,
            minTarget: asset && asset.maxParallel || 0,
            maxTarget: 0
        };
        trace.forEach(function(item) {
            if (!item) return;
            if (item.event === 'renegotiate-request') summary.renegotiations += 1;
            if (item.event === 'chunk-retry') summary.retries += 1;
            if (item.event === 'chunk-error') summary.errors += 1;
            if (item.event === 'soft-recovery') summary.softRecoveries += 1;
            if (item.event === 'hard-recovery') summary.hardRecoveries += 1;
            summary.peakInflightBytes = Math.max(summary.peakInflightBytes, Number(item.inflightBytes || 0));
            summary.peakEncryptedCacheBytes = Math.max(summary.peakEncryptedCacheBytes, Number(item.encryptedCacheBytes || 0));
            if (item.target > 0) {
                summary.minTarget = summary.minTarget ? Math.min(summary.minTarget, item.target) : item.target;
                summary.maxTarget = Math.max(summary.maxTarget, item.target);
            }
        });
        return summary;
    }

    function preferredUploadChunkSize(fileSize) {
        var maxValue = isLikelyMobile() ? TASFA_UPLOAD_CHUNK_MOBILE_MAX : TASFA_UPLOAD_CHUNK_MAX;
        var saved = 0;
        try { saved = Number(localStorage.getItem(TASFA_UPLOAD_CHUNK_STORE) || 0); } catch (err) {}
        var base = saved || (isLikelyMobile() ? 12 * 1024 * 1024 : UPLOAD_CHUNK_SIZE);
        base = Math.round(base / TASFA_UPLOAD_CHUNK_STEP_DOWN) * TASFA_UPLOAD_CHUNK_STEP_DOWN;
        return Math.min(Math.max(TASFA_UPLOAD_CHUNK_MIN, clampNumber(base, TASFA_UPLOAD_CHUNK_MIN, maxValue)), Math.max(TASFA_UPLOAD_CHUNK_MIN, fileSize || maxValue));
    }

    function rememberUploadChunkSize(value) {
        var maxValue = isLikelyMobile() ? TASFA_UPLOAD_CHUNK_MOBILE_MAX : TASFA_UPLOAD_CHUNK_MAX;
        var next = Math.round(clampNumber(value, TASFA_UPLOAD_CHUNK_MIN, maxValue) / TASFA_UPLOAD_CHUNK_STEP_DOWN) * TASFA_UPLOAD_CHUNK_STEP_DOWN;
        try { localStorage.setItem(TASFA_UPLOAD_CHUNK_STORE, String(next)); } catch (err) {}
        return next;
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
                try { xhr._tasfaIdleTimeout = true; } catch (err) {}
                try { xhr.abort(); } catch (err2) {}
            }, timeoutMs);
        }
        arm();
        return { arm: arm, clear: clear };
    }

    function ensureTasfaStats(asset) {
        asset.linkStats = asset.linkStats || {
            retryEvents: 0,
            timeoutEvents: 0,
            failureEvents: 0,
            successEvents: 0,
            fastStreak: 0,
            consecutiveFailures: 0,
            ewmaMs: 0,
            ewmaMbps: 0,
            lastChunkMs: 0,
            qualitySamples: [],
            lastRenegotiateAt: 0,
            lastGuardedAt: 0,
            lastSoftRecoveryAt: 0,
            fastRecoveryUntil: 0,
            lastDropAt: 0,
            lastFailureKind: '',
            sameFailureStreak: 0,
            throughputSlope: 0,
            lastProgressAt: 0,
            progressSilenceMs: 0,
            retryFreeStreak: 0
        };
        return asset.linkStats;
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

    function pushTasfaQuality(asset, value) {
        var stats = ensureTasfaStats(asset);
        stats.qualitySamples.push(clampNumber(value, 0, 1));
        if (stats.qualitySamples.length > 12) stats.qualitySamples.shift();
    }

    function pushTasfaQualityFromMetrics(asset, metrics) {
        var stats = ensureTasfaStats(asset);
        metrics = metrics || {};
        var mbps = Number(metrics.mbps || stats.ewmaMbps || 0);
        var slope = Number(stats.throughputSlope || 0);
        var inflight = Math.max(1, Number(asset.activeChunkPosts || 1));
        var maxParallel = Math.max(1, Number(asset.maxParallel || inflight));
        var silenceMs = Number(stats.progressSilenceMs || 0);
        var retryFree = Math.min(Number(stats.retryFreeStreak || 0), 8);
        var score = 0.35 + clampNumber(mbps / 40, 0, 0.35);
        score += clampNumber(slope / 25, -0.12, 0.12);
        score += clampNumber(inflight / maxParallel, 0, 1) * 0.08;
        score += retryFree * 0.015;
        if (silenceMs > 10000) score -= clampNumber((silenceMs - 10000) / 60000, 0, 0.18);
        if (metrics.failure) score -= metrics.timeout ? 0.24 : 0.12;
        pushTasfaQuality(asset, score);
    }

    function predictedTasfaQuality(asset) {
        var stats = ensureTasfaStats(asset);
        var value = wynnEpsilonAitken(stats.qualitySamples);
        return clampNumber(value, 0, 1);
    }

    function predictedTasfaSignal(asset) {
        var stats = ensureTasfaStats(asset);
        var samples = stats.qualitySamples || [];
        var value = predictedTasfaQuality(asset);
        if (samples.length < 3) return { value: value, confidence: 0.25 };
        var recent = samples.slice(-4);
        var mean = recent.reduce(function(sum, v) { return sum + Number(v || 0); }, 0) / recent.length;
        var variance = recent.reduce(function(sum, v) {
            var d = Number(v || 0) - mean;
            return sum + (d * d);
        }, 0) / recent.length;
        var confidence = clampNumber(1 - (variance * 6), 0.1, 1);
        return { value: value, confidence: confidence };
    }

    function predictedTasfaOutlook(asset, chunkIndex) {
        var stats = ensureTasfaStats(asset);
        var signal = predictedTasfaSignal(asset);
        var retryCount = asset.retryCounts ? Number(asset.retryCounts[chunkIndex] || 0) : 0;
        var timeoutRisk = clampNumber((stats.timeoutEvents || 0) / 8, 0, 0.5);
        timeoutRisk += clampNumber((stats.progressSilenceMs || 0) / 90000, 0, 0.3);
        timeoutRisk += signal.value < 0.35 ? (0.35 - signal.value) : 0;
        var fallbackNeed = clampNumber((retryCount / 12) + ((stats.sameFailureStreak || 0) / 12) + (1 - signal.value) * 0.25, 0, 1);
        var recoveryChance = clampNumber(signal.value + ((stats.retryFreeStreak || 0) * 0.04) + ((stats.throughputSlope || 0) > 0 ? 0.12 : 0), 0, 1);
        return {
            timeoutRisk: clampNumber(timeoutRisk, 0, 1),
            fallbackNeed: fallbackNeed,
            recoveryChance: recoveryChance,
            confidence: signal.confidence
        };
    }

    function hasPredictableTasfaErrorPattern(asset, chunkIndex) {
        var stats = ensureTasfaStats(asset);
        var retryCount = asset.retryCounts ? Number(asset.retryCounts[chunkIndex] || 0) : 0;
        return retryCount >= 5 || (stats.consecutiveFailures || 0) >= 5 || (stats.sameFailureStreak || 0) >= 5 || (stats.timeoutEvents || 0) >= 4;
    }

    function predictUploadRule(asset, chunkIndex) {
        var retryCount = asset.retryCounts ? Number(asset.retryCounts[chunkIndex] || 0) : 0;
        var signal = predictedTasfaSignal(asset);
        var predicted = signal.value;
        var outlook = predictedTasfaOutlook(asset, chunkIndex);
        var predictable = hasPredictableTasfaErrorPattern(asset, chunkIndex);
        if (retryCount >= 10 || (predictable && outlook.fallbackNeed >= 0.75 && predicted < 0.24 && signal.confidence >= 0.6)) return 'fallback';
        if (predictable && retryCount >= 5) return 'guarded';
        if (predictable && outlook.timeoutRisk >= 0.55 && predicted < 0.3 && signal.confidence >= 0.75) return 'guarded';
        return 'normal';
    }

    function applyPredictedUploadRule(asset, rule) {
        var stats = ensureTasfaStats(asset);
        var now = Date.now();
        if (rule === 'guarded' && (asset.targetParallel || 1) > 1) {
            if (now - (stats.lastGuardedAt || 0) < 10000 && isGoodTasfaLink()) return;
            stats.lastGuardedAt = now;
            var floor = Math.min(asset.maxParallel || UPLOAD_DEFAULT_PARALLEL, isGoodTasfaLink() ? (isLikelyMobile() ? 4 : 8) : (isLikelyMobile() ? 2 : 4));
            if ((asset.targetParallel || 1) <= floor) return;
            asset.targetParallel = Math.max(floor, (asset.targetParallel || 1) - 1);
            stats.lastDropAt = now;
            stats.fastRecoveryUntil = now + TASFA_FAST_RECOVERY_MS;
            tasfaTrace(asset, 'guarded', { predicted: predictedTasfaQuality(asset).toFixed(3) });
            scheduleUploadRenegotiate(asset, false);
        } else if (rule === 'fallback') {
            asset.targetParallel = Math.max(1, asset.maxParallel || 1);
            stats.fastRecoveryUntil = now + TASFA_FAST_RECOVERY_MS;
            tasfaTrace(asset, 'fallback-rule', { predicted: predictedTasfaQuality(asset).toFixed(3) });
            scheduleUploadRenegotiate(asset, true);
        }
    }

    function tasfaLinkFormValues(asset, extra) {
        var stats = ensureTasfaStats(asset);
        var conn = getConnectionSnapshot();
        var values = {
            link_effective_type: conn.effectiveType,
            link_downlink_mbps: conn.downlink > 0 ? conn.downlink.toFixed(2) : '',
            link_rtt_ms: conn.rtt > 0 ? String(Math.round(conn.rtt)) : String(Math.round(stats.ewmaMs || 0)),
            link_retry_events: String(stats.retryEvents || 0),
            link_timeout_events: String(stats.timeoutEvents || 0),
            link_save_data: conn.saveData
        };
        if (extra) {
            Object.keys(extra).forEach(function(key) { values[key] = extra[key]; });
        }
        return values;
    }

    function maybeTuneUploadChunkHint(asset, success, durationMs, bytes) {
        var stats = ensureTasfaStats(asset);
        if (success) {
            var mbps = durationMs > 0 ? ((bytes * 8) / durationMs / 1000) : 0;
            var previousMbps = stats.ewmaMbps || mbps;
            stats.successEvents += 1;
            stats.fastStreak += 1;
            stats.consecutiveFailures = 0;
            stats.sameFailureStreak = 0;
            stats.lastFailureKind = '';
            stats.retryFreeStreak = (stats.retryFreeStreak || 0) + 1;
            stats.lastChunkMs = durationMs > 0 ? durationMs : stats.lastChunkMs;
            stats.ewmaMs = stats.ewmaMs ? (stats.ewmaMs * 0.75 + durationMs * 0.25) : durationMs;
            stats.ewmaMbps = stats.ewmaMbps ? (stats.ewmaMbps * 0.75 + mbps * 0.25) : mbps;
            stats.throughputSlope = stats.throughputSlope ? (stats.throughputSlope * 0.6 + (mbps - previousMbps) * 0.4) : (mbps - previousMbps);
            stats.progressSilenceMs = 0;
            pushTasfaQualityFromMetrics(asset, { mbps: mbps });
            if (stats.fastStreak >= 6 && (stats.ewmaMbps >= 25 || durationMs < 12000)) {
                rememberUploadChunkSize(preferredUploadChunkSize(asset.fileSize) + TASFA_UPLOAD_CHUNK_STEP_UP);
                stats.fastStreak = 0;
            }
        } else {
            stats.fastStreak = 0;
            stats.consecutiveFailures = (stats.consecutiveFailures || 0) + 1;
            stats.retryFreeStreak = 0;
            pushTasfaQualityFromMetrics(asset, { failure: true });
            rememberUploadChunkSize(preferredUploadChunkSize(asset.fileSize) - TASFA_UPLOAD_CHUNK_STEP_DOWN);
        }
    }

    function scheduleUploadRenegotiate(asset, immediate) {
        if (!asset || !asset.uploadId || !asset.uploadToken || asset.isCancelling) return;
        var now = Date.now();
        var stats = ensureTasfaStats(asset);
        if (!immediate && now - stats.lastRenegotiateAt < 5000) return;
        if (!isGoodTasfaLink() || predictedTasfaQuality(asset) < 0.55) {
            forceTasfaReconnect(asset, 'slow-link');
            return;
        }
        stats.lastRenegotiateAt = now;
        if (asset.renegotiateTimer) clearTimeout(asset.renegotiateTimer);
        asset.renegotiateTimer = setTimeout(function() {
            asset.renegotiateTimer = null;
            var suggested = Math.max(1, Math.min(asset.targetParallel || asset.maxParallel || UPLOAD_DEFAULT_PARALLEL, asset.maxParallel || UPLOAD_DEFAULT_PARALLEL));
            var values = tasfaLinkFormValues(asset, {
                upload_id: asset.uploadId,
                upload_token: asset.uploadToken,
                suggested_parallel: String(suggested)
            });
            tasfaTrace(asset, 'renegotiate-request', {
                suggested: suggested,
                retryEvents: stats.retryEvents || 0,
                timeoutEvents: stats.timeoutEvents || 0,
                predicted: predictedTasfaQuality(asset).toFixed(3)
            });
            fetch(UPLOAD_RENEGOTIATE_ENDPOINT, {
                method: 'POST',
                headers: { 'Content-Type': 'application/x-www-form-urlencoded', 'Accept': 'application/json', 'X-Requested-With': 'XMLHttpRequest' },
                body: encodeFormBody(values)
            }).then(function(response) {
                if (!response.ok) return null;
                return response.json();
            }).then(function(payload) {
                if (!payload || payload.ok === false) return;
                var limit = Number(payload.max_parallel_chunks || asset.maxParallel || UPLOAD_DEFAULT_PARALLEL);
                var current = Number(payload.current_parallel_chunks || payload.initial_parallel_chunks || suggested);
                var previousTarget = asset.targetParallel || suggested;
                asset.maxParallel = Math.max(1, Math.min(limit || suggested, asset.totalChunks || limit || suggested));
                asset.targetParallel = Math.max(1, Math.min(current || suggested, asset.maxParallel));
                if (asset.targetParallel < previousTarget) {
                    stats.lastDropAt = Date.now();
                    stats.fastRecoveryUntil = Date.now() + TASFA_FAST_RECOVERY_MS;
                    if (isGoodTasfaLink()) {
                        asset.targetParallel = Math.max(asset.targetParallel, Math.min(previousTarget - 1, asset.maxParallel));
                    }
                }
                asset.dispatchPacingMs = Math.max(0, Number(payload.dispatch_pacing_ms || 0));
                if (isGoodTasfaLink() && asset.dispatchPacingMs > 1) asset.dispatchPacingMs = 0;
                tasfaTrace(asset, 'renegotiate-response', {
                    serverCurrent: current,
                    serverMax: limit,
                    appliedTarget: asset.targetParallel,
                    appliedPacing: asset.dispatchPacingMs
                });
            }).catch(function() {});
        }, immediate ? 0 : 300);
    }

    function forceTasfaReconnect(asset, reason) {
        if (!asset || asset.isCancelling || !asset.file) return;
        if (asset.forceReconnectTimer) return;
        asset.forceReconnectTimer = setTimeout(function() {
            asset.forceReconnectTimer = null;
            if (!asset || asset.isCancelling || asset.fid !== null) return;
            if (asset.renegotiateTimer) {
                clearTimeout(asset.renegotiateTimer);
                asset.renegotiateTimer = null;
            }
            asset.uploadRunId = (asset.uploadRunId || 0) + 1;
            if (asset.xhrs && asset.xhrs.length) {
                asset.xhrs.slice().forEach(function(xhr) {
                    try { xhr.abort(); } catch (err) {}
                });
            }
            asset.xhrs = [];
            asset.isUploading = false;
            asset.isNetworkPaused = false;
            tasfaTrace(asset, 'force-reconnect', { reason: reason || 'slow-link' });
            setTasfaStatus(asset, 'retrying');
            resumeTasfaUpload(asset, asset.file);
        }, 0);
    }

    function recordTasfaSuccess(asset, bytes, durationMs) {
        maybeTuneUploadChunkHint(asset, true, durationMs, bytes);
        if (asset.maxParallel && (asset.targetParallel || 1) < asset.maxParallel) {
            var stats = ensureTasfaStats(asset);
            if (stats.successEvents % 2 === 0 || Date.now() < (stats.fastRecoveryUntil || 0)) {
                var step = Date.now() < (stats.fastRecoveryUntil || 0) ? (isGoodTasfaLink() ? 4 : 2) : 1;
                asset.targetParallel = Math.min(asset.maxParallel, (asset.targetParallel || 1) + step);
                tasfaTrace(asset, 'success-ramp', { durationMs: Math.round(durationMs || 0), mbps: bytes && durationMs ? (((bytes * 8) / durationMs / 1000).toFixed(2)) : '0' });
                scheduleUploadRenegotiate(asset, false);
            }
        }
    }

    function recordTasfaFailure(asset, kind) {
        var stats = ensureTasfaStats(asset);
        stats.failureEvents += 1;
        stats.retryEvents += 1;
        if (kind === 'timeout') stats.timeoutEvents += 1;
        if (stats.lastFailureKind === kind) stats.sameFailureStreak = (stats.sameFailureStreak || 0) + 1;
        else stats.sameFailureStreak = 1;
        stats.lastFailureKind = kind;
        maybeTuneUploadChunkHint(asset, false, 0, 0);
        var goodLink = isGoodTasfaLink();
        var floor = Math.min(asset.maxParallel || UPLOAD_DEFAULT_PARALLEL, goodLink ? (isLikelyMobile() ? 4 : 8) : (isLikelyMobile() ? 2 : 4));
        var predictable = (stats.consecutiveFailures || 0) >= 5 || (stats.sameFailureStreak || 0) >= 5 || (kind === 'timeout' && (stats.timeoutEvents || 0) >= 4);
        var shouldReduce = predictable && ((kind === 'timeout' && !goodLink) || stats.failureEvents % 5 === 0 || (stats.consecutiveFailures || 0) >= 6);
        if (shouldReduce && (asset.targetParallel || 1) > floor) {
            asset.targetParallel = Math.max(floor, (asset.targetParallel || 1) - 1);
            stats.lastDropAt = Date.now();
            stats.fastRecoveryUntil = Date.now() + TASFA_FAST_RECOVERY_MS;
        } else if ((asset.targetParallel || 1) < (asset.maxParallel || 1)) {
            asset.targetParallel = Math.min(asset.maxParallel || 1, (asset.targetParallel || 1) + (goodLink ? 2 : 1));
        }
        tasfaTrace(asset, 'failure', { kind: kind, floor: floor, predictable: predictable ? 1 : 0, reduced: shouldReduce ? 1 : 0 });
        if (kind === 'timeout' || predictable || !goodLink) {
            forceTasfaReconnect(asset, kind || 'network');
        } else {
            scheduleUploadRenegotiate(asset, true);
        }
    }

    function touchTasfaActivity(asset) {
        if (asset) asset.lastTasfaActivityAt = Date.now();
    }

    function recordTasfaProgress(asset) {
        var stats = ensureTasfaStats(asset);
        var now = Date.now();
        stats.progressSilenceMs = stats.lastProgressAt ? Math.max(0, now - stats.lastProgressAt) : 0;
        stats.lastProgressAt = now;
    }

    function stopTasfaWatchdog(asset) {
        if (asset && asset.watchdogTimer) {
            clearInterval(asset.watchdogTimer);
            asset.watchdogTimer = null;
        }
    }

    function startTasfaWatchdog(asset) {
        if (!asset || asset.watchdogTimer) return;
        touchTasfaActivity(asset);
        (function tick() {
            if (!asset || asset.fid !== null || asset.failed || asset.isCancelling) {
                asset.watchdogTimer = null;
                return;
            }
            if (!asset.uploadId || asset.isNetworkPaused) {
                asset.watchdogTimer = setTimeout(tick, 2000);
                return;
            }
            var idleMs = Date.now() - (asset.lastTasfaActivityAt || 0);
            var stats = ensureTasfaStats(asset);
            stats.progressSilenceMs = stats.lastProgressAt ? Math.max(stats.progressSilenceMs || 0, Date.now() - stats.lastProgressAt) : idleMs;
            if (idleMs >= TASFA_SOFT_STALL_MS && Date.now() - (stats.lastSoftRecoveryAt || 0) >= TASFA_SOFT_STALL_MS) {
                stats.lastSoftRecoveryAt = Date.now();
                stats.fastRecoveryUntil = Date.now() + TASFA_FAST_RECOVERY_MS;
                setTasfaStatus(asset, 'retrying');
                tasfaTrace(asset, 'soft-recovery', { idleMs: idleMs });
                forceTasfaReconnect(asset, 'soft-stall');
            }
            if (idleMs < TASFA_HARD_STALL_MS) {
                asset.watchdogTimer = setTimeout(tick, 2000);
                return;
            }
            asset.uploadRunId = (asset.uploadRunId || 0) + 1;
            if (asset.xhrs && asset.xhrs.length) {
                asset.xhrs.slice().forEach(function(xhr) {
                    try { xhr.abort(); } catch (err) {}
                });
            }
            asset.xhrs = [];
            asset.isUploading = false;
            tasfaTrace(asset, 'hard-recovery', { idleMs: idleMs });
            setTasfaStatus(asset, 'checking');
            resumeTasfaUpload(asset, asset.file);
            asset.watchdogTimer = setTimeout(tick, 2000);
        })();
    }

    function lineIsTableSeparator(line) {
        return /^\|?[\s:-]+\|[\s|:-]*$/.test(line.trim());
    }

    function lineStartsBlock(line) {
        var trimmed = line.trim();
        return !trimmed ||
            /^#{1,6}\s+/.test(trimmed) ||
            /^(```|~~~)/.test(trimmed) ||
            /^>\s?/.test(trimmed) ||
            /^(-|\*|\+)\s+/.test(trimmed) ||
            /^\d+\.\s+/.test(trimmed) ||
            /^(-{3,}|\*{3,}|_{3,})$/.test(trimmed);
    }

    function splitTableRow(line) {
        var row = line.trim();
        if (row.charAt(0) === '|') row = row.slice(1);
        if (row.charAt(row.length - 1) === '|') row = row.slice(0, -1);
        return row.split('|').map(function(cell) {
            return applyInline(cell.trim());
        });
    }

    function applyInline(text) {
        var html = escapeHtml(text);
        html = html.replace(/!\[([^\]]*)\]\(([^)\s]+)(?:\s+"([^"]+)")?\)/g, function(_, alt, src, title) {
            var isVideo = /\.(mp4|mov|mkv|webm|avi|m4v)(\?.*)?$/i.test(src);
            var poster = '';
            
            if (src.indexOf('/file/download/') === 0) {
                var fidStr = src.split('/').pop();
                var fid = parseInt(fidStr);
                var asset = AssetRegistry.find(function(a) { return a.fid === fid; });
                if (asset) {
                    if (asset.mime_type && asset.mime_type.indexOf('video/') === 0) isVideo = true;
                    if (asset.thumb_path) poster = asset.thumb_path;
                }
            }

            if (isVideo) {
                var vattrs = "style='max-width:100%;height:auto;display:block' muted playsinline preload='metadata' controls";
                if (poster && poster.indexOf('public/uploads/') === 0) {
                    vattrs += " poster='/assets/uploads/" + escapeHtml(poster.slice(15)) + "'";
                }
                if (src.indexOf('/file/download/') === 0) {
                    vattrs = "data-tasfa-download='" + escapeHtml(src) + "' " + vattrs;
                    return "<video " + vattrs + "></video>";
                }
                return "<video src='" + escapeHtml(src) + "' " + vattrs + "></video>";
            }

            var attrs = "src='" + escapeHtml(src) + "' alt='" + escapeHtml(alt) + "' loading='lazy'";
            if (src.indexOf('/file/download/') === 0) {
                attrs = "data-tasfa-download='" + escapeHtml(src) + "' alt='" + escapeHtml(alt) + "' loading='lazy'";
            }
            if (title) attrs += " title='" + escapeHtml(title) + "'";
            return "<img " + attrs + ">";
        });
        html = html.replace(/\[([^\]]+)\]\(([^)\s]+)(?:\s+"([^"]+)")?\)/g, function(_, label, href, title) {
            var attrs = "href='" + escapeHtml(href) + "'";
            if (/^https?:\/\//.test(href)) attrs += " target='_blank' rel='noopener noreferrer'";
            if (title) attrs += " title='" + escapeHtml(title) + "'";
            return "<a " + attrs + ">" + label + "</a>";
        });
        html = html.replace(/`([^`]+)`/g, '<code>$1</code>');
        html = html.replace(/\*\*([^*]+)\*\*/g, '<strong>$1</strong>');
        html = html.replace(/\*([^*\n]+)\*/g, '<em>$1</em>');
        html = html.replace(/~~([^~]+)~~/g, '<del>$1</del>');
        return html;
    }

    function renderMarkdown(md) {
        if (!md || !md.trim()) {
            return "<p style='color:var(--muted)'>Nothing to preview yet.</p>";
        }

        var codeBlocks = [];
        var normalized = md.replace(/\r\n/g, '\n');
        normalized = normalized.replace(/```([\w-]*)\n([\s\S]*?)```/g, function(_, lang, code) {
            var token = '@@CODEBLOCK' + codeBlocks.length + '@@';
            var cls = lang ? " class='language-" + escapeHtml(lang) + "'" : '';
            codeBlocks.push("<pre><code" + cls + ">" + escapeHtml(code.replace(/\n$/, '')) + "</code></pre>");
            return token;
        });

        var lines = normalized.split('\n');
        var html = [];
        var i = 0;

        while (i < lines.length) {
            var line = lines[i];
            var trimmed = line.trim();

            if (!trimmed) {
                i += 1;
                continue;
            }

            if (/^@@CODEBLOCK\d+@@$/.test(trimmed)) {
                html.push(trimmed);
                i += 1;
                continue;
            }

            if (/^( {4}|\t)/.test(line)) {
                var indented = [];
                while (i < lines.length) {
                    var codeLine = lines[i];
                    if (/^( {4}|\t)/.test(codeLine)) {
                        indented.push(codeLine);
                        i += 1;
                        continue;
                    }
                    if (!codeLine.trim()) {
                        indented.push('');
                        i += 1;
                        continue;
                    }
                    break;
                }

                if (USE_FORMATTER) {
                    var minIndent = Infinity;
                    indented.forEach(function(codeLine) {
                        if (!codeLine.trim()) return;
                        var m = codeLine.match(/^[ \t]*/);
                        var width = m ? m[0].replace(/\t/g, '    ').length : 0;
                        if (width < minIndent) minIndent = width;
                    });

                    if (!isFinite(minIndent)) minIndent = 0;

                    indented = indented.map(function(codeLine) {
                        if (!codeLine.trim() || minIndent <= 0) return codeLine;
                        var col = 0;
                        var cut = 0;
                        while (cut < codeLine.length && col < minIndent) {
                            var ch = codeLine.charAt(cut);
                            if (ch === ' ') col += 1;
                            else if (ch === '\t') col += 4;
                            else break;
                            cut += 1;
                        }
                        return codeLine.slice(cut);
                    });
                }

                html.push('<pre><code>' + escapeHtml(indented.join('\n')) + '</code></pre>');
                continue;
            }

            if (/^#{1,6}\s+/.test(trimmed)) {
                var level = trimmed.match(/^#+/)[0].length;
                html.push('<h' + level + '>' + applyInline(trimmed.slice(level).trim()) + '</h' + level + '>');
                i += 1;
                continue;
            }

            if (/^(-{3,}|\*{3,}|_{3,})$/.test(trimmed)) {
                html.push('<hr>');
                i += 1;
                continue;
            }

            if (trimmed.indexOf('|') !== -1 && i + 1 < lines.length && lineIsTableSeparator(lines[i + 1])) {
                var headers = splitTableRow(lines[i]);
                var table = ['<table><thead><tr>'];
                headers.forEach(function(cell) {
                    table.push('<th>' + cell + '</th>');
                });
                table.push('</tr></thead><tbody>');
                i += 2;
                while (i < lines.length && lines[i].trim().indexOf('|') !== -1) {
                    var cells = splitTableRow(lines[i]);
                    table.push('<tr>');
                    cells.forEach(function(cell) {
                        table.push('<td>' + cell + '</td>');
                    });
                    table.push('</tr>');
                    i += 1;
                }
                table.push('</tbody></table>');
                html.push(table.join(''));
                continue;
            }

            if (/^>\s?/.test(trimmed)) {
                var quoteLines = [];
                while (i < lines.length && /^>\s?/.test(lines[i].trim())) {
                    quoteLines.push(applyInline(lines[i].trim().replace(/^>\s?/, '')));
                    i += 1;
                }
                html.push('<blockquote><p>' + quoteLines.join('<br>') + '</p></blockquote>');
                continue;
            }

            if (/^(-|\*|\+)\s+/.test(trimmed) || /^\d+\.\s+/.test(trimmed)) {
                var ordered = /^\d+\.\s+/.test(trimmed);
                var tag = ordered ? 'ol' : 'ul';
                var list = ['<' + tag + '>'];
                while (i < lines.length) {
                    var candidate = lines[i].trim();
                    if (ordered && /^\d+\.\s+/.test(candidate)) {
                        list.push('<li>' + applyInline(candidate.replace(/^\d+\.\s+/, '')) + '</li>');
                        i += 1;
                        continue;
                    }
                    if (!ordered && /^(-|\*|\+)\s+/.test(candidate)) {
                        list.push('<li>' + applyInline(candidate.replace(/^(-|\*|\+)\s+/, '')) + '</li>');
                        i += 1;
                        continue;
                    }
                    break;
                }
                list.push('</' + tag + '>');
                html.push(list.join(''));
                continue;
            }

            var paragraph = [applyInline(trimmed)];
            i += 1;
            while (i < lines.length && !lineStartsBlock(lines[i]) && !/^@@CODEBLOCK\d+@@$/.test(lines[i].trim())) {
                paragraph.push(applyInline(lines[i].trim()));
                i += 1;
            }
            html.push('<p>' + paragraph.join('<br>') + '</p>');
        }

        var rendered = html.join('');
        return rendered.replace(/@@CODEBLOCK(\d+)@@/g, function(_, idx) {
            return codeBlocks[Number(idx)] || '';
        });
    }

    function insertAtCursor(text, selectOffset) {
        if (!ta) return;
        var start = ta.selectionStart;
        var end = ta.selectionEnd;
        ta.value = ta.value.slice(0, start) + text + ta.value.slice(end);
        var caret = start + (typeof selectOffset === 'number' ? selectOffset : text.length);
        ta.selectionStart = ta.selectionEnd = caret;
        ta.focus();
        schedulePreview();
    }

    function toggleWrap(wrap, placeholder) {
        if (!ta) return;
        var start = ta.selectionStart;
        var end = ta.selectionEnd;
        var selected = ta.value.slice(start, end) || placeholder || '';
        var wrapped = wrap + selected + wrap;
        ta.value = ta.value.slice(0, start) + wrapped + ta.value.slice(end);
        ta.selectionStart = start + wrap.length;
        ta.selectionEnd = ta.selectionStart + selected.length;
        ta.focus();
        schedulePreview();
    }

    function prependToSelection(prefix, placeholder) {
        if (!ta) return;
        var start = ta.selectionStart;
        var end = ta.selectionEnd;
        var selected = ta.value.slice(start, end) || placeholder || '';
        var lines = selected.split('\n').map(function(line) {
            return prefix + line;
        }).join('\n');
        ta.value = ta.value.slice(0, start) + lines + ta.value.slice(end);
        ta.selectionStart = start;
        ta.selectionEnd = start + lines.length;
        ta.focus();
        schedulePreview();
    }

    function removeMarkdownByUrl(url) {
        if (!ta || !url) return;
        var pattern = new RegExp('!?\\[[^\\]]*\\]\\(' + escapeRegExp(url) + '\\)\\n?', 'g');
        ta.value = ta.value.replace(pattern, '');
    }

    function replaceAllInEditor(fromValue, toValue) {
        if (!ta || !fromValue) return;
        ta.value = ta.value.split(fromValue).join(toValue);
    }

    function editorHasUrl(url) {
        return !!(ta && url && ta.value.indexOf(url) !== -1);
    }

    function insertAssetMarkdown(asset) {
        var url = asset.url || (asset.fid !== null ? ('/file/download/' + asset.fid) : '');
        if (!url) return;
        var name = asset.filename || 'file';
        var isMedia = /^image\//.test(asset.mime_type || '') || /^video\//.test(asset.mime_type || '') || /^audio\//.test(asset.mime_type || '');
        if (isMedia) {
            insertAtCursor('![' + name + '](' + url + ')\n');
        } else {
            insertAtCursor('[' + name + '](' + url + ')\n');
        }
    }

    function switchTab(nextTab) {
        Array.prototype.forEach.call(tabButtons, function(button) {
            button.classList.toggle('is-active', button.getAttribute('data-editor-tab') === nextTab);
            button.className = button.getAttribute('data-editor-tab') === nextTab ? 'btn' : 'btn btn-outline';
        });
        Array.prototype.forEach.call(panes, function(pane) {
            var active = pane.getAttribute('data-editor-pane') === nextTab;
            pane.classList.toggle('is-active', active);
            pane.style.display = active ? 'block' : 'none';
        });
        if (nextTab === 'preview') updatePreview();
    }

    function updateMetrics() {
        if (!ta) return;
        var words = ta.value.trim() ? ta.value.trim().split(/\s+/).length : 0;
        var minutes = words ? Math.max(1, Math.ceil(words / 220)) : 0;
        if (wordCount) wordCount.textContent = words + ' words';
        if (readingTime) readingTime.textContent = minutes + ' min read';
    }

    function updatePreview() {
        if (!preview || !ta) return;
        preview.innerHTML = renderMarkdown(ta.value);
        if (typeof window.initMarkdownAffordances === 'function') {
            window.initMarkdownAffordances(preview);
        }
        updateMetrics();
        if (syncStatus) {
            syncStatus.textContent = 'Local preview updated';
        }
    }

    function schedulePreview() {
        if (syncStatus) syncStatus.textContent = 'Editing locally';
        clearTimeout(timer);
        timer = setTimeout(updatePreview, 120);
    }

    function syncMediaMeta() {
        var metaInput = document.getElementById('media-meta');
        if (!metaInput) return;
        metaInput.value = JSON.stringify(
            AssetRegistry
                .filter(function(asset) { return asset.fid !== null; })
                .map(function(asset) {
                    return { fid: asset.fid, mode: asset.mode, delete_pin: asset.deletePin || '' };
                })
        );
    }

    function removeAssetRecord(asset) {
        AssetRegistry = AssetRegistry.filter(function(item) {
            return item.client_uuid !== asset.client_uuid;
        });
        FileUploadQueue = FileUploadQueue.filter(function(uuid) {
            return uuid !== asset.client_uuid;
        });
        syncMediaMeta();
    }

    function setModeButtons(ui, mode) {
        ui.btnInline.className = mode === 'inline' ? 'btn media-inline-btn' : 'btn btn-outline media-inline-btn';
        ui.btnAttachment.className = mode === 'attachment' ? 'btn media-attachment-btn' : 'btn btn-outline media-attachment-btn';
    }

    function prependFileRepoCard(asset, response) {
        if (!isFileRepoMode) return;
        var list = document.getElementById('file-repo-list');
        var empty = document.getElementById('file-repo-empty');
        if (empty) empty.remove();
        if (!list) {
            list = document.createElement('div');
            list.className = 'post-grid';
            list.id = 'file-repo-list';
            var anchor = document.getElementById('file-repo-list-anchor');
            if (!anchor || !anchor.parentNode) return;
            anchor.parentNode.insertBefore(list, anchor.nextSibling);
        }

        var deletePinHtml = response.delete_pin
            ? "<div style='margin-top:10px;font-size:12px;color:var(--muted)'>Delete PIN: <code>" + escapeHtml(response.delete_pin) + "</code></div>"
            : "";
        var article = document.createElement('article');
        article.className = 'card';
        article.innerHTML =
            "<h4 style='margin-top:0'>" + escapeHtml(asset.filename) + "</h4>" +
            "<p style='color:var(--muted);font-size:13px'>" +
            escapeHtml(response.mime_type || 'unknown/mime') + " &middot; " +
            String(response.size || asset.fileSize || 0) + " bytes</p>" +
            "<div class='file-card-actions' style='display:flex;gap:10px;flex-wrap:wrap;margin-top:12px'>" +
            "<a href='/file/" + String(asset.fid) + "' class='btn btn-outline' style='font-size:12px;padding:4px 10px'>Open</a>" +
            "<a href='#' data-tasfa-download-link='" + escapeHtml(asset.url || ('/file/download/' + String(asset.fid))) + "' class='btn' style='font-size:12px;padding:4px 10px'>Download</a>" +
            "</div>" +
            deletePinHtml;
        list.insertBefore(article, list.firstChild);
    }

    function createMediaCard(filename, previewUrl, mediaType) {
        var card = document.createElement('div');
        card.className = 'media-card';

        var thumb = document.createElement('div');
        thumb.className = 'media-thumb';
        if (mediaType === 'image') {
            var img = document.createElement('img');
            img.src = previewUrl;
            img.style.display = 'block';
            img.style.width = '100%';
            img.style.height = '100%';
            img.style.maxWidth = 'none';
            img.style.objectFit = 'cover';
            img.style.filter = 'grayscale(1) contrast(1.08)';
            thumb.appendChild(img);
        } else if (mediaType === 'video') {
            var vid = document.createElement('video');
            vid.src = previewUrl;
            vid.muted = true;
            vid.playsInline = true;
            vid.preload = 'metadata';
            vid.style.display = 'block';
            vid.style.width = '100%';
            vid.style.height = '100%';
            vid.style.objectFit = 'cover';
            vid.style.filter = 'grayscale(1) contrast(1.08)';
            thumb.appendChild(vid);
        } else if (mediaType === 'audio') {
            var aud = document.createElement('audio');
            aud.src = previewUrl;
            aud.controls = true;
            aud.preload = 'metadata';
            aud.style.width = '100%';
            aud.style.height = '28px';
            aud.style.alignSelf = 'center';
            thumb.appendChild(aud);
        } else {
            var icon = document.createElement('span');
            icon.style.fontSize = '22px';
            icon.textContent = 'FILE';
            thumb.appendChild(icon);
        }

        var info = document.createElement('div');
        info.className = 'media-info';

        var name = document.createElement('div');
        name.className = 'media-name';
        name.textContent = filename;

        var status = document.createElement('div');
        status.className = 'media-status';
        status.textContent = isEditorMode ? 'Queued [0%]' : 'Queued';

        var progress = document.createElement('div');
        progress.className = 'media-progress-bar';

        var progressInner = document.createElement('div');
        progressInner.className = 'media-progress-inner';
        progress.appendChild(progressInner);

        info.appendChild(name);
        info.appendChild(status);
        info.appendChild(progress);

        var actions = document.createElement('div');
        actions.className = 'media-actions';

        var btnInsert = document.createElement('button');
        btnInsert.type = 'button';
        btnInsert.className = 'btn btn-outline media-insert-btn';
        btnInsert.textContent = 'Insert';
        btnInsert.disabled = true;

        var btnInline = document.createElement('button');
        btnInline.type = 'button';
        btnInline.className = 'btn media-inline-btn';
        btnInline.textContent = 'Inline';
        btnInline.disabled = true;

        var btnAttachment = document.createElement('button');
        btnAttachment.type = 'button';
        btnAttachment.className = 'btn btn-outline media-attachment-btn';
        btnAttachment.textContent = 'Attachment';
        btnAttachment.disabled = true;

        var btnUpload = document.createElement('button');
        btnUpload.type = 'button';
        btnUpload.className = 'btn media-upload-btn';
        btnUpload.textContent = 'Upload';
        btnUpload.style.display = 'none';

        var btnCancel = document.createElement('button');
        btnCancel.type = 'button';
        btnCancel.className = 'btn btn-outline media-cancel-btn';
        btnCancel.textContent = 'Cancel';
        btnCancel.style.display = 'none';

        var btnRemove = document.createElement('button');
        btnRemove.type = 'button';
        btnRemove.className = 'btn btn-outline media-remove-btn';
        btnRemove.textContent = 'Remove';
        btnRemove.style.display = 'none';

        var btnRetry = document.createElement('button');
        btnRetry.type = 'button';
        btnRetry.className = 'btn media-retry-btn';
        btnRetry.textContent = 'Retry';
        btnRetry.style.display = 'none';

        var btnDelete = document.createElement('button');
        btnDelete.type = 'button';
        btnDelete.className = 'btn btn-outline media-delete-btn';
        btnDelete.textContent = 'Delete';
        btnDelete.style.display = 'none';

        actions.appendChild(btnInsert);
        actions.appendChild(btnInline);
        actions.appendChild(btnAttachment);
        actions.appendChild(btnUpload);
        actions.appendChild(btnCancel);
        actions.appendChild(btnRemove);
        actions.appendChild(btnRetry);
        actions.appendChild(btnDelete);

        card.appendChild(thumb);
        card.appendChild(info);
        card.appendChild(actions);

        return {
            el: card,
            thumbImg: thumb.querySelector('img'),
            status: status,
            progress: progress,
            progressInner: progressInner,
            btnInsert: btnInsert,
            btnInline: btnInline,
            btnAttachment: btnAttachment,
            btnUpload: btnUpload,
            btnCancel: btnCancel,
            btnRemove: btnRemove,
            btnRetry: btnRetry,
            btnDelete: btnDelete
        };
    }

    function enqueueFileUpload(asset) {
        if (!asset || asset.isUploading || asset.fid !== null || asset.failed) return;
        if (FileUploadQueue.indexOf(asset.client_uuid) !== -1) return;
        if (!asset.uploadQueueSeq) asset.uploadQueueSeq = ++FileUploadQueueSeq;
        FileUploadQueue.push(asset.client_uuid);
        FileUploadQueue.sort(function(aUuid, bUuid) {
            var a = AssetRegistry.find(function(item) { return item.client_uuid === aUuid; });
            var b = AssetRegistry.find(function(item) { return item.client_uuid === bUuid; });
            var ap = a && a.transferProfile ? a.transferProfile.priority : 0;
            var bp = b && b.transferProfile ? b.transferProfile.priority : 0;
            if (ap !== bp) return bp - ap;
            return ((a && a.uploadQueueSeq) || 0) - ((b && b.uploadQueueSeq) || 0);
        });
        if (asset.ui && asset.ui.status) asset.ui.status.textContent = 'Waiting...';
        if (asset.ui && asset.ui.btnUpload) asset.ui.btnUpload.disabled = true;
        updateMediaCardUI(asset);
    }

    function updateMediaCardUI(asset) {
        if (!asset || !asset.ui) return;
        var ui = asset.ui;
        var isQueued = FileUploadQueue.indexOf(asset.client_uuid) !== -1;
        var isUploading = asset.isUploading && !asset.failed && asset.fid === null && !asset.isCancelling;
        var isDone = asset.fid !== null && !asset.failed;
        var isFailed = asset.failed;
        var isPending = !asset.isExisting && asset.fid === null && !asset.isUploading && !asset.uploadId && !asset.failed;

        function hide(el) { if (el) el.style.display = 'none'; }
        function show(el) { if (el) el.style.display = ''; }

        hide(ui.btnInsert);
        hide(ui.btnInline);
        hide(ui.btnAttachment);
        hide(ui.btnUpload);
        hide(ui.btnDelete);
        hide(ui.btnCancel);
        hide(ui.btnRemove);
        hide(ui.btnRetry);

        if (isFailed) {
            if (!asset.tooLarge) show(ui.btnRetry);
            show(ui.btnRemove);
        } else if (isUploading) {
            show(ui.btnCancel);
        } else if (isPending || isQueued) {
            if (!isEditorMode) {
                show(ui.btnUpload);
            }
            show(ui.btnRemove);
        } else if (isDone) {
            if (isEditorMode) {
                show(ui.btnInsert);
                show(ui.btnInline);
                show(ui.btnAttachment);
            } else {
                show(ui.btnInsert);
                show(ui.btnInline);
            }
            show(ui.btnDelete);
        }

        if (isDone) {
            if (ui.btnInsert) ui.btnInsert.disabled = false;
            if (ui.btnInline) ui.btnInline.disabled = false;
            if (ui.btnAttachment) ui.btnAttachment.disabled = !isEditorMode;
        }
    }

    function processFileUploadQueue() {
        if (isFileUploadRunning) return;
        var nextUuid = FileUploadQueue.shift();
        if (!nextUuid) return;
        var asset = AssetRegistry.find(function(a) { return a.client_uuid === nextUuid; });
        if (!asset || asset.fid !== null || asset.isUploading || asset.failed) {
            processFileUploadQueue();
            return;
        }
        isFileUploadRunning = true;
        startQueuedAssetUpload(asset);
    }

    function bindAssetControls(asset) {
        var ui = asset.ui;

        function cleanupAssetUI() {
            removeMarkdownByUrl(asset.url || asset.placeholderUrl);
            if (asset.blobUrl) {
                URL.revokeObjectURL(asset.blobUrl);
                asset.blobUrl = null;
            }
            if (asset.ui && asset.ui.el) {
                asset.ui.el.remove();
            }
            removeAssetRecord(asset);
            updateFileRepoUploadButton();
            schedulePreview();
        }

        ui.btnCancel.onclick = function() {
            asset.isCancelling = true;
            if (asset.xhrs && asset.xhrs.length) {
                asset.xhrs.slice().forEach(function(xhr) {
                    try { xhr.abort(); } catch (err) {}
                });
            }
            if (asset.uploadId) {
                fetch(UPLOAD_CANCEL_ENDPOINT, {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
                    body: 'upload_ids=' + encodeURIComponent(JSON.stringify([asset.uploadId]))
                });
            }
            asset.isUploading = false;
            isFileUploadRunning = false;
            processFileUploadQueue();
            cleanupAssetUI();
        };

        ui.btnRemove.onclick = function() {
            if (asset.isUploading && !asset.isCancelling) {
                asset.isCancelling = true;
                if (asset.xhrs && asset.xhrs.length) {
                    asset.xhrs.slice().forEach(function(xhr) {
                        try { xhr.abort(); } catch (err) {}
                    });
                }
                if (asset.uploadId) {
                    fetch(UPLOAD_CANCEL_ENDPOINT, {
                        method: 'POST',
                        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
                        body: 'upload_ids=' + encodeURIComponent(JSON.stringify([asset.uploadId]))
                    });
                }
                asset.isUploading = false;
                isFileUploadRunning = false;
                processFileUploadQueue();
            }
            cleanupAssetUI();
        };

        ui.btnRetry.onclick = function() {
            if (!asset.file || asset.fid !== null) return;
            asset.failed = false;
            asset.isUploading = false;
            asset.isCancelling = false;
            asset.xhrs = [];
            resetDisplayedProgress(asset);
            if (asset.uploadId && asset.uploadToken && asset.isSessionExpired) {
                asset.isSessionExpired = false;
                verifyAllChunksBeforeComplete(asset, asset.file);
                return;
            }
            asset.uploadId = null;
            asset.uploadToken = null;
            if (isEditorMode) {
                startTasfaUpload(asset, asset.file);
            } else {
                enqueueFileUpload(asset);
                processFileUploadQueue();
            }
        };

        ui.btnDelete.onclick = function() {
            if (asset.fid !== null) {
                fetch('/file/delete', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
                    body: 'id=' + encodeURIComponent(String(asset.fid)) +
                        '&delete_pin=' + encodeURIComponent(asset.deletePin || '')
                });
            }
            cleanupAssetUI();
        };

        if (!isEditorMode) {
            ui.btnUpload.onclick = function() {
                enqueueFileUpload(asset);
                processFileUploadQueue();
            };
            ui.btnInsert.textContent = 'Open';
            ui.btnInsert.onclick = function() {
                if (asset.fid !== null) window.location.href = '/file/' + asset.fid;
            };
            ui.btnInline.textContent = 'Download';
            ui.btnInline.onclick = function() {
                if (asset.url && typeof window.openTasfaDownload === 'function') {
                    window.openTasfaDownload(asset.url);
                }
            };
            ui.btnAttachment.style.display = 'none';
            ui.btnInsert.disabled = asset.fid === null;
            ui.btnInline.disabled = asset.fid === null;
        } else {
            ui.btnInsert.onclick = function() {
                insertAssetMarkdown(asset);
            };

            ui.btnInline.onclick = function() {
                if (!asset.url) return;
                asset.mode = 'inline';
                if (!editorHasUrl(asset.url)) {
                    insertAssetMarkdown(asset);
                } else {
                    schedulePreview();
                }
                setModeButtons(ui, asset.mode);
                syncMediaMeta();
            };

            ui.btnAttachment.onclick = function() {
                if (!asset.url) return;
                asset.mode = 'attachment';
                removeMarkdownByUrl(asset.url);
                setModeButtons(ui, asset.mode);
                syncMediaMeta();
                schedulePreview();
            };

            setModeButtons(ui, asset.mode);
        }

        updateMediaCardUI(asset);
    }

    function finalizeUploadSuccess(asset, response) {
        stopTasfaWatchdog(asset);
        asset.fid = response.fid || response.id || asset.fid;
        asset.url = response.url;
        asset.blob_url = response.url || '';
        asset.filename = response.filename || asset.filename;
        asset.mime_type = response.mime_type || asset.mime_type || '';
        asset.thumb_path = response.thumb_path || '';
        asset.preview_path = response.preview_path || '';
        asset.deletePin = response.delete_pin || '';
        asset.isUploading = false;
        asset.failed = false;
        asset.isCancelling = false;
        asset.xhrs = [];
        asset.mode = 'attachment';
        asset.displayPercent = 100;
        asset.ui.status.textContent = response.delete_pin
            ? ((isEditorMode ? 'Uploaded. Save delete PIN: ' : 'Uploaded. Save delete PIN: ') + response.delete_pin)
            : (isEditorMode ? 'Uploaded and available for inline placement' : 'Uploaded to file repository');
        if (asset.ui.progressInner) {
            if (asset.ui.progress) {
                asset.ui.progress.classList.remove('is-completing');
                asset.ui.progress.style.display = '';
            }
            asset.ui.progressInner.style.width = '100%';
            if (asset.ui.progress) {
                window.setTimeout(function() {
                    if (!asset.ui || !asset.ui.progress) return;
                    asset.ui.progress.classList.add('is-completing');
                    window.setTimeout(function() {
                        if (!asset.ui || !asset.ui.progress) return;
                        asset.ui.progress.style.display = 'none';
                        asset.ui.progress.classList.remove('is-completing');
                    }, 380);
                }, 120);
            }
        }
        asset.ui.btnInsert.disabled = false;
        asset.ui.btnInline.disabled = false;
        asset.ui.btnAttachment.disabled = !isEditorMode;
        if (asset.ui.thumbImg) {
            asset.ui.thumbImg.style.filter = 'none';
        }
        if (isEditorMode) {
            setModeButtons(asset.ui, asset.mode);
            if (asset.placeholderUrl) {
                replaceAllInEditor(asset.placeholderUrl, asset.url);
                asset.placeholderUrl = null;
            }
            var finalUrl = asset.url || (asset.fid !== null ? ('/file/download/' + asset.fid) : '');
            if (finalUrl && !editorHasUrl(finalUrl) && !editorHasUrl('/file/download/' + asset.fid)) {
                insertAssetMarkdown(asset);
            }
            asset.ui.status.textContent = response.delete_pin
                ? ('Uploaded and inserted. Save delete PIN: ' + response.delete_pin)
                : 'Uploaded and inserted into markdown';
        }
        bindAssetControls(asset);
        syncMediaMeta();
        updateFileRepoUploadButton();
        updateSubmitButtons();
        schedulePreview();
        prependFileRepoCard(asset, response);
        if (asset.blobUrl) {
            URL.revokeObjectURL(asset.blobUrl);
            asset.blobUrl = null;
        }
        isFileUploadRunning = false;
        processFileUploadQueue();
    }

    function markUploadFailure(asset, message) {
        stopTasfaWatchdog(asset);
        asset.isUploading = false;
        asset.failed = true;
        if (message === 'upload too large') {
            asset.ui.status.textContent = 'Error: File size too large';
        } else {
            asset.ui.status.textContent = message;
        }
        asset.ui.btnDelete.disabled = false;
        asset.xhrs = [];
        updateFileRepoUploadButton();
        updateSubmitButtons();
        isFileUploadRunning = false;
        processFileUploadQueue();
        updateMediaCardUI(asset);
    }

    function fileRepoPendingAssets() {
        return AssetRegistry.filter(function(asset) {
            return !asset.isExisting && asset.fid === null && !asset.isUploading && !asset.uploadId && !asset.failed;
        });
    }

    function fileRepoActiveUploads() {
        return AssetRegistry.filter(function(asset) {
            return !asset.isExisting && asset.fid === null && asset.isUploading;
        });
    }

    function hasActiveUploads() {
        return fileRepoActiveUploads().length > 0;
    }

    function updateSubmitButtons() {
        if (!submitButtons || !submitButtons.length) return;
        var locked = hasActiveUploads();
        Array.prototype.forEach.call(submitButtons, function(button) {
            button.disabled = locked;
            if (locked) {
                button.setAttribute('aria-disabled', 'true');
                button.title = 'Wait for uploads to finish';
            } else {
                button.removeAttribute('aria-disabled');
                button.removeAttribute('title');
            }
        });
        if (syncStatus && locked) syncStatus.textContent = 'Uploading files, save is locked';
    }

    function updateFileRepoUploadButton() {
        if (!isFileRepoMode || !fileRepoUploadButton) return;
        var pending = fileRepoPendingAssets().length;
        fileRepoUploadButton.disabled = pending === 0 && FileUploadQueue.length === 0;
        if (isFileUploadRunning) {
            fileRepoUploadButton.textContent = 'Uploading...';
        } else {
            fileRepoUploadButton.textContent = 'Upload queued files';
        }
        updateSubmitButtons();
    }

    function updateAssetProgress(asset) {
        if (!asset || !asset.ui || !asset.ui.progressInner) return;
        var totalTransferred = asset.confirmedBytes || 0;
        if (asset.inflightBytes) {
            for (var i = 0; i < asset.inflightBytes.length; i++) {
                totalTransferred += Number(asset.inflightBytes[i] || 0);
            }
        }
        totalTransferred = Math.max(0, Math.min(Number(asset.fileSize || 0), totalTransferred));
        var percent = asset.fileSize ? Math.min(100, Math.round((totalTransferred / asset.fileSize) * 100)) : 0;
        var previous = Number(asset.displayPercent || 0);
        if (Number.isFinite(previous)) percent = Math.max(previous, percent);
        if (asset.uploadMethod === 'tasfa' && asset.fid === null && percent >= 100) {
            percent = 99;
        }
        asset.displayPercent = percent;
        asset.ui.status.textContent = percent >= 100 ? 'Upload complete, processing...' : ('Uploading [' + percent + '%]');
        asset.ui.progressInner.style.width = percent + '%';
    }

    function resetDisplayedProgress(asset) {
        if (!asset) return;
        asset.displayPercent = 0;
        if (asset.ui && asset.ui.progressInner) {
            asset.ui.progressInner.style.width = '0%';
        }
        if (asset.ui && asset.ui.progress) {
            asset.ui.progress.classList.remove('is-completing');
            asset.ui.progress.style.display = '';
        }
    }

    function uploadFilePlain(asset, file) {
        asset.isUploading = true;
        asset.failed = false;
        resetDisplayedProgress(asset);
        asset.ui.status.textContent = 'Uploading...';
        updateFileRepoUploadButton();
        updateSubmitButtons();

        var xhr = new XMLHttpRequest();
        asset.xhrs.push(xhr);

        var formData = new FormData();
        formData.append('file', file);

        xhr.open('POST', PLAIN_UPLOAD_ENDPOINT + '?post_id=' + encodeURIComponent(window.POST_ID || 0), true);
        xhr.setRequestHeader('X-Requested-With', 'XMLHttpRequest');
        var watchdog = armXhrIdleTimeout(xhr, TASFA_CONNECT_TIMEOUT_MS);
        var uploadStartedAt = Date.now();
        var tinyProgressFloor = Math.max(1024, Math.min(file.size - 1, Math.floor(file.size * 0.01)));

        xhr.upload.onprogress = function(event) {
            if (!event.lengthComputable) return;
            var elapsed = Date.now() - uploadStartedAt;
            if (event.loaded > 0 && event.loaded <= 1 && elapsed >= 500) {
                try { xhr._tasfaTinyProgress = true; xhr.abort(); } catch (err) {}
                return;
            }
            if (event.loaded > 0 && event.loaded < tinyProgressFloor && elapsed >= 1500) {
                try { xhr._tasfaTinyProgress = true; xhr.abort(); } catch (err2) {}
                return;
            }
            watchdog.arm();
            asset.confirmedBytes = event.loaded;
            updateAssetProgress(asset);
        };

        xhr.onprogress = function() {
            watchdog.arm();
        };

        xhr.onload = function() {
            watchdog.clear();
            asset.xhrs = asset.xhrs.filter(function(x) { return x !== xhr; });
            if (xhr.status === 200) {
                var payload = null;
                try { payload = JSON.parse(xhr.responseText); } catch (e) {}
                if (payload && payload.ok) {
                    asset.fid = payload.file_id !== undefined ? payload.file_id : (payload.id !== undefined ? payload.id : asset.fid);
                    asset.url = payload.url;
                    asset.mime_type = payload.mime_type || asset.mime_type || '';
                    asset.confirmedBytes = file.size;
                    finalizeUploadSuccess(asset, payload);
                    if (isFileRepoMode) {
                        isFileUploadRunning = false;
                        processFileUploadQueue();
                        updateFileRepoUploadButton();
                    }
                    return;
                }
                if (payload && payload.error) {
                    markUploadFailure(asset, 'Upload failed [' + payload.error + ']');
                    if (isFileRepoMode) {
                        isFileUploadRunning = false;
                        processFileUploadQueue();
                        updateFileRepoUploadButton();
                    }
                    return;
                }
                markUploadFailure(asset, 'Upload failed [invalid upload response]');
                if (isFileRepoMode) {
                    isFileUploadRunning = false;
                    processFileUploadQueue();
                    updateFileRepoUploadButton();
                }
                return;
            }
            markUploadFailure(asset, 'Upload failed [' + xhr.status + ']');
            if (isFileRepoMode) {
                isFileUploadRunning = false;
                processFileUploadQueue();
                updateFileRepoUploadButton();
            }
        };

        xhr.onerror = xhr.ontimeout = function() {
            watchdog.clear();
            asset.xhrs = asset.xhrs.filter(function(x) { return x !== xhr; });
            markUploadFailure(asset, 'Upload failed [network]');
            if (isFileRepoMode) {
                isFileUploadRunning = false;
                processFileUploadQueue();
                updateFileRepoUploadButton();
            }
        };

        xhr.onabort = function() {
            watchdog.clear();
            asset.xhrs = asset.xhrs.filter(function(x) { return x !== xhr; });
            markUploadFailure(asset, 'Upload failed [' + (xhr._tasfaIdleTimeout || xhr._tasfaTinyProgress ? 'timeout' : 'abort') + ']');
            if (isFileRepoMode) {
                isFileUploadRunning = false;
                processFileUploadQueue();
                updateFileRepoUploadButton();
            }
        };

        xhr.send(formData);
    }

    function encodeFormBody(values) {
        return Object.keys(values).map(function(k) {
            return encodeURIComponent(k) + '=' + encodeURIComponent(values[k]);
        }).join('&');
    }

    function hexToBytes(hex) {
        if (!hex || hex.length % 2 !== 0) return null;
        var out = new Uint8Array(hex.length / 2);
        for (var i = 0; i < out.length; i++) {
            var value = parseInt(hex.substr(i * 2, 2), 16);
            if (!isFinite(value)) return null;
            out[i] = value;
        }
        return out;
    }

    function bytesToHex(bytes) {
        var out = '';
        for (var i = 0; i < bytes.length; i++) {
            out += bytes[i].toString(16).padStart(2, '0');
        }
        return out;
    }

    function deriveStreamIv(seedBytes, chunkIndex) {
        var iv = new Uint8Array(seedBytes);
        iv[8] ^= (chunkIndex >> 24) & 0xff;
        iv[9] ^= (chunkIndex >> 16) & 0xff;
        iv[10] ^= (chunkIndex >> 8) & 0xff;
        iv[11] ^= chunkIndex & 0xff;
        return iv;
    }

    function firstEightBytesToBigInt(bytes) {
        var value = 0n;
        for (var i = 0; i < 8 && i < bytes.length; i++) {
            value = (value << 8n) + BigInt(bytes[i]);
        }
        return value;
    }

    function positiveMod(value, modulus) {
        if (!modulus || modulus <= 0n) return 0n;
        var out = value % modulus;
        return out < 0n ? out + modulus : out;
    }

    function uploadModulus(asset) {
        var n = Number(asset.modulusM || 0);
        if (!isFinite(n) || n <= 0) return 1n;
        return BigInt(Math.floor(n));
    }

    function ensureHtpGroup(asset, file, groupIndex) {
        if (!window.crypto || !crypto.subtle || typeof BigInt === 'undefined') {
            return Promise.resolve(null);
        }
        asset.htpGroups = asset.htpGroups || {};
        asset.htpReadyGroups = asset.htpReadyGroups || {};
        if (asset.htpReadyGroups[groupIndex]) return Promise.resolve(asset.htpReadyGroups[groupIndex]);
        if (asset.htpGroups[groupIndex]) return asset.htpGroups[groupIndex];

        asset.htpGroups[groupIndex] = (async function() {
            var modulus = uploadModulus(asset);
            var groupStart = groupIndex * 6;
            var groupEnd = Math.min(groupStart + 6, asset.totalChunks);
            var rawScalars = [0n, 0n, 0n, 0n, 0n, 0n];
            var scalars = [0n, 0n, 0n, 0n, 0n, 0n];
            var tags = ['', '', '', '', '', ''];
            for (var ci = groupStart; ci < groupEnd; ci++) {
                var start = ci * asset.chunkSize;
                var end = Math.min(start + asset.chunkSize, file.size);
                var data = await file.slice(start, end).arrayBuffer();
                var digest = new Uint8Array(await crypto.subtle.digest('SHA-512', data));
                tags[ci - groupStart] = bytesToHex(digest);
                rawScalars[ci - groupStart] = positiveMod(firstEightBytesToBigInt(digest), modulus);
                scalars[ci - groupStart] = rawScalars[ci - groupStart];
            }

            var l1 = positiveMod(scalars[0] + scalars[1] + scalars[2], modulus);
            var l2 = positiveMod(scalars[2] + scalars[3] + scalars[4], modulus);
            var l3 = positiveMod(scalars[4] + scalars[5] + scalars[0], modulus);
            if (groupStart + 3 < asset.totalChunks) {
                scalars[3] = positiveMod(scalars[3] + (l1 - l2), modulus);
            }
            if (groupStart + 5 < asset.totalChunks) {
                scalars[5] = positiveMod(scalars[5] + (l1 - l3), modulus);
            }

            var result = {};
            for (var cj = groupStart; cj < groupEnd; cj++) {
                var slot = cj - groupStart;
                result[cj] = {
                    hashTag: tags[slot],
                    rawScalar: rawScalars[slot].toString(10),
                    magicScalar: scalars[slot].toString(10)
                };
            }
            asset.htpReadyGroups[groupIndex] = result;
            return result;
        })().catch(function() {
            return null;
        });
        return asset.htpGroups[groupIndex];
    }

    function peekHtpHeaders(asset, chunkIndex) {
        if (!asset || !asset.htpReadyGroups) return null;
        var group = asset.htpReadyGroups[Math.floor(chunkIndex / 6)];
        return group && group[chunkIndex] ? group[chunkIndex] : null;
    }

    function getHtpHeaders(asset, file, chunkIndex) {
        return ensureHtpGroup(asset, file, Math.floor(chunkIndex / 6)).then(function(group) {
            return group && group[chunkIndex] ? group[chunkIndex] : null;
        });
    }

    function importUploadStreamKey(asset) {
        if (asset.streamCryptoKey && asset._lastStreamKeyHex === asset.streamKeyHex) return Promise.resolve(asset.streamCryptoKey);
        if (!window.crypto || !crypto.subtle) return Promise.reject(new Error('crypto unavailable'));
        var keyBytes = hexToBytes(asset.streamKeyHex || '');
        if (!keyBytes || keyBytes.length !== 32) return Promise.reject(new Error('stream key unavailable'));
        return crypto.subtle.importKey('raw', keyBytes, { name: 'AES-GCM' }, false, ['encrypt']).then(function(key) {
            asset.streamCryptoKey = key;
            asset._lastStreamKeyHex = asset.streamKeyHex;
            return key;
        });
    }

    function tasfaFallbackPrefetchBudget(asset) {
        var chunkSize = Math.max(1, Number(asset.chunkSize || UPLOAD_CHUNK_SIZE));
        var chunks = asset.transferProfile && asset.transferProfile.highPerformance ? 2 : 1;
        return Math.min(TASFA_FALLBACK_PREFETCH_MAX_BYTES, chunkSize * chunks);
    }

    function releaseEncryptedChunk(asset, chunkIndex) {
        if (!asset || !asset.encryptedCache) return;
        if (asset.encryptedCacheSizes && asset.encryptedCacheSizes[chunkIndex]) {
            asset.encryptedCacheBytes = Math.max(0, (asset.encryptedCacheBytes || 0) - asset.encryptedCacheSizes[chunkIndex]);
            delete asset.encryptedCacheSizes[chunkIndex];
        }
        delete asset.encryptedCache[chunkIndex];
    }

    function startTasfaUpload(asset, file) {
        asset.isUploading = true;
        asset.failed = false;
        asset.isCancelling = false;
        asset.isNetworkPaused = false;
        asset.xhrs = asset.xhrs || [];
        asset.fallbackChain = Promise.resolve();
        asset.confirmedBytes = 0;
        asset.targetParallel = asset.targetParallel || UPLOAD_DEFAULT_PARALLEL;
        startTasfaWatchdog(asset);
        resetDisplayedProgress(asset);
        setTasfaStatus(asset, 'preparing');
        updateFileRepoUploadButton();
        updateSubmitButtons();

        if (asset.uploadId && asset.uploadToken) {
            resumeTasfaUpload(asset, file);
            return;
        }

        function doInit(retries) {
            retries = retries || 0;
            var controller = new AbortController();
            var timeoutId = setTimeout(function() { controller.abort(); }, TASFA_CONNECT_TIMEOUT_MS);
            var chunkCount = Math.max(1, Math.ceil(file.size / asset.chunkSize));
            var values = tasfaLinkFormValues(asset, {
                filename: file.name,
                total_size: String(file.size),
                chunk_count: String(chunkCount),
                chunk_size: String(asset.chunkSize),
                post_id: String(window.POST_ID || 0),
                session_id: asset.client_uuid || ''
            });
            fetch(UPLOAD_INIT_ENDPOINT, {
                method: 'POST',
                headers: { 'Content-Type': 'application/x-www-form-urlencoded', 'Accept': 'application/json', 'X-Requested-With': 'XMLHttpRequest' },
                body: encodeFormBody(values),
                signal: controller.signal
            }).then(function(response) {
                clearTimeout(timeoutId);
                if (response.status === 429 && retries < 30) {
                    return response.json().then(function(payload) {
                        asset.ui.status.textContent = (payload && payload.error) || 'Wait a second...';
                        var delay = (payload.retry_after || 5) * 1000;
                        setTimeout(function() { doInit(retries + 1); }, delay);
                    }).catch(function() {
                        setTimeout(function() { doInit(retries + 1); }, 5000);
                    });
                }
                if (!response.ok) {
                    return response.json().then(function(payload) {
                        throw new Error((payload && payload.error) || ('init:' + response.status));
                    }).catch(function(err) {
                        if (err && err.message && err.message.indexOf('init:') === 0) throw err;
                        throw new Error('init:' + response.status);
                    });
                }
                return response.json();
            }).then(function(payload) {
                if (!payload || payload.queued) return;
                if (payload && payload.already_done) {
                    asset.fid = payload.file_id;
                    asset.url = payload.url || ('/file/download/' + String(payload.file_id || ''));
                    finalizeUploadSuccess(asset, payload);
                    return;
                }
                if (!payload || payload.ok === false || !payload.upload_id || !payload.upload_token) {
                    throw new Error((payload && payload.error) || 'upload init failed');
                }
                asset.uploadId = payload.upload_id;
                asset.uploadToken = payload.upload_token;
                asset.streamKeyHex = payload.stream_key_hex || '';
                asset.streamIvSeedHex = payload.stream_iv_seed_hex || '';
                asset.modulusM = payload.modulus_M || 1;
                asset.chunkSize = Number(payload.chunk_size || asset.chunkSize || UPLOAD_CHUNK_SIZE);
                asset.totalChunks = Math.max(1, Math.ceil(file.size / asset.chunkSize));
                asset.maxParallel = Math.max(1, Math.min(Number(payload.max_parallel_chunks) || UPLOAD_DEFAULT_PARALLEL, asset.totalChunks));
                asset.targetParallel = Math.max(1, Math.min(Number(payload.current_parallel_chunks || payload.initial_parallel_chunks) || asset.maxParallel, asset.maxParallel));
                asset.dispatchPacingMs = Math.max(0, Number(payload.dispatch_pacing_ms || 0));
                applyTasfaTransferProfile(asset);
                asset.inflightBytes = new Array(asset.totalChunks).fill(0);
                asset.retryCounts = new Array(asset.totalChunks).fill(0);
                asset.completedChunks = 0;
                tasfaTrace(asset, 'init', {
                    chunkSize: asset.chunkSize,
                    totalChunks: asset.totalChunks,
                    serverCurrent: asset.targetParallel,
                    serverMax: asset.maxParallel,
                    pacing: asset.dispatchPacingMs
                });
                asset.ui.status.textContent = 'Uploading...';
                runSimpleChunkUpload(asset, file);
            }).catch(function(error) {
                clearTimeout(timeoutId);
                if (error && error.message && error.message.indexOf('init:429') !== -1) return;
                if (error && error.name === 'AbortError') {
                    if (retries < 20) {
                        setTasfaStatus(asset, 'retrying');
                        setTimeout(function() { doInit(retries + 1); }, 2000);
                        return;
                    }
                }
                var message = error && error.message ? error.message : 'Upload failed';
                markUploadFailure(asset, message);
            });
        }
        doInit();
    }

    function resumeTasfaUpload(asset, file, attempt) {
        attempt = attempt || 1;
        if (asset.isCancelling) return;
        asset.isUploading = true;
        asset.isNetworkPaused = false;
        startTasfaWatchdog(asset);
        touchTasfaActivity(asset);
        setTasfaStatus(asset, 'checking');
        var body = 'upload_id=' + encodeURIComponent(asset.uploadId) +
            '&upload_token=' + encodeURIComponent(asset.uploadToken);
        var controller = new AbortController();
        var timeoutId = setTimeout(function() { controller.abort(); }, TASFA_CONNECT_TIMEOUT_MS);
        fetch(UPLOAD_STATUS_ENDPOINT, {
            method: 'POST',
            headers: { 'Content-Type': 'application/x-www-form-urlencoded', 'Accept': 'application/json', 'X-Requested-With': 'XMLHttpRequest' },
            body: body,
            signal: controller.signal
        }).then(function(response) {
            clearTimeout(timeoutId);
            if (!response.ok) throw new Error('status:' + response.status);
            return response.json();
        }).then(function(payload) {
            if (!payload || payload.ok === false) throw new Error((payload && payload.error) || 'resume failed');
            var bitmap = payload.received_bitmap || '';
            var newChunkSize = Number(payload.chunk_size || asset.chunkSize || UPLOAD_CHUNK_SIZE);
            if (newChunkSize !== asset.chunkSize) {
                asset.encryptedCache = null;
                asset.htpGroups = null;
                asset._aadCache = null;
            }
            asset.chunkSize = newChunkSize;
            asset.totalChunks = Math.max(1, Math.ceil(file.size / asset.chunkSize));
            var newStreamKeyHex = payload.stream_key_hex || asset.streamKeyHex || '';
            if (newStreamKeyHex !== asset.streamKeyHex) {
                asset.streamCryptoKey = null;
                asset._lastStreamKeyHex = null;
            }
            asset.streamKeyHex = newStreamKeyHex;
            var newStreamIvSeedHex = payload.stream_iv_seed_hex || asset.streamIvSeedHex || '';
            if (newStreamIvSeedHex !== asset.streamIvSeedHex) {
                asset._streamIvSeedBytes = null;
            }
            asset.streamIvSeedHex = newStreamIvSeedHex;
            asset.modulusM = payload.modulus_M || asset.modulusM || 1;
            asset.maxParallel = Math.max(1, Math.min(Number(payload.max_parallel_chunks) || UPLOAD_DEFAULT_PARALLEL, asset.totalChunks));
            asset.targetParallel = Math.max(1, Math.min(Number(payload.current_parallel_chunks || payload.initial_parallel_chunks) || asset.maxParallel, asset.maxParallel));
            asset.dispatchPacingMs = Math.max(0, Number(payload.dispatch_pacing_ms || 0));
            applyTasfaTransferProfile(asset);
            asset.inflightBytes = new Array(asset.totalChunks).fill(0);
            asset.retryCounts = new Array(asset.totalChunks).fill(0);
            asset.fallbackChain = Promise.resolve();
            asset.completedChunks = 0;
            asset.confirmedBytes = 0;
            var pending = [];
            for (var i = 0; i < asset.totalChunks; i++) {
                if (i < bitmap.length && bitmap[i] === '1') {
                    asset.completedChunks += 1;
                    asset.confirmedBytes += Math.min(asset.chunkSize, file.size - (i * asset.chunkSize));
                } else {
                    pending.push(i);
                }
            }
            asset.pendingChunks = pending;
            tasfaTrace(asset, 'resume-status', {
                chunkSize: asset.chunkSize,
                totalChunks: asset.totalChunks,
                pending: pending.length,
                serverCurrent: asset.targetParallel,
                serverMax: asset.maxParallel,
                pacing: asset.dispatchPacingMs
            });
            asset.ui.status.textContent = 'Uploading...';
            runSimpleChunkUpload(asset, file);
        }).catch(function(error) {
            clearTimeout(timeoutId);
            if (attempt < 30) {
                setTasfaStatus(asset, 'retrying');
                setTimeout(function() { resumeTasfaUpload(asset, file, attempt + 1); }, Math.min(30000, 1000 * attempt));
                return;
            }
            var message = error && error.message ? error.message : 'Resume failed';
            markUploadFailure(asset, message);
        });
    }

    function runSimpleChunkUpload(asset, file) {
        var runId = (asset.uploadRunId || 0) + 1;
        asset.uploadRunId = runId;
        asset.isUploading = true;
        asset.isNetworkPaused = false;
        touchTasfaActivity(asset);
        var pending = asset.pendingChunks;
        if (!pending || !pending.length) {
            pending = [];
            for (var i = 0; i < asset.totalChunks; i++) pending.push(i);
        }
        asset.pendingChunks = null;
        asset.targetParallel = Math.max(1, Math.min(asset.targetParallel || asset.maxParallel || UPLOAD_DEFAULT_PARALLEL, asset.maxParallel || UPLOAD_DEFAULT_PARALLEL));
        asset.activeChunkPosts = 0;
        var poolFailed = false;

        function postChunk(chunkIndex) {
            ensureHtpGroup(asset, file, Math.floor(chunkIndex / 6)).catch(function() {});
            return new Promise(function(resolve, reject) {
                var start = chunkIndex * asset.chunkSize;
                var end = Math.min(start + asset.chunkSize, file.size);
                var blob = file.slice(start, end);
                var size = end - start;
                var xhr = new XMLHttpRequest();
                var htp = peekHtpHeaders(asset, chunkIndex);
                xhr._tasfaChunkIndex = chunkIndex;
                asset.xhrs.push(xhr);
                asset.inflightBytes[chunkIndex] = 0;

                xhr.open('POST', UPLOAD_ENDPOINT, true);
                xhr.setRequestHeader('Accept', 'application/json');
                xhr.setRequestHeader('X-Requested-With', 'XMLHttpRequest');
                xhr.setRequestHeader('X-TASFA-Upload-ID', asset.uploadId);
	                xhr.setRequestHeader('X-TASFA-Upload-Token', asset.uploadToken);
	                xhr.setRequestHeader('X-TASFA-Chunk-Index', String(chunkIndex));
	                if (htp && htp.hashTag) xhr.setRequestHeader('X-TASFA-Hash-Tag', htp.hashTag);
	                if (htp && htp.rawScalar) xhr.setRequestHeader('X-TASFA-Raw-Scalar', htp.rawScalar);
	                if (htp && htp.magicScalar) xhr.setRequestHeader('X-TASFA-Magic-Scalar', htp.magicScalar);
	                var stats = ensureTasfaStats(asset);
	                var rttHint = Math.round((stats.lastChunkMs || stats.ewmaMs || 0));
	                if (rttHint > 0) xhr.setRequestHeader('X-TASFA-Chunk-RTT', String(rttHint));

                var watchdog = armXhrIdleTimeout(xhr, TASFA_CONNECT_TIMEOUT_MS);
                var chunkStartedAt = Date.now();
                var tinyProgressFloor = Math.max(1024, Math.min(size - 1, Math.floor(size * 0.01)));

                xhr.upload.onprogress = function(event) {
                    if (!event.lengthComputable) return;
                    var elapsed = Date.now() - chunkStartedAt;
                    if (event.loaded > 0 && event.loaded <= 1 && elapsed >= 500) {
                        try { xhr._tasfaTinyProgress = true; xhr.abort(); } catch (err) {}
                        return;
                    }
                    if (event.loaded > 0 && event.loaded < tinyProgressFloor && elapsed >= 1500) {
                        try { xhr._tasfaTinyProgress = true; xhr.abort(); } catch (err2) {}
                        return;
                    }
                    watchdog.arm();
                    touchTasfaActivity(asset);
                    recordTasfaProgress(asset);
                    asset.inflightBytes[chunkIndex] = event.loaded;
                    updateAssetProgress(asset);
                };

                xhr.onprogress = function() {
                    watchdog.arm();
                };

                xhr.onload = function() {
                    watchdog.clear();
                    touchTasfaActivity(asset);
                    asset.xhrs = asset.xhrs.filter(function(x) { return x !== xhr; });
                    asset.inflightBytes[chunkIndex] = 0;
                    if (xhr.status === 200 || xhr.status === 204) {
                        asset.confirmedBytes += size;
                        resolve({ ok: true, chunkIndex: chunkIndex });
                    } else if (xhr.status === 429) {
                        var delay = 3000;
                        var waitMessage = 'Wait a second...';
                        try {
                            var resp = JSON.parse(xhr.responseText);
                            if (resp.retry_after) delay = resp.retry_after * 1000;
                            if (resp.error) waitMessage = resp.error;
                        } catch(e) {}
                        asset.ui.status.textContent = waitMessage;
                        resolve({ retry: true, chunkIndex: chunkIndex, delay: delay });
                    } else {
                        reject(new Error('status:' + xhr.status));
                    }
                };

                xhr.onerror = function() {
                    watchdog.clear();
                    asset.xhrs = asset.xhrs.filter(function(x) { return x !== xhr; });
                    asset.inflightBytes[chunkIndex] = 0;
                    reject(new Error('network'));
                };

                xhr.ontimeout = function() {
                    watchdog.clear();
                    asset.xhrs = asset.xhrs.filter(function(x) { return x !== xhr; });
                    asset.inflightBytes[chunkIndex] = 0;
                    reject(new Error('timeout'));
                };

                xhr.onabort = function() {
                    watchdog.clear();
                    asset.xhrs = asset.xhrs.filter(function(x) { return x !== xhr; });
                    asset.inflightBytes[chunkIndex] = 0;
                    reject(new Error(xhr._tasfaIdleTimeout || xhr._tasfaTinyProgress ? 'timeout' : 'abort'));
                };

                xhr.send(blob);
            });
        }

        function encryptChunk(chunkIndex, force) {
            if (asset.encryptedCache && asset.encryptedCache[chunkIndex]) {
                return asset.encryptedCache[chunkIndex];
            }
            var start = chunkIndex * asset.chunkSize;
            var end = Math.min(start + asset.chunkSize, file.size);
            var size = end - start;
            var budget = tasfaFallbackPrefetchBudget(asset);
            if (!force && (asset.encryptedCacheBytes || 0) + size > budget) {
                return Promise.reject(new Error('prefetch-budget'));
            }
            if (!asset.encryptedCache) asset.encryptedCache = {};
            if (!asset.encryptedCacheSizes) asset.encryptedCacheSizes = {};
            asset.encryptedCacheBytes = (asset.encryptedCacheBytes || 0) + size;
            asset.encryptedCacheSizes[chunkIndex] = size;
            ensureHtpGroup(asset, file, Math.floor(chunkIndex / 6)).catch(function() {});
            var promise = importUploadStreamKey(asset).then(function(key) {
                var htp = peekHtpHeaders(asset, chunkIndex);
                var blob = file.slice(start, end);
                if (!asset._streamIvSeedBytes) {
                    asset._streamIvSeedBytes = hexToBytes(asset.streamIvSeedHex || '');
                }
                var seed = asset._streamIvSeedBytes;
                if (!seed || seed.length !== 12) throw new Error('stream iv unavailable');
                var iv = deriveStreamIv(seed, chunkIndex);
                if (!asset._aadCache) asset._aadCache = {};
                if (!asset._aadCache[chunkIndex]) {
                    asset._aadCache[chunkIndex] = new TextEncoder().encode((asset.uploadId || '') + ':' + String(chunkIndex));
                }
                var aad = asset._aadCache[chunkIndex];
                return blob.arrayBuffer().then(function(plain) {
                    return crypto.subtle.encrypt({
                        name: 'AES-GCM',
                        iv: iv,
                        additionalData: aad,
                        tagLength: 128
                    }, key, plain);
                }).then(function(cipher) {
                    return { cipher: cipher, htp: htp, size: size };
                }).catch(function(err) {
                    releaseEncryptedChunk(asset, chunkIndex);
                    throw err;
                });
            });
            asset.encryptedCache[chunkIndex] = promise;
            return promise;
        }

        function postEncryptedChunk(chunkIndex) {
            return encryptChunk(chunkIndex, true).then(function(enc) {
                return new Promise(function(resolve, reject) {
                    var xhr = new XMLHttpRequest();
                    xhr._tasfaChunkIndex = chunkIndex;
                    asset.xhrs.push(xhr);
                    asset.inflightBytes[chunkIndex] = 0;
                    xhr.open('POST', UPLOAD_ENDPOINT, true);
                    xhr.setRequestHeader('Accept', 'application/json');
                    xhr.setRequestHeader('Content-Type', 'application/octet-stream');
                    xhr.setRequestHeader('X-Requested-With', 'XMLHttpRequest');
                    xhr.setRequestHeader('X-TASFA-Upload-ID', asset.uploadId);
	                    xhr.setRequestHeader('X-TASFA-Upload-Token', asset.uploadToken);
	                    xhr.setRequestHeader('X-TASFA-Chunk-Index', String(chunkIndex));
	                    xhr.setRequestHeader('X-TASFA-Stream-Mode', 'aes-256-gcm');
	                    if (enc.htp && enc.htp.hashTag) xhr.setRequestHeader('X-TASFA-Hash-Tag', enc.htp.hashTag);
	                    if (enc.htp && enc.htp.rawScalar) xhr.setRequestHeader('X-TASFA-Raw-Scalar', enc.htp.rawScalar);
	                    if (enc.htp && enc.htp.magicScalar) xhr.setRequestHeader('X-TASFA-Magic-Scalar', enc.htp.magicScalar);
	                    var stats = ensureTasfaStats(asset);
	                    var rttHint = Math.round((stats.lastChunkMs || stats.ewmaMs || 0));
	                    if (rttHint > 0) xhr.setRequestHeader('X-TASFA-Chunk-RTT', String(rttHint));
	                    var watchdog = armXhrIdleTimeout(xhr, TASFA_CONNECT_TIMEOUT_MS);
	                    var chunkStartedAt = Date.now();
	                    var tinyProgressFloor = Math.max(1024, Math.min(enc.size - 1, Math.floor(enc.size * 0.01)));
                    xhr.upload.onprogress = function(event) {
                        if (!event.lengthComputable) return;
                        var elapsed = Date.now() - chunkStartedAt;
                        if (event.loaded > 0 && event.loaded <= 1 && elapsed >= 500) {
                            try { xhr._tasfaTinyProgress = true; xhr.abort(); } catch (err) {}
                            return;
                        }
                        if (event.loaded > 0 && event.loaded < tinyProgressFloor && elapsed >= 1500) {
                            try { xhr._tasfaTinyProgress = true; xhr.abort(); } catch (err2) {}
                            return;
                        }
                        watchdog.arm();
                        touchTasfaActivity(asset);
                        recordTasfaProgress(asset);
                        asset.inflightBytes[chunkIndex] = event.loaded;
                        updateAssetProgress(asset);
                    };
                    xhr.onprogress = function() {
                        watchdog.arm();
                    };
                    xhr.onload = function() {
                        watchdog.clear();
                        touchTasfaActivity(asset);
                        asset.xhrs = asset.xhrs.filter(function(x) { return x !== xhr; });
                        asset.inflightBytes[chunkIndex] = 0;
                        if (xhr.status === 200 || xhr.status === 204) {
                            asset.confirmedBytes += enc.size;
                            releaseEncryptedChunk(asset, chunkIndex);
                            resolve({ ok: true, chunkIndex: chunkIndex });
                        } else if (xhr.status === 429) {
                            var delay = 3000;
                            try {
                                var resp = JSON.parse(xhr.responseText);
                                if (resp.retry_after) delay = resp.retry_after * 1000;
                            } catch(e) {}
                            resolve({ retry: true, chunkIndex: chunkIndex, delay: delay });
                        } else {
                            releaseEncryptedChunk(asset, chunkIndex);
                            reject(new Error('fallback:' + xhr.status));
                        }
                    };
                    xhr.onerror = function() {
                        watchdog.clear();
                        asset.xhrs = asset.xhrs.filter(function(x) { return x !== xhr; });
                        asset.inflightBytes[chunkIndex] = 0;
                        releaseEncryptedChunk(asset, chunkIndex);
                        reject(new Error('fallback:network'));
                    };
                    xhr.ontimeout = function() {
                        watchdog.clear();
                        asset.xhrs = asset.xhrs.filter(function(x) { return x !== xhr; });
                        asset.inflightBytes[chunkIndex] = 0;
                        releaseEncryptedChunk(asset, chunkIndex);
                        reject(new Error('fallback:timeout'));
                    };
                    xhr.onabort = function() {
                        watchdog.clear();
                        asset.xhrs = asset.xhrs.filter(function(x) { return x !== xhr; });
                        asset.inflightBytes[chunkIndex] = 0;
                        releaseEncryptedChunk(asset, chunkIndex);
                        reject(new Error(xhr._tasfaIdleTimeout || xhr._tasfaTinyProgress ? 'fallback:timeout' : 'fallback:abort'));
                    };
                    xhr.send(enc.cipher);
                });
            });
        }

        function prefetchEncrypted(count) {
            for (var i = 0; i < pending.length && count > 0; i++) {
                var ci = pending[i];
                if (!asset.encryptedCache || !asset.encryptedCache[ci]) {
                    encryptChunk(ci, false).catch(function() {});
                    count--;
                }
            }
        }

        function prefetchHtpGroups(count) {
            for (var i = 0; i < pending.length && count > 0; i++) {
                var ci = pending[i];
                var gi = Math.floor(ci / 6);
                if (!asset.htpGroups || !asset.htpGroups[gi]) {
                    ensureHtpGroup(asset, file, gi);
                    count--;
                }
            }
        }

        function postEncryptedChunkSerial(chunkIndex) {
            asset.fallbackChain = (asset.fallbackChain || Promise.resolve()).catch(function() {}).then(function() {
                setTasfaStatus(asset, 'active');
                return postEncryptedChunk(chunkIndex);
            });
            return asset.fallbackChain;
        }

        function worker() {
            return new Promise(function(resolve) {
                function next() {
                    if (runId !== asset.uploadRunId || poolFailed || asset.isCancelling || pending.length === 0) {
                        resolve();
                        return;
                    }
                    if (asset.isNetworkPaused) {
                        setTimeout(next, 1000);
                        return;
                    }
                    if ((asset.activeChunkPosts || 0) >= (asset.targetParallel || 1)) {
                        setTimeout(next, 25);
                        return;
                    }
	                    var chunkIndex = pending.shift();
	                    var prefetchCount = Math.max(4, (asset.targetParallel || 1) - (asset.activeChunkPosts || 0));
	                    prefetchHtpGroups(prefetchCount);
	                    var startedAt = Date.now();
	                    var predictedRule = predictUploadRule(asset, chunkIndex);
	                    applyPredictedUploadRule(asset, predictedRule);
	                    if (predictedRule !== 'normal') prefetchEncrypted(prefetchCount);
	                    asset.activeChunkPosts = (asset.activeChunkPosts || 0) + 1;
                        tasfaTrace(asset, 'chunk-start', { chunkIndex: chunkIndex, mode: predictedRule });
                    function runPost() {
                    if (predictedRule === 'fallback') setTasfaStatus(asset, 'active');
                    var request = predictedRule === 'fallback' ? postEncryptedChunk(chunkIndex) : postChunk(chunkIndex);
                    request.then(function(result) {
                        asset.activeChunkPosts = Math.max(0, (asset.activeChunkPosts || 1) - 1);
                        if (runId !== asset.uploadRunId) { resolve(); return; }
                        if (result && result.retry) {
                            asset.retryCounts[chunkIndex] = (asset.retryCounts[chunkIndex] || 0) + 1;
                            recordTasfaFailure(asset, 'busy');
                            tasfaTrace(asset, 'chunk-retry', { chunkIndex: chunkIndex, mode: predictedRule, delay: result.delay || 3000 });
                            if (asset.retryCounts[chunkIndex] < 20) {
                                setTimeout(function() {
                                    pending.push(chunkIndex);
                                    next();
                                }, result.delay || 3000);
                                return;
                            }
                            postEncryptedChunk(chunkIndex).then(function() {
                                if (runId !== asset.uploadRunId) { resolve(); return; }
                                var retryBytes = Math.min(asset.chunkSize, file.size - (chunkIndex * asset.chunkSize));
                                var retryElapsedMs = Date.now() - startedAt;
                                tasfaTrace(asset, 'chunk-success', {
                                    chunkIndex: chunkIndex,
                                    mode: 'fallback-after-busy',
                                    durationMs: retryElapsedMs,
                                    mbps: retryElapsedMs > 0 ? (((retryBytes * 8) / retryElapsedMs / 1000).toFixed(2)) : '0'
                                });
                                recordTasfaSuccess(asset, retryBytes, retryElapsedMs);
                                asset.completedChunks += 1;
                                updateAssetProgress(asset);
                                next();
                            }).catch(function() {
                                if (runId !== asset.uploadRunId) { resolve(); return; }
                                asset.retryCounts[chunkIndex] = (asset.retryCounts[chunkIndex] || 0) + 1;
                                if (asset.retryCounts[chunkIndex] < 40) {
                                    pending.push(chunkIndex);
                                    setTimeout(next, Math.min(30000, 500 * asset.retryCounts[chunkIndex]));
                                } else {
                                    poolFailed = true;
                                    resolve();
                                }
                            });
                            return;
                        }
                        var chunkBytes = Math.min(asset.chunkSize, file.size - (chunkIndex * asset.chunkSize));
                        var elapsedMs = Date.now() - startedAt;
                        tasfaTrace(asset, 'chunk-success', {
                            chunkIndex: chunkIndex,
                            mode: predictedRule,
                            durationMs: elapsedMs,
                            mbps: elapsedMs > 0 ? (((chunkBytes * 8) / elapsedMs / 1000).toFixed(2)) : '0'
                        });
                        recordTasfaSuccess(asset, chunkBytes, elapsedMs);
                        asset.completedChunks += 1;
                        updateAssetProgress(asset);
                        next();
                    }).catch(function(err) {
                        asset.activeChunkPosts = Math.max(0, (asset.activeChunkPosts || 1) - 1);
                        if (runId !== asset.uploadRunId) { resolve(); return; }
                        var msg = err && err.message ? err.message : 'network';
                        if ((msg === 'abort' || msg === 'fallback:abort') && (asset.isNetworkPaused || asset.isCancelling)) {
                            pending.push(chunkIndex);
                            resolve();
                            return;
                        }
                        recordTasfaFailure(asset, msg.indexOf('timeout') !== -1 ? 'timeout' : 'network');
                        tasfaTrace(asset, 'chunk-error', { chunkIndex: chunkIndex, mode: predictedRule, message: msg });
                        asset.retryCounts[chunkIndex] = (asset.retryCounts[chunkIndex] || 0) + 1;
                        if (asset.retryCounts[chunkIndex] < 12) {
                            pending.push(chunkIndex);
                            next();
                        } else {
                            postEncryptedChunk(chunkIndex).then(function() {
                                if (runId !== asset.uploadRunId) { resolve(); return; }
                                var fallbackBytes = Math.min(asset.chunkSize, file.size - (chunkIndex * asset.chunkSize));
                                var fallbackElapsedMs = Date.now() - startedAt;
                                tasfaTrace(asset, 'chunk-success', {
                                    chunkIndex: chunkIndex,
                                    mode: 'fallback-after-error',
                                    durationMs: fallbackElapsedMs,
                                    mbps: fallbackElapsedMs > 0 ? (((fallbackBytes * 8) / fallbackElapsedMs / 1000).toFixed(2)) : '0'
                                });
                                recordTasfaSuccess(asset, fallbackBytes, fallbackElapsedMs);
                                asset.completedChunks += 1;
                                updateAssetProgress(asset);
                                next();
                            }).catch(function() {
                                if (runId !== asset.uploadRunId) { resolve(); return; }
                                asset.retryCounts[chunkIndex] = (asset.retryCounts[chunkIndex] || 0) + 1;
                                if (asset.retryCounts[chunkIndex] < 40) {
                                    pending.push(chunkIndex);
                                    setTimeout(next, Math.min(30000, 500 * asset.retryCounts[chunkIndex]));
                                } else {
                                    poolFailed = true;
                                    resolve();
                                }
                            });
                        }
                    });
                    }
                    if (asset.dispatchPacingMs > 0) setTimeout(runPost, asset.dispatchPacingMs);
                    else runPost();
                }
                next();
            });
        }

        var workers = [];
        for (var i = 0; i < asset.maxParallel; i++) {
            workers.push(worker());
        }
        Promise.all(workers).then(function() {
            if (runId !== asset.uploadRunId || asset.isCancelling) return;
            verifyAllChunksBeforeComplete(asset, asset.file);
        });
    }

    function verifyAllChunksBeforeComplete(asset, file) {
        asset.serverVerifyRounds = (asset.serverVerifyRounds || 0) + 1;
        if (asset.serverVerifyRounds > 50) {
            markUploadFailure(asset, 'Upload failed [too many verify rounds]');
            return;
        }
        setTasfaStatus(asset, 'verifying');
        var body = 'upload_id=' + encodeURIComponent(asset.uploadId) +
            '&upload_token=' + encodeURIComponent(asset.uploadToken);
        var controller = new AbortController();
        var timeoutId = setTimeout(function() { controller.abort(); }, TASFA_CONNECT_TIMEOUT_MS);
        fetch(UPLOAD_STATUS_ENDPOINT, {
            method: 'POST',
            headers: { 'Content-Type': 'application/x-www-form-urlencoded', 'Accept': 'application/json', 'X-Requested-With': 'XMLHttpRequest' },
            body: body,
            signal: controller.signal
        }).then(function(response) {
            clearTimeout(timeoutId);
            if (!response.ok) throw new Error('status:' + response.status);
            return response.json();
        }).then(function(payload) {
            if (!payload || payload.ok === false) throw new Error((payload && payload.error) || 'verify failed');
            if (payload.chunk_size && Number(payload.chunk_size) > 0) {
                asset.chunkSize = Number(payload.chunk_size);
                asset.totalChunks = Number(payload.chunk_count || asset.totalChunks || 0);
            }
            var bitmap = payload.received_bitmap || '';
            var pending = [];
            var pendingByStatus = [];
            if (payload.missing_vertices && Array.isArray(payload.missing_vertices)) {
                pendingByStatus = payload.missing_vertices
                    .map(function(v) {
                        if (!v) return null;
                        var ci = Number(v.chunk_index);
                        if (!Number.isFinite(ci) || ci < 0) return null;
                        return {
                            chunkIndex: ci,
                            groupIndex: Number(v.group_index),
                            vertexIndex: Number(v.vertex_index)
                        };
                    })
                    .filter(function(v) { return v && v.chunkIndex < asset.totalChunks; });
            }
            asset.confirmedBytes = 0;
            asset.completedChunks = 0;
            for (var i = 0; i < asset.totalChunks; i++) {
                if (i < bitmap.length && bitmap[i] === '1') {
                    asset.completedChunks += 1;
                    asset.confirmedBytes += Math.min(asset.chunkSize, file.size - (i * asset.chunkSize));
                } else {
                    pending.push(i);
                }
            }
            if (pendingByStatus.length > 0) {
                var pendingSet = Object.create(null);
                pendingByStatus.forEach(function(v) { pendingSet[String(v.chunkIndex)] = true; });
                pending = pending.filter(function(ci) { return !!pendingSet[String(ci)]; });
                pending.sort(function(a, b) { return a - b; });
            }
            if (pending.length > 0) {
                var nextMissing = pending[0];
                setTasfaStatus(asset, 'resending');
                tasfaTrace(asset, 'resend-missing', { chunkIndex: nextMissing, pending: pending.length });
                asset.pendingChunks = pending;
                asset.retryCounts = new Array(asset.totalChunks).fill(0);
                runSimpleChunkUpload(asset, file);
            } else {
                updateAssetProgress(asset);
                completeTasfaUpload(asset);
            }
        }).catch(function(error) {
            clearTimeout(timeoutId);
            var msg = error && error.message ? error.message : 'verify failed';
            if (msg.indexOf('status:401') !== -1 || msg.indexOf('status:429') !== -1 || msg.indexOf('status:5') !== -1 || msg === 'verify failed' || msg === 'The user aborted a request.') {
                setTasfaStatus(asset, 'retrying');
                setTimeout(function() { verifyAllChunksBeforeComplete(asset, file); }, Math.min(30000, 1000 * asset.serverVerifyRounds));
                return;
            }
            if (msg.indexOf('status:') === 0) {
                markUploadFailure(asset, 'Upload failed [' + msg.split(':')[1] + ']');
            } else {
                markUploadFailure(asset, 'Upload failed [' + msg + ']');
            }
        });
    }

    function completeTasfaUpload(asset, attempt) {
        attempt = attempt || 1;
        setTasfaStatus(asset, 'finalizing');
        var xhr = new XMLHttpRequest();
        asset.xhrs.push(xhr);
        xhr.open('POST', UPLOAD_COMPLETE_ENDPOINT, true);
        xhr.setRequestHeader('Content-Type', 'application/x-www-form-urlencoded');
        xhr.setRequestHeader('Accept', 'application/json');
        xhr.setRequestHeader('X-Requested-With', 'XMLHttpRequest');
        var watchdog = armXhrIdleTimeout(xhr, TASFA_CONNECT_TIMEOUT_MS);
        xhr.onload = function() {
            watchdog.clear();
            asset.xhrs = asset.xhrs.filter(function(x) { return x !== xhr; });
            if (xhr.status === 200) {
                var payload = null;
                try { payload = JSON.parse(xhr.responseText); } catch (e) {}
                if (payload && payload.ok && payload.url) {
                    finalizeUploadSuccess(asset, payload);
                    return;
                }
            }
            if (xhr.status === 202) {
                var processingPayload = null;
                try { processingPayload = JSON.parse(xhr.responseText); } catch (e) {}
                if (processingPayload && processingPayload.processing) {
                    var pollDelay = Math.max(500, Math.min(5000, Number(processingPayload.retry_after || 1) * 1000));
                    setTasfaStatus(asset, 'finalizing');
                    setTimeout(function() { completeTasfaUpload(asset, attempt); }, pollDelay);
                    return;
                }
            }
            if (xhr.status === 401) {
                if (attempt < 20) {
                    var delay = Math.min(30000, Math.pow(2, attempt) * 1000);
                    setTasfaStatus(asset, 'retrying');
                    setTimeout(function() { completeTasfaUpload(asset, attempt + 1); }, delay);
                    return;
                }
            }
            if (xhr.status === 409) {
                var payload = null;
                try { payload = JSON.parse(xhr.responseText); } catch (e) {}
                if (payload && payload.retry_targets && payload.retry_targets.length > 0) {
                    asset.htpRetryCount = (asset.htpRetryCount || 0) + 1;
                    if (asset.htpRetryCount > 20) {
                        markUploadFailure(asset, 'Upload failed [integrity retry limit]');
                        return;
                    }
                    setTasfaStatus(asset, 'resending');
                    asset.pendingChunks = payload.retry_targets.slice();
                    asset.confirmedBytes = 0;
                    asset.completedChunks = 0;
                    var bitmap2 = payload.received_bitmap || '';
                    for (var i = 0; i < asset.totalChunks; i++) {
                        if (i < bitmap2.length && bitmap2[i] === '1') {
                            asset.confirmedBytes += Math.min(asset.chunkSize, asset.file.size - (i * asset.chunkSize));
                            asset.completedChunks += 1;
                        }
                    }
                    runSimpleChunkUpload(asset, asset.file);
                    return;
                }
                if (payload && payload.received_bitmap) {
                    asset.htpRetryCount = (asset.htpRetryCount || 0) + 1;
                    if (asset.htpRetryCount > 20) {
                        markUploadFailure(asset, 'Upload failed [integrity retry limit]');
                        return;
                    }
                    setTasfaStatus(asset, 'resending');
                    var bitmap = payload.received_bitmap;
                    var pending = [];
                    asset.confirmedBytes = 0;
                    asset.completedChunks = 0;
                    for (var i = 0; i < asset.totalChunks; i++) {
                        if (i < bitmap.length && bitmap[i] === '1') {
                            asset.completedChunks += 1;
                            asset.confirmedBytes += Math.min(asset.chunkSize, asset.file.size - (i * asset.chunkSize));
                        } else {
                            pending.push(i);
                        }
                    }
                    asset.pendingChunks = pending;
                    runSimpleChunkUpload(asset, asset.file);
                    return;
                }
            }
            if (attempt < 20 && (xhr.status >= 500 || xhr.status === 0 || xhr.status === 429)) {
                var delay = Math.min(30000, Math.pow(2, attempt) * 1000);
                setTasfaStatus(asset, 'retrying');
                setTimeout(function() { completeTasfaUpload(asset, attempt + 1); }, delay);
                return;
            }
            markUploadFailure(asset, 'Upload failed [' + xhr.status + ']');
        };
        xhr.onerror = xhr.ontimeout = function() {
            watchdog.clear();
            asset.xhrs = asset.xhrs.filter(function(x) { return x !== xhr; });
            if (attempt < 20) {
                var delay = Math.min(30000, Math.pow(2, attempt) * 1000);
                setTasfaStatus(asset, 'retrying');
                setTimeout(function() { completeTasfaUpload(asset, attempt + 1); }, delay);
                return;
            }
            markUploadFailure(asset, 'Upload failed [network]');
        };
        xhr.onabort = function() {
            watchdog.clear();
            asset.xhrs = asset.xhrs.filter(function(x) { return x !== xhr; });
            if (attempt < 20) {
                var delay = Math.min(30000, Math.pow(2, attempt) * 1000);
                setTasfaStatus(asset, 'retrying');
                setTimeout(function() { completeTasfaUpload(asset, attempt + 1); }, delay);
                return;
            }
            markUploadFailure(asset, 'Upload failed [' + (xhr._tasfaIdleTimeout ? 'timeout' : 'abort') + ']');
        };
        xhr.send(encodeFormBody({ upload_id: asset.uploadId, upload_token: asset.uploadToken }));
    }

    function startQueuedAssetUpload(asset) {
        if (!asset || asset.isExisting || asset.fid !== null || asset.isUploading || asset.failed) return;
        if (asset.uploadMethod === 'plain') {
            uploadFilePlain(asset, asset.file);
            return;
        }
        startTasfaUpload(asset, asset.file);
    }

    function flushQueuedFileRepoUploads() {
        if (!isFileRepoMode) return;
        fileRepoPendingAssets().forEach(enqueueFileUpload);
        processFileUploadQueue();
        updateFileRepoUploadButton();
    }

    function uploadFile(file) {
        if (!uploadPreview) return;
        var useTasfa = USE_TASFA && file.size > TASFA_MIN_SIZE_BYTES;
        var transferProfile = classifyTasfaTransfer(file);
        var clientUuid = generateUUID();
        var blobUrl = URL.createObjectURL(file);
        var placeholderUrl = isEditorMode ? (blobUrl + '#' + clientUuid) : null;
        var mediaType = /^image\//.test(file.type || '') ? 'image' : /^video\//.test(file.type || '') ? 'video' : /^audio\//.test(file.type || '') ? 'audio' : 'file';
        var ui = createMediaCard(file.name, blobUrl, mediaType);
        var asset = {
            client_uuid: clientUuid,
            fid: null,
            filename: file.name,
            file: file,
            blobUrl: blobUrl,
            url: null,
            placeholderUrl: placeholderUrl,
            mode: isEditorMode ? 'inline' : 'attachment',
            xhrs: [],
            uploadId: null,
            uploadToken: null,
            streamKeyHex: '',
            streamIvSeedHex: '',
            streamCryptoKey: null,
            modulusM: 1,
            htpGroups: {},
            fallbackChain: Promise.resolve(),
            fileSize: file.size,
            chunkSize: preferredUploadChunkSize(file.size),
            confirmedBytes: 0,
            totalChunks: 0,
            maxParallel: UPLOAD_DEFAULT_PARALLEL,
            targetParallel: UPLOAD_DEFAULT_PARALLEL,
            dispatchPacingMs: 0,
            inflightBytes: [],
            retryCounts: [],
            completedChunks: 0,
            uploadRunId: 0,
            displayPercent: 0,
            isUploading: false,
            failed: false,
            isCancelling: false,
            isNetworkPaused: false,
            isBackgroundPaused: false,
            uploadMethod: useTasfa ? 'tasfa' : 'plain',
            transferProfile: transferProfile,
            ui: ui
        };
        AssetRegistry.push(asset);
        if (uploadPreview) uploadPreview.appendChild(ui.el);
        bindAssetControls(asset);
        updateFileRepoUploadButton();
        updateSubmitButtons();
        var maxUploadSize = Number(window.BLOG_MAX_UPLOAD_SIZE || 0);
        if (maxUploadSize > 0 && file.size > maxUploadSize) {
            asset.tooLarge = true;
            asset.failed = true;
            asset.ui.status.textContent = 'Upload too large';
            asset.xhrs = [];
            updateFileRepoUploadButton();
            updateSubmitButtons();
            updateMediaCardUI(asset);
            return;
        }
        if (isEditorMode) {
            if (useTasfa) { startTasfaUpload(asset, file); }
            else { uploadFilePlain(asset, file); }
        }
    }

    function bootstrapExistingAssets() {
        if (!isEditorMode || !uploadPreview) return;
        var cards = uploadPreview.querySelectorAll('.existing-media');
        Array.prototype.forEach.call(cards, function(card, index) {
            var fid = Number(card.getAttribute('data-fid'));
            var filename = card.getAttribute('data-filename') || 'Attachment';
            var url = card.getAttribute('data-url') || ('/file/download/' + fid);
            var mode = card.getAttribute('data-mode') || 'attachment';
            var mime = card.getAttribute('data-mime') || '';
            var btnInsert = document.createElement('button');
            btnInsert.type = 'button';
            btnInsert.className = 'btn btn-outline media-insert-btn';
            btnInsert.textContent = 'Insert';
            var actions = card.querySelector('.media-actions');
            if (actions) actions.insertBefore(btnInsert, actions.firstChild);

            var asset = {
                client_uuid: 'existing-' + fid + '-' + index,
                fid: fid,
                filename: filename,
                blobUrl: null,
                url: url,
                mime_type: mime,
                placeholderUrl: null,
                mode: mode,
                xhr: null,
                isExisting: true,
                ui: {
                    el: card,
                    thumbImg: card.querySelector('img'),
                    status: card.querySelector('.media-status'),
                    progress: card.querySelector('.media-progress-bar'),
                    progressInner: card.querySelector('.media-progress-inner'),
                    btnInsert: btnInsert,
                    btnInline: card.querySelector('.media-inline-btn'),
                    btnAttachment: card.querySelector('.media-attachment-btn'),
                    btnDelete: card.querySelector('.media-delete-btn')
                }
            };
            if (asset.ui.progress) {
                asset.ui.progress.style.display = 'none';
            }
            if (asset.ui.progressInner) {
                asset.ui.progressInner.style.width = '0%';
            }
            AssetRegistry.push(asset);
            bindAssetControls(asset);
        });
        syncMediaMeta();
    }

    function onFileBatch(files) {
        if (!files) return;
        for (var i = 0; i < files.length; i += 1) {
            uploadFile(files[i]);
        }
    }

    if (dropzone) {
        dropzone.addEventListener('dragover', function(event) {
            event.preventDefault();
            dropzone.classList.add('is-dragging');
        });
        dropzone.addEventListener('dragleave', function(event) {
            event.preventDefault();
            dropzone.classList.remove('is-dragging');
        });
        dropzone.addEventListener('drop', function(event) {
            event.preventDefault();
            dropzone.classList.remove('is-dragging');
            if (!event.dataTransfer || !event.dataTransfer.files) return;
            onFileBatch(event.dataTransfer.files);
        });
    }

    if (fileInput) {
        fileInput.addEventListener('change', function(event) {
            onFileBatch(event.target.files);
            fileInput.value = '';
        });
    }

    if (fileRepoUploadButton) {
        fileRepoUploadButton.addEventListener('click', flushQueuedFileRepoUploads);
        updateFileRepoUploadButton();
    }

    if (ta) {
        ta.addEventListener('input', schedulePreview);
        ta.addEventListener('paste', function(event) {
            if (!event.clipboardData || !event.clipboardData.files || !event.clipboardData.files.length) return;
            event.preventDefault();
            onFileBatch(event.clipboardData.files);
        });
    }

    Array.prototype.forEach.call(tabButtons, function(button) {
        button.addEventListener('click', function() {
            switchTab(button.getAttribute('data-editor-tab'));
        });
    });

    Array.prototype.forEach.call(toolbarButtons, function(button) {
        button.addEventListener('click', function() {
            var wrap = button.getAttribute('data-md-wrap');
            var prefix = button.getAttribute('data-md-prefix');
            var block = button.getAttribute('data-md-block');
            var placeholder = button.getAttribute('data-md-placeholder') || '';
            if (wrap) {
                toggleWrap(wrap, placeholder);
            } else if (prefix) {
                prependToSelection(prefix, placeholder);
            } else if (block) {
                insertAtCursor(block + '\n');
            }
        });
    });

    if (form) {
        form.addEventListener('submit', function(event) {
            if (hasActiveUploads()) {
                event.preventDefault();
                updateSubmitButtons();
                return;
            }
            isSubmitting = true;
            syncMediaMeta();
        });
    }

    window.addEventListener('offline', function() {
        AssetRegistry.forEach(function(asset) {
            if (!asset || asset.isExisting || asset.fid !== null || !asset.isUploading) return;
            asset.ui.status.textContent = 'Waiting for network';
            asset.isNetworkPaused = true;
            asset.isUploading = false;
            asset.uploadRunId = (asset.uploadRunId || 0) + 1;
            if (asset.xhrs && asset.xhrs.length) {
                asset.xhrs.slice().forEach(function(xhr) {
                    try { xhr.abort(); } catch (err) {}
                });
            }
        });
    });

    window.addEventListener('online', function() {
        AssetRegistry.forEach(function(asset) {
            if (!asset || asset.isExisting || asset.fid !== null || !asset.uploadId || asset.failed) return;
            if (asset.uploadMethod === 'tasfa') {
                asset.failed = false;
                asset.isNetworkPaused = false;
                startTasfaUpload(asset, asset.file);
            }
        });
    });

    document.addEventListener('visibilitychange', function() {
        if (document.visibilityState === 'hidden') {
            AssetRegistry.forEach(function(asset) {
                if (!asset || asset.isExisting || asset.fid !== null || !asset.isUploading || asset.failed || asset.isCancelling) return;
                asset.isBackgroundPaused = false;
            });
        } else {
            AssetRegistry.forEach(function(asset) {
                if (!asset || asset.isExisting || asset.fid !== null || !asset.uploadId || asset.failed) return;
                asset.isBackgroundPaused = false;
                if (asset.uploadMethod === 'tasfa' && !asset.isUploading) {
                    asset.failed = false;
                    startTasfaUpload(asset, asset.file);
                }
            });
        }
    });

    window.addEventListener('beforeunload', function() {
        if (isSubmitting) return;
        var transientFids = AssetRegistry
            .filter(function(asset) { return !asset.isExisting && asset.fid !== null; })
            .map(function(asset) { return asset.fid; });
        if (!transientFids.length || !navigator.sendBeacon) return;
        var formData = new FormData();
        if (transientFids.length) formData.append('fids', JSON.stringify(transientFids));
        navigator.sendBeacon(UPLOAD_CANCEL_ENDPOINT, formData);
    });

    window.cleanupUploadedFiles = function(event) {
        var transientFids = AssetRegistry
            .filter(function(asset) { return !asset.isExisting && asset.fid !== null; })
            .map(function(asset) { return asset.fid; });
        var transientUploadIds = AssetRegistry
            .filter(function(asset) { return !asset.isExisting && asset.fid === null && asset.uploadId; })
            .map(function(asset) { return asset.uploadId; });
        if (!transientFids.length && !transientUploadIds.length) return;
        event.preventDefault();
        var targetUrl = event.currentTarget.href;
        fetch(UPLOAD_CANCEL_ENDPOINT, {
            method: 'POST',
            headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
            body: 'fids=' + encodeURIComponent(JSON.stringify(transientFids)) +
                '&upload_ids=' + encodeURIComponent(JSON.stringify(transientUploadIds))
        }).finally(function() {
            window.location.href = targetUrl;
        });
    };

    if (titleInput) {
        titleInput.addEventListener('keydown', function(event) {
            if (event.key === 'Enter') {
                event.preventDefault();
                ta.focus();
            }
        });
    }

    if (summaryInput) {
        summaryInput.addEventListener('input', function() {
            if (syncStatus) syncStatus.textContent = 'Publish rail updated';
        });
    }

    document.addEventListener('submit', function(event) {
        var form = event.target;
        if (!form || !form.classList.contains('file-delete-form')) return;
        event.preventDefault();
        var btn = form.querySelector('button[type="submit"]');
        if (btn) btn.disabled = true;
        fetch(form.action || '/file/delete', {
            method: 'POST',
            headers: { 'Content-Type': 'application/x-www-form-urlencoded', 'X-Requested-With': 'XMLHttpRequest' },
            body: new URLSearchParams(new FormData(form))
        }).then(function(response) {
            if (!response.ok) throw new Error('status:' + response.status);
            var card = form.closest('article');
            if (card && card.parentNode) {
                card.remove();
                var list = document.getElementById('file-repo-list');
                if (list && !list.children.length) {
                    var emptyCard = document.createElement('div');
                    emptyCard.className = 'card';
                    emptyCard.style.cssText = 'text-align:center;padding:40px 20px;color:var(--muted);';
                    emptyCard.textContent = 'No files uploaded yet.';
                    list.parentNode.insertBefore(emptyCard, list.nextSibling);
                    list.remove();
                    var h3 = document.querySelector('h3');
                    if (h3) {
                        var match = h3.textContent.match(/Files\s*\(/);
                        if (match) h3.textContent = 'Files';
                    }
                }
            } else {
                window.location.reload();
            }
        }).catch(function(err) {
            console.error('Delete failed:', err);
            if (btn) btn.disabled = false;
            alert('Failed to delete file.');
        });
    });

    (function() {
        var audioCtx = null;
        function ensureAudioCtx() {
            if (!audioCtx && (window.AudioContext || window.webkitAudioContext)) {
                audioCtx = new (window.AudioContext || window.webkitAudioContext)();
            }
            return audioCtx;
        }
        function playSilentBuffer() {
            var ctx = ensureAudioCtx();
            if (!ctx) return;
            if (ctx.state === 'suspended') ctx.resume();
            var osc = ctx.createOscillator();
            var gain = ctx.createGain();
            gain.gain.value = 0.001;
            osc.connect(gain);
            gain.connect(ctx.destination);
            osc.start();
            osc.stop(ctx.currentTime + 0.001);
        }
        window._tasfaKeepAlive = setInterval(playSilentBuffer, 30000);
    })();

    bootstrapExistingAssets();
    updateSubmitButtons();
    if (isEditorMode) {
        switchTab('write');
        updatePreview();
    }
    }

    if (document.readyState === 'loading') {
        document.addEventListener('DOMContentLoaded', init, { once: true });
    } else {
        init();
    }
})();

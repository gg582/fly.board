var CHUNK_STORE = 'chunks';
var DB_NAME = 'tasfa_cache';
var DB_VERSION = 1;

function openDB() {
    return new Promise(function(resolve) {
        var req = indexedDB.open(DB_NAME, DB_VERSION);
        req.onupgradeneeded = function(e) {
            var db = e.target.result;
            if (!db.objectStoreNames.contains(CHUNK_STORE)) {
                db.createObjectStore(CHUNK_STORE);
            }
        };
        req.onsuccess = function(e) { resolve(e.target.result); };
        req.onerror = function() { resolve(null); };
    });
}

function putChunk(db, key, data) {
    return new Promise(function(resolve) {
        var tx = db.transaction(CHUNK_STORE, 'readwrite');
        tx.objectStore(CHUNK_STORE).put(data, key);
        tx.oncomplete = function() { resolve(); };
        tx.onerror = function() { resolve(); };
    });
}

async function broadcast(data) {
    var clients = await self.clients.matchAll({ type: 'window', includeUncontrolled: true });
    clients.forEach(function(c) { c.postMessage(data); });
}

var activeDownloads = {};

function handshakeUrl(baseUrl) {
    if (baseUrl.indexOf('/file/download/') === 0) return baseUrl + '/handshake';
    if (baseUrl.indexOf('/assets/img/') === 0) return '/assets/tasfa/img/' + encodeURIComponent(baseUrl.slice('/assets/img/'.length)) + '/handshake';
    if (baseUrl.indexOf('/assets/uploads/') === 0) return '/assets/tasfa/uploads/' + encodeURIComponent(baseUrl.slice('/assets/uploads/'.length)) + '/handshake';
    return null;
}

function chunkUrl(baseUrl, sid, tok, idx) {
    if (baseUrl.indexOf('/file/download/') === 0) {
        return baseUrl + '/chunk/' + idx + '?session_id=' + encodeURIComponent(sid) + '&session_token=' + encodeURIComponent(tok);
    }
    if (baseUrl.indexOf('/assets/img/') === 0) {
        return '/assets/tasfa/img/' + encodeURIComponent(baseUrl.slice('/assets/img/'.length)) + '/chunk/' + idx + '?session_id=' + encodeURIComponent(sid) + '&session_token=' + encodeURIComponent(tok);
    }
    if (baseUrl.indexOf('/assets/uploads/') === 0) {
        return '/assets/tasfa/uploads/' + encodeURIComponent(baseUrl.slice('/assets/uploads/'.length)) + '/chunk/' + idx + '?session_id=' + encodeURIComponent(sid) + '&session_token=' + encodeURIComponent(tok);
    }
    return null;
}

async function doHandshake(baseUrl) {
    var url = handshakeUrl(baseUrl);
    if (!url) return null;
    var res = await fetch(url, { credentials: 'same-origin' });
    return res.json();
}

async function downloadFile(baseUrl) {
    if (activeDownloads[baseUrl]) return;

    var session = await doHandshake(baseUrl);
    if (!session || !session.ok) {
        await broadcast({ type: 'DOWNLOAD_PROGRESS', baseUrl: baseUrl, error: true });
        return;
    }

    var db = await openDB();
    if (!db) return;

    var chunkCount = session.chunkCount;
    var sid = session.sessionId;
    var tok = session.sessionToken;
    var filename = session.filename || 'download';

    activeDownloads[baseUrl] = { chunkCount: chunkCount, completed: 0, aborted: false, filename: filename };

    var completed = 0;
    for (var i = 0; i < chunkCount; i++) {
        var dl = activeDownloads[baseUrl];
        if (!dl || dl.aborted) break;

        var cached = await new Promise(function(resolve) {
            var tx = db.transaction(CHUNK_STORE, 'readonly');
            var req = tx.objectStore(CHUNK_STORE).get(baseUrl + ':' + i);
            req.onsuccess = function(e) { resolve(e.target.result); };
            req.onerror = function() { resolve(null); };
        });

        if (cached) {
            completed++;
            activeDownloads[baseUrl].completed = completed;
            await broadcast({ type: 'DOWNLOAD_PROGRESS', baseUrl: baseUrl, completed: completed, total: chunkCount, filename: filename, done: false });
            continue;
        }

        var url = chunkUrl(baseUrl, sid, tok, i);
        try {
            var res = await fetch(url, { credentials: 'same-origin' });
            if (!res.ok) continue;
            var buf = await res.arrayBuffer();
            await putChunk(db, baseUrl + ':' + i, buf);
            completed++;
            activeDownloads[baseUrl].completed = completed;
            await broadcast({ type: 'DOWNLOAD_PROGRESS', baseUrl: baseUrl, completed: completed, total: chunkCount, filename: filename, done: false });
        } catch (e) {
            console.error('SW chunk error', e);
        }
    }

    delete activeDownloads[baseUrl];
    await broadcast({ type: 'DOWNLOAD_PROGRESS', baseUrl: baseUrl, completed: completed, total: chunkCount, filename: filename, done: true });
}

self.addEventListener('install', function(e) {
    self.skipWaiting();
});

self.addEventListener('activate', function(e) {
    e.waitUntil(self.clients.claim());
});

self.addEventListener('message', function(e) {
    if (e.data.type === 'START_DOWNLOAD') {
        downloadFile(e.data.baseUrl);
    } else if (e.data.type === 'STOP_DOWNLOAD') {
        var dl = activeDownloads[e.data.baseUrl];
        if (dl) dl.aborted = true;
        delete activeDownloads[e.data.baseUrl];
    }
});

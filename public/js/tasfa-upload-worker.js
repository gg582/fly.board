self.onmessage = function(event) {
    var data = event.data || {};
    if (data.type !== 'prepare-upload') return;

    Promise.resolve().then(function() {
        return createChunkSignature(data);
    }).then(function(signature) {
        return gzipChunk(data.chunkBuffer).then(function(result) {
            return {
                id: data.id,
                ok: true,
                signature: signature,
                payloadBuffer: result.payloadBuffer,
                contentEncoding: result.contentEncoding
            };
        });
    }).then(function(result) {
        self.postMessage(result, [result.payloadBuffer]);
    }).catch(function(error) {
        self.postMessage({
            id: data.id,
            ok: false,
            error: error && error.message ? error.message : 'worker failure'
        });
    });
};

function createChunkSignature(data) {
    if (!self.crypto || !self.crypto.subtle || !self.TextEncoder) {
        return Promise.reject(new Error('crypto unavailable'));
    }
    var enc = new TextEncoder();
    var message = [
        data.uploadId,
        String(data.chunkIndex),
        String(data.blockOffset),
        data.nonce || '',
        data.vertexId || '',
        String(data.magicSum)
    ].join(':');
    return self.crypto.subtle.importKey(
        'raw',
        enc.encode(data.uploadSecret || ''),
        { name: 'HMAC', hash: 'SHA-256' },
        false,
        ['sign']
    ).then(function(key) {
        return self.crypto.subtle.sign('HMAC', key, enc.encode(message));
    }).then(function(signature) {
        var bytes = new Uint8Array(signature);
        var hex = '';
        for (var i = 0; i < bytes.length; i += 1) {
            hex += bytes[i].toString(16).padStart(2, '0');
        }
        return hex;
    });
}

function gzipChunk(buffer) {
    if (typeof CompressionStream !== 'function') {
        return Promise.resolve({ payloadBuffer: buffer, contentEncoding: 'identity' });
    }
    var stream = new Blob([buffer]).stream().pipeThrough(new CompressionStream('gzip'));
    return new Response(stream).arrayBuffer().then(function(gzipped) {
        if (gzipped.byteLength < buffer.byteLength) {
            return { payloadBuffer: gzipped, contentEncoding: 'gzip' };
        }
        return { payloadBuffer: buffer, contentEncoding: 'identity' };
    });
}

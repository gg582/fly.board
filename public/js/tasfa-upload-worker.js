self.__tasfaHmacKeys = self.__tasfaHmacKeys || {};

self.onmessage = function(event) {
    var data = event.data || {};
    if (data.type !== 'prepare-upload' && data.type !== 'prepare-upload-batch') return;

    Promise.resolve().then(function() {
        if (data.type === 'prepare-upload-batch') {
            return prepareChunkBatch(data).then(function(chunks) {
                var transferList = chunks.map(function(chunk) { return chunk.payloadBuffer; });
                return {
                    id: data.id,
                    ok: true,
                    chunks: chunks,
                    transferList: transferList
                };
            });
        }
        return prepareSingleChunk(data).then(function(result) {
            return {
                id: data.id,
                ok: true,
                signature: result.signature,
                payloadBuffer: result.payloadBuffer,
                contentEncoding: result.contentEncoding,
                transferList: [result.payloadBuffer]
            };
        });
    }).then(function(result) {
        self.postMessage(result, result.transferList || []);
    }).catch(function(error) {
        self.postMessage({
            id: data.id,
            ok: false,
            error: error && error.message ? error.message : 'worker failure'
        });
    });
};

function getOrImportHmacKey(secret) {
    if (!secret) return Promise.reject(new Error('missing upload secret'));
    if (self.__tasfaHmacKeys[secret]) return Promise.resolve(self.__tasfaHmacKeys[secret]);
    var enc = new TextEncoder();
    return self.crypto.subtle.importKey(
        'raw',
        enc.encode(secret),
        { name: 'HMAC', hash: 'SHA-256' },
        false,
        ['sign']
    ).then(function(key) {
        self.__tasfaHmacKeys[secret] = key;
        return key;
    });
}

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
    return getOrImportHmacKey(data.uploadSecret || '').then(function(key) {
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

function prepareSingleChunk(data) {
    return createChunkSignature(data).then(function(signature) {
        var payloadPromise = data.shouldCompress
            ? gzipChunk(data.chunkBuffer)
            : Promise.resolve({ payloadBuffer: data.chunkBuffer, contentEncoding: 'identity' });
        return payloadPromise.then(function(result) {
            return {
                signature: signature,
                payloadBuffer: result.payloadBuffer,
                contentEncoding: result.contentEncoding
            };
        });
    });
}

function prepareChunkBatch(data) {
    var chunks = Array.isArray(data.chunks) ? data.chunks : [];
    return Promise.all(chunks.map(function(chunk) {
        return prepareSingleChunk({
            uploadId: data.uploadId,
            uploadSecret: data.uploadSecret,
            shouldCompress: data.shouldCompress,
            chunkIndex: chunk.chunkIndex,
            blockOffset: chunk.blockOffset,
            nonce: chunk.nonce,
            vertexId: chunk.vertexId,
            magicSum: chunk.magicSum,
            chunkBuffer: chunk.chunkBuffer
        }).then(function(result) {
            return {
                chunkIndex: chunk.chunkIndex,
                signature: result.signature,
                payloadBuffer: result.payloadBuffer,
                contentEncoding: result.contentEncoding
            };
        });
    }));
}

function gzipChunk(buffer) {
    if (!buffer || typeof CompressionStream !== 'function') {
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

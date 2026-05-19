self.__tasfaStreamKeys = self.__tasfaStreamKeys || {};

self.onmessage = function(event) {
    var data = event.data || {};
    if (data.type !== 'prepare-upload' && data.type !== 'prepare-upload-batch') return;

    Promise.resolve().then(function() {
        if (data.type === 'prepare-upload-batch') {
            return prepareChunkBatch(data).then(function(chunks) {
                var transferList = chunks.map(function(chunk) { return chunk.payloadBuffer; });
                return { id: data.id, ok: true, chunks: chunks, transferList: transferList };
            });
        }
        return prepareSingleChunk(data).then(function(result) {
            return { id: data.id, ok: true, payloadBuffer: result.payloadBuffer, transferList: [result.payloadBuffer] };
        });
    }).then(function(result) {
        self.postMessage(result, result.transferList || []);
    }).catch(function(error) {
        self.postMessage({ id: data.id, ok: false, error: error && error.message ? error.message : 'worker failure' });
    });
};

function hexToBytes(hex) {
    var source = String(hex || '');
    if (!source || source.length % 2 !== 0) return null;
    var bytes = new Uint8Array(source.length / 2);
    for (var i = 0; i < source.length; i += 2) {
        bytes[i / 2] = parseInt(source.slice(i, i + 2), 16);
    }
    return bytes;
}

function deriveStreamIv(seedHex, chunkIndex) {
    var seed = hexToBytes(seedHex);
    if (!seed || seed.length !== 12) throw new Error('invalid stream iv seed');
    seed[8] ^= (chunkIndex >>> 24) & 0xff;
    seed[9] ^= (chunkIndex >>> 16) & 0xff;
    seed[10] ^= (chunkIndex >>> 8) & 0xff;
    seed[11] ^= chunkIndex & 0xff;
    return seed;
}

function buildStreamAad(data) {
    var enc = new TextEncoder();
    return enc.encode((data.uploadId || '') + ':' + String(data.chunkIndex || 0));
}

function getOrImportStreamKey(keyHex) {
    if (!keyHex) return Promise.reject(new Error('missing stream key'));
    if (self.__tasfaStreamKeys[keyHex]) return Promise.resolve(self.__tasfaStreamKeys[keyHex]);
    var keyBytes = hexToBytes(keyHex);
    if (!keyBytes) return Promise.reject(new Error('invalid stream key'));
    return self.crypto.subtle.importKey('raw', keyBytes, { name: 'AES-GCM' }, false, ['encrypt']).then(function(key) {
        self.__tasfaStreamKeys[keyHex] = key;
        return key;
    });
}

function encryptChunkBuffer(data) {
    if (!self.crypto || !self.crypto.subtle || !self.TextEncoder) {
        return Promise.reject(new Error('crypto unavailable'));
    }
    var iv = deriveStreamIv(data.streamIvSeedHex, Number(data.chunkIndex || 0));
    var aad = buildStreamAad(data);
    return getOrImportStreamKey(data.streamKeyHex || '').then(function(key) {
        return self.crypto.subtle.encrypt({ name: 'AES-GCM', iv: iv, additionalData: aad, tagLength: 128 }, key, data.chunkBuffer);
    }).then(function(buffer) {
        return { payloadBuffer: buffer };
    });
}

function prepareSingleChunk(data) {
    return encryptChunkBuffer(data).then(function(result) {
        return { payloadBuffer: result.payloadBuffer };
    });
}

function prepareChunkBatch(data) {
    var chunks = Array.isArray(data.chunks) ? data.chunks : [];
    return Promise.all(chunks.map(function(chunk) {
        return prepareSingleChunk({
            uploadId: data.uploadId,
            streamKeyHex: data.streamKeyHex,
            streamIvSeedHex: data.streamIvSeedHex,
            chunkIndex: chunk.chunkIndex,
            chunkBuffer: chunk.chunkBuffer
        }).then(function(result) {
            return { chunkIndex: chunk.chunkIndex, payloadBuffer: result.payloadBuffer };
        });
    }));
}

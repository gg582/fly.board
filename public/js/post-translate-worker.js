var TRANSFORMERS_JS_URL = 'https://cdn.jsdelivr.net/npm/@huggingface/transformers@3.8.1/dist/transformers.min.js';
var NLLB_MODEL = 'Xenova/nllb-200-distilled-600M';
var transformersPromise = null;
var languagesPromise = null;
var translatorPromise = null;

function getTransformers() {
    if (!transformersPromise) {
        transformersPromise = import(TRANSFORMERS_JS_URL).then(function(module) {
            module.env.useBrowserCache = true;
            return module;
        });
    }
    return transformersPromise;
}

function getLanguages() {
    if (!languagesPromise) {
        languagesPromise = getTransformers().then(function(module) {
            return module.AutoTokenizer.from_pretrained(NLLB_MODEL);
        }).then(function(tokenizer) {
            return tokenizer.language_codes;
        });
    }
    return languagesPromise;
}

function getTranslator(requestId) {
    if (!translatorPromise) {
        translatorPromise = getLanguages().then(function() {
            return getTransformers();
        }).then(function(module) {
            return module.pipeline('translation', NLLB_MODEL, {
                dtype: 'q4',
                progress_callback: function(progress) {
                    if (progress.status === 'progress' && progress.progress != null) {
                        var text = progress.progress >= 100
                            ? 'Preparing translation engine…'
                            : 'Downloading translation model… ' + Math.round(progress.progress) + '%';
                        postMessage({type: 'progress', requestId: requestId, text: text});
                    }
                }
            });
        });
    }
    return translatorPromise;
}

self.onmessage = function(event) {
    var message = event.data || {};
    if (message.type === 'languages') {
        getLanguages().then(function(codes) {
            postMessage({type: 'languages', codes: codes});
        }).catch(function() {
            /* Keep the initial compact list usable if metadata retrieval fails. */
        });
        return;
    }
    if (message.type !== 'translate') return;

    getTranslator(message.requestId).then(function(translator) {
        postMessage({type: 'model-ready', requestId: message.requestId});
        return translator(message.chunks, {
            src_lang: message.sourceLanguage,
            tgt_lang: message.target
        });
    }).then(function(results) {
        var parts = results.map(function(result) {
            return result.translation_text;
        });
        postMessage({type: 'done', requestId: message.requestId, target: message.target, parts: parts});
    }).catch(function(error) {
        translatorPromise = null;
        postMessage({type: 'error', requestId: message.requestId, message: String(error)});
    });
};

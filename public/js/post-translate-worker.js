var TRANSFORMERS_JS_URL = 'https://cdn.jsdelivr.net/npm/@huggingface/transformers@3.8.1/dist/transformers.min.js';
var ENGLISH = 'eng_Latn';
var LANGUAGE_MODELS = {
    kor_Hang: {code: 'ko', toEnglish: 'Xenova/opus-mt-ko-en', fromEnglish: 'noticemkjung/opus-mt-tc-big-en-ko-ONNX'},
    jpn_Jpan: {code: 'jap', toEnglish: 'Xenova/opus-mt-jap-en', fromEnglish: 'Xenova/opus-mt-en-jap'},
    zho_Hans: {code: 'zh', toEnglish: 'Xenova/opus-mt-zh-en', fromEnglish: 'Xenova/opus-mt-en-zh'},
    spa_Latn: {code: 'es', toEnglish: 'Xenova/opus-mt-es-en', fromEnglish: 'Xenova/opus-mt-en-es'},
    fra_Latn: {code: 'fr', toEnglish: 'Xenova/opus-mt-fr-en', fromEnglish: 'Xenova/opus-mt-en-fr'},
    deu_Latn: {code: 'de', toEnglish: 'Xenova/opus-mt-de-en', fromEnglish: 'Xenova/opus-mt-en-de'},
    por_Latn: {code: 'pt', toEnglish: 'Xenova/opus-mt-ROMANCE-en', fromEnglish: 'Xenova/opus-mt-en-ROMANCE', token: 'por'},
    rus_Cyrl: {code: 'ru', toEnglish: 'Xenova/opus-mt-ru-en', fromEnglish: 'Xenova/opus-mt-en-ru'},
    arb_Arab: {code: 'ar', toEnglish: 'Xenova/opus-mt-ar-en', fromEnglish: 'Xenova/opus-mt-en-ar'},
    hin_Deva: {code: 'hi', toEnglish: 'Xenova/opus-mt-hi-en', fromEnglish: 'Xenova/opus-mt-en-hi'},
    ind_Latn: {code: 'id', toEnglish: 'Xenova/opus-mt-id-en', fromEnglish: 'Xenova/opus-mt-en-id'},
    vie_Latn: {code: 'vi', toEnglish: 'Xenova/opus-mt-vi-en', fromEnglish: 'Xenova/opus-mt-en-vi'}
};
var transformersPromise = null;
var translatorCache = {};

function getTransformers() {
    if (!transformersPromise) {
        transformersPromise = import(TRANSFORMERS_JS_URL).then(function(module) {
            module.env.useBrowserCache = true;
            return module;
        });
    }
    return transformersPromise;
}

function getTranslator(model, requestId) {
    if (!translatorCache[model]) {
        translatorCache[model] = getTransformers().then(function(module) {
            return module.pipeline('translation', model, {
                dtype: 'q4',
                progress_callback: function(progress) {
                    if (progress.status === 'progress' && progress.progress != null) {
                        var text = progress.progress >= 100
                            ? 'Preparing translation engine…'
                            : 'Downloading language model… ' + Math.round(progress.progress) + '%';
                        postMessage({type: 'progress', requestId: requestId, text: text});
                    }
                }
            });
        }).catch(function(error) {
            delete translatorCache[model];
            throw error;
        });
    }
    return translatorCache[model];
}

function translatedText(result) {
    return (Array.isArray(result) ? result[0] : result).translation_text;
}

function translateBatch(chunks, model, requestId, languageToken) {
    var input = languageToken
        ? chunks.map(function(chunk) { return '>>' + languageToken + '<< ' + chunk; })
        : chunks;
    return getTranslator(model, requestId).then(function(translator) {
        postMessage({type: 'model-ready', requestId: requestId});
        // Passing the complete array makes Transformers.js tokenize and infer as one batch.
        return translator(input);
    }).then(function(results) {
        return results.map(translatedText);
    });
}

function routeTranslation(message) {
    var source = message.sourceLanguage;
    var target = message.target;
    var sourceModel = LANGUAGE_MODELS[source];
    var targetModel = LANGUAGE_MODELS[target];

    if (source === ENGLISH && targetModel) {
        return translateBatch(message.chunks, targetModel.fromEnglish, message.requestId, targetModel.token);
    }
    if (target === ENGLISH && sourceModel) {
        return translateBatch(message.chunks, sourceModel.toEnglish, message.requestId, sourceModel.token);
    }
    if (sourceModel && targetModel) {
        return translateBatch(message.chunks, sourceModel.toEnglish, message.requestId, sourceModel.token)
            .then(function(englishChunks) {
                return translateBatch(englishChunks, targetModel.fromEnglish, message.requestId, targetModel.token);
            });
    }
    return Promise.reject(new Error('Unsupported translation language pair'));
}

self.onmessage = function(event) {
    var message = event.data || {};
    if (message.type !== 'translate') return;

    routeTranslation(message).then(function(parts) {
        postMessage({type: 'done', requestId: message.requestId, target: message.target, parts: parts});
    }).catch(function(error) {
        postMessage({type: 'error', requestId: message.requestId, message: String(error)});
    });
};

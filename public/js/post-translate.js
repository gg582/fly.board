(function() {
    'use strict';

    var button = document.getElementById('post-translate');
    var source = document.querySelector('article .markdown-body');
    var output = document.getElementById('post-translation');
    var status = document.getElementById('translation-msg');
    if (!button || !source || !output) return;

    var TRANSLATORS_JS_URL = 'https://cdn.jsdelivr.net/npm/@huggingface/transformers@3.8.1/dist/transformers.min.js';
    var MODEL_KO_EN = 'Xenova/opus-mt-ko-en';
    var MODEL_EN_KO = 'Xenova/opus-mt-en-ko';
    var translatorCache = {};
    var translated = false;

    function getTranslatableText() {
        var clone = source.cloneNode(true);
        clone.querySelectorAll('pre, code, script, style, .math-block, .math-inline, .tikz-block, .katex').forEach(function(node) {
            node.remove();
        });
        return (clone.innerText || clone.textContent || '').replace(/\n{3,}/g, '\n\n').trim();
    }

    function hasMostlyKorean(text) {
        var korean = (text.match(/[\uac00-\ud7a3]/g) || []).length;
        var latin = (text.match(/[A-Za-z]/g) || []).length;
        return korean > latin * 0.15;
    }

    function splitText(text) {
        var paragraphs = text.split(/\n{2,}/);
        var chunks = [];
        paragraphs.forEach(function(paragraph) {
            paragraph = paragraph.trim();
            while (paragraph.length > 0) {
                if (paragraph.length <= 450) {
                    chunks.push(paragraph);
                    break;
                }
                var cut = paragraph.lastIndexOf(' ', 450);
                if (cut < 120) cut = 450;
                chunks.push(paragraph.slice(0, cut));
                paragraph = paragraph.slice(cut).trim();
            }
        });
        return chunks;
    }

    async function getTranslator(model) {
        if (!translatorCache[model]) {
            status.textContent = 'Loading translation model…';
            translatorCache[model] = import(TRANSLATORS_JS_URL).then(function(module) {
                return module.pipeline('translation', model, {
                    dtype: 'q4f16',
                    progress_callback: function(progress) {
                        if (progress.status === 'progress' && progress.progress != null) {
                            status.textContent = 'Downloading translation model… ' + Math.round(progress.progress) + '%';
                        }
                    }
                });
            });
        }
        return translatorCache[model];
    }

    function renderTranslation(parts, targetName) {
        output.textContent = '';
        var heading = document.createElement('h2');
        heading.textContent = 'Machine translation (' + targetName + ')';
        output.appendChild(heading);
        parts.forEach(function(part) {
            var paragraph = document.createElement('p');
            paragraph.textContent = part;
            output.appendChild(paragraph);
        });
        var note = document.createElement('p');
        note.style.color = 'var(--muted)';
        note.style.fontSize = '13px';
        note.textContent = 'Translated locally in your browser with Transformers.js.';
        output.appendChild(note);
        output.hidden = false;
    }

    button.addEventListener('click', async function() {
        if (translated) {
            output.hidden = !output.hidden;
            button.setAttribute('aria-pressed', String(!output.hidden));
            button.setAttribute('aria-label', output.hidden ? 'Translate' : 'Hide translation');
            button.title = output.hidden ? 'Translate' : 'Hide translation';
            status.textContent = '';
            return;
        }

        var text = getTranslatableText();
        if (!text) {
            status.textContent = 'There is no text to translate.';
            return;
        }

        var koreanSource = hasMostlyKorean(text);
        var model = koreanSource ? MODEL_KO_EN : MODEL_EN_KO;
        var targetName = koreanSource ? 'English' : 'Korean';
        var chunks = splitText(text);
        button.disabled = true;

        try {
            var translate = await getTranslator(model);
            var results = [];
            for (var i = 0; i < chunks.length; i++) {
                status.textContent = 'Translating… ' + (i + 1) + '/' + chunks.length;
                var result = await translate(chunks[i]);
                results.push(result[0].translation_text);
            }
            renderTranslation(results, targetName);
            translated = true;
            button.setAttribute('aria-label', 'Hide translation');
            button.title = 'Hide translation';
            button.setAttribute('aria-pressed', 'true');
            status.textContent = '';
        } catch (error) {
            console.error('Post translation failed', error);
            delete translatorCache[model];
            status.textContent = 'Translation could not be loaded. Check your connection and try again.';
        } finally {
            button.disabled = false;
        }
    });
})();

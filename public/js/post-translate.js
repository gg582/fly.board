(function() {
    'use strict';

    var button = document.getElementById('post-translate');
    var targetSelect = document.getElementById('post-translate-target');
    var source = document.querySelector('article .markdown-body');
    var output = document.getElementById('post-translation');
    var status = document.getElementById('translation-msg');
    if (!button || !targetSelect || !source || !output) return;

    var TRANSLATORS_JS_URL = 'https://cdn.jsdelivr.net/npm/@huggingface/transformers@3.8.1/dist/transformers.min.js';
    var MODEL_KO_EN = 'Xenova/opus-mt-ko-en';
    var MODEL_EN_KO = 'Xenova/opus-mt-en-ko';
    var translatorCache = {};
    var state = 'idle';
    var renderedTarget = '';

    function setState(next) {
        state = next;
        var busy = next === 'loading' || next === 'translating';
        button.disabled = busy;
        targetSelect.disabled = busy;
        button.dataset.translationState = next;
    }

    function setTranslateButtonVisible(visible) {
        button.setAttribute('aria-pressed', String(visible));
        button.setAttribute('aria-label', visible ? 'Hide translation' : 'Translate');
        button.title = visible ? 'Hide translation' : 'Translate';
    }

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

    function targetName(target) {
        return target === 'ko' ? 'Korean' : 'English';
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
                /* Persist downloaded model files in the browser Cache API so
                 * later page loads reuse them instead of downloading again. */
                module.env.useBrowserCache = true;
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
        if (state === 'loading' || state === 'translating') return;

        var target = targetSelect.value;
        if (state === 'rendered' && renderedTarget === target) {
            output.hidden = !output.hidden;
            setTranslateButtonVisible(!output.hidden);
            status.textContent = '';
            return;
        }

        var text = getTranslatableText();
        if (!text) {
            status.textContent = 'There is no text to translate.';
            return;
        }

        var koreanSource = hasMostlyKorean(text);
        if ((koreanSource && target === 'ko') || (!koreanSource && target === 'en')) {
            output.hidden = true;
            setTranslateButtonVisible(false);
            setState('idle');
            status.textContent = 'The post already appears to be in ' + targetName(target) + '.';
            return;
        }
        var model = target === 'en' ? MODEL_KO_EN : MODEL_EN_KO;
        var chunks = splitText(text);
        setState('loading');

        try {
            var translate = await getTranslator(model);
            setState('translating');
            var results = [];
            for (var i = 0; i < chunks.length; i++) {
                status.textContent = 'Translating… ' + (i + 1) + '/' + chunks.length;
                var result = await translate(chunks[i]);
                results.push(result[0].translation_text);
            }
            renderTranslation(results, targetName(target));
            renderedTarget = target;
            setTranslateButtonVisible(true);
            setState('rendered');
            status.textContent = '';
        } catch (error) {
            console.error('Post translation failed', error);
            delete translatorCache[model];
            setState('error');
            status.textContent = 'Translation could not be loaded. Check your connection and try again.';
        }
    });

    targetSelect.addEventListener('change', function() {
        if (state !== 'rendered' || targetSelect.value === renderedTarget) return;
        output.hidden = true;
        renderedTarget = '';
        setTranslateButtonVisible(false);
        setState('idle');
        status.textContent = '';
    });

    var initialText = getTranslatableText();
    if (initialText) targetSelect.value = hasMostlyKorean(initialText) ? 'en' : 'ko';
    setState('idle');
})();

(function() {
    'use strict';

    var button = document.getElementById('post-translate');
    var targetSelect = document.getElementById('post-translate-target');
    var source = document.querySelector('article .markdown-body');
    var output = document.getElementById('post-translation');
    var status = document.getElementById('translation-msg');
    if (!button || !targetSelect || !source || !output || !window.Worker) return;

    var state = 'idle';
    var renderedTarget = '';
    var requestId = 0;
    var activeRequestId = 0;
    var worker = null;
    var displayNames = typeof Intl !== 'undefined' && Intl.DisplayNames
        ? new Intl.DisplayNames(['en'], {type: 'language'}) : null;

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

    function detectSourceLanguage(text) {
        if (/[\uac00-\ud7a3]/.test(text)) return 'kor_Hang';
        if (/[\u3040-\u30ff]/.test(text)) return 'jpn_Jpan';
        if (/[\u4e00-\u9fff]/.test(text)) return 'zho_Hans';
        if (/[\u0400-\u04ff]/.test(text)) return 'rus_Cyrl';
        if (/[\u0600-\u06ff]/.test(text)) return 'arb_Arab';
        if (/[\u0900-\u097f]/.test(text)) return 'hin_Deva';
        return 'eng_Latn';
    }

    function languageLabel(code) {
        var language = code.slice(0, 3);
        var name = language;
        if (displayNames) {
            try {
                name = displayNames.of(language) || language;
            } catch (error) {
                name = language;
            }
        }
        return name + ' — ' + code;
    }

    function populateTargetLanguages(codes) {
        if (!Array.isArray(codes) || codes.length === 0) return;
        var selected = targetSelect.value;
        var options = codes.slice().sort(function(a, b) {
            return languageLabel(a).localeCompare(languageLabel(b));
        });
        targetSelect.textContent = '';
        options.forEach(function(code) {
            var option = document.createElement('option');
            option.value = code;
            option.textContent = languageLabel(code);
            targetSelect.appendChild(option);
        });
        if (options.indexOf(selected) >= 0) targetSelect.value = selected;
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

    function renderTranslation(parts, target) {
        output.textContent = '';
        var heading = document.createElement('h2');
        heading.textContent = 'Machine translation (' + languageLabel(target) + ')';
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

    function startWorker() {
        worker = new Worker('/assets/js/post-translate-worker.js?v=1');
        worker.addEventListener('message', function(event) {
            var message = event.data || {};
            if (message.type === 'languages') {
                populateTargetLanguages(message.codes || []);
                return;
            }
            if (message.requestId !== activeRequestId) return;
            if (message.type === 'progress') {
                status.textContent = message.text;
            } else if (message.type === 'model-ready') {
                setState('translating');
            } else if (message.type === 'done') {
                renderTranslation(message.parts || [], message.target);
                renderedTarget = message.target;
                setTranslateButtonVisible(true);
                setState('rendered');
                status.textContent = '';
            } else if (message.type === 'error') {
                setState('error');
                status.textContent = 'Translation could not be loaded. Check your connection and try again.';
            }
        });
        worker.addEventListener('error', function() {
            setState('error');
            status.textContent = 'Translation worker could not be started.';
        });
        worker.postMessage({type: 'languages'});
    }

    button.addEventListener('click', function() {
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
        var sourceLanguage = detectSourceLanguage(text);
        if (sourceLanguage === target) {
            output.hidden = true;
            setTranslateButtonVisible(false);
            setState('idle');
            status.textContent = 'The post already appears to be in the selected language.';
            return;
        }

        activeRequestId = ++requestId;
        setState('loading');
        status.textContent = 'Loading translation model…';
        worker.postMessage({
            type: 'translate',
            requestId: activeRequestId,
            sourceLanguage: sourceLanguage,
            target: target,
            chunks: splitText(text)
        });
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
    if (initialText) targetSelect.value = detectSourceLanguage(initialText) === 'kor_Hang' ? 'eng_Latn' : 'kor_Hang';
    setState('idle');
    startWorker();
})();

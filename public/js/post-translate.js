(function() {
    'use strict';

    var button = document.getElementById('post-translate');
    var targetSelect = document.getElementById('post-translate-target');
    var source = document.querySelector('article .markdown-body');
    var output = document.getElementById('post-translation');
    var status = document.getElementById('translation-msg');
    if (!button || !targetSelect || !source || !output || !window.fetch) return;

    var state = 'idle';
    var renderedTarget = '';
    var requestId = 0;
    var activeRequestId = 0;
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
        note.textContent = 'Machine-translated by the site translation service.';
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
        var sourceLanguage = detectSourceLanguage(text);
        if (sourceLanguage === target) {
            output.hidden = true;
            setTranslateButtonVisible(false);
            setState('idle');
            status.textContent = 'The post already appears to be in the selected language.';
            return;
        }

        activeRequestId = ++requestId;
        setState('translating');
        status.textContent = 'Translating…';
        var controller = typeof AbortController !== 'undefined' ? new AbortController() : null;
        var timeout = controller ? setTimeout(function() { controller.abort(); }, 60000) : null;
        try {
            var response = await fetch('/api/translate', {
                method: 'POST',
                headers: {'Content-Type': 'application/json'},
                signal: controller ? controller.signal : undefined,
                body: JSON.stringify({source: sourceLanguage, target: target, chunks: splitText(text)})
            });
            var result = await response.json();
            if (!response.ok || !result.ok || !Array.isArray(result.parts)) {
                throw new Error(result.error || 'Translation request failed (' + response.status + ')');
            }
            if (activeRequestId !== requestId) return;
            renderTranslation(result.parts, target);
            renderedTarget = target;
            setTranslateButtonVisible(true);
            setState('rendered');
            status.textContent = '';
        } catch (error) {
            console.error('Post translation failed:', error);
            setState('error');
            status.textContent = 'Translation is temporarily unavailable. Please try again.';
        } finally {
            if (timeout) clearTimeout(timeout);
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
    if (initialText) targetSelect.value = detectSourceLanguage(initialText) === 'kor_Hang' ? 'eng_Latn' : 'kor_Hang';
    setState('idle');
})();

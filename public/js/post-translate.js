(function() {
    'use strict';

    var button = document.getElementById('post-translate');
    var targetSelect = document.getElementById('post-translate-target');
    var source = document.querySelector('article .markdown-body');
    var status = document.getElementById('translation-msg');
    var postTitleNode = document.querySelector('article h1, header h1, .post-header h1, h1');
    if (!button || !targetSelect || !source || !window.fetch) return;

    var state = 'idle';
    var renderedTarget = '';
    var requestId = 0;
    var activeRequestId = 0;
    var originalBlocks = [];

    function setState(next) {
        state = next;
        var busy = next === 'loading' || next === 'translating';
        button.disabled = busy;
        targetSelect.disabled = busy;
        button.dataset.translationState = next;
    }

    function setTranslateButtonVisible(visible) {
        button.setAttribute('aria-pressed', String(visible));
        button.setAttribute('aria-label', visible ? 'Show original text' : 'Translate');
        button.title = visible ? 'Show original text' : 'Translate';
    }

    function detectSourceLanguage(text) {
        if (/[\uac00-\ud7a3]/.test(text)) return 'ko';
        if (/[\u3040-\u30ff]/.test(text)) return 'ja';
        if (/[\u4e00-\u9fff]/.test(text)) return 'zh';
        if (/[\u0400-\u04ff]/.test(text)) return 'ru';
        if (/[\u0600-\u06ff]/.test(text)) return 'ar';
        if (/[\u0900-\u097f]/.test(text)) return 'hi';
        return 'en';
    }

    function getTranslatableBlocks() {
        var blocks = [];
        var skipTags = ['pre', 'script', 'style', 'table', 'blockquote', 'details'];

        if (postTitleNode) {
            var titleClone = postTitleNode.cloneNode(true);
            titleClone.querySelectorAll('code, .math-inline, .katex').forEach(function(el) { el.remove(); });
            var titleText = (titleClone.innerText || titleClone.textContent || '').trim();
            if (titleText.length > 0) {
                blocks.push({ element: postTitleNode, originalHtml: postTitleNode.innerHTML, text: titleText });
            }
        }

        function walk(node) {
            if (node.nodeType !== Node.ELEMENT_NODE) return;
            var tag = node.tagName.toLowerCase();
            if (skipTags.indexOf(tag) !== -1 ||
                node.classList.contains('math-block') || node.classList.contains('tikz-block')) {
                return;
            }

            if (/^h[1-6]$/.test(tag) || tag === 'p' || tag === 'li') {
                var clone = node.cloneNode(true);
                clone.querySelectorAll('code, .math-inline, .katex').forEach(function(el) { el.remove(); });
                var text = (clone.innerText || clone.textContent || '').trim();
                if (text.length > 0) {
                    blocks.push({ element: node, originalHtml: node.innerHTML, text: text });
                }
                return;
            }

            node.childNodes.forEach(function(child) {
                walk(child);
            });
        }

        source.childNodes.forEach(function(child) {
            walk(child);
        });

        return blocks;
    }

    function restoreOriginal() {
        originalBlocks.forEach(function(item) {
            item.element.innerHTML = item.originalHtml;
            item.element.style.opacity = '1';
        });
    }

    button.addEventListener('click', async function() {
        if (state === 'loading' || state === 'translating') return;
        var target = targetSelect.value;
        if (state === 'rendered' && renderedTarget === target) {
            restoreOriginal();
            renderedTarget = '';
            setTranslateButtonVisible(false);
            setState('idle');
            status.textContent = '';
            return;
        }

        var blocks = getTranslatableBlocks();
        if (blocks.length === 0) {
            status.textContent = 'There is no text to translate.';
            return;
        }

        var fullText = blocks.map(function(b) { return b.text; }).join(' ');
        var sourceLanguage = detectSourceLanguage(fullText);
        if (sourceLanguage === target) {
            setState('idle');
            status.textContent = 'The post already appears to be in the selected language.';
            setTimeout(function() {
                if (status.textContent === 'The post already appears to be in the selected language.') {
                    status.textContent = '';
                }
            }, 3000);
            return;
        }

        activeRequestId = ++requestId;
        var currentRequestId = activeRequestId;
        setState('translating');
        status.textContent = 'Translating…';

        originalBlocks = blocks;
        blocks.forEach(function(b) {
            b.element.style.opacity = '0.5';
        });

        var controller = typeof AbortController !== 'undefined' ? new AbortController() : null;
        var timeout = controller ? setTimeout(function() { controller.abort(); }, 60000) : null;
        try {
            var batchSize = 6;
            for (var i = 0; i < blocks.length; i += batchSize) {
                if (activeRequestId !== currentRequestId) return;
                var batchBlocks = blocks.slice(i, i + batchSize);
                var chunks = batchBlocks.map(function(b) { return b.text; });
                var response = await fetch('/api/translate', {
                    method: 'POST',
                    headers: {'Content-Type': 'application/json'},
                    signal: controller ? controller.signal : undefined,
                    body: JSON.stringify({source: sourceLanguage, target: target, chunks: chunks})
                });
                var result = await response.json();
                if (!response.ok || !result.ok || !Array.isArray(result.parts)) {
                    throw new Error(result.error || 'Translation request failed (' + response.status + ')');
                }
                if (activeRequestId !== currentRequestId) return;
                for (var j = 0; j < result.parts.length; j++) {
                    var idx = i + j;
                    if (blocks[idx]) {
                        var targetNode = blocks[idx].element;
                        targetNode.textContent = result.parts[j];
                        targetNode.style.opacity = '1';
                    }
                }
            }

            if (activeRequestId !== currentRequestId) return;
            renderedTarget = target;
            setTranslateButtonVisible(true);
            setState('rendered');
            status.textContent = '';
        } catch (error) {
            console.error('Post translation failed:', error);
            restoreOriginal();
            setState('error');
            status.textContent = 'Translation is temporarily unavailable. Please try again.';
            setTimeout(function() {
                if (state === 'error') {
                    status.textContent = '';
                    setState('idle');
                }
            }, 4000);
        } finally {
            if (timeout) clearTimeout(timeout);
        }
    });

    targetSelect.addEventListener('change', function() {
        if (state !== 'rendered' || targetSelect.value === renderedTarget) return;
        restoreOriginal();
        renderedTarget = '';
        setTranslateButtonVisible(false);
        setState('idle');
        status.textContent = '';
    });

    var firstBlocks = getTranslatableBlocks();
    if (firstBlocks.length > 0) {
        var sampleText = firstBlocks.map(function(b) { return b.text; }).join(' ');
        targetSelect.value = detectSourceLanguage(sampleText) === 'ko' ? 'en' : 'ko';
    }
    setState('idle');
})();

(function(){
    /* Normalize setext-style runs accidentally pasted into protected LaTeX
     * and TikZ source. This also repairs already-rendered post markup. */
    function normalizeStandaloneEquals(source) {
        return String(source || '').replace(/^[ \t]*={2,}[ \t]*$/gm, '=');
    }

    /* TikZJax exposes its compiler as window.onload.  Posts and the editor
     * load this file after DOMContentLoaded, so relying on the normal onload
     * event leaves newly inserted diagrams unprocessed.  Keep one shared
     * promise and explicitly invoke the compiler once the runtime is ready. */
    function loadTikZJax() {
        if (window.__flyboardTikzJaxReady) return window.__flyboardTikzJaxReady;

        window.__flyboardTikzJaxReady = new Promise(function(resolve, reject) {
            var existing = document.getElementById('tikzjax-script');
            if (existing) {
                if (existing.dataset.loaded === '1') {
                    resolve();
                } else {
                    existing.addEventListener('load', function(){ resolve(); }, {once: true});
                    existing.addEventListener('error', function(){ reject(new Error('TikZJax failed to load')); }, {once: true});
                }
                return;
            }

        var css = document.createElement('link');
        css.rel = 'stylesheet';
        /* TikZJax was unpublished from npm, so the old jsDelivr URL returns
         * 404 and leaves every diagram as its source text.  Use TikZJax's
         * official, versioned distribution instead. */
        css.href = 'https://tikzjax.com/v1/fonts.css';
        document.head.appendChild(css);

        var script = document.createElement('script');
        script.id = 'tikzjax-script';
        script.src = 'https://tikzjax.com/v1/tikzjax.js';
        script.onload = function() {
            /* The official runtime exposes no public process() function. It
             * installs its compiler as window.onload, which never fires when
             * the runtime is loaded later from Preview. Preserve that function
             * and invoke it after inserting TikZ source nodes below. */
            if (typeof window.onload === 'function') {
                window.__flyboardTikzJaxProcess = window.onload;
            }
            script.dataset.loaded = '1';
            resolve();
        };
        script.onerror = function() { reject(new Error('TikZJax failed to load')); };
        document.head.appendChild(script);
        });

        /* A failed network request must not poison every later render. */
        window.__flyboardTikzJaxReady.catch(function(){
            window.__flyboardTikzJaxReady = null;
        });
        return window.__flyboardTikzJaxReady;
    }

    function processTikZJax() {
        if (typeof window.__flyboardTikzJaxProcess !== 'function') return;
        try { window.__flyboardTikzJaxProcess.call(window); } catch (e) {
            document.querySelectorAll('.tikz-render[data-tikz-state="pending"]').forEach(function(el){
                markTikZFailure(el, e);
            });
        }
    }

    var TIKZ_RETRY_DELAY_MS = 6000;
    var TIKZ_MAX_RETRIES = 3;

    function hasTikZOutput(wrapper) {
        return !!(wrapper && wrapper.querySelector('svg'));
    }

    /* TikZJax is the preferred renderer: it keeps the diagram source in the
     * browser.  Some browsers, extensions, and slow WebAssembly starts do
     * fail to complete it though.  After retrying TikZJax, ask the public
     * latexonline.cc compiler for a one-page PDF.  An <object> avoids CORS
     * requirements and lets the browser render the returned PDF directly. */
    function publicTikZDocument(code) {
        if (code.includes('\\begin{document}')) return code;
        /* Libraries and packages are preamble commands. Pull line-oriented
         * declarations out before opening the document so diagrams authored
         * with \usetikzlibrary continue to work in the public fallback. */
        var preamble = [];
        code = code.replace(/^\s*(\\(?:usepackage(?:\[[^\]]*\])?\{[^}]+\}|usetikzlibrary\{[^}]+\}))\s*$/gm,
            function(_, declaration) {
                preamble.push(declaration);
                return '';
            });
        return '\\documentclass[tikz,border=2pt]{standalone}\n' +
               '\\usepackage{tikz}\n' + preamble.join('\n') + '\n\\begin{document}\n' + code +
               '\n\\end{document}';
    }

    function renderWithPublicTikZProvider(wrapper, error) {
        if (!wrapper || hasTikZOutput(wrapper) || wrapper.dataset.tikzProviderRequested === '1') return;
        var script = wrapper.querySelector('script[type="text/tikz"]');
        var rawElement = script || wrapper.querySelector('.tikz-block, code.language-tikz, div.tikz');
        var source = rawElement && (rawElement.getAttribute('data-raw-tikz') || rawElement.textContent);
        if (!source || source.length > 12000) {
            markTikZFailure(wrapper, error || 'TikZ source is unavailable or too large for the public fallback');
            return;
        }

        wrapper.dataset.tikzProviderRequested = '1';
        wrapper.dataset.tikzState = 'public-pending';
        var notice = document.createElement('div');
        notice.className = 'tikz-status';
        notice.textContent = 'Retrying with public TikZ renderer…';
        wrapper.insertBefore(notice, wrapper.firstChild);

        var object = document.createElement('object');
        object.className = 'tikz-public-render';
        object.type = 'application/pdf';
        object.setAttribute('aria-label', 'TikZ diagram rendered by public LaTeX service');
        object.data = 'https://latexonline.cc/compile?command=pdflatex&text=' +
            encodeURIComponent(publicTikZDocument(prepareTikZCode(source)));
        object.onload = function() {
            notice.remove();
            wrapper.dataset.tikzState = 'public-rendered';
        };
        object.onerror = function() {
            object.remove();
            notice.remove();
            markTikZFailure(wrapper, error || 'public TikZ provider failed');
        };
        wrapper.appendChild(object);
    }

    function markTikZFailure(wrapper, error) {
        if (!wrapper || wrapper.querySelector('svg')) return;
        wrapper.dataset.tikzState = 'error';
        wrapper.setAttribute('role', 'img');
        wrapper.setAttribute('aria-label', 'TikZ diagram could not be rendered');
        var message = wrapper.querySelector('.tikz-error');
        if (!message) {
            message = document.createElement('div');
            message.className = 'tikz-error';
            message.textContent = 'Unable to render TikZ diagram.';
            wrapper.insertBefore(message, wrapper.firstChild);
        }
        if (window.console && console.warn) console.warn('TikZJax render failed', error || 'unknown error');
    }

    function retryTikZRender(wrapper, attempt) {
        if (!wrapper || hasTikZOutput(wrapper) || wrapper.dataset.tikzProviderRequested === '1') return;
        if (attempt >= TIKZ_MAX_RETRIES) {
            renderWithPublicTikZProvider(wrapper, 'TikZJax timed out after retries');
            return;
        }
        setTimeout(function() {
            if (!wrapper || hasTikZOutput(wrapper) || wrapper.dataset.tikzProviderRequested === '1') return;
            var script = wrapper.querySelector('script[type="text/tikz"]');
            if (script) {
                /* TikZJax marks source nodes as processed. Reinsert a fresh
                 * node so its onload compiler sees this retry as new input. */
                var fresh = document.createElement('script');
                fresh.type = 'text/tikz';
                fresh.textContent = script.textContent;
                Array.prototype.slice.call(script.attributes).forEach(function(attr) {
                    fresh.setAttribute(attr.name, attr.value);
                });
                script.parentNode.replaceChild(fresh, script);
            }
            processTikZJax();
            retryTikZRender(wrapper, attempt + 1);
        }, TIKZ_RETRY_DELAY_MS);
    }

    function prepareTikZCode(code) {
        var trimmed = normalizeStandaloneEquals(code).trim();
        if (!trimmed.includes('\\begin{document}') && !trimmed.includes('\\begin{tikzpicture}')) {
            return '\\begin{tikzpicture}\n' + trimmed + '\n\\end{tikzpicture}';
        }
        return trimmed;
    }

    function themeTikZDiagram(wrapper) {
        if (!wrapper) return;
        var black = /^(#000(?:000)?|black|rgb\(0\s*,\s*0\s*,\s*0\s*\))$/i;
        var applyTheme = function() {
            wrapper.querySelectorAll('svg [fill], svg [stroke]').forEach(function(node) {
                ['fill', 'stroke'].forEach(function(attribute) {
                    var value = node.getAttribute(attribute);
                    if (value && black.test(value.trim())) {
                        node.setAttribute(attribute, 'var(--tikz-ink)');
                    }
                });
                var style = node.getAttribute('style');
                if (style) {
                    node.setAttribute('style', style
                        .replace(/(fill|stroke)\s*:\s*(#000(?:000)?|black|rgb\(0\s*,\s*0\s*,\s*0\s*\))/gi,
                            '$1:var(--tikz-ink)'));
                }
            });
        };

        applyTheme();
        /* TikZJax can append the SVG after its compiler callback returns.
         * Disconnect once the diagram is in place; a live observer per
         * diagram would keep firing on every unrelated DOM change. */
        var observer = new MutationObserver(function() {
            applyTheme();
            if (wrapper.querySelector('svg')) {
                var publicRender = wrapper.querySelector('.tikz-public-render');
                if (publicRender) publicRender.remove();
                var status = wrapper.querySelector('.tikz-status');
                if (status) status.remove();
                wrapper.dataset.tikzState = 'rendered';
                observer.disconnect();
            }
        });
        observer.observe(wrapper, {childList: true, subtree: true});
    }

    function renderBlogMath(elem){
        if (!elem) return;
        if (typeof katex !== 'undefined') {
            elem.querySelectorAll('.math-block').forEach(function(el){
                if (!el.hasAttribute('data-raw-math')) el.setAttribute('data-raw-math', normalizeStandaloneEquals(el.textContent));
                try { katex.render(el.getAttribute('data-raw-math') || '', el, {throwOnError: false, displayMode: true}); } catch(e) {}
            });
            elem.querySelectorAll('.math-inline').forEach(function(el){
                if (!el.hasAttribute('data-raw-math')) el.setAttribute('data-raw-math', normalizeStandaloneEquals(el.textContent));
                try { katex.render(el.getAttribute('data-raw-math') || '', el, {throwOnError: false, displayMode: false}); } catch(e) {}
            });
        }
        var tikzElements = elem.querySelectorAll('.tikz-block, code.language-tikz, script[type="text/tikz"], div.tikz');
        if (tikzElements.length > 0) {
            loadTikZJax().then(function(){
                tikzElements.forEach(function(el){
                    if (el.dataset.tikzProcessed === '1') return;
                    var code = prepareTikZCode(el.textContent);
                    var dataPackages = el.getAttribute('data-has-packages') === 'true';
                    var pre = (el.tagName.toLowerCase() === 'code') ? el.parentElement : null;

                    var script = document.createElement('script');
                    script.type = 'text/tikz';
                    script.setAttribute('data-raw-tikz', el.textContent || code);
                    if (dataPackages || code.includes('\\usetikzlibrary') || code.includes('\\usepackage')) {
                        script.setAttribute('data-has-packages', 'true');
                    }
                    script.textContent = code;

                    var targetNode = (pre && pre.tagName.toLowerCase() === 'pre') ? pre : el;
                    if (targetNode.parentNode) {
                        var wrapper = document.createElement('div');
                        wrapper.className = 'tikz-render';
                        wrapper.dataset.tikzState = 'pending';
                        el.dataset.tikzProcessed = '1';
                        targetNode.parentNode.replaceChild(wrapper, targetNode);
                        wrapper.appendChild(script);
                        themeTikZDiagram(wrapper);
                    }
                });
                processTikZJax();
                /* Retry each diagram independently. A large post therefore
                 * does not make already-finished diagrams wait for one flat,
                 * document-wide timeout. */
                document.querySelectorAll('.tikz-render[data-tikz-state="pending"]').forEach(function(el){
                    retryTikZRender(el, 0);
                });
            }).catch(function(error){
                tikzElements.forEach(function(el){
                    var wrapper = el.closest ? el.closest('.tikz-render') : null;
                    if (wrapper) {
                        renderWithPublicTikZProvider(wrapper, error);
                    } else if (el.parentNode) {
                        /* Keep a visible diagnostic when the runtime itself
                         * cannot be downloaded (CSP/offline/blocked CDN). */
                        var fallback = document.createElement('div');
                        fallback.className = 'tikz-render';
                        fallback.dataset.tikzState = 'error';
                        fallback.setAttribute('role', 'img');
                        fallback.setAttribute('aria-label', 'TikZ diagram could not be rendered');
                        var message = document.createElement('div');
                        message.className = 'tikz-error';
                        message.textContent = 'Unable to render TikZ diagram.';
                        fallback.appendChild(message);
                        fallback.appendChild(el.cloneNode(true));
                        el.parentNode.replaceChild(fallback, el);
                        renderWithPublicTikZProvider(fallback, error);
                    }
                });
            });
        }
    }
    if (document.readyState === 'loading') {
        document.addEventListener('DOMContentLoaded', function(){ renderBlogMath(document.body); });
    } else {
        renderBlogMath(document.body);
    }
    window.__renderBlogMath = renderBlogMath;
})();

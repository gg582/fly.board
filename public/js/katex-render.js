(function(){
    /* Collapse dragged-out operators in protected LaTeX and TikZ source.
     * Unicode minus and ASCII hyphens can be mixed by pasted text. */
    function normalizeOperatorRuns(source) {
        return String(source || '')
            .replace(/={2,}/g, '=')
            .replace(/[\u2212-]{2,}/g, '-');
    }

    /* TikZ path syntax gives meaning to operator runs that math normalizes
     * away: '--' is the segment connector.  Collapsing hyphen runs produced
     * '- (2,0)' and every compiler rejected the path, so TikZ source only
     * gets the Unicode minus unified and '=' runs collapsed. */
    function normalizeTikZOperators(source) {
        return String(source || '')
            .replace(/−/g, '-')
            .replace(/={2,}/g, '=');
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

    function hasTikZOutput(wrapper) {
        return !!(wrapper && wrapper.querySelector('svg'));
    }

    /* TikZJax is the preferred renderer: it keeps the diagram source in the
     * browser.  Some browsers, extensions, and slow WebAssembly starts do
     * fail to complete it though, and its minimal engine lacks popular TikZ
     * dialect packages.  Those diagrams are handed to public compile engines
     * (latexonline.cc, texlive.net) which return a one-page PDF.  An <object>
     * avoids CORS requirements and lets the browser render the PDF directly. */
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

    /* Public compile engines, tried in order and cycled indefinitely when
     * one flakes.  Each takes a full LaTeX document and returns a one-page
     * PDF renderable by the browser via an <object> (no CORS needed). */
    var TIKZ_PUBLIC_PROVIDERS = [
        function(doc) {
            return 'https://latexonline.cc/compile?command=pdflatex&text=' + encodeURIComponent(doc);
        },
        function(doc) {
            return 'https://texlive.net/cgi-bin/latexcgi?command=pdflatex&return=pdf' +
                '&filename[]=document.tex&filecontents[]=' + encodeURIComponent(doc);
        },
        function(doc) {
            return 'https://latexonline.cc/compile?command=xelatex&text=' + encodeURIComponent(doc);
        }
    ];

    function renderWithPublicTikZProvider(wrapper, error) {
        if (!wrapper || hasTikZOutput(wrapper) || wrapper.dataset.tikzProviderRequested === '1') return;
        var script = wrapper.querySelector('script[type="text/tikz"]');
        var rawElement = script || wrapper.querySelector('.tikz-block, code.language-tikz, div.tikz');
        var source = rawElement && (rawElement.getAttribute('data-raw-tikz') || rawElement.textContent);
        if (!source || source.length > 12000) {
            markTikZFailure(wrapper, error || 'TikZ source is unavailable or too large for the public fallback');
            return;
        }

        var providerIndex = parseInt(wrapper.dataset.tikzProviderIndex || '0', 10) || 0;
        var makeUrl = TIKZ_PUBLIC_PROVIDERS[providerIndex % TIKZ_PUBLIC_PROVIDERS.length];
        wrapper.dataset.tikzProviderIndex = String(providerIndex + 1);

        wrapper.dataset.tikzProviderRequested = '1';
        wrapper.dataset.tikzState = 'public-pending';
        var notice = wrapper.querySelector('.tikz-status');
        if (!notice) {
            notice = document.createElement('div');
            notice.className = 'tikz-status';
            wrapper.insertBefore(notice, wrapper.firstChild);
        }
        notice.textContent = 'Retrying with public TikZ renderer…';

        var object = document.createElement('object');
        object.className = 'tikz-public-render';
        object.type = 'application/pdf';
        object.setAttribute('aria-label', 'TikZ diagram rendered by public LaTeX service');
        object.data = makeUrl(publicTikZDocument(prepareTikZCode(source)));
        object.onload = function() {
            notice.remove();
            wrapper.dataset.tikzState = 'public-rendered';
        };
        object.onerror = function() {
            object.remove();
            /* One engine flaking says nothing about the others; move to the
             * next provider and keep cycling until something renders. */
            wrapper.dataset.tikzProviderRequested = '0';
            wrapper.dataset.tikzState = 'pending';
            setTimeout(function() {
                renderWithPublicTikZProvider(wrapper, error);
            }, TIKZ_RETRY_DELAY_MS);
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

    /* Retry TikZJax indefinitely for plain diagrams: slow WebAssembly starts
     * or flaky runs eventually succeed, and each attempt is cheap.  Diagrams
     * that need packages TikZJax's minimal engine lacks (tikz-cd, pgfplots,
     * circuitikz, ...) never succeed there, so after a few attempts hand
     * them to the public engine chain, which cycles providers forever. */
    var TIKZJAX_DIALECT_ATTEMPTS = 3;

    function retryTikZRender(wrapper, attempt) {
        if (!wrapper || hasTikZOutput(wrapper) || wrapper.dataset.tikzProviderRequested === '1') return;
        var current = wrapper.querySelector('script[type="text/tikz"]');
        if (current && current.getAttribute('data-has-packages') === 'true' && attempt >= TIKZJAX_DIALECT_ATTEMPTS) {
            renderWithPublicTikZProvider(wrapper, 'TikZJax cannot compile this TikZ dialect');
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

    /* Well-known TikZ dialects need their backing package in the preamble.
     * Detect the idioms and declare the package when the author omitted it,
     * so plain ```tikz blocks of tikzcd/pgfplots/circuitikz/chemfig/...
     * compile without extra boilerplate. */
    var TIKZ_DIALECT_PACKAGES = [
        { test: /\\begin\s*\{tikzcd\}|\\arrow\s*[\[\{]/, name: 'tikz-cd', decl: '\\usepackage{tikz-cd}' },
        { test: /\\begin\s*\{axis\}|\\addplot\b/, name: 'pgfplots', decl: '\\usepackage{pgfplots}\n\\pgfplotsset{compat=1.17}' },
        { test: /\\begin\s*\{circuitikz\}|\\draw[^\n]*\bto\s*\[\s*(?:R|C|L|D|led|battery|voltage|current|short|open)\b/, name: 'circuitikz', decl: '\\usepackage{circuitikz}' },
        { test: /\\chemfig\b/, name: 'chemfig', decl: '\\usepackage{chemfig}' },
        { test: /\\tdplot/, name: 'tikz-3dplot', decl: '\\usepackage{tikz-3dplot}' },
        { test: /\\begin\s*\{forest\}/, name: 'forest', decl: '\\usepackage{forest}' },
        { test: /\\begin\s*\{ganttchart\}/, name: 'pgfgantt', decl: '\\usepackage{pgfgantt}' },
        { test: /\\begin\s*\{pgflowchart|flowchart/, name: 'tikz flowchart lib', decl: '\\usetikzlibrary{shapes,arrows,positioning}' },
        { test: /\\tkz(?:DefPoint|DrawSegment|DrawLine|DrawCircle|LabelPoint|MarkAngle|FillAngle)/, name: 'tkz-euclide', decl: '\\usepackage{tkz-euclide}' },
        { test: /\\pgfplotstable/, name: 'pgfplotstable', decl: '\\usepackage{pgfplotstable}' },
        { test: /\\mindmap\b|\bconcept\s*(?:color|\[)/, name: 'mindmap', decl: '\\usetikzlibrary{mindmap,trees}' },
        { test: /matrix\s+of\s+(?:math\s+)?nodes/, name: 'matrix', decl: '\\usetikzlibrary{matrix}' },
        { test: /\\node\s*\[[^\]]*\b(?:state|accepting)\b/, name: 'automata', decl: '\\usetikzlibrary{automata,positioning}' },
        { test: /\$\([^)]*\)\s*!|calc\b[^\n]*\$\(/, name: 'calc', decl: '\\usetikzlibrary{calc}' },
        { test: /\\node\s*\[[^\]]*\b(?:alice|bob|businessman|criminal|devil|duck|maninblack|sailor|shield|santa)\b/, name: 'tikzpeople', decl: '\\usepackage{tikzpeople}' },
        { test: /\\chessboard\b|\\setchessboard\b/, name: 'xskak', decl: '\\usepackage{xskak}' }
    ];

    function normalizeTikZDialects(code) {
        var out = String(code || '');
        /* Collect the packages/libraries the author already declared and
         * compare against those, not against the dialect name: a diagram
         * that merely mentions "matrix" must not suppress the injection. */
        var declared = {};
        var declPattern = /\\(?:usepackage(?:\[[^\]]*\])?|usetikzlibrary)\s*\{([^}]+)\}/g;
        var match;
        while ((match = declPattern.exec(out)) !== null) {
            match[1].split(',').forEach(function(token) {
                declared[token.trim()] = true;
            });
        }
        TIKZ_DIALECT_PACKAGES.forEach(function(dialect) {
            var provides = [];
            var declMatch;
            declPattern.lastIndex = 0;
            while ((declMatch = declPattern.exec(dialect.decl)) !== null) {
                declMatch[1].split(',').forEach(function(token) { provides.push(token.trim()); });
            }
            var alreadyDeclared = provides.length > 0 && provides.every(function(token) { return declared[token]; });
            if (!alreadyDeclared && dialect.test.test(out)) {
                out = dialect.decl + '\n' + out;
            }
        });
        return out;
    }

    function prepareTikZCode(code) {
        var trimmed = normalizeTikZDialects(normalizeTikZOperators(code)).trim();
        if (!trimmed.includes('\\begin{document}') && !trimmed.includes('\\begin{tikzpicture}') &&
            !trimmed.includes('\\begin{tikzcd}') && !trimmed.includes('\\begin{circuitikz}') &&
            !trimmed.includes('\\begin{forest}') && !trimmed.includes('\\chemfig')) {
            /* chemfig and friends live at document level; only bare drawing
             * commands (\draw, \node, ...) get a tikzpicture wrapper. */
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
                var errorMessage = wrapper.querySelector('.tikz-error');
                if (errorMessage) errorMessage.remove();
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
                if (!el.hasAttribute('data-raw-math')) el.setAttribute('data-raw-math', normalizeOperatorRuns(el.textContent));
                try { katex.render(el.getAttribute('data-raw-math') || '', el, {throwOnError: false, displayMode: true}); } catch(e) {}
            });
            elem.querySelectorAll('.math-inline').forEach(function(el){
                if (!el.hasAttribute('data-raw-math')) el.setAttribute('data-raw-math', normalizeOperatorRuns(el.textContent));
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

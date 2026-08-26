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

    /* TikZJax's WebAssembly core and fonts take well over 6s to download and
     * compile on a cold cache, so a short delay here made nearly every diagram
     * fall through to the public providers.  Give each attempt enough room
     * for a slow wasm start before declaring failure. */
    var TIKZ_RETRY_DELAY_MS = 15000;

    /* Lazy PDF.js loader, used only when a diagram falls back to a public
     * provider.  The providers return a one-page PDF; PDF.js renders it to a
     * canvas so the result looks like an inline diagram, not a PDF viewer. */
    function loadPdfJs() {
        if (window.__flyboardPdfJsReady) return window.__flyboardPdfJsReady;
        window.__flyboardPdfJsReady = new Promise(function(resolve, reject) {
            if (window.pdfjsLib) { resolve(window.pdfjsLib); return; }
            var script = document.createElement('script');
            /* 2.16 legacy UMD build: runs under our CSP without a module
             * worker.  The real Worker cannot be constructed from cdnjs
             * (worker-src allows only 'self' and blob:), so pdf.js falls back
             * to its fake worker, which loads the worker code through a
             * regular script tag (allowed by script-src) on the main thread. */
            script.src = 'https://cdnjs.cloudflare.com/ajax/libs/pdf.js/2.16.105/pdf.min.js';
            script.onload = function() {
                var lib = window.pdfjsLib;
                if (!lib) { reject(new Error('PDF.js failed to initialize')); return; }
                lib.GlobalWorkerOptions.workerSrc =
                    'https://cdnjs.cloudflare.com/ajax/libs/pdf.js/2.16.105/pdf.worker.min.js';
                resolve(lib);
            };
            script.onerror = function() { reject(new Error('PDF.js failed to load')); };
            document.head.appendChild(script);
        });
        /* A failed network request must not poison later renders. */
        window.__flyboardPdfJsReady.catch(function(){ window.__flyboardPdfJsReady = null; });
        return window.__flyboardPdfJsReady;
    }

    function hasTikZOutput(wrapper) {
        return !!(wrapper && wrapper.querySelector('svg'));
    }

    /* TikZJax is the preferred renderer: it keeps the diagram source in the
     * browser.  Some browsers, extensions, and slow WebAssembly starts do
     * fail to complete it though, and its minimal engine lacks popular TikZ
     * dialect packages.  Those diagrams are handed to public compile engines
     * (latexonline.cc, texlive.net) which return a one-page PDF that we
     * rasterize onto a canvas with PDF.js. */
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
     * PDF, rendered to a canvas by PDF.js after the fetch. */
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

        var compiledDoc = publicTikZDocument(prepareTikZCode(source));
        var targetUrl = makeUrl(compiledDoc);

        var onPublicFailure = function() {
            var oldRender = wrapper.querySelector('.tikz-public-render');
            if (oldRender) oldRender.remove();
            wrapper.dataset.tikzProviderRequested = '0';
            wrapper.dataset.tikzState = 'pending';
            setTimeout(function() {
                renderWithPublicTikZProvider(wrapper, error);
            }, TIKZ_RETRY_DELAY_MS);
        };

        var onPublicSuccess = function(imgElement) {
            if (notice) notice.remove();
            var err = wrapper.querySelector('.tikz-error');
            if (err) err.remove();
            wrapper.dataset.tikzState = 'public-rendered';
        };

        /* The public providers answer with a one-page PDF, which an <img>
         * cannot decode and an <object> shows as the browser's full PDF
         * viewer (toolbar, page frame and all).  Render the PDF to a canvas
         * with PDF.js so the fallback looks like the diagram image TikZJax
         * would have produced.  PDF.js is loaded lazily, only when a diagram
         * actually falls back. */
        var renderPublicBlob = function(blob) {
            var showImage = function(url, revoke) {
                var img = document.createElement('img');
                img.className = 'tikz-public-render';
                img.alt = 'TikZ diagram rendered by public LaTeX service';
                img.onload = function() {
                    if (revoke) URL.revokeObjectURL(url);
                    onPublicSuccess(img);
                };
                img.onerror = function() {
                    if (revoke) URL.revokeObjectURL(url);
                    img.remove();
                    onPublicFailure();
                };
                img.src = url;
                wrapper.appendChild(img);
            };

            if (blob.type && blob.type.indexOf('image/') === 0) {
                showImage(URL.createObjectURL(blob), true);
                return;
            }

            loadPdfJs().then(function(pdfjsLib) {
                return blob.arrayBuffer().then(function(data) {
                    return pdfjsLib.getDocument({ data: data }).promise;
                });
            }).then(function(pdf) {
                return pdf.getPage(1);
            }).then(function(page) {
                var viewport = page.getViewport({ scale: 2 });
                var canvas = document.createElement('canvas');
                canvas.className = 'tikz-public-render';
                canvas.setAttribute('role', 'img');
                canvas.setAttribute('aria-label', 'TikZ diagram rendered by public LaTeX service');
                canvas.width = viewport.width;
                canvas.height = viewport.height;
                return page.render({
                    canvasContext: canvas.getContext('2d'),
                    viewport: viewport
                }).promise.then(function() {
                    wrapper.appendChild(canvas);
                    onPublicSuccess(canvas);
                });
            }).catch(function() {
                onPublicFailure();
            });
        };

        if (typeof fetch === 'function') {
            fetch(targetUrl)
                .then(function(res) {
                    if (!res.ok) throw new Error('HTTP ' + res.status);
                    return res.blob();
                })
                .then(function(blob) {
                    if (!blob || blob.size === 0) throw new Error('Empty response');
                    renderPublicBlob(blob);
                })
                .catch(function() {
                    onPublicFailure();
                });
        } else {
            /* No fetch API: still need the bytes for PDF.js, so use XHR. */
            var xhr = new XMLHttpRequest();
            xhr.open('GET', targetUrl);
            xhr.responseType = 'blob';
            xhr.onload = function() {
                if (xhr.status >= 200 && xhr.status < 300 && xhr.response && xhr.response.size > 0) {
                    renderPublicBlob(xhr.response);
                } else {
                    onPublicFailure();
                }
            };
            xhr.onerror = function() { onPublicFailure(); };
            xhr.send();
        }
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

    /* Retry TikZJax for plain diagrams: slow WebAssembly starts
     * or flaky runs eventually succeed. Diagrams that need packages
     * or LaTeX constructs TikZJax's minimal engine lacks (tikz-cd, pgfplots,
     * circuitikz, font commands, ...) fall back after a few attempts
     * to the public engine chain, which cycles providers forever. */
    var TIKZJAX_DIALECT_ATTEMPTS = 3;

    function retryTikZRender(wrapper, attempt) {
        if (!wrapper || hasTikZOutput(wrapper) || wrapper.dataset.tikzProviderRequested === '1') return;
        if (attempt >= TIKZJAX_DIALECT_ATTEMPTS) {
            renderWithPublicTikZProvider(wrapper, 'TikZ compilation timed out or dialect unsupported');
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

    var TIKZ_LATEX_SHIMS =
        '\\ifx\\small\\undefined\\def\\small{}\\fi\n' +
        '\\ifx\\footnotesize\\undefined\\def\\footnotesize{}\\fi\n' +
        '\\ifx\\scriptsize\\undefined\\def\\scriptsize{}\\fi\n' +
        '\\ifx\\tiny\\undefined\\def\\tiny{}\\fi\n' +
        '\\ifx\\large\\undefined\\def\\large{}\\fi\n' +
        '\\ifx\\Large\\undefined\\def\\Large{}\\fi\n' +
        '\\ifx\\LARGE\\undefined\\def\\LARGE{}\\fi\n' +
        '\\ifx\\huge\\undefined\\def\\huge{}\\fi\n' +
        '\\ifx\\Huge\\undefined\\def\\Huge{}\\fi\n' +
        '\\ifx\\normalsize\\undefined\\def\\normalsize{}\\fi\n' +
        '\\ifx\\text\\undefined\\def\\text#1{\\hbox{#1}}\\fi\n' +
        '\\ifx\\textbf\\undefined\\def\\textbf#1{{\\bf #1}}\\fi\n' +
        '\\ifx\\textit\\undefined\\def\\textit#1{{\\it #1}}\\fi\n';

    function prepareTikZCode(code) {
        var trimmed = normalizeTikZDialects(normalizeTikZOperators(code)).trim();
        var wrapped = trimmed;
        if (!trimmed.includes('\\begin{document}') && !trimmed.includes('\\begin{tikzpicture}') &&
            !trimmed.includes('\\begin{tikzcd}') && !trimmed.includes('\\begin{circuitikz}') &&
            !trimmed.includes('\\begin{forest}') && !trimmed.includes('\\chemfig')) {
            /* chemfig and friends live at document level; only bare drawing
             * commands (\draw, \node, ...) get a tikzpicture wrapper. */
            wrapped = '\\begin{tikzpicture}\n' + trimmed + '\n\\end{tikzpicture}';
        }
        if (!wrapped.includes('\\ifx\\small\\undefined')) {
            wrapped = TIKZ_LATEX_SHIMS + wrapped;
        }
        return wrapped;
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

    /* --- Overflow fitting for rendered math -------------------------------
     * KaTeX never wraps display math by itself, so a wide formula silently
     * overflows the column.  After rendering we measure the real width and
     * apply two remedies, in order:
     *   1. line-break insertion: split the TeX source at top-level binary
     *      operators (=, +, -, ,) and render the pieces as stacked lines;
     *   2. size adjustment: scale the font down until the block fits.
     * A horizontal scrollbar remains as the last-resort fallback. */

    /* Split points are only meaningful outside braces/ environments, so walk
     * the source tracking brace depth and find top-level operators. */
    function splitMathAtTopLevel(source) {
        var depth = 0;
        var pieces = [];
        var last = 0;
        for (var i = 0; i < source.length; i++) {
            var ch = source[i];
            if (ch === '\\') { i++; continue; } /* skip command names/escaped chars */
            if (ch === '{') depth++;
            else if (ch === '}') depth = Math.max(0, depth - 1);
            else if (depth === 0 && (ch === '=' || ch === '+' || ch === ',')) {
                pieces.push(source.slice(last, i + 1));
                last = i + 1;
            } else if (depth === 0 && (ch === '-' || ch === '\u2212')) {
                /* A leading minus is a sign, not a break point. */
                if (i > last) {
                    pieces.push(source.slice(last, i));
                    last = i;
                }
            }
        }
        pieces.push(source.slice(last));
        return pieces.map(function(p){ return p.trim(); })
                     .filter(function(p){ return p.length > 0; });
    }

    function mathAvailableWidth(el) {
        var parent = el.parentElement;
        if (!parent) return el.clientWidth;
        var style = window.getComputedStyle(parent);
        var padding = parseFloat(style.paddingLeft) + parseFloat(style.paddingRight);
        return Math.max(0, parent.clientWidth - padding);
    }

    function mathOverflows(el) {
        return el.scrollWidth > mathAvailableWidth(el) + 1;
    }

    function renderMathInto(el, tex) {
        katex.render(tex, el, {throwOnError: false, displayMode: true});
    }

    function fitDisplayMath(el) {
        if (typeof katex === 'undefined' || !el) return;
        var raw = el.getAttribute('data-raw-math') || '';
        el.style.fontSize = '';
        el.style.overflowX = '';

        if (!mathOverflows(el)) return;

        /* 1) Try inserting line breaks at top-level operators.  Only accept
         *    the split when every line is narrower than the unsplit block. */
        var pieces = splitMathAtTopLevel(raw);
        if (pieces.length > 1) {
            var originalWidth = el.scrollWidth;
            var lines = pieces.map(function(piece) {
                var line = document.createElement('div');
                line.className = 'math-block-line';
                try { renderMathInto(line, piece); } catch (e) { line.textContent = piece; }
                return line;
            });
            el.textContent = '';
            lines.forEach(function(line){ el.appendChild(line); });
            if (el.scrollWidth >= originalWidth) {
                /* Splitting gained nothing (one piece still dominates); put
                 * the original render back and fall through to scaling. */
                try { renderMathInto(el, raw); } catch (e) { el.textContent = raw; }
            }
        }

        if (!mathOverflows(el)) return;

        /* 2) Scale down until the block fits, bounded so it stays legible. */
        var fontSize = parseFloat(window.getComputedStyle(el).fontSize) || 16;
        var guard = 0;
        while (mathOverflows(el) && fontSize > 9 && guard < 24) {
            fontSize *= 0.92;
            el.style.fontSize = fontSize.toFixed(2) + 'px';
            guard++;
        }

        /* 3) Last resort: keep the content reachable via horizontal scroll. */
        if (mathOverflows(el)) el.style.overflowX = 'auto';
    }

    var fitResizeScheduled = false;
    window.addEventListener('resize', function() {
        if (fitResizeScheduled) return;
        fitResizeScheduled = true;
        setTimeout(function() {
            fitResizeScheduled = false;
            document.querySelectorAll('.math-block').forEach(function(el){
                /* Restore the single-line render before re-measuring so a
                 * wider viewport can undo an earlier split/scale. */
                var raw = el.getAttribute('data-raw-math');
                if (raw && typeof katex !== 'undefined') {
                    try { renderMathInto(el, raw); } catch (e) {}
                }
                fitDisplayMath(el);
            });
        }, 150);
    });

    function renderBlogMath(elem){
        if (!elem) return;
        if (typeof katex !== 'undefined') {
            elem.querySelectorAll('.math-block').forEach(function(el){
                if (!el.hasAttribute('data-raw-math')) el.setAttribute('data-raw-math', normalizeOperatorRuns(el.textContent));
                try { katex.render(el.getAttribute('data-raw-math') || '', el, {throwOnError: false, displayMode: true}); } catch(e) {}
                fitDisplayMath(el);
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
                        var script = document.createElement('script');
                        script.type = 'text/tikz';
                        script.setAttribute('data-raw-tikz', el.textContent);
                        script.textContent = prepareTikZCode(el.textContent);
                        fallback.appendChild(script);
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

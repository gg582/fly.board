(function(){
    function loadTikZJax(hasExtraPackages, cb) {
        if (window.__flyboardTikzJaxProcess) {
            if (cb) cb();
            return;
        }
        if (document.getElementById('tikzjax-script')) {
            if (cb) {
                var el = document.getElementById('tikzjax-script');
                if (el.dataset.loaded === '1') cb();
                else el.addEventListener('load', cb);
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
            if (cb) cb();
        };
        document.head.appendChild(script);
    }

    function processTikZJax() {
        if (typeof window.__flyboardTikzJaxProcess !== 'function') return;
        try { window.__flyboardTikzJaxProcess.call(window); } catch (e) {}
    }

    function prepareTikZCode(code) {
        var trimmed = code.trim();
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
        /* TikZJax can append the SVG after its compiler callback returns. */
        new MutationObserver(applyTheme).observe(wrapper, {childList: true, subtree: true});
    }

    function renderBlogMath(elem){
        if (!elem) return;
        if (typeof katex !== 'undefined') {
            elem.querySelectorAll('.math-block').forEach(function(el){
                try { katex.render(el.textContent, el, {throwOnError: false, displayMode: true}); } catch(e) {}
            });
            elem.querySelectorAll('.math-inline').forEach(function(el){
                try { katex.render(el.textContent, el, {throwOnError: false, displayMode: false}); } catch(e) {}
            });
        }
        var tikzElements = elem.querySelectorAll('.tikz-block, code.language-tikz, script[type="text/tikz"], div.tikz');
        if (tikzElements.length > 0) {
            var needsExtra = false;
            tikzElements.forEach(function(el){
                var content = el.textContent || '';
                if (content.includes('\\usetikzlibrary') || content.includes('\\usepackage') || el.getAttribute('data-has-packages') === 'true') {
                    needsExtra = true;
                }
            });

            loadTikZJax(needsExtra, function(){
                tikzElements.forEach(function(el){
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
                        targetNode.parentNode.replaceChild(wrapper, targetNode);
                        wrapper.appendChild(script);
                        themeTikZDiagram(wrapper);
                    }
                });
                processTikZJax();
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

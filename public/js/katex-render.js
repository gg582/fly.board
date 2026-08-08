(function(){
    function loadTikZJax(hasExtraPackages, cb) {
        if (window.tikzjax) {
            if (cb) cb();
            return;
        }
        if (document.getElementById('tikzjax-script')) {
            if (cb) {
                var el = document.getElementById('tikzjax-script');
                el.addEventListener('load', cb);
            }
            return;
        }
        var css = document.createElement('link');
        css.rel = 'stylesheet';
        css.href = 'https://cdn.jsdelivr.net/npm/tikzjax@1.0.5/dist/tikzjax.css';
        document.head.appendChild(css);

        var script = document.createElement('script');
        script.id = 'tikzjax-script';
        script.src = 'https://cdn.jsdelivr.net/npm/tikzjax@1.0.5/tikzjax.js';
        if (cb) script.onload = cb;
        document.head.appendChild(script);
    }

    function prepareTikZCode(code) {
        var trimmed = code.trim();
        if (!trimmed.includes('\\begin{document}') && !trimmed.includes('\\begin{tikzpicture}')) {
            return '\\begin{tikzpicture}\n' + trimmed + '\n\\end{tikzpicture}';
        }
        return trimmed;
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
                    if (dataPackages || code.includes('\\usetikzlibrary') || code.includes('\\usepackage')) {
                        script.setAttribute('data-has-packages', 'true');
                    }
                    script.textContent = code;

                    var targetNode = (pre && pre.tagName.toLowerCase() === 'pre') ? pre : el;
                    if (targetNode.parentNode) {
                        targetNode.parentNode.replaceChild(script, targetNode);
                    }
                });
                if (window.tikzjax && typeof window.tikzjax.process === 'function') {
                    window.tikzjax.process();
                }
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



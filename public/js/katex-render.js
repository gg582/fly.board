(function(){
    function renderBlogMath(elem){
        if (!elem || typeof katex === 'undefined') return;
        elem.querySelectorAll('.math-block').forEach(function(el){
            try { katex.render(el.textContent, el, {throwOnError: false, displayMode: true}); } catch(e) {}
        });
        elem.querySelectorAll('.math-inline').forEach(function(el){
            try { katex.render(el.textContent, el, {throwOnError: false, displayMode: false}); } catch(e) {}
        });
    }
    if (document.readyState === 'loading') {
        document.addEventListener('DOMContentLoaded', function(){ renderBlogMath(document.body); });
    } else {
        renderBlogMath(document.body);
    }
    window.__renderBlogMath = renderBlogMath;
})();

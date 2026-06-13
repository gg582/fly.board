(function(){
    // 1. Highlight.js init
    if (window.hljs) {
        if (document.readyState === 'loading') {
            document.addEventListener('DOMContentLoaded', function(){ hljs.highlightAll(); });
        } else {
            hljs.highlightAll();
        }
    }

    // 2. Mobile Layout
    function hasMobileUa(){
        var n=window.navigator||{};
        if(n.userAgentData&&n.userAgentData.mobile===true)return true;
        var ua=n.userAgent||'';
        return /Android|webOS|iPhone|iPod|BlackBerry|BB10|IEMobile|Windows Phone|Opera Mini|Mobi|Mobile/i.test(ua);
    }
    function hasMobileAspect(){
        var de=document.documentElement;
        var w=window.innerWidth||de.clientWidth||(window.screen&&window.screen.width)||0;
        var h=window.innerHeight||de.clientHeight||(window.screen&&window.screen.height)||0;
        if(!w||!h)return false;
        var s=Math.min(w,h),l=Math.max(w,h);
        return l>0&&s/l<=0.65;
    }
    function hasCoarseInput(){
        var n=window.navigator||{};
        return !!((window.matchMedia&&matchMedia('(pointer: coarse)').matches)||n.maxTouchPoints>1);
    }
    function shouldUseMobileNav(){return hasMobileUa()||(hasCoarseInput()&&hasMobileAspect());}
    function isMobileLayout(){return document.documentElement.classList.contains('mobile')||(document.body&&document.body.classList.contains('mobile'));}
    function setMobileLayout(on){document.documentElement.classList.toggle('mobile',on);if(document.body)document.body.classList.toggle('mobile',on);}
    function closeMobileNav(){
        var nav=document.querySelector('.nav-links');
        var overlay=document.querySelector('.mobile-overlay');
        var btn=document.querySelector('.burger-btn');
        if(nav){nav.classList.remove('open');nav.style.display='';}
        if(overlay)overlay.classList.remove('open');
        if(btn){btn.classList.remove('open');btn.setAttribute('aria-expanded','false');}
        if(document.body)document.body.style.overflow='';
    }
    function syncMobileLayout(){
        var on=shouldUseMobileNav();
        setMobileLayout(on);
        if(!on)closeMobileNav();
    }
    syncMobileLayout();
    window.addEventListener('resize',syncMobileLayout);
    window.addEventListener('orientationchange',syncMobileLayout);
    if(document.readyState==='loading'){document.addEventListener('DOMContentLoaded',syncMobileLayout);}else{syncMobileLayout();}
    function toggleMobileNav(){
        syncMobileLayout();
        if(!isMobileLayout())return;
        var nav=document.querySelector('.nav-links');
        var overlay=document.querySelector('.mobile-overlay');
        var btn=document.querySelector('.burger-btn');
        if(!nav||!overlay||!btn)return;
        var open=!nav.classList.contains('open');
        nav.classList.toggle('open',open);
        nav.style.display=open?'flex':'';
        overlay.classList.toggle('open',open);
        btn.classList.toggle('open',open);
        btn.setAttribute('aria-expanded',open?'true':'false');
        document.body.style.overflow=open?'hidden':'';
    }
    window.toggleMobileNav=toggleMobileNav;

    function bindMobileNav(){
        var btn=document.querySelector('.burger-btn');
        var overlay=document.querySelector('.mobile-overlay');
        if(btn&&!btn.dataset.navBound){
            btn.dataset.navBound='1';
            btn.addEventListener('click',function(e){
                e.preventDefault();
                toggleMobileNav();
            });
        }
        if(overlay&&!overlay.dataset.navBound){
            overlay.dataset.navBound='1';
            overlay.addEventListener('click',function(e){
                e.preventDefault();
                toggleMobileNav();
            });
        }
    }
    if(document.readyState==='loading'){
        document.addEventListener('DOMContentLoaded',bindMobileNav);
    }else{
        bindMobileNav();
    }

    // 3. Theme toggle (simple click-to-toggle)
    var CACHE_KEY='fly_themes_v2';
    var HL_LIGHT='https://cdnjs.cloudflare.com/ajax/libs/highlight.js/11.9.0/styles/github.min.css';
    var HL_DARK='https://cdnjs.cloudflare.com/ajax/libs/highlight.js/11.9.0/styles/github-dark.min.css';
    function buildCss(t){
        var css='';
        for(var k in t.vars)css+=k+':'+t.vars[k]+';';
        css=':root{'+css+'}';
        for(var i=0;i<t.rules.length;i++){var r=t.rules[i];css+=r.sel+'{';for(var d in r.decls)css+=d+':'+r.decls[d]+';';css+='}';}
        return css;
    }
    function applyTheme(t){var s=document.getElementById('dyn-theme');if(s)s.textContent=buildCss(t);}
    function findTheme(arr,name){for(var i=0;i<arr.length;i++)if(arr[i].name===name)return arr[i];return arr[0];}
    function setHlCss(name){
        var pres=document.querySelectorAll('.markdown-body pre');
        for(var i=0;i<pres.length;i++)pres[i].style.opacity='0.5';
        var l=document.getElementById('hl-theme');
        if(l)l.href=(name==='light'?HL_LIGHT:HL_DARK);
        setTimeout(function(){for(var i=0;i<pres.length;i++)pres[i].style.opacity='';},200);
    }
    function updateBtn(name){
        var btn=document.getElementById('theme-toggle-btn');
        if(btn)btn.textContent=(name==='dark'?'●':'○');
    }
    var d=document.documentElement;
    var stored=localStorage.getItem(CACHE_KEY);
    var themes=null;
    try{var p=JSON.parse(stored);if(Array.isArray(p))themes=p;}catch(e){}
    var c=document.cookie.match(/theme=(\w+)/);
    var mode=c?c[1]:(d.classList.contains('dark')?'dark':'light');
    if(!/^(light|dark|ocean|forest|sepia)$/.test(mode))mode='light';
    function applyCached(){if(themes){applyTheme(findTheme(themes,mode));}}
    applyCached();
    fetch('/themes.json').then(function(r){return r.json();}).then(function(arr){
        localStorage.setItem(CACHE_KEY,JSON.stringify(arr));
        themes=arr;
        applyTheme(findTheme(arr,mode));
    });
    window.toggleTheme=function(name){
        if(!name)name=(mode==='light'?'dark':'light');
        mode=name;
        document.cookie='theme='+mode+';path=/;max-age=31536000';
        setHlCss(mode);
        updateBtn(mode);
        if(themes){applyTheme(findTheme(themes,mode));return;}
        fetch('/themes.json').then(function(r){return r.json();}).then(function(arr){
            localStorage.setItem(CACHE_KEY,JSON.stringify(arr));
            themes=arr;
            applyTheme(findTheme(arr,mode));
        });
    };
    function bindThemeToggle(){
        var themeBtn=document.getElementById('theme-toggle-btn');
        if(themeBtn&&!themeBtn.dataset.themeBound){
            themeBtn.dataset.themeBound='1';
            themeBtn.addEventListener('click',function(e){
                e.preventDefault();
                toggleTheme();
            });
        }
    }
    if(document.readyState==='loading'){
        document.addEventListener('DOMContentLoaded',bindThemeToggle);
    }else{
        bindThemeToggle();
    }

    // 4. Boards Dropdown
    function renderBoardsDropdown(arr){
        var list=document.getElementById('boards-dropdown-list');
        if(!list)return;
        list.innerHTML='';
        for(var i=0;i<arr.length;i++){
            var b=arr[i];
            if(!b||!b.slug||!b.name)continue;
            var a=document.createElement('a');
            a.className='nav-board-subitem';
            a.href='/board/'+encodeURIComponent(b.slug);
            a.textContent=b.name;
            list.appendChild(a);
        }
        if(!list.children.length){
            var e=document.createElement('span');
            e.className='nav-board-empty';
            e.textContent='No boards';
            list.appendChild(e);
        }
    }
    function loadBoardsDropdown(){
        fetch('/api/boards',{headers:{'Accept':'application/json'}}).then(function(r){
            if(!r.ok)throw new Error('boards fetch failed');
            return r.json();
        }).then(function(arr){
            if(Array.isArray(arr))renderBoardsDropdown(arr);
        }).catch(function(){});
    }
    if(document.readyState==='loading'){
        document.addEventListener('DOMContentLoaded',loadBoardsDropdown);
    }else{
        loadBoardsDropdown();
    }

    // 5. Boards Dropdown hover helper (desktop)
    function bindBoardsDropdown(){
        var dd=document.querySelector('.nav-board-dropdown');
        var menu=document.getElementById('boards-dropdown');
        if(!dd||!menu||dd.dataset.ddBound)return;
        dd.dataset.ddBound='1';
        dd.addEventListener('mouseenter',function(){
            menu.classList.add('open');
        });
        dd.addEventListener('mouseleave',function(e){
            if(!menu.contains(e.relatedTarget)){
                menu.classList.remove('open');
            }
        });
        menu.addEventListener('mouseleave',function(e){
            if(!dd.contains(e.relatedTarget)){
                menu.classList.remove('open');
            }
        });
    }
    if(document.readyState==='loading'){
        document.addEventListener('DOMContentLoaded',bindBoardsDropdown);
    }else{
        bindBoardsDropdown();
    }

    // 6. Admin Dropdown hover helper (desktop)
    function bindAdminDropdown(){
        var dd=document.querySelector('.nav-admin-dropdown');
        var menu=dd?dd.querySelector('.nav-admin-menu'):null;
        if(!dd||!menu||dd.dataset.ddBound)return;
        dd.dataset.ddBound='1';
        dd.addEventListener('mouseenter',function(){
            menu.classList.add('open');
        });
        dd.addEventListener('mouseleave',function(e){
            if(!menu.contains(e.relatedTarget)){
                menu.classList.remove('open');
            }
        });
        menu.addEventListener('mouseleave',function(e){
            if(!dd.contains(e.relatedTarget)){
                menu.classList.remove('open');
            }
        });
    }
    if(document.readyState==='loading'){
        document.addEventListener('DOMContentLoaded',bindAdminDropdown);
    }else{
        bindAdminDropdown();
    }

    // 7. Advanced search toggle (replaces inline onclick for CSP)
    function bindAdvSearchToggle(){
        var btn=document.querySelector('.adv-toggle-btn');
        var el=document.getElementById('adv-search');
        if(!btn||!el||btn.dataset.advBound)return;
        btn.dataset.advBound='1';
        btn.addEventListener('click',function(){
            var open=el.style.display!=='none';
            el.style.display=open?'none':'block';
            btn.classList.toggle('open',!open);
        });
    }
    if(document.readyState==='loading'){
        document.addEventListener('DOMContentLoaded',bindAdvSearchToggle);
    }else{
        bindAdvSearchToggle();
    }

    // 8. Generic toggle buttons (replaces inline onclick for edit/reply panels)
    function bindToggleButtons(){
        document.querySelectorAll('[data-toggle-target]').forEach(function(btn){
            if(btn.dataset.toggleBound)return;
            btn.dataset.toggleBound='1';
            btn.addEventListener('click',function(){
                var el=document.getElementById(btn.dataset.toggleTarget);
                if(!el)return;
                el.style.display=el.style.display==='none'?'block':'none';
            });
        });
    }
    if(document.readyState==='loading'){
        document.addEventListener('DOMContentLoaded',bindToggleButtons);
    }else{
        bindToggleButtons();
    }

    // 9. Confirm dialogs (replaces inline onclick/onsubmit for delete actions)
    function bindConfirmActions(){
        document.querySelectorAll('a[data-confirm], button[data-confirm], form[data-confirm]').forEach(function(el){
            if(el.dataset.confirmBound)return;
            el.dataset.confirmBound='1';
            if(el.tagName==='FORM'){
                el.addEventListener('submit',function(e){
                    if(!confirm(el.dataset.confirm))e.preventDefault();
                });
            }else{
                el.addEventListener('click',function(e){
                    if(!confirm(el.dataset.confirm))e.preventDefault();
                });
            }
        });
    }
    if(document.readyState==='loading'){
        document.addEventListener('DOMContentLoaded',bindConfirmActions);
    }else{
        bindConfirmActions();
    }

    // 10. Service Worker
    if('serviceWorker'in navigator){navigator.serviceWorker.register('/sw.js');}
})();

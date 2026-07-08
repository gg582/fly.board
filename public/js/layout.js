(function(){
    // 1. Mobile Layout
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
        if(open){
            loadBoardsDropdown();
        }
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

    // 2. Theme toggle (simple click-to-toggle)
    var CACHE_KEY='fly_themes_v2';
    var CACHE_TTL_MS=300000; // 5 minutes: long enough to avoid FOUC, short enough to pick up config changes
    // monotonic generation counter so async theme operations never overwrite a newer toggle
    var themeGen=0;
    function buildCss(t){
        var css='';
        for(var k in t.vars)css+=k+':'+t.vars[k]+';';
        css=':root{'+css+'}';
        for(var i=0;i<t.rules.length;i++){var r=t.rules[i];css+=r.sel+'{';for(var d in r.decls)css+=d+':'+r.decls[d]+';';css+='}';}
        return css;
    }
    function applyTheme(t){var s=document.getElementById('dyn-theme');if(s)s.textContent=buildCss(t);}
    function findTheme(arr,name){for(var i=0;i<arr.length;i++)if(arr[i].name===name)return arr[i];return arr[0];}
    function setHlCss(name,gen){
        if(gen!==undefined&&gen!==themeGen)return;
        var link=document.getElementById('hl-theme');
        if(!link)return;
        // Skip if the requested highlight theme is already active.
        if(link.dataset.active===name)return;
        link.href=(name==='light')?'/assets/inline/highlight-light.css':'/assets/inline/highlight-dark.css';
        link.dataset.active=name;
    }
    function syncHlTheme(){
        // Guard against stale highlight state after navigation or cache restore.
        var link=document.getElementById('hl-theme');
        if(link && link.dataset.active!==mode){
            setHlCss(mode);
        }
    }
    function updateBtn(name,gen){
        if(gen!==undefined&&gen!==themeGen)return;
        var btn=document.getElementById('theme-toggle-btn');
        if(btn)btn.textContent=(name==='dark'?'●':'○');
    }
    function saveThemes(arr){
        try{localStorage.setItem(CACHE_KEY,JSON.stringify({themes:arr,ts:Date.now()}));}catch(e){}
    }
    function loadThemes(){
        try{
            var p=JSON.parse(localStorage.getItem(CACHE_KEY));
            if(p && Array.isArray(p.themes) && typeof p.ts==='number' && (Date.now()-p.ts)<CACHE_TTL_MS){
                return p.themes;
            }
        }catch(e){}
        return null;
    }
    var d=document.documentElement;
    var themes=loadThemes();
    var c=document.cookie.match(/theme=(\w+)/);
    var mode=c?c[1]:(d.classList.contains('dark')?'dark':'light');
    if(!/^(light|dark|ocean|forest|sepia)$/.test(mode))mode='light';
    function applyCached(){
        if(!themes)return;
        applyTheme(findTheme(themes,mode));
        syncHlTheme();
    }
    applyCached();
    // Ensure highlight theme matches the resolved mode on every full page load.
    // This covers navigation from pages that had no code blocks (and therefore
    // no hl-theme element) and stale back/forward cache restorations.
    syncHlTheme();
    if (!themes) {
        fetch('/themes.json',{cache:'no-store',credentials:'same-origin'}).then(function(r){return r.json();}).then(function(arr){
            if(themeGen!==0)return;
            saveThemes(arr);
            themes=arr;
            applyTheme(findTheme(arr,mode));
            syncHlTheme();
        }).catch(function(){/* network error: use cached or skip */});
    } else {
        var isMobileTheme = shouldUseMobileNav();
        var connTheme = navigator.connection || navigator.mozConnection || navigator.webkitConnection || {};
        var isSlowTheme = isMobileTheme || (connTheme.rtt && connTheme.rtt > 1000);
        setTimeout(function() {
            fetch('/themes.json',{cache:'no-store',credentials:'same-origin'}).then(function(r){return r.json();}).then(function(arr){
                if(themeGen!==0)return;
                saveThemes(arr);
                themes=arr;
            }).catch(function(){});
        }, isSlowTheme ? 5000 : 2000);
    }
    window.toggleTheme=function(name){
        var myGen=++themeGen;
        if(!name)name=(mode==='light'?'dark':'light');
        mode=name;
        document.cookie='theme='+mode+';path=/;max-age=31536000';
        setHlCss(mode,myGen);
        updateBtn(mode,myGen);
        if(themes){applyTheme(findTheme(themes,mode));return;}
        fetch('/themes.json',{cache:'no-store',credentials:'same-origin'}).then(function(r){return r.json();}).then(function(arr){
            if(myGen!==themeGen)return;
            saveThemes(arr);
            themes=arr;
            applyTheme(findTheme(arr,mode));
        }).catch(function(){/* network error: keep current theme */});
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

    // 3. Boards dropdown
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
    var boardsLoaded = false;
    function loadBoardsDropdown(){
        if (boardsLoaded) return;
        fetch('/api/boards',{credentials:'same-origin',headers:{'Accept':'application/json'}}).then(function(r){
            if(!r.ok)throw new Error('boards fetch failed');
            return r.json();
        }).then(function(arr){
            if(Array.isArray(arr)) {
                renderBoardsDropdown(arr);
                boardsLoaded = true;
            }
        }).catch(function(){});
    }

    // 4. Boards dropdown hover helper (desktop)
    function bindBoardsDropdown(){
        var dd=document.querySelector('.nav-board-dropdown');
        var menu=document.getElementById('boards-dropdown');
        if(!dd||!menu||dd.dataset.ddBound)return;
        dd.dataset.ddBound='1';
        dd.addEventListener('mouseenter',function(){
            loadBoardsDropdown();
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

    // 5. Admin dropdown hover helper (desktop)
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

    // 6. Advanced search toggle (replaces inline onclick for CSP)
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

    // 7. Generic toggle buttons (replaces inline onclick for edit/reply panels)
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

    // 8. Confirm dialogs (replaces inline onclick/onsubmit for delete actions)
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

    // 9. Service Worker
    if('serviceWorker'in navigator){
        function registerSw(attempt){
            navigator.serviceWorker.register('/sw.js').then(function(reg){
                if(typeof console!=='undefined'&&console.log) console.log('[SW] registered:', reg.scope);
            }).catch(function(err){
                if(typeof console!=='undefined'&&console.warn) console.warn('[SW] registration failed (attempt '+attempt+'):', err);
                // KR<->NG worst case: TCP+TLS ~12 s reconnect cycle.
                // Start at 5 s (one cycle), back off up to 90 s over 8 attempts.
                if(attempt < 8){
                    var delay = Math.min(5000 * Math.pow(2, attempt - 1), 90000);
                    setTimeout(function(){ registerSw(attempt + 1); }, delay);
                }
            });
        }
        // Defer SW registration to prevent connection bottleneck on mobile / high-latency links
        var isMobileSw = shouldUseMobileNav();
        var connSw = navigator.connection || navigator.mozConnection || navigator.webkitConnection || {};
        var isSlowSw = isMobileSw || (connSw.rtt && connSw.rtt > 1000) || (connSw.effectiveType && (connSw.effectiveType === '2g' || connSw.effectiveType === '3g'));
        var regDelay = isSlowSw ? 3000 : 1000;
        
        if (document.readyState === 'complete') {
            setTimeout(function(){ registerSw(1); }, regDelay);
        } else {
            window.addEventListener('load', function() {
                setTimeout(function(){ registerSw(1); }, regDelay);
            });
        }
    }

    // 10. Sub-resource load failure recovery.
    //     Covers Firefox tracking-protection drops, .zip-TLD heuristic blocks,
    //     and high-RTT first-connection races that produce ERR_CONNECTION_REFUSED
    //     before the SW is active enough to intercept.
    (function(){
        if(!window.addEventListener) return;
        // retried[url] = attempt count
        var retried = {};
        // KR<->NG worst case: allow 8 retries over ~12-min window so a
        // script can survive a badly degraded intercontinental path.
        var MAX_SCRIPT_RETRIES = 8;
        function retryScript(url, attempt){
            // 5000 ms base = one TCP+TLS reconnect cycle on KR<->NG.
            // Doubles each retry up to 90 s; +0-2000 ms jitter.
            var delay = Math.min(5000 * Math.pow(2, attempt - 1), 90000)
                        + Math.floor(Math.random() * 2000);
            setTimeout(function(){
                var s = document.createElement('script');
                s.src = url;
                // Preserve defer semantics: do NOT set async so that execution
                // order relative to other deferred scripts is kept.
                s.defer = true;
                s.onerror = function(){
                    if(attempt < MAX_SCRIPT_RETRIES){
                        retryScript(url, attempt + 1);
                    }
                };
                var container = document.head || document.body || document.documentElement;
                if(container) container.appendChild(s);
            }, delay);
        }
        window.addEventListener('error', function(e){
            var target = e.target;
            if(!target || (target.tagName !== 'SCRIPT' && target.tagName !== 'LINK')) return;
            var url = target.src || target.href || '';
            if(!url || url.indexOf(window.location.origin) !== 0) return;
            if((retried[url] || 0) >= MAX_SCRIPT_RETRIES) return;
            retried[url] = (retried[url] || 0) + 1;
            if(target.tagName === 'SCRIPT'){
                try { target.remove(); } catch(err) {}
                retryScript(url, retried[url]);
            }
        }, true);
    })();
})();

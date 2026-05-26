(function(){
    // 1. Highlight.js init
    if (window.hljs) {
        if (document.readyState === 'loading') {
            document.addEventListener('DOMContentLoaded', function(){ hljs.highlightAll(); });
        } else {
            hljs.highlightAll();
        }
    }

    // 2. Mobile Layout & Theme & Boards Dropdown
    function hasMobileUa(){var n=window.navigator||{};if(n.userAgentData&&n.userAgentData.mobile===true)return true;var ua=n.userAgent||'';return /Android|webOS|iPhone|iPod|BlackBerry|BB10|IEMobile|Windows Phone|Opera Mini|Mobi|Mobile/i.test(ua);}
    function hasMobileAspect(){var de=document.documentElement;var w=window.innerWidth||de.clientWidth||(window.screen&&window.screen.width)||0;var h=window.innerHeight||de.clientHeight||(window.screen&&window.screen.height)||0;if(!w||!h)return false;var s=Math.min(w,h),l=Math.max(w,h);return l>0&&s/l<=0.65;}
    function hasCoarseInput(){var n=window.navigator||{};return !!((window.matchMedia&&matchMedia('(pointer: coarse)').matches)||n.maxTouchPoints>1);}
    function shouldUseMobileNav(){return hasMobileUa()||(hasCoarseInput()&&hasMobileAspect());}
    function isMobileLayout(){return document.documentElement.classList.contains('mobile')||(document.body&&document.body.classList.contains('mobile'));}
    function setMobileLayout(on){document.documentElement.classList.toggle('mobile',on);if(document.body)document.body.classList.toggle('mobile',on);}
    function closeMobileNav(){var nav=document.querySelector('.nav-links');var overlay=document.querySelector('.mobile-overlay');var btn=document.querySelector('.burger-btn');if(nav){nav.classList.remove('open');nav.style.display='';}if(overlay)overlay.classList.remove('open');if(btn){btn.classList.remove('open');btn.setAttribute('aria-expanded','false');}if(document.body)document.body.style.overflow='';}
    function syncMobileLayout(){var on=shouldUseMobileNav();setMobileLayout(on);if(!on)closeMobileNav();}
    syncMobileLayout();window.addEventListener('resize',syncMobileLayout);window.addEventListener('orientationchange',syncMobileLayout);if(document.readyState==='loading'){document.addEventListener('DOMContentLoaded',syncMobileLayout);}else{syncMobileLayout();}
    function toggleMobileNav(){syncMobileLayout();if(!isMobileLayout())return;var nav=document.querySelector('.nav-links');var overlay=document.querySelector('.mobile-overlay');var btn=document.querySelector('.burger-btn');if(!nav||!overlay||!btn)return;var open=!nav.classList.contains('open');nav.classList.toggle('open',open);nav.style.display=open?'flex':'';overlay.classList.toggle('open',open);btn.classList.toggle('open',open);btn.setAttribute('aria-expanded',open?'true':'false');document.body.style.overflow=open?'hidden':'';}
    window.toggleMobileNav=toggleMobileNav;
    var CACHE_KEY='fly_themes_v2';
    var MIX_KEY='fly_theme_mix';
    var HL_LIGHT='https://cdnjs.cloudflare.com/ajax/libs/highlight.js/11.9.0/styles/github.min.css';
    var HL_DARK='https://cdnjs.cloudflare.com/ajax/libs/highlight.js/11.9.0/styles/github-dark.min.css';
    function buildCss(t){var css='';for(var k in t.vars)css+=k+':'+t.vars[k]+';';css=':root{'+css+'}';for(var i=0;i<t.rules.length;i++){var r=t.rules[i];css+=r.sel+'{';for(var d in r.decls)css+=d+':'+r.decls[d]+';';css+='}';}return css;}
    function applyTheme(t){var s=document.getElementById('dyn-theme');if(s)s.textContent=buildCss(t);}
    function findTheme(arr,name){for(var i=0;i<arr.length;i++)if(arr[i].name===name)return arr[i];return arr[0];}
    function lerpHex(a,b,t){var ar=parseInt(a.substr(1,2),16),ag=parseInt(a.substr(3,2),16),ab=parseInt(a.substr(5,2),16);var br=parseInt(b.substr(1,2),16),bg=parseInt(b.substr(3,2),16),bb=parseInt(b.substr(5,2),16);var rr=Math.round(ar+(br-ar)*t),rg=Math.round(ag+(bg-ag)*t),rb=Math.round(ab+(bb-ab)*t);return '#'+((1<<24)+(rr<<16)+(rg<<8)+rb).toString(16).slice(1);}
    function lerpTheme(t1,t2,ratio){var t={name:'custom',vars:{},rules:t1.rules};for(var k in t1.vars){if(t2.vars[k]&&t1.vars[k][0]==='#'&&t2.vars[k][0]==='#'){t.vars[k]=lerpHex(t1.vars[k],t2.vars[k],ratio);}else{t.vars[k]=t1.vars[k];}}return t;}
    function setHlCss(name){var pres=document.querySelectorAll('.markdown-body pre');for(var i=0;i<pres.length;i++)pres[i].style.opacity='0.5';var l=document.getElementById('hl-theme');if(l)l.href=(name==='light'?HL_LIGHT:HL_DARK);setTimeout(function(){for(var i=0;i<pres.length;i++)pres[i].style.opacity='';},200);}
    function updateBtn(name){var label=document.getElementById('theme-toggle-label');if(label)label.textContent=(name==='light'?'Dark':'Light');var opts=document.querySelectorAll('.theme-option');for(var i=0;i<opts.length;i++){var v=opts[i].getAttribute('data-theme');opts[i].classList.toggle('active',v===name);}}
    function rotateToggle(){var icon=document.getElementById('theme-spin');if(!icon)return;icon.classList.remove('spin');void icon.offsetWidth;icon.classList.add('spin');}
    function closeMenu(){var m=document.getElementById('theme-dropdown');var btn=document.querySelector('.theme-toggle-btn');if(m)m.classList.remove('open');if(btn)btn.setAttribute('aria-expanded','false');}
    function renderBoardsDropdown(arr){var list=document.getElementById('boards-dropdown-list');if(!list)return;list.innerHTML='';for(var i=0;i<arr.length;i++){var b=arr[i];if(!b||!b.slug||!b.name)continue;var a=document.createElement('a');a.className='nav-board-subitem';a.href='/board/'+encodeURIComponent(b.slug);a.textContent=b.name;list.appendChild(a);}if(!list.children.length){var e=document.createElement('span');e.className='nav-board-empty';e.textContent='No boards';list.appendChild(e);}}
    function loadBoardsDropdown(){fetch('/api/boards',{headers:{'Accept':'application/json'}}).then(function(r){if(!r.ok)throw new Error('boards fetch failed');return r.json();}).then(function(arr){if(Array.isArray(arr))renderBoardsDropdown(arr);}).catch(function(){});}
    var d=document.documentElement;var stored=localStorage.getItem(CACHE_KEY);var themes=null;try{var p=JSON.parse(stored);if(Array.isArray(p))themes=p;}catch(e){}var c=document.cookie.match(/theme=(\w+)/);var mode=c?c[1]:(d.classList.contains('dark')?'dark':'light');if(!/^(light|dark|ocean|forest|sepia)$/.test(mode))mode='light';function applyCached(){if(themes){applyTheme(findTheme(themes,mode));}}applyCached();fetch('/themes.json').then(function(r){return r.json();}).then(function(arr){localStorage.setItem(CACHE_KEY,JSON.stringify(arr));themes=arr;applyTheme(findTheme(arr,mode));});window.toggleTheme=function(name){if(!name)name=(mode==='light'?'dark':'light');mode=name;document.cookie='theme='+mode+';path=/;max-age=31536000';setHlCss(mode);updateBtn(mode);rotateToggle();closeMenu();if(themes){applyTheme(findTheme(themes,mode));return;}fetch('/themes.json').then(function(r){return r.json();}).then(function(arr){localStorage.setItem(CACHE_KEY,JSON.stringify(arr));themes=arr;applyTheme(findTheme(arr,mode));});};
    window.toggleThemeMenu=function(ev){if(ev)ev.stopPropagation();var m=document.getElementById('theme-dropdown');var btn=document.querySelector('.theme-toggle-btn');if(!m)return;var open=!m.classList.contains('open');m.classList.toggle('open',open);if(btn)btn.setAttribute('aria-expanded',open?'true':'false');};
    document.addEventListener('click',function(ev){var btn=document.querySelector('.theme-toggle-btn');var dd=document.getElementById('theme-dropdown');if(btn&&btn.contains(ev.target))return;if(dd&&dd.contains(ev.target))return;closeMenu();});
    if(document.readyState==='loading'){document.addEventListener('DOMContentLoaded',loadBoardsDropdown);}else{loadBoardsDropdown();}

    // 3. Service Worker
    if('serviceWorker'in navigator){navigator.serviceWorker.register('/sw.js');}
})();

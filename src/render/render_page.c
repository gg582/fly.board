#define _POSIX_C_SOURCE 200809L
#include "render.h"
#include "render_internal.h"
#include "theme.h"
#include "config/config.h"
#include <cwist/core/html/builder.h>
#include <cwist/core/mem/alloc.h>
#include <cwist/core/sstring/sstring.h>
#include <string.h>
#include <stdio.h>

static __thread char g_nav_profile_name[128];
static __thread char g_nav_profile_account[128];

void render_set_nav_profile(const char *display_name, const char *account_name) {
    if (display_name && display_name[0]) {
        snprintf(g_nav_profile_name, sizeof(g_nav_profile_name), "%s", display_name);
    } else {
        g_nav_profile_name[0] = '\0';
    }

    if (account_name && account_name[0]) {
        snprintf(g_nav_profile_account, sizeof(g_nav_profile_account), "%s", account_name);
    } else {
        g_nav_profile_account[0] = '\0';
    }
}

static void render_clear_nav_profile(void) {
    g_nav_profile_name[0] = '\0';
    g_nav_profile_account[0] = '\0';
}

cwist_sstring *render_page(const char *title, const char *body_html, bool dark, const char *user_role, const char *profile_pic) {

    cwist_html_element_t *html = cwist_html_element_create("html");
    cwist_html_element_add_attr(html, "lang", "ko");

    cwist_html_element_t *head = cwist_html_element_create("head");
    cwist_html_element_t *meta = cwist_html_element_create("meta");
    cwist_html_element_add_attr(meta, "charset", "utf-8");
    cwist_html_element_t *vp = cwist_html_element_create("meta");
    cwist_html_element_add_attr(vp, "name", "viewport");
    cwist_html_element_add_attr(vp, "content", "width=device-width, initial-scale=1");
    cwist_html_element_t *title_el = cwist_html_element_create("title");
    cwist_html_element_set_text(title_el, title);
    cwist_html_element_add_child(head, meta);
    cwist_html_element_add_child(head, vp);
    cwist_html_element_add_child(head, title_el);

    if (g_config.favicon[0]) {
        cwist_html_element_t *favicon_el = cwist_html_element_create("link");
        cwist_html_element_add_attr(favicon_el, "rel", "icon");
        char favicon_url[512];
        snprintf(favicon_url, sizeof(favicon_url), "/assets/img/%s", g_config.favicon);
        cwist_html_element_add_attr(favicon_el, "href", favicon_url);
        cwist_html_element_add_child(head, favicon_el);
    }

    /* Preconnect + dns-prefetch to critical origins */
    cwist_html_element_t *preconnect_jsdelivr = cwist_html_element_create("link");
    cwist_html_element_add_attr(preconnect_jsdelivr, "rel", "preconnect");
    cwist_html_element_add_attr(preconnect_jsdelivr, "href", "https://cdn.jsdelivr.net");
    cwist_html_element_add_attr(preconnect_jsdelivr, "crossorigin", "");
    cwist_html_element_add_child(head, preconnect_jsdelivr);

    cwist_html_element_t *dns_jsdelivr = cwist_html_element_create("link");
    cwist_html_element_add_attr(dns_jsdelivr, "rel", "dns-prefetch");
    cwist_html_element_add_attr(dns_jsdelivr, "href", "https://cdn.jsdelivr.net");
    cwist_html_element_add_child(head, dns_jsdelivr);

    cwist_html_element_t *preconnect_cdnjs = cwist_html_element_create("link");
    cwist_html_element_add_attr(preconnect_cdnjs, "rel", "preconnect");
    cwist_html_element_add_attr(preconnect_cdnjs, "href", "https://cdnjs.cloudflare.com");
    cwist_html_element_add_attr(preconnect_cdnjs, "crossorigin", "");
    cwist_html_element_add_child(head, preconnect_cdnjs);

    cwist_html_element_t *dns_cdnjs = cwist_html_element_create("link");
    cwist_html_element_add_attr(dns_cdnjs, "rel", "dns-prefetch");
    cwist_html_element_add_attr(dns_cdnjs, "href", "https://cdnjs.cloudflare.com");
    cwist_html_element_add_child(head, dns_cdnjs);


    /* Web Fonts */
    cwist_html_element_t *font_preconnect_google = cwist_html_element_create("link");
    cwist_html_element_add_attr(font_preconnect_google, "rel", "preconnect");
    cwist_html_element_add_attr(font_preconnect_google, "href", "https://fonts.googleapis.com");
    cwist_html_element_add_child(head, font_preconnect_google);

    cwist_html_element_t *font_preconnect_gstatic = cwist_html_element_create("link");
    cwist_html_element_add_attr(font_preconnect_gstatic, "rel", "preconnect");
    cwist_html_element_add_attr(font_preconnect_gstatic, "href", "https://fonts.gstatic.com");
    cwist_html_element_add_attr(font_preconnect_gstatic, "crossorigin", "");
    cwist_html_element_add_child(head, font_preconnect_gstatic);

    cwist_html_element_t *font_space = cwist_html_element_create("link");
    cwist_html_element_add_attr(font_space, "rel", "stylesheet");
    cwist_html_element_add_attr(font_space, "href", "https://fonts.googleapis.com/css2?family=Space+Grotesk:wght@400;500;700&family=IBM+Plex+Sans+KR:wght@400;500;700&family=Inter:wght@400;500;600;700&family=Source+Serif+4:ital,wght@0,400;0,600;1,400&display=swap");
    cwist_html_element_add_child(head, font_space);

    cwist_html_element_t *font_pretendard = cwist_html_element_create("link");
    cwist_html_element_add_attr(font_pretendard, "rel", "stylesheet");
    cwist_html_element_add_attr(font_pretendard, "href", "https://cdn.jsdelivr.net/gh/orioncactus/pretendard@v1.3.9/dist/web/variable/pretendardvariable-dynamic-subset.css");
    cwist_html_element_add_child(head, font_pretendard);

    cwist_html_element_t *font_d2coding = cwist_html_element_create("link");
    cwist_html_element_add_attr(font_d2coding, "rel", "stylesheet");
    cwist_html_element_add_attr(font_d2coding, "href", "https://cdn.jsdelivr.net/gh/joungkyun/font-d2coding@master/d2coding.css");
    cwist_html_element_add_child(head, font_d2coding);

    /* Highlight.js syntax highlighting */
    cwist_html_element_t *hl_css = cwist_html_element_create("link");
    cwist_html_element_add_attr(hl_css, "rel", "stylesheet");
    cwist_html_element_add_attr(hl_css, "id", "hl-theme");
    cwist_html_element_add_attr(hl_css, "href",
        dark
        ? "https://cdnjs.cloudflare.com/ajax/libs/highlight.js/11.9.0/styles/github-dark.min.css"
        : "https://cdnjs.cloudflare.com/ajax/libs/highlight.js/11.9.0/styles/github.min.css");
    cwist_html_element_add_child(head, hl_css);

    cwist_html_element_t *hl_js = cwist_html_element_create("script");
    cwist_html_element_add_attr(hl_js, "src",
        "https://cdnjs.cloudflare.com/ajax/libs/highlight.js/11.9.0/highlight.min.js");
    cwist_html_element_add_child(head, hl_js);

    cwist_html_element_t *hl_fortran = cwist_html_element_create("script");
    cwist_html_element_add_attr(hl_fortran, "src",
        "https://cdnjs.cloudflare.com/ajax/libs/highlight.js/11.9.0/languages/fortran.min.js");
    cwist_html_element_add_child(head, hl_fortran);

    cwist_html_element_t *hl_init = cwist_html_element_create("script");
    cwist_html_element_set_text(hl_init,
        "document.addEventListener('DOMContentLoaded',function(){hljs.highlightAll();});");
    cwist_html_element_add_child(head, hl_init);

    /* Progressive multi-theme loader: inline critical CSS to prevent FOUC */
    char *critical_css = theme_build_css(dark);
    cwist_html_element_t *dyn_style = cwist_html_element_create("style");
    cwist_html_element_add_attr(dyn_style, "id", "dyn-theme");
    cwist_html_element_set_text(dyn_style, critical_css ? critical_css : "");
    cwist_html_element_add_child(head, dyn_style);
    free(critical_css);

    cwist_html_element_t *script = cwist_html_element_create("script");
    cwist_html_element_set_text(script,
        "(function(){"
        "function hasMobileUa(){var n=window.navigator||{};if(n.userAgentData&&n.userAgentData.mobile===true)return true;var ua=n.userAgent||'';return /Android|webOS|iPhone|iPod|BlackBerry|BB10|IEMobile|Windows Phone|Opera Mini|Mobi|Mobile/i.test(ua);}"
        "function hasMobileAspect(){var de=document.documentElement;var w=window.innerWidth||de.clientWidth||(window.screen&&window.screen.width)||0;var h=window.innerHeight||de.clientHeight||(window.screen&&window.screen.height)||0;if(!w||!h)return false;var s=Math.min(w,h),l=Math.max(w,h);return l>0&&s/l<=0.65;}"
        "function hasCoarseInput(){var n=window.navigator||{};return !!((window.matchMedia&&matchMedia('(pointer: coarse)').matches)||n.maxTouchPoints>1);}"
        "function shouldUseMobileNav(){return hasMobileUa()||(hasCoarseInput()&&hasMobileAspect());}"
        "function isMobileLayout(){return document.documentElement.classList.contains('mobile')||(document.body&&document.body.classList.contains('mobile'));}"
        "function setMobileLayout(on){document.documentElement.classList.toggle('mobile',on);if(document.body)document.body.classList.toggle('mobile',on);}"
        "function closeMobileNav(){var nav=document.querySelector('.nav-links');var overlay=document.querySelector('.mobile-overlay');var btn=document.querySelector('.burger-btn');if(nav){nav.classList.remove('open');nav.style.display='';}if(overlay)overlay.classList.remove('open');if(btn){btn.classList.remove('open');btn.setAttribute('aria-expanded','false');}if(document.body)document.body.style.overflow='';}"
        "function syncMobileLayout(){var on=shouldUseMobileNav();setMobileLayout(on);if(!on)closeMobileNav();}"
        "syncMobileLayout();window.addEventListener('resize',syncMobileLayout);window.addEventListener('orientationchange',syncMobileLayout);if(document.readyState==='loading'){document.addEventListener('DOMContentLoaded',syncMobileLayout);}else{syncMobileLayout();}"
        "function toggleMobileNav(){syncMobileLayout();if(!isMobileLayout())return;var nav=document.querySelector('.nav-links');var overlay=document.querySelector('.mobile-overlay');var btn=document.querySelector('.burger-btn');if(!nav||!overlay||!btn)return;var open=!nav.classList.contains('open');nav.classList.toggle('open',open);nav.style.display=open?'flex':'';overlay.classList.toggle('open',open);btn.classList.toggle('open',open);btn.setAttribute('aria-expanded',open?'true':'false');document.body.style.overflow=open?'hidden':'';}"
        "window.toggleMobileNav=toggleMobileNav;"
        "var CACHE_KEY='fly_themes_v2';"
        "var MIX_KEY='fly_theme_mix';"
        "var HL_LIGHT='https://cdnjs.cloudflare.com/ajax/libs/highlight.js/11.9.0/styles/github.min.css';"
        "var HL_DARK='https://cdnjs.cloudflare.com/ajax/libs/highlight.js/11.9.0/styles/github-dark.min.css';"
        "function buildCss(t){var css='';for(var k in t.vars)css+=k+':'+t.vars[k]+';';css=':root{'+css+'}';"
        "for(var i=0;i<t.rules.length;i++){var r=t.rules[i];css+=r.sel+'{';for(var d in r.decls)css+=d+':'+r.decls[d]+';';css+='}';}return css;}"
        "function applyTheme(t){var s=document.getElementById('dyn-theme');if(s)s.textContent=buildCss(t);}"
        "function findTheme(arr,name){for(var i=0;i<arr.length;i++)if(arr[i].name===name)return arr[i];return arr[0];}"
        "function lerpHex(a,b,t){var ar=parseInt(a.substr(1,2),16),ag=parseInt(a.substr(3,2),16),ab=parseInt(a.substr(5,2),16);"
        "var br=parseInt(b.substr(1,2),16),bg=parseInt(b.substr(3,2),16),bb=parseInt(b.substr(5,2),16);"
        "var rr=Math.round(ar+(br-ar)*t),rg=Math.round(ag+(bg-ag)*t),rb=Math.round(ab+(bb-ab)*t);"
        "return '#'+((1<<24)+(rr<<16)+(rg<<8)+rb).toString(16).slice(1);}"
        "function lerpTheme(t1,t2,ratio){var t={name:'custom',vars:{},rules:t1.rules};"
        "for(var k in t1.vars){if(t2.vars[k]&&t1.vars[k][0]==='#'&&t2.vars[k][0]==='#'){t.vars[k]=lerpHex(t1.vars[k],t2.vars[k],ratio);}else{t.vars[k]=t1.vars[k];}}return t;}"
        "function setHlCss(name){var pres=document.querySelectorAll('.markdown-body pre');"
        "for(var i=0;i<pres.length;i++)pres[i].style.opacity='0.5';"
        "var l=document.getElementById('hl-theme');if(l)l.href=(name==='light'?HL_LIGHT:HL_DARK);"
        "setTimeout(function(){for(var i=0;i<pres.length;i++)pres[i].style.opacity='';},200);}"
        "function updateBtn(name){var label=document.getElementById('theme-toggle-label');if(label)label.textContent=(name==='light'?'Dark':'Light');"
        "var opts=document.querySelectorAll('.theme-option');for(var i=0;i<opts.length;i++){"
        "var v=opts[i].getAttribute('data-theme');opts[i].classList.toggle('active',v===name);}}"
        "function rotateToggle(){var icon=document.getElementById('theme-spin');if(!icon)return;"
        "icon.classList.remove('spin');void icon.offsetWidth;icon.classList.add('spin');}"
        "function closeMenu(){var m=document.getElementById('theme-dropdown');"
        "var btn=document.querySelector('.theme-toggle-btn');"
        "if(m)m.classList.remove('open');if(btn)btn.setAttribute('aria-expanded','false');}"
        "function renderBoardsDropdown(arr){var list=document.getElementById('boards-dropdown-list');if(!list)return;"
        "list.innerHTML='';for(var i=0;i<arr.length;i++){var b=arr[i];"
        "if(!b||!b.slug||!b.name)continue;var a=document.createElement('a');"
        "a.className='nav-board-subitem';a.href='/board/'+encodeURIComponent(b.slug);a.textContent=b.name;list.appendChild(a);}"
        "if(!list.children.length){var e=document.createElement('span');e.className='nav-board-empty';e.textContent='No boards';list.appendChild(e);}}"
        "function loadBoardsDropdown(){fetch('/api/boards',{headers:{'Accept':'application/json'}})"
        ".then(function(r){if(!r.ok)throw new Error('boards fetch failed');return r.json();})"
        ".then(function(arr){if(Array.isArray(arr))renderBoardsDropdown(arr);})"
        ".catch(function(){});}"
        "var d=document.documentElement;var stored=localStorage.getItem(CACHE_KEY);var themes=null;"
        "try{var p=JSON.parse(stored);if(Array.isArray(p))themes=p;}catch(e){}"
        "var c=document.cookie.match(/theme=(\\w+)/);var mode=c?c[1]:(d.classList.contains('dark')?'dark':'light');"
        "if(!/^(light|dark|ocean|forest|sepia)$/.test(mode))mode='light';"
        "function applyCached(){if(themes){applyTheme(findTheme(themes,mode));}}"
        "applyCached();"
        "fetch('/themes.json').then(function(r){return r.json();}).then(function(arr){"
        "localStorage.setItem(CACHE_KEY,JSON.stringify(arr));themes=arr;"
        "applyTheme(findTheme(arr,mode));});"
        "window.toggleTheme=function(name){"
        "if(!name)name=(mode==='light'?'dark':'light');mode=name;"
        "document.cookie='theme='+mode+';path=/;max-age=31536000';"
        "setHlCss(mode);updateBtn(mode);rotateToggle();closeMenu();"
        "if(themes){applyTheme(findTheme(themes,mode));return;}"
        "fetch('/themes.json').then(function(r){return r.json();}).then(function(arr){"
        "localStorage.setItem(CACHE_KEY,JSON.stringify(arr));themes=arr;applyTheme(findTheme(arr,mode));});"
        "};"
        "window.toggleThemeMenu=function(ev){if(ev)ev.stopPropagation();"
        "var m=document.getElementById('theme-dropdown');var btn=document.querySelector('.theme-toggle-btn');"
        "if(!m)return;var open=!m.classList.contains('open');m.classList.toggle('open',open);"
        "if(btn)btn.setAttribute('aria-expanded',open?'true':'false');};"
        "document.addEventListener('click',function(ev){var btn=document.querySelector('.theme-toggle-btn');var dd=document.getElementById('theme-dropdown');if(btn&&btn.contains(ev.target))return;if(dd&&dd.contains(ev.target))return;closeMenu();});"
        "if(document.readyState==='loading'){document.addEventListener('DOMContentLoaded',loadBoardsDropdown);}else{loadBoardsDropdown();}"
        "})();");
    cwist_html_element_add_child(head, script);

    cwist_html_element_t *sw_script = cwist_html_element_create("script");
    cwist_html_element_set_text(sw_script,
        "if('serviceWorker'in navigator){navigator.serviceWorker.register('/sw.js');}");
    cwist_html_element_add_child(head, sw_script);

    cwist_html_element_t *body = cwist_html_element_create("body");
    if (dark) cwist_html_element_add_class(body, "dark");


    /* Nav */
    cwist_html_element_t *nav = cwist_html_element_create("nav");
    cwist_html_element_add_class(nav, "topbar");

    cwist_html_element_t *burger_btn = cwist_html_element_create("button");
    cwist_html_element_add_attr(burger_btn, "type", "button");
    cwist_html_element_add_attr(burger_btn, "class", "burger-btn");
    cwist_html_element_add_attr(burger_btn, "aria-label", "Menu");
    cwist_html_element_add_attr(burger_btn, "aria-expanded", "false");
    cwist_html_element_add_attr(burger_btn, "onclick", "toggleMobileNav()");
    cwist_html_element_t *burger_icon = cwist_html_element_create("span");
    cwist_html_element_add_class(burger_icon, "burger-icon");
    cwist_html_element_set_text(burger_icon, "\u2630");
    cwist_html_element_add_child(burger_btn, burger_icon);
    cwist_html_element_add_child(nav, burger_btn);

    cwist_html_element_t *brand = cwist_html_element_create("a");
    cwist_html_element_add_attr(brand, "href", "/");
    cwist_html_element_add_class(brand, "topbar-brand");
    cwist_html_element_t *brand_title = cwist_html_element_create("span");
    cwist_html_element_add_class(brand_title, "topbar-title");
    cwist_html_element_set_text(brand_title, g_config.title);
    cwist_html_element_add_child(brand, brand_title);
    cwist_html_element_add_child(nav, brand);

    cwist_html_element_t *navlinks = cwist_html_element_create("div");
    cwist_html_element_add_class(navlinks, "nav-links");
    cwist_html_element_add_child(navlinks, nav_link("/", "Home"));
    cwist_html_element_t *boards_wrap = cwist_html_element_create("div");
    cwist_html_element_add_class(boards_wrap, "nav-board-dropdown");
    cwist_html_element_t *boards_link = cwist_html_element_create("a");
    cwist_html_element_add_attr(boards_link, "href", "/boards");
    cwist_html_element_add_attr(boards_link, "class", "nav-item nav-board-trigger");
    cwist_html_element_set_text(boards_link, "Boards");
    cwist_html_element_add_child(boards_wrap, boards_link);
    cwist_html_element_t *boards_menu = cwist_html_element_create("div");
    cwist_html_element_add_attr(boards_menu, "id", "boards-dropdown");
    cwist_html_element_add_class(boards_menu, "nav-board-menu");
    cwist_html_element_t *boards_all = cwist_html_element_create("a");
    cwist_html_element_add_attr(boards_all, "href", "/boards");
    cwist_html_element_add_attr(boards_all, "class", "nav-board-subitem nav-board-subitem-all");
    cwist_html_element_set_text(boards_all, "All Boards");
    cwist_html_element_add_child(boards_menu, boards_all);
    cwist_html_element_t *boards_list = cwist_html_element_create("div");
    cwist_html_element_add_attr(boards_list, "id", "boards-dropdown-list");
    cwist_html_element_add_class(boards_list, "nav-board-menu-list");
    cwist_html_element_add_child(boards_menu, boards_list);
    cwist_html_element_add_child(boards_wrap, boards_menu);
    cwist_html_element_add_child(navlinks, boards_wrap);
    cwist_html_element_add_child(navlinks, nav_link("/files", "Files"));
    if (user_role && strcmp(user_role, "admin") == 0) {
        cwist_html_element_add_child(navlinks, nav_link("/admin/users", "Admin"));
    }
    /* Mobile account header (first in navlinks) */
    cwist_html_element_t *mobile_acct = cwist_html_element_create("div");
    cwist_html_element_add_class(mobile_acct, "mobile-account-header");

    cwist_html_element_t *mobile_pic_link = cwist_html_element_create("a");
    cwist_html_element_add_attr(mobile_pic_link, "href", (user_role && user_role[0]) ? "/profile" : "/login");
    cwist_html_element_add_class(mobile_pic_link, "mobile-profile-link");

    const char *display_pp = profile_pic;
    if (display_pp && display_pp[0]) {
        cwist_html_element_t *img = cwist_html_element_create("img");
        cwist_html_element_add_attr(img, "src", display_pp);
        cwist_html_element_add_attr(img, "alt", "Profile");
        cwist_html_element_add_class(img, "mobile-profile-pic");
        cwist_html_element_add_child(mobile_pic_link, img);
    } else {
        cwist_html_element_t *default_icon = cwist_html_element_create("span");
        cwist_html_element_add_class(default_icon, "mobile-profile-pic mobile-profile-default");
        cwist_html_element_set_text(default_icon, "\u263A");
        cwist_html_element_add_child(mobile_pic_link, default_icon);
    }
    cwist_html_element_add_child(mobile_acct, mobile_pic_link);

    cwist_html_element_t *mobile_auth_wrap = cwist_html_element_create("div");
    cwist_html_element_add_class(mobile_auth_wrap, "mobile-auth-btns");
    if (user_role && user_role[0]) {
        cwist_html_element_t *logout_btn = nav_link("/logout", "Logout");
        cwist_html_element_add_class(logout_btn, "btn btn-outline");
        cwist_html_element_add_child(mobile_auth_wrap, logout_btn);
    } else {
        cwist_html_element_t *login_btn = nav_link("/login", "Login");
        cwist_html_element_add_class(login_btn, "btn btn-outline");
        cwist_html_element_add_child(mobile_auth_wrap, login_btn);
        cwist_html_element_t *reg_btn = nav_link("/register", "Register");
        cwist_html_element_add_class(reg_btn, "btn btn-outline");
        cwist_html_element_add_child(mobile_auth_wrap, reg_btn);
    }
    cwist_html_element_add_child(mobile_acct, mobile_auth_wrap);
    cwist_html_element_add_child(navlinks, mobile_acct);

    if (user_role && user_role[0]) {
        const char *display_pp2 = profile_pic;

        if (display_pp2 && display_pp2[0]) {
            cwist_html_element_t *p_link = cwist_html_element_create("a");
            cwist_html_element_add_attr(p_link, "href", "/profile");
            cwist_html_element_t *img = cwist_html_element_create("img");
            cwist_html_element_add_attr(img, "src", display_pp2);
            cwist_html_element_add_attr(img, "width", "24");
            cwist_html_element_add_attr(img, "height", "24");
            cwist_html_element_add_class(img, "profile-pic-small");
            cwist_html_element_add_child(p_link, img);
            cwist_html_element_add_class(p_link, "nav-item desktop-only");
            cwist_html_element_add_child(navlinks, p_link);
        } else {
            cwist_html_element_t *p_link = nav_link("/profile", "Profile");
            cwist_html_element_add_class(p_link, "desktop-only");
            cwist_html_element_add_child(navlinks, p_link);
        }
        cwist_html_element_t *logout_link = nav_link("/logout", "Logout");
        cwist_html_element_add_class(logout_link, "desktop-only");
        cwist_html_element_add_child(navlinks, logout_link);
    } else {
        cwist_html_element_t *login_link = nav_link("/login", "Login");
        cwist_html_element_add_class(login_link, "desktop-only");
        cwist_html_element_add_child(navlinks, login_link);
        cwist_html_element_t *reg_link = nav_link("/register", "Register");
        cwist_html_element_add_class(reg_link, "desktop-only");
        cwist_html_element_add_child(navlinks, reg_link);
    }

    cwist_html_element_add_child(nav, navlinks);

    /* Theme toggle (inline in header) */
    cwist_html_element_t *theme_wrapper = cwist_html_element_create("div");
    cwist_html_element_add_class(theme_wrapper, "theme-switch");

    cwist_html_element_t *theme_btn = cwist_html_element_create("button");
    cwist_html_element_add_attr(theme_btn, "type", "button");
    cwist_html_element_add_attr(theme_btn, "onclick", "toggleThemeMenu(event)");
    cwist_html_element_add_attr(theme_btn, "class", "btn btn-outline theme-toggle-btn");
    cwist_html_element_add_attr(theme_btn, "aria-expanded", "false");
    cwist_html_element_set_text(theme_btn, dark ? "\u25CF" : "\u25CB");
    cwist_html_element_add_child(theme_wrapper, theme_btn);

    cwist_html_element_t *theme_menu = cwist_html_element_create("div");
    cwist_html_element_add_attr(theme_menu, "id", "theme-dropdown");
    cwist_html_element_add_class(theme_menu, "theme-dropdown");
    cwist_html_element_add_attr(theme_menu, "onclick", "event.stopPropagation()");

    cwist_html_element_t *theme_light = cwist_html_element_create("button");
    cwist_html_element_add_attr(theme_light, "type", "button");
    cwist_html_element_add_attr(theme_light, "class", dark ? "theme-option" : "theme-option active");
    cwist_html_element_add_attr(theme_light, "data-theme", "light");
    cwist_html_element_add_attr(theme_light, "onclick", "toggleTheme('light')");
    cwist_html_element_set_text(theme_light, "\u25CB");
    cwist_html_element_add_child(theme_menu, theme_light);

    cwist_html_element_t *theme_dark = cwist_html_element_create("button");
    cwist_html_element_add_attr(theme_dark, "type", "button");
    cwist_html_element_add_attr(theme_dark, "class", dark ? "theme-option active" : "theme-option");
    cwist_html_element_add_attr(theme_dark, "data-theme", "dark");
    cwist_html_element_add_attr(theme_dark, "onclick", "toggleTheme('dark')");
    cwist_html_element_set_text(theme_dark, "\u25CF");
    cwist_html_element_add_child(theme_menu, theme_dark);

    cwist_html_element_add_child(theme_wrapper, theme_menu);
    cwist_html_element_add_child(nav, theme_wrapper);

    cwist_html_element_t *shell = cwist_html_element_create("div");
    cwist_html_element_add_class(shell, "shell fade-in");

    cwist_html_element_t *main_el = cwist_html_element_create("main");
    cwist_html_element_add_class(main_el, "content");
    cwist_html_element_set_text(main_el, body_html);
    cwist_html_element_add_child(shell, main_el);

    cwist_html_element_t *footer = cwist_html_element_create("footer");
    cwist_html_element_add_class(footer, "site-footer");

    cwist_html_element_t *footer_content = cwist_html_element_create("div");
    cwist_html_element_add_class(footer_content, "footer-content");

    cwist_html_element_t *footer_text = cwist_html_element_create("span");
    cwist_html_element_set_text(footer_text, g_config.brand_footer);
    cwist_html_element_add_child(footer_content, footer_text);

    cwist_html_element_t *footer_logo = cwist_html_element_create("img");
    char footer_logo_path[512];
    if (g_config.blog_logo[0]) {
        snprintf(footer_logo_path, sizeof(footer_logo_path), "/assets/img/%s", g_config.blog_logo);
    } else {
        strcpy(footer_logo_path, "/assets/img/logo.png");
    }
    cwist_html_element_add_attr(footer_logo, "src", footer_logo_path);
    cwist_html_element_add_attr(footer_logo, "alt", "Logo");
    cwist_html_element_add_attr(footer_logo, "width", "24");
    cwist_html_element_add_attr(footer_logo, "height", "16");
    cwist_html_element_add_attr(footer_logo, "data-tasfa-skip", "1");
    cwist_html_element_add_attr(footer_logo, "fetchpriority", "high");
    cwist_html_element_add_class(footer_logo, "footer-logo");
    cwist_html_element_add_child(footer_content, footer_logo);

    cwist_html_element_add_child(footer, footer_content);

    cwist_html_element_add_child(body, nav);

    cwist_html_element_t *overlay = cwist_html_element_create("div");
    cwist_html_element_add_class(overlay, "mobile-overlay");
    cwist_html_element_add_attr(overlay, "onclick", "toggleMobileNav()");
    cwist_html_element_add_child(body, overlay);

    cwist_html_element_add_child(body, shell);
    cwist_html_element_add_child(body, footer);

    cwist_html_element_t *tasfa_script = cwist_html_element_create("script");
    cwist_html_element_add_attr(tasfa_script, "src", "/assets/js/tasfa-download.js");
    cwist_html_element_add_child(body, tasfa_script);

    cwist_html_element_add_child(html, head);
    cwist_html_element_add_child(html, body);

    cwist_sstring *out = cwist_html_render(html);
    cwist_html_element_destroy(html);
    if (out) {
        cwist_sstring *doc = cwist_sstring_create();
        cwist_sstring_assign(doc, "<!doctype html>");
        cwist_sstring_append_sstring(doc, out);
        cwist_sstring_destroy(out);
        return doc;
    }
    return NULL;
}

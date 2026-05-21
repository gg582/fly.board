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
    cwist_html_element_add_attr(font_space, "href", "https://fonts.googleapis.com/css2?family=Manrope:wght@400;500;600;700;800&family=Sora:wght@400;500;600;700;800&family=IBM+Plex+Sans+KR:wght@400;500;600;700&display=swap");
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

    /* KaTeX for math rendering */
    cwist_html_element_t *katex_css = cwist_html_element_create("link");
    cwist_html_element_add_attr(katex_css, "rel", "stylesheet");
    cwist_html_element_add_attr(katex_css, "href", "https://cdn.jsdelivr.net/npm/katex@0.16.9/dist/katex.min.css");
    cwist_html_element_add_child(head, katex_css);

    cwist_html_element_t *katex_js = cwist_html_element_create("script");
    cwist_html_element_add_attr(katex_js, "src", "https://cdn.jsdelivr.net/npm/katex@0.16.9/dist/katex.min.js");
    cwist_html_element_add_child(head, katex_js);

    cwist_html_element_t *katex_init = cwist_html_element_create("script");
    cwist_html_element_set_text(katex_init,
        "document.addEventListener('DOMContentLoaded',function(){"
        "if(typeof katex==='undefined')return;"
        "document.querySelectorAll('.math-inline').forEach(function(el){"
        "try{katex.render(el.textContent,el,{throwOnError:false});}catch(e){}"
        "});"
        "document.querySelectorAll('.math-block').forEach(function(el){"
        "try{katex.render(el.textContent,el,{displayMode:true,throwOnError:false});}catch(e){}"
        "});"
        "});");
    cwist_html_element_add_child(head, katex_init);

    cwist_html_element_t *tasfa_js = cwist_html_element_create("script");
    cwist_html_element_add_attr(tasfa_js, "src", "/assets/js/tasfa-download.js");
    cwist_html_element_add_attr(tasfa_js, "defer", "");
    cwist_html_element_add_child(head, tasfa_js);

    cwist_html_element_t *editor_js = cwist_html_element_create("script");
    cwist_html_element_add_attr(editor_js, "src", "/assets/js/editor.js");
    cwist_html_element_add_attr(editor_js, "defer", "");
    cwist_html_element_add_child(head, editor_js);

    cwist_html_element_t *device_script = cwist_html_element_create("script");
    cwist_html_element_set_text(device_script,
        "(function(){"
        "var ua=navigator.userAgent||'';"
        "var mobile=/Android|webOS|iPhone|iPad|iPod|BlackBerry|IEMobile|Opera Mini|Mobile|Silk/i.test(ua);"
        "var touch=(navigator.maxTouchPoints||0)>1;"
        "var screenWidth=(typeof window!=='undefined'&&window.screen&&window.screen.width)?window.screen.width:0;"
        "if(mobile||(touch&&screenWidth>0&&screenWidth<=1024)){document.documentElement.classList.add('mobile-device');}"
        "})();");
    cwist_html_element_add_child(head, device_script);

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
        "function lockBodyScroll(){document.documentElement.classList.add('nav-open');document.body.classList.add('nav-open');}"
        "function unlockBodyScroll(){document.documentElement.classList.remove('nav-open');document.body.classList.remove('nav-open');}"
        "function setMobileNav(open){var nav=document.querySelector('.nav-links');var overlay=document.querySelector('.mobile-overlay');var btn=document.querySelector('.burger-btn');if(!nav||!overlay||!btn)return;nav.classList.toggle('open',open);nav.style.display=open?'flex':'';overlay.classList.toggle('open',open);btn.classList.toggle('open',open);btn.setAttribute('aria-expanded',open?'true':'false');if(open){lockBodyScroll();}else{unlockBodyScroll();}}"
        "window.toggleMobileNav=function(){var nav=document.querySelector('.nav-links');if(!nav)return;setMobileNav(!nav.classList.contains('open'));};"
        "var CACHE_KEY='fly_themes';"
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
        "function updateBtn(name){var label=document.getElementById('theme-toggle-label');var btn=document.querySelector('.theme-toggle-btn');"
        "if(label)label.textContent=(name==='light'?'Dark':'Light');"
        "if(btn)btn.setAttribute('aria-label',name==='light'?'Switch to dark mode':'Switch to light mode');}"
        "function rotateToggle(){var icon=document.getElementById('theme-spin');if(!icon)return;"
        "icon.classList.remove('spin');void icon.offsetWidth;icon.classList.add('spin');}"
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
        "setHlCss(mode);updateBtn(mode);rotateToggle();"
        "if(themes){applyTheme(findTheme(themes,mode));return;}"
        "fetch('/themes.json').then(function(r){return r.json();}).then(function(arr){"
        "localStorage.setItem(CACHE_KEY,JSON.stringify(arr));themes=arr;applyTheme(findTheme(arr,mode));});"
        "};"
        "document.addEventListener('click',function(ev){var nav=document.querySelector('.nav-links');var btn=document.querySelector('.burger-btn');if(!nav||!btn||!nav.classList.contains('open'))return;if(nav.contains(ev.target)||btn.contains(ev.target))return;setMobileNav(false);});"
        "document.addEventListener('keydown',function(ev){if(ev.key==='Escape'){setMobileNav(false);}});"
        "if(document.readyState==='loading'){document.addEventListener('DOMContentLoaded',loadBoardsDropdown);}else{loadBoardsDropdown();}"
        "})();");
    cwist_html_element_add_child(head, script);

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
    cwist_html_element_set_text(burger_icon, "◇");
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

    cwist_html_element_t *search_form = cwist_html_element_create("form");
    cwist_html_element_add_attr(search_form, "action", "/search");
    cwist_html_element_add_attr(search_form, "method", "get");
    cwist_html_element_add_class(search_form, "topbar-search");
    cwist_html_element_t *search_input = cwist_html_element_create("input");
    cwist_html_element_add_attr(search_input, "type", "text");
    cwist_html_element_add_attr(search_input, "name", "search");
    cwist_html_element_add_attr(search_input, "placeholder", "Search...");
    cwist_html_element_add_attr(search_input, "required", "");
    cwist_html_element_t *search_btn = cwist_html_element_create("button");
    cwist_html_element_add_attr(search_btn, "type", "submit");
    cwist_html_element_add_attr(search_btn, "class", "btn");
    cwist_html_element_set_text(search_btn, "Search");
    cwist_html_element_add_child(search_form, search_input);
    cwist_html_element_add_child(search_form, search_btn);
    cwist_html_element_add_child(navlinks, search_form);

    cwist_html_element_add_child(navlinks, nav_link("/", "Home"));
    cwist_html_element_t *boards_wrap = cwist_html_element_create("div");
    cwist_html_element_add_class(boards_wrap, "nav-board-dropdown");
    cwist_html_element_add_class(boards_wrap, "desktop-only");
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
    cwist_html_element_t *mobile_boards = nav_link("/boards", "All Boards");
    cwist_html_element_add_class(mobile_boards, "mobile-only");
    cwist_html_element_add_child(navlinks, mobile_boards);
    cwist_html_element_add_child(navlinks, nav_link("/files", "Files"));
    if (user_role && strcmp(user_role, "admin") == 0) {
        cwist_html_element_add_child(navlinks, nav_link("/admin/users", "Admin"));
    }
    if (user_role && user_role[0]) {
        const char *display_pp = profile_pic;

        if (display_pp && display_pp[0]) {
            cwist_html_element_t *p_link = cwist_html_element_create("a");
            cwist_html_element_add_attr(p_link, "href", "/profile");
            cwist_html_element_t *img = cwist_html_element_create("img");
            if (strncmp(display_pp, "/assets/uploads/", 16) == 0 || strncmp(display_pp, "/file/download/", 15) == 0) {
                cwist_html_element_add_attr(img, "src", "data:image/gif;base64,R0lGODlhAQABAIAAAAAAAP///yH5BAEAAAAALAAAAAABAAEAAAIBRAA7");
                cwist_html_element_add_attr(img, "data-tasfa-download", display_pp);
            } else {
                cwist_html_element_add_attr(img, "src", display_pp);
            }
            cwist_html_element_add_attr(img, "width", "24");
            cwist_html_element_add_attr(img, "height", "24");
            cwist_html_element_add_class(img, "profile-pic-small");
            cwist_html_element_add_child(p_link, img);
            cwist_html_element_add_child(navlinks, p_link);
        } else {
            cwist_html_element_add_child(navlinks, nav_link("/profile", "Profile"));
        }
        cwist_html_element_add_child(navlinks, nav_link("/logout", "Logout"));
    } else {
        cwist_html_element_add_child(navlinks, nav_link("/login", "Login"));
        cwist_html_element_add_child(navlinks, nav_link("/register", "Register"));
    }

    /* Single theme toggle */
    cwist_html_element_t *theme_wrapper = cwist_html_element_create("div");
    cwist_html_element_add_class(theme_wrapper, "theme-switch");

    cwist_html_element_t *theme_btn = cwist_html_element_create("button");
    cwist_html_element_add_attr(theme_btn, "type", "button");
    cwist_html_element_add_attr(theme_btn, "onclick", "toggleTheme()");
    cwist_html_element_add_attr(theme_btn, "class", "btn btn-outline theme-toggle-btn");
    cwist_html_element_add_attr(theme_btn, "aria-label", dark ? "Switch to light mode" : "Switch to dark mode");
    cwist_html_element_set_text(theme_btn, "");
    cwist_html_element_t *theme_icon = cwist_html_element_create("span");
    cwist_html_element_add_attr(theme_icon, "id", "theme-spin");
    cwist_html_element_add_attr(theme_icon, "aria-hidden", "true");
    cwist_html_element_add_class(theme_icon, "theme-spin-icon");
    cwist_html_element_set_text(theme_icon, "◇");
    cwist_html_element_add_child(theme_btn, theme_icon);
    cwist_html_element_t *theme_label = cwist_html_element_create("span");
    cwist_html_element_add_attr(theme_label, "id", "theme-toggle-label");
    cwist_html_element_set_text(theme_label, dark ? "Light" : "Dark");
    cwist_html_element_add_child(theme_btn, theme_label);
    cwist_html_element_add_child(theme_wrapper, theme_btn);
    cwist_html_element_add_child(navlinks, theme_wrapper);

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
    cwist_html_element_add_class(footer_logo, "footer-logo");
    cwist_html_element_add_child(footer_content, footer_logo);

    cwist_html_element_add_child(footer, footer_content);

    cwist_html_element_add_child(body, nav);
    cwist_html_element_add_child(body, navlinks);

    cwist_html_element_t *overlay = cwist_html_element_create("div");
    cwist_html_element_add_class(overlay, "mobile-overlay");
    cwist_html_element_add_attr(overlay, "onclick", "toggleMobileNav()");
    cwist_html_element_add_child(body, overlay);

    cwist_html_element_add_child(body, shell);
    cwist_html_element_add_child(body, footer);

    if (g_config.use_rss) {
        cwist_html_element_t *rss_link = cwist_html_element_create("a");
        cwist_html_element_add_attr(rss_link, "href", "/rss.xml");
        cwist_html_element_add_attr(rss_link, "title", "RSS Feed");
        cwist_html_element_add_attr(rss_link, "aria-label", "RSS Feed");
        cwist_html_element_add_attr(rss_link, "target", "_blank");
        cwist_html_element_add_class(rss_link, "rss-corner-btn");

        cwist_html_element_t *rss_icon = cwist_html_element_create("svg");
        cwist_html_element_add_attr(rss_icon, "xmlns", "http://www.w3.org/2000/svg");
        cwist_html_element_add_attr(rss_icon, "width", "20");
        cwist_html_element_add_attr(rss_icon, "height", "20");
        cwist_html_element_add_attr(rss_icon, "viewBox", "0 0 24 24");
        cwist_html_element_add_attr(rss_icon, "fill", "none");
        cwist_html_element_add_attr(rss_icon, "stroke", "currentColor");
        cwist_html_element_add_attr(rss_icon, "stroke-width", "2.5");
        cwist_html_element_add_attr(rss_icon, "stroke-linecap", "round");
        cwist_html_element_add_attr(rss_icon, "stroke-linejoin", "round");

        cwist_html_element_t *rss_path1 = cwist_html_element_create("path");
        cwist_html_element_add_attr(rss_path1, "d", "M4 11a9 9 0 0 1 9 9");
        cwist_html_element_add_child(rss_icon, rss_path1);

        cwist_html_element_t *rss_path2 = cwist_html_element_create("path");
        cwist_html_element_add_attr(rss_path2, "d", "M4 4a16 16 0 0 1 16 16");
        cwist_html_element_add_child(rss_icon, rss_path2);

        cwist_html_element_t *rss_circle = cwist_html_element_create("circle");
        cwist_html_element_add_attr(rss_circle, "cx", "5");
        cwist_html_element_add_attr(rss_circle, "cy", "19");
        cwist_html_element_add_attr(rss_circle, "r", "1.5");
        cwist_html_element_add_attr(rss_circle, "fill", "currentColor");
        cwist_html_element_add_child(rss_icon, rss_circle);

        cwist_html_element_add_child(rss_link, rss_icon);
        cwist_html_element_add_child(body, rss_link);
    }

    cwist_html_element_t *config_script = cwist_html_element_create("script");
    char config_js[256];
    snprintf(config_js, sizeof(config_js),
             "window.BLOG_USE_TASFA=%s;window.BLOG_MAX_UPLOAD_SIZE=%lld;",
             g_config.use_tasfa ? "true" : "false",
             g_config.max_upload_size);
    cwist_html_element_set_text(config_script, config_js);
    cwist_html_element_add_child(body, config_script);

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

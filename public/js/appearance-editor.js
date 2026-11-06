(function(){
    'use strict';
    var form=document.getElementById('appearance-form');
    var frame=document.getElementById('appearance-preview');
    var modeSelect=document.getElementById('appearance-preview-mode');
    var pageSelect=document.getElementById('appearance-preview-page');
    var sizeSelect=document.getElementById('appearance-preview-size');
    if(!form||!frame||!modeSelect||!pageSelect||!sizeSelect)return;
    var themes=[];
    var uploadedTheme=null;
    var fontUrl=null;
    var scriptUrl=null;
    var renderQueued=false;

    function field(name){
        var element=form.elements[name];
        return element&&typeof element.value==='string'?element.value:'';
    }
    function selectedTheme(){
        return field(modeSelect.value==='dark'?'dark_theme':'light_theme');
    }
    function themeByName(name){
        if(name==='custom'&&uploadedTheme)return uploadedTheme;
        for(var i=0;i<themes.length;i++)if(themes[i].name===name)return themes[i];
        return themes[0]||{name:'light',vars:{},rules:[]};
    }
    function buildThemeCss(theme){
        var css=':root{';
        Object.keys(theme.vars||{}).forEach(function(key){css+=key+':'+theme.vars[key]+';';});
        css+='}';
        (theme.rules||[]).forEach(function(rule){
            css+=rule.sel+'{';
            Object.keys(rule.decls||{}).forEach(function(key){css+=key+':'+rule.decls[key]+';';});
            css+='}';
        });
        return css;
    }
    function withUploadedFont(stack){
        return fontUrl?'"'+field('font_face_family').replace(/["']/g,'')+'",'+stack:stack;
    }
    function typographyCss(){
        var body=withUploadedFont(field('font_body'));
        var heading=withUploadedFont(field('font_heading'));
        var ui=withUploadedFont(field('font_ui'));
        var code=withUploadedFont(field('font_code'));
        var quote=withUploadedFont(field('font_blockquote'));
        var display=withUploadedFont(field('font_display'));
        var css='body{font-family:'+body+';font-weight:'+field('font_weight_body')+';letter-spacing:'+field('letter_spacing_body')+';}';
        css+='h1,h2,h3,h4,h5,h6,.topbar-title,.brand-title{font-family:'+heading+';}';
        css+='.nav-item,.btn,.post-meta,.comment-meta,.tag,.post-row-meta{font-family:'+ui+';}';
        css+='pre,code{font-family:'+code+';}blockquote{font-family:'+quote+';}';
        css+='.board-line-title,.post-row-title{font-family:'+display+';}';
        var selectors={
            h1:'h1',h2:'h2',h3:'h3',h4:'h4',h5h6:'h5,h6',
            topbar_title:'.topbar-title,.brand-title',btn:'.btn',
            board_line_title:'.board-line-title',hero_h1:'.hero h1',
            post_h1:'.post-title',md_h1:'.markdown-body h1',md_h2:'.markdown-body h2',md_h3:'.markdown-body h3'
        };
        Object.keys(selectors).forEach(function(key){
            css+=selectors[key]+'{font-weight:'+field('font_weight_'+key)+';letter-spacing:'+field('letter_spacing_'+key)+';}';
        });
        css+='.hero p{letter-spacing:'+field('letter_spacing_hero_p')+';}';
        return css;
    }
    function fontSourceCss(){
        var css='';
        if(field('font_import_url'))css+='@import url("'+field('font_import_url').replace(/"/g,'')+'");';
        if(fontUrl)css+='@font-face{font-family:"'+field('font_face_family').replace(/["']/g,'')+'";font-display:swap;src:url("'+fontUrl+'");}';
        else if(field('font_face_src'))css+='@font-face{font-family:"'+field('font_face_family').replace(/["']/g,'')+'";font-display:swap;src:'+field('font_face_src')+';}';
        return css;
    }
    function layoutCss(){
        var density=field('density');
        var rowPadding=density==='compact'?'10px 0':density==='spacious'?'42px 0':'28px 0';
        var width=field('content_width')==='narrow'?'1100px':field('content_width')==='wide'?'2200px':'1800px';
        var css='.shell{max-width:'+width+';margin:0 auto;padding:32px;box-sizing:border-box;}';
        css+='.post-row,.post-row.featured{padding:'+rowPadding+';}.board-post-item{padding:'+(density==='compact'?'8px 12px':density==='spacious'?'28px 20px':'20px 18px')+';}';
        css+='.board-list{gap:'+(density==='compact'?'12px':density==='spacious'?'40px':'28px')+';}';
        if(field('content_style')==='community'){
            css+='.hero{padding-top:28px;padding-bottom:28px;}.hero h1{font-size:clamp(1.8rem,3vw,2.8rem);}';
            css+='.post-row-title,.post-row.featured .post-row-title{font-size:clamp(1.15rem,1.7vw,1.5rem);font-weight:750;line-height:1.35;border-bottom:0;padding-bottom:0;width:auto;}';
            css+='.post-row-summary{font-size:.95rem;line-height:1.45;padding-top:4px;}';
        }
        if(field('list_style')==='cards')css+='.post-list{gap:16px;}.post-row,.post-row.featured{padding:24px;border:1px solid var(--glass-border);border-radius:12px;background:var(--glass-bg);}';
        if(field('list_style')==='table')css+='.post-row,.post-row.featured{padding-top:10px;padding-bottom:10px;}.post-row-title,.post-row.featured .post-row-title{display:inline;font-size:1rem;font-weight:650;line-height:1.4;border-bottom:0;padding-bottom:0;}.post-row-summary{display:none;}.post-row-head{display:inline-flex;padding:0 8px 0 0;}.post-row-meta{display:inline-flex;padding:0 0 0 10px;font-size:12px;}';
        if(field('navigation')==='hamburger')css+='.burger-btn{display:inline-flex;}.nav-links{display:none;position:absolute;top:58px;right:0;width:min(340px,100%);padding:14px;box-sizing:border-box;flex-direction:column;align-items:stretch;background:var(--panel);border:1px solid var(--glass-border);border-radius:12px;z-index:20;}.nav-links.open{display:flex;}';
        else css+='.burger-btn{display:none;}';
        return css;
    }
    function navigationMarkup(){
        return '<header class="topbar"><button type="button" class="burger-btn" aria-label="Menu" aria-expanded="false"><span class="burger-icon">☰</span></button>'+
            '<a class="topbar-brand" href="#"><span class="topbar-title">Fly Board</span></a>'+
            '<nav class="nav-links"><a class="nav-item">Home</a><a class="nav-item">Boards</a><a class="nav-item">Archive</a><a class="nav-item">Files</a></nav>'+
            '<button class="btn btn-outline" type="button">○</button></header>';
    }
    function listMarkup(){
        return '<section class="hero"><h1>Stories with character</h1><p>한글, English, и Кириллица in a live layout preview.</p></section>'+
            '<section class="board-line"><div class="board-line-head"><h2 class="board-line-title">Engineering notes</h2><span class="tag">SERIES</span></div>'+
            '<p class="board-card-desc">A blog can emphasize a continuing series, while a community keeps discussion scanning fast.</p></section>'+
            '<div class="post-list board-typography-list">'+
            '<article class="post-row featured"><div class="post-row-head"><span class="tag">WASM</span></div><a class="post-row-title">Building a visual tag map with a small WASM kernel</a>'+
            '<p class="post-row-summary">A featured story with enough excerpt text to compare editorial and community presentation.</p><div class="post-row-meta"><span>6 Nov 2026</span><span>12 comments</span><span>5 min read</span></div></article>'+
            '<article class="post-row"><div class="post-row-head"><span class="tag">CWIST</span></div><a class="post-row-title">A practical guide to streaming responses</a>'+
            '<p class="post-row-summary">Change density, list style, content width, and navigation to compare the actual reading rhythm.</p><div class="post-row-meta"><span>4 Nov 2026</span><span>8 comments</span></div></article>'+
            '<article class="post-row"><div class="post-row-head"><span class="tag">SQLite</span></div><a class="post-row-title">Notes on durable community writes</a>'+
            '<p class="post-row-summary">Compact rows remove excerpts and tighten metadata for a conventional board index.</p><div class="post-row-meta"><span>1 Nov 2026</span><span>21 comments</span></div></article></div>';
    }
    function articleMarkup(){
        return '<article class="post-content"><h1 class="post-title">A typographically detailed post</h1><div class="post-meta">6 November 2026 · 5 min read</div>'+
            '<div class="markdown-body"><h1>Markdown heading one</h1><h2>Markdown heading two</h2><h3>Markdown heading three</h3>'+
            '<p>Adjust the controls and this text changes immediately. 가독성과 개성을 함께 확인할 수 있습니다.</p>'+
            '<blockquote>A quote can use a contrasting serif stack without changing body copy.</blockquote>'+
            '<pre><code>const preview = "live";\nconsole.log(preview);</code></pre></div></article>';
    }
    function previewMarkup(){
        return navigationMarkup()+'<main class="shell">'+(pageSelect.value==='article'?articleMarkup():listMarkup())+'</main>';
    }
    function render(){
        renderQueued=false;
        var theme=themeByName(selectedTheme());
        var externalScript=scriptUrl||field('font_script_url');
        var script=externalScript?'<script src="'+externalScript.replace(/["<>]/g,'')+'"><\/script>':'';
        var bodyClass='nav-'+field('navigation')+' layout-'+field('content_style')+' density-'+field('density')+' list-'+field('list_style')+' width-'+field('content_width');
        frame.srcdoc='<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><style>'+
            fontSourceCss()+buildThemeCss(theme)+typographyCss()+layoutCss()+
            '.topbar{display:flex;justify-content:space-between;align-items:center;position:relative}.nav-links{display:flex;gap:12px;align-items:center}.topbar-brand{text-decoration:none}.post-content{margin-top:24px}.markdown-body{line-height:1.75}pre{overflow:auto;padding:14px}blockquote{margin-left:0;padding-left:16px}.hero{margin-bottom:24px}.board-line{margin-bottom:24px}.tag{display:inline-block;padding:3px 8px}.post-row-title{cursor:pointer}.post-list{padding:0}.shell{width:100%;box-sizing:border-box}@media(max-width:600px){.shell{padding:18px}.post-row-meta{display:flex;padding:6px 0 0}}</style>'+
            '</head><body class="'+bodyClass+'">'+previewMarkup()+script+'<script>var b=document.querySelector(".burger-btn"),n=document.querySelector(".nav-links");if(b)b.addEventListener("click",function(){var o=n.classList.toggle("open");b.setAttribute("aria-expanded",o?"true":"false");});<\/script></body></html>';
    }
    function scheduleRender(){
        if(renderQueued)return;
        renderQueued=true;
        setTimeout(render,0);
    }
    function readFile(input,asText,callback){
        var file=input.files&&input.files[0];
        if(!file)return;
        var reader=new FileReader();
        reader.onload=function(){callback(reader.result);scheduleRender();};
        reader.onerror=function(){input.setCustomValidity('Could not read this file');input.reportValidity();};
        if(asText)reader.readAsText(file);else reader.readAsDataURL(file);
    }
    form.addEventListener('input',function(event){
        if(event.target.matches('[data-appearance-setting]'))scheduleRender();
    });
    form.addEventListener('change',function(event){
        var target=event.target;
        if(target.matches('[data-appearance-setting]'))scheduleRender();
        if(target.matches('[data-theme-radio]')){
            modeSelect.value=target.name==='dark_theme'?'dark':'light';
            scheduleRender();
        }else if(target.matches('[data-preview-theme]')){
            readFile(target,true,function(text){
                try{
                    var parsed=JSON.parse(text);
                    if(!parsed.vars||!Array.isArray(parsed.rules))throw new Error('vars and rules are required');
                    parsed.name='custom';uploadedTheme=parsed;target.setCustomValidity('');
                    form.querySelectorAll('[data-custom-choice]').forEach(function(choice){choice.disabled=false;});
                }catch(error){target.setCustomValidity(error.message);target.reportValidity();}
            });
        }else if(target.matches('[data-preview-font]')){
            if(fontUrl)URL.revokeObjectURL(fontUrl);
            fontUrl=null;
            readFile(target,false,function(url){fontUrl=url;});
        }else if(target.matches('[data-preview-script]')){
            if(scriptUrl)URL.revokeObjectURL(scriptUrl);
            scriptUrl=null;
            var file=target.files&&target.files[0];
            if(file)scriptUrl=URL.createObjectURL(file);
            scheduleRender();
        }
    });
    document.getElementById('appearance-reset').addEventListener('click',function(){
        uploadedTheme=null;
        if(fontUrl)URL.revokeObjectURL(fontUrl);
        if(scriptUrl)URL.revokeObjectURL(scriptUrl);
        fontUrl=null;scriptUrl=null;
        setTimeout(scheduleRender,0);
    });
    modeSelect.addEventListener('change',scheduleRender);
    pageSelect.addEventListener('change',scheduleRender);
    sizeSelect.addEventListener('change',function(){
        frame.classList.toggle('appearance-preview-mobile',sizeSelect.value==='mobile');
        scheduleRender();
    });
    fetch('/themes.json',{credentials:'same-origin'}).then(function(response){
        if(!response.ok)throw new Error('Theme list unavailable');
        return response.json();
    }).then(function(list){themes=list;scheduleRender();}).catch(function(error){
        var note=document.createElement('p');
        note.className='appearance-note';note.textContent=error.message;
        frame.parentNode.appendChild(note);
    });
})();

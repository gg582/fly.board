(function(){
    'use strict';
    var form=document.getElementById('appearance-form');
    var frame=document.getElementById('appearance-preview');
    var modeSelect=document.getElementById('appearance-preview-mode');
    if(!form||!frame||!modeSelect)return;
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
    function previewMarkup(){
        return '<header class="topbar"><a class="brand-title" href="#">Fly Board</a><nav class="nav-links"><a class="nav-item">Boards</a><a class="nav-item">Archive</a><button class="btn">New Post</button></nav></header>'+
            '<main class="container"><section class="hero"><h1>Stories with character</h1><p>한글, English, и Кириллица in a live glassmorphism preview.</p></section>'+
            '<section class="board-line"><div class="board-line-head"><h2 class="board-line-title">Engineering notes</h2><span class="tag">WASM</span></div>'+
            '<p class="board-card-desc">A board title, metadata, buttons, and body text use their own font settings.</p></section>'+
            '<article class="post-content"><h1 class="post-title">A typographically detailed post</h1><div class="post-meta">6 November 2026 · 5 min read</div>'+
            '<div class="markdown-body"><h1>Markdown heading one</h1><h2>Markdown heading two</h2><h3>Markdown heading three</h3>'+
            '<p>Adjust the controls and this text changes immediately. 가독성과 개성을 함께 확인할 수 있습니다.</p>'+
            '<blockquote>A quote can use a contrasting serif stack without changing body copy.</blockquote>'+
            '<pre><code>const preview = "live";\nconsole.log(preview);</code></pre></div></article></main>';
    }
    function render(){
        renderQueued=false;
        var theme=themeByName(selectedTheme());
        var externalScript=scriptUrl||field('font_script_url');
        var script=externalScript?'<script src="'+externalScript.replace(/["<>]/g,'')+'"><\/script>':'';
        frame.srcdoc='<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><style>'+
            fontSourceCss()+buildThemeCss(theme)+typographyCss()+
            '.container{padding-top:24px}.topbar{display:flex;justify-content:space-between;align-items:center}.nav-links{display:flex;gap:12px;align-items:center}.brand-title{text-decoration:none}.post-content{margin-top:24px}.markdown-body{line-height:1.75}pre{overflow:auto;padding:14px}blockquote{margin-left:0;padding-left:16px}</style>'+
            '</head><body>'+previewMarkup()+script+'</body></html>';
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
    fetch('/themes.json',{credentials:'same-origin'}).then(function(response){
        if(!response.ok)throw new Error('Theme list unavailable');
        return response.json();
    }).then(function(list){themes=list;scheduleRender();}).catch(function(error){
        var note=document.createElement('p');
        note.className='appearance-note';note.textContent=error.message;
        frame.parentNode.appendChild(note);
    });
})();

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
    /* Mirrors layout_panel_radius_css() in src/config/config.c. */
    function panelRadius(){
        var mode=field('panel_radius');
        if(mode==='site'){
            var px=32*(parseFloat(form.getAttribute('data-site-roundness'))||0);
            return px>0?px.toFixed(1)+'px':'0';
        }
        if(mode==='custom')return (parseInt(field('panel_radius_px'),10)||0)+'px';
        return field('panel_style')==='square'?'0':field('panel_style')==='round'?'18px':'32px';
    }
    function panelSide(){
        return field('panel_side')==='right'?'right':'left';
    }
    function panelWidth(){
        var w=parseInt(field('panel_width'),10);
        return isNaN(w)?380:Math.max(240,Math.min(520,w));
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
        if(field('navigation')==='hamburger'){
            var panelColor=field('panel_color')||'var(--panel)';
            var right=panelSide()==='right';
            css+='.burger-btn{display:inline-flex;'+(right?'order:99;':'')+'}.nav-links{display:none;position:absolute;top:58px;'+(right?'right:0;left:auto;':'left:0;right:auto;')+'width:min('+panelWidth()+'px,100%);padding:14px;box-sizing:border-box;flex-direction:column;align-items:stretch;background:'+panelColor+';border:1px solid var(--glass-border);border-radius:'+panelRadius()+';z-index:20;}.nav-links.open{display:flex;}';
            if(field('panel_style')==='bubble')css+='.nav-links{border-width:2px;box-shadow:0 24px 70px var(--shadow);}';
        }else css+='.burger-btn{display:none;}';
        return css;
    }
    function transparentFormats(){
        var all=form.elements.transparent_format_all;
        if(all&&all.checked)return 'all';
        var selected=[];
        form.querySelectorAll('[data-transparent-format]:checked').forEach(function(input){selected.push(input.value);});
        return selected.join(',');
    }
    function transparentImageCss(){
        var control=form.elements.invert_transparent_images;
        if(!control||!control.checked)return '';
        var formats=transparentFormats();
        if(formats!=='all'&&formats.split(',').indexOf('svg')<0)return '';
        return 'html[data-theme-base=dark] body.transparent-image-invert-enabled img[data-svg-transparent=true]:not(.hero-bg){filter:invert(1) hue-rotate(180deg);}';
    }
    function navigationMarkup(){
        return '<header class="topbar"><button type="button" class="burger-btn" aria-label="Menu" aria-expanded="true"><span class="burger-icon">☰</span></button>'+
            '<a class="topbar-brand" href="#"><span class="topbar-title">Fly Board</span></a>'+
            '<nav class="nav-links open"><div class="appearance-panel-resize" role="separator" aria-orientation="vertical" aria-label="Resize menu panel" aria-valuemin="240" aria-valuemax="520" aria-valuenow="'+panelWidth()+'" tabindex="0"><span class="appearance-panel-size">'+panelWidth()+' px</span></div><a class="nav-item">Home</a><a class="nav-item">Boards</a><a class="nav-item">Archive</a><a class="nav-item">Files</a></nav>'+
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
            '<figure><img data-svg-transparent="true" alt="Transparent SVG preview" width="180" height="72" src="data:image/svg+xml,%3Csvg xmlns=%22http://www.w3.org/2000/svg%22 width=%22180%22 height=%2272%22 viewBox=%220 0 180 72%22%3E%3Ccircle cx=%2236%22 cy=%2236%22 r=%2224%22 fill=%22black%22/%3E%3Cpath d=%22M76 24h80M76 40h64M76 56h48%22 stroke=%22black%22 stroke-width=%226%22 stroke-linecap=%22round%22/%3E%3C/svg%3E"><figcaption>Transparent background SVG</figcaption></figure>'+
            '<blockquote>A quote can use a contrasting serif stack without changing body copy.</blockquote>'+
            '<pre><code class="hljs language-c">'+
            '<span class="hljs-meta">#<span class="hljs-keyword">include</span> <span class="hljs-string">&lt;stdio.h&gt;</span></span>\n\n'+
            '<span class="hljs-comment">/* Preview of the code highlight theme */</span>\n'+
            '<span class="hljs-type">int</span> <span class="hljs-title function_">main</span><span class="hljs-params">(<span class="hljs-type">void</span>)</span> {\n'+
            '    <span class="hljs-type">const</span> <span class="hljs-type">char</span> *mode = <span class="hljs-string">&quot;live&quot;</span>;\n'+
            '    <span class="hljs-keyword">for</span> (<span class="hljs-type">int</span> i = <span class="hljs-number">0</span>; i &lt; <span class="hljs-number">3</span>; i++)\n'+
            '        <span class="hljs-built_in">printf</span>(<span class="hljs-string">&quot;%s %d\\n&quot;</span>, mode, i);\n'+
            '    <span class="hljs-keyword">return</span> <span class="hljs-literal">NULL</span> == mode;\n}</code></pre></div></article>';
    }
    function codeThemeLink(){
        var select=form.elements[modeSelect.value==='dark'?'code_theme_dark':'code_theme_light'];
        var option=select&&select.options[select.selectedIndex];
        var href=option&&option.getAttribute('data-href');
        return href?'<link rel="stylesheet" href="'+href.replace(/["<>]/g,'')+'">':'';
    }
    function previewMarkup(){
        return navigationMarkup()+'<main class="shell">'+(pageSelect.value==='article'?articleMarkup():listMarkup())+'</main>';
    }
    function render(){
        renderQueued=false;
        var theme=themeByName(selectedTheme());
        var externalScript=scriptUrl||field('font_script_url');
        var script=externalScript?'<script src="'+externalScript.replace(/["<>]/g,'')+'"><\/script>':'';
        var bodyClass='nav-'+field('navigation')+' layout-'+field('content_style')+' density-'+field('density')+' list-'+field('list_style')+' width-'+field('content_width')+' panel-'+field('panel_style');
        if(form.elements.invert_transparent_images&&form.elements.invert_transparent_images.checked)bodyClass+=' transparent-image-invert-enabled';
        /* KDE Plasma 6 style edit handle: drag the panel's inner edge (or use
         * the arrow keys) to resize; the width snaps to 10 px and is sent
         * back to the form. */
        var panelScript='<script>(function(){var b=document.querySelector(".burger-btn"),n=document.querySelector(".nav-links"),h=document.querySelector(".appearance-panel-resize"),label=h&&h.querySelector(".appearance-panel-size"),side="'+panelSide()+'",w='+panelWidth()+',drag=null;'+
            'if(b)b.addEventListener("click",function(){var o=n.classList.toggle("open");b.setAttribute("aria-expanded",o?"true":"false");});'+
            'if(!h)return;'+
            'function set(nw,commit){nw=Math.max(240,Math.min(520,Math.round(nw/10)*10));w=nw;n.style.width=nw+"px";label.textContent=nw+" px";h.setAttribute("aria-valuenow",nw);if(commit)parent.postMessage({type:"fly-panel-width",width:nw},"*");}'+
            'h.addEventListener("pointerdown",function(e){e.preventDefault();drag={x:e.clientX,w:n.getBoundingClientRect().width};try{h.setPointerCapture(e.pointerId);}catch(_){}n.classList.add("resizing");});'+
            'h.addEventListener("pointermove",function(e){if(!drag)return;var dx=e.clientX-drag.x;set(side==="right"?drag.w-dx:drag.w+dx,false);});'+
            'function end(e){if(!drag)return;drag=null;try{h.releasePointerCapture(e.pointerId);}catch(_){}n.classList.remove("resizing");set(w,true);}'+
            'h.addEventListener("pointerup",end);h.addEventListener("pointercancel",end);'+
            'h.addEventListener("keydown",function(e){var grow=side==="right"?"ArrowLeft":"ArrowRight",shrink=side==="right"?"ArrowRight":"ArrowLeft";if(e.key===grow){e.preventDefault();set(w+10,true);}else if(e.key===shrink){e.preventDefault();set(w-10,true);}});'+
            '})();<\/script>';
        var resizeCss='.appearance-panel-resize{display:none;position:absolute;top:0;bottom:0;'+(panelSide()==='right'?'left:-6px;':'right:-6px;')+'width:12px;cursor:ew-resize;touch-action:none;user-select:none;z-index:2}'+
            '.appearance-panel-resize::before{content:"";position:absolute;top:50%;left:4px;width:4px;height:44px;margin-top:-22px;border-radius:2px;background:var(--accent,#3daee9);opacity:.55;transition:opacity .15s}'+
            '.appearance-panel-resize:hover::before,.appearance-panel-resize:focus-visible::before,.resizing .appearance-panel-resize::before{opacity:1}'+
            '.appearance-panel-size{position:absolute;top:50%;'+(panelSide()==='right'?'left:16px;':'right:16px;')+'transform:translateY(-50%);padding:3px 8px;border-radius:6px;background:var(--accent,#3daee9);color:#fff;font:600 11px/1.4 system-ui,sans-serif;white-space:nowrap;opacity:0;pointer-events:none;transition:opacity .15s}'+
            '.appearance-panel-resize:hover .appearance-panel-size,.appearance-panel-resize:focus-visible .appearance-panel-size,.resizing .appearance-panel-size{opacity:1}'+
            '.nav-hamburger .appearance-panel-resize{display:block}.nav-links.resizing{outline:2px dashed var(--accent,#3daee9);outline-offset:2px}';
        frame.srcdoc='<!doctype html><html data-theme-base="'+modeSelect.value+'"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">'+codeThemeLink()+'<style>'+
            fontSourceCss()+buildThemeCss(theme)+typographyCss()+layoutCss()+transparentImageCss()+
            '.topbar{display:flex;justify-content:space-between;align-items:center;position:relative}.nav-links{display:flex;gap:12px;align-items:center}.topbar-brand{text-decoration:none}.post-content{margin-top:24px}.markdown-body{line-height:1.75}pre{overflow:auto;padding:14px}blockquote{margin-left:0;padding-left:16px}.hero{margin-bottom:24px}.board-line{margin-bottom:24px}.tag{display:inline-block;padding:3px 8px}.post-row-title{cursor:pointer}.post-list{padding:0}.shell{width:100%;box-sizing:border-box}'+resizeCss+'@media(max-width:600px){.shell{padding:18px}.post-row-meta{display:flex;padding:6px 0 0}}</style>'+
            '</head><body class="'+bodyClass+'" data-invert-transparent-formats="'+transparentFormats()+'">'+previewMarkup()+script+panelScript+'</body></html>';
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
    var colorEnabled=document.getElementById('appearance-panel-color-enabled');
    var colorPicker=document.getElementById('appearance-panel-color-picker');
    var colorAlpha=document.getElementById('appearance-panel-alpha');
    function syncPanelColor(){
        var alpha=Math.round(parseInt(colorAlpha.value,10)*255/100).toString(16).padStart(2,'0');
        form.elements.panel_color.value=colorEnabled.checked?colorPicker.value+alpha:'';
        scheduleRender();
    }
    [colorPicker,colorAlpha].forEach(function(control){control.addEventListener('input',syncPanelColor);});
    colorEnabled.addEventListener('change',syncPanelColor);
    window.addEventListener('message',function(event){
        if(event.source!==frame.contentWindow||!event.data||event.data.type!=='fly-panel-width')return;
        var width=Math.max(240,Math.min(520,parseInt(event.data.width,10)||380));
        form.elements.panel_width.value=String(width);
        document.getElementById('appearance-panel-width-value').value=String(width);
    });
    var radiusSlider=form.elements.panel_radius_px;
    function syncRadiusControls(){
        if(radiusSlider)radiusSlider.disabled=field('panel_radius')!=='custom';
    }
    form.addEventListener('input',function(event){
        if(event.target.matches('[data-panel-radius]'))document.getElementById('appearance-panel-radius-value').value=event.target.value;
        if(event.target.matches('[data-appearance-setting]'))scheduleRender();
    });
    form.addEventListener('change',function(event){
        var target=event.target;
        if(target.name==='panel_radius')syncRadiusControls();
        if(target.matches('[data-code-theme]')){
            // Show the changed mode on an article so the code block is visible.
            modeSelect.value=target.name==='code_theme_dark'?'dark':'light';
            pageSelect.value='article';
        }
        if(target.matches('[data-transparent-format]'))form.elements.transparent_format_all.checked=false;
        if(target.matches('[data-transparent-format-all]')){
            form.querySelectorAll('[data-transparent-format]').forEach(function(input){input.checked=target.checked;});
        }
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
        setTimeout(function(){
            syncRadiusControls();
            document.getElementById('appearance-panel-width-value').value=field('panel_width');
            document.getElementById('appearance-panel-radius-value').value=field('panel_radius_px');
            scheduleRender();
        },0);
    });
    modeSelect.addEventListener('change',scheduleRender);
    pageSelect.addEventListener('change',scheduleRender);
    sizeSelect.addEventListener('change',function(){
        frame.classList.toggle('appearance-preview-mobile',sizeSelect.value==='mobile');
        scheduleRender();
    });
    /* Files go up in chunks before the form is submitted, so the final POST
     * stays small whatever the font size. */
    var STAGE_SLOTS=['font_file','font_script_file','theme_file'],STAGE_CHUNK=2*1024*1024,staging=false;
    function stageToken(){
        var bytes=new Uint8Array(16),hex='';
        (window.crypto||window.msCrypto).getRandomValues(bytes);
        for(var i=0;i<bytes.length;i++)hex+=(bytes[i]<16?'0':'')+bytes[i].toString(16);
        return hex;
    }
    function stageFile(slot,file,token,progress){
        var offset=0;
        function next(attempt){
            if(offset>=file.size)return Promise.resolve();
            var end=Math.min(file.size,offset+STAGE_CHUNK);
            var url='/admin/appearance/upload?slot='+slot+'&token='+token+'&offset='+offset+'&total='+file.size;
            return fetch(url,{method:'POST',credentials:'same-origin',headers:{'Content-Type':'application/octet-stream'},body:file.slice(offset,end)})
                .then(function(response){
                    if(!response.ok){var e=new Error(response.status===413?'File is too large':'Upload failed ('+response.status+')');e.fatal=response.status===413||response.status===400;throw e;}
                    offset=end;progress(offset);return next(0);
                },function(error){throw error;})
                .catch(function(error){
                    if(error.fatal||attempt>=2)throw error;
                    return new Promise(function(r){setTimeout(r,500*(attempt+1));}).then(function(){return next(attempt+1);});
                });
        }
        return next(0);
    }
    form.addEventListener('submit',function(event){
        if(form.getAttribute('data-staged')==='1')return;
        var inputs=STAGE_SLOTS.map(function(name){return form.elements[name];}).filter(function(input){return input&&input.files&&input.files[0];});
        if(!inputs.length)return;
        event.preventDefault();
        if(staging)return;
        staging=true;
        var button=form.querySelector('button[type=submit]');
        var status=document.getElementById('appearance-upload-status');
        if(!status){status=document.createElement('span');status.id='appearance-upload-status';status.className='appearance-note';status.setAttribute('role','status');button.parentNode.appendChild(status);}
        var total=0,done=0;
        inputs.forEach(function(input){total+=input.files[0].size;});
        button.disabled=true;
        var chain=Promise.resolve(),staged=[];
        inputs.forEach(function(input){
            chain=chain.then(function(){
                var file=input.files[0],token=stageToken(),base=done;
                return stageFile(input.name,file,token,function(sent){
                    status.textContent='Uploading '+Math.round((base+sent)*100/Math.max(1,total))+'%';
                }).then(function(){done+=file.size;staged.push({input:input,token:token,name:file.name});});
            });
        });
        chain.then(function(){
            staged.forEach(function(item){
                [['staged_'+item.input.name,item.token],['staged_'+item.input.name+'_name',item.name]].forEach(function(pair){
                    var hidden=document.createElement('input');
                    hidden.type='hidden';hidden.name=pair[0];hidden.value=pair[1];hidden.setAttribute('data-staged-field','');
                    form.appendChild(hidden);
                });
                item.input.disabled=true;
            });
            status.textContent='Saving…';
            form.setAttribute('data-staged','1');
            form.submit();
        }).catch(function(error){
            staging=false;button.disabled=false;
            status.textContent=error.message||'Upload failed';
        });
    });
    window.addEventListener('pageshow',function(event){
        if(!event.persisted)return;
        // Back/forward cache: undo the staged submit so the form works again.
        form.removeAttribute('data-staged');staging=false;
        form.querySelectorAll('[data-staged-field]').forEach(function(el){el.remove();});
        STAGE_SLOTS.forEach(function(name){if(form.elements[name])form.elements[name].disabled=false;});
        var button=form.querySelector('button[type=submit]');if(button)button.disabled=false;
    });
    var themeVersion=document.documentElement.getAttribute('data-theme-version')||'0';
    fetch('/themes.json?v='+encodeURIComponent(themeVersion),{credentials:'same-origin'}).then(function(response){
        if(!response.ok)throw new Error('Theme list unavailable');
        return response.json();
    }).then(function(list){themes=list;scheduleRender();}).catch(function(error){
        var note=document.createElement('p');
        note.className='appearance-note';note.textContent=error.message;
        frame.parentNode.appendChild(note);
    });
})();

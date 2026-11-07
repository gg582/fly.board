(function(root,factory){
    var api=factory();
    if(typeof module==='object'&&module.exports){module.exports=api;return;}
    root.FlyboardTransparentImages=api;
    root.FlyboardSvgInvert=api;
    function start(){api.observe(document);}
    if(document.readyState==='loading')document.addEventListener('DOMContentLoaded',start,{once:true});
    else start();
})(typeof window!=='undefined'?window:globalThis,function(){
    'use strict';
    var SELECTOR='img:not(.hero-bg):not([data-logo-img]):not([data-no-svg-invert]):not([data-no-image-invert])';

    function normalizeFormat(format){
        format=(format||'').toLowerCase();
        if(format==='jpeg')return 'jpg';
        if(format==='svg+xml')return 'svg';
        if(format==='x-icon'||format==='vnd.microsoft.icon')return 'ico';
        return format;
    }

    function imageFormat(img){
        var src=img.currentSrc||img.getAttribute('src')||'';
        var match=src.match(/\.([a-z0-9]+)(?:[?#]|$)/i);
        if(match)return normalizeFormat(match[1]);
        match=src.match(/^data:image\/([a-z0-9.+-]+)/i);
        if(match)return normalizeFormat(match[1]);
        if(/\/file\/(?:preview|download)\/\d+(?:[?#]|$)/i.test(src)){
            match=(img.getAttribute('alt')||'').match(/\.([a-z0-9]+)$/i);
            if(match)return normalizeFormat(match[1]);
        }
        return '';
    }

    function isSelectedImage(img,doc){
        var formats=doc&&doc.body?doc.body.getAttribute('data-invert-transparent-formats'):'svg';
        if(!formats)formats='svg';
        if(formats==='all')return true;
        var format=imageFormat(img);
        if(!format)return false;
        return formats.split(',').map(function(value){return value.trim();}).indexOf(format)>=0;
    }

    function hasTransparentBackground(pixels,width,height){
        if(!pixels||width<1||height<1||pixels.length<width*height*4)return false;
        var points=[
            [0,0],[width-1,0],[0,height-1],[width-1,height-1],
            [Math.floor(width/2),0],[Math.floor(width/2),height-1],
            [0,Math.floor(height/2)],[width-1,Math.floor(height/2)]
        ];
        var transparent=0;
        for(var i=0;i<points.length;i++){
            var x=points[i][0],y=points[i][1];
            if(pixels[(y*width+x)*4+3]<250)transparent++;
        }
        return transparent*2>=points.length;
    }

    function inspectImage(img,doc){
        if(!img||!img.dataset)return false;
        if(img.dataset.svgTransparencyChecked&&img.dataset.svgTransparencyChecked!=='pending')return true;
        if(!isSelectedImage(img,doc)){img.dataset.svgTransparencyChecked='not-selected';return false;}
        if(!img.complete||!img.naturalWidth||!img.naturalHeight){
            if(img.dataset.svgTransparencyChecked==='pending')return false;
            img.dataset.svgTransparencyChecked='pending';
            img.addEventListener('load',function(){
                delete img.dataset.svgTransparencyChecked;
                inspectImage(img,doc);
            },{once:true});
            img.addEventListener('error',function(){img.dataset.svgTransparencyChecked='unavailable';},{once:true});
            return false;
        }
        try{
            var scale=Math.min(1,64/Math.max(img.naturalWidth,img.naturalHeight));
            var width=Math.max(1,Math.round(img.naturalWidth*scale));
            var height=Math.max(1,Math.round(img.naturalHeight*scale));
            var canvas=doc.createElement('canvas');
            canvas.width=width;canvas.height=height;
            var context=canvas.getContext('2d',{willReadFrequently:true});
            if(!context)throw new Error('2D canvas unavailable');
            context.drawImage(img,0,0,width,height);
            var pixels=context.getImageData(0,0,width,height).data;
            img.dataset.svgTransparent=hasTransparentBackground(pixels,width,height)?'true':'false';
            img.dataset.svgTransparencyChecked='checked';
            return true;
        }catch(error){
            img.dataset.svgTransparencyChecked='unavailable';
            return false;
        }
    }

    function inversionEnabled(doc){
        return doc&&doc.body&&(doc.body.classList.contains('transparent-image-invert-enabled')||
               doc.body.classList.contains('svg-invert-enabled'));
    }

    function scan(root){
        var doc=root.nodeType===9?root:root.ownerDocument;
        if(!inversionEnabled(doc))return;
        if(root.matches&&root.matches(SELECTOR))inspectImage(root,doc);
        if(root.querySelectorAll)root.querySelectorAll(SELECTOR).forEach(function(img){inspectImage(img,doc);});
    }

    function observe(doc){
        scan(doc);
        if(!inversionEnabled(doc)||doc.body.dataset.svgInvertObserved||typeof MutationObserver==='undefined')return;
        doc.body.dataset.svgInvertObserved='1';
        var observer=new MutationObserver(function(records){
            records.forEach(function(record){
                if(record.type==='attributes'){
                    if(!record.target.matches||!record.target.matches(SELECTOR))return;
                    delete record.target.dataset.svgTransparencyChecked;
                    delete record.target.dataset.svgTransparent;
                    inspectImage(record.target,doc);
                    return;
                }
                record.addedNodes.forEach(function(node){if(node.nodeType===1)scan(node);});
            });
        });
        observer.observe(doc.body,{childList:true,subtree:true,attributes:true,attributeFilter:['src']});
    }

    return {imageFormat:imageFormat,hasTransparentBackground:hasTransparentBackground,inspectImage:inspectImage,scan:scan,observe:observe};
});

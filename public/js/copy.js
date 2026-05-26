(function(){
function initCopyButtons(){
document.querySelectorAll('.markdown-body pre').forEach(function(pre){
if(pre.querySelector('.code-copy'))return;
var btn=document.createElement('button');
btn.className='code-copy';
btn.type='button';
btn.textContent='Copy';
btn.addEventListener('click',function(){
var code=pre.querySelector('code');
var text=code?code.textContent:pre.textContent;
navigator.clipboard.writeText(text).then(function(){
btn.textContent='Copied!';
setTimeout(function(){btn.textContent='Copy';},1500);
});
});
pre.appendChild(btn);
});
}
if(document.readyState==='loading'){
document.addEventListener('DOMContentLoaded',initCopyButtons);
}else{
initCopyButtons();
}
})();

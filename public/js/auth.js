(function(){
async function sha512(str){
const buf=await crypto.subtle.digest('SHA-512',new TextEncoder().encode(str));
return Array.from(new Uint8Array(buf)).map(b=>b.toString(16).padStart(2,'0')).join('');
}
document.querySelectorAll('form[action="/login"],form[action="/register"],form[action="/account/password"]').forEach(function(form){
var submitting=false;
form.addEventListener('submit',async function(e){
if(submitting){e.preventDefault();return;}
var pws=form.querySelectorAll('input[type="password"]');
var unhashed=[];
pws.forEach(function(pw){if(!pw.dataset.hashed)unhashed.push(pw);});
if(unhashed.length>0){
e.preventDefault();
submitting=true;
for(var i=0;i<unhashed.length;i++){
var pw=unhashed[i];
var hashVal=await sha512('fly.board'+pw.value);
var hidden=document.createElement('input');
hidden.type='hidden';
hidden.name=pw.name;
hidden.value=hashVal;
form.appendChild(hidden);
pw.removeAttribute('name');
pw.dataset.hashed='1';
}
form.submit();
}
});
});
})();

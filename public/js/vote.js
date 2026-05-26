(function(){
var wrap=document.getElementById('post-voting');
if(!wrap)return;
var pid=wrap.getAttribute('data-post-id');
var uv=parseInt(wrap.getAttribute('data-user-vote'),10)||0;

function updateVoteStyle(v){
var up=document.getElementById('vote-up');
var down=document.getElementById('vote-down');
if(!up||!down)return;
up.style.borderColor=v==1?'var(--accent)':'';
up.style.color=v==1?'var(--accent)':'';
down.style.borderColor=v==-1?'var(--accent)':'';
down.style.color=v==-1?'var(--accent)':'';
}

updateVoteStyle(uv);

function sendVote(vt){
fetch('/post/vote',{
method:'POST',
headers:{'Content-Type':'application/x-www-form-urlencoded'},
body:'post_id='+pid+'&vote_type='+vt
})
.then(function(r){return r.json();})
.then(function(d){
if(d.ok){
document.getElementById('vote-up').innerHTML='&#9650; '+d.up;
document.getElementById('vote-down').innerHTML='&#9660; '+d.down;
updateVoteStyle(d.user_vote);
}
});
}

var upBtn=document.getElementById('vote-up');
var downBtn=document.getElementById('vote-down');
if(upBtn)upBtn.addEventListener('click',function(){sendVote(1);});
if(downBtn)downBtn.addEventListener('click',function(){sendVote(-1);});
})();

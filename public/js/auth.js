(function(){
async function sha512(str){
    if (!window.crypto || !window.crypto.subtle) return null;
    try {
        const buf = await crypto.subtle.digest('SHA-512', new TextEncoder().encode(str));
        return Array.from(new Uint8Array(buf)).map(b => b.toString(16).padStart(2, '0')).join('');
    } catch (e) {
        return null;
    }
}
document.querySelectorAll('form[action="/login"],form[action="/register"],form[action="/account/password"]').forEach(function(form){
    var submitting = false;
    form.addEventListener('submit', async function(e){
        if (submitting) { e.preventDefault(); return; }
        if (!window.crypto || !window.crypto.subtle) return; // Fall back to normal submit
        var pws = form.querySelectorAll('input[type="password"]');
        var unhashed = [];
        pws.forEach(function(pw){ if (!pw.dataset.hashed) unhashed.push(pw); });
        if (unhashed.length > 0) {
            e.preventDefault();
            submitting = true;
            try {
                for (var i = 0; i < unhashed.length; i++) {
                    var pw = unhashed[i];
                    var hashVal = await sha512('fly.board' + pw.value);
                    if (!hashVal) {
                        submitting = false;
                        form.submit();
                        return;
                    }
                    var hidden = document.createElement('input');
                    hidden.type = 'hidden';
                    hidden.name = pw.name;
                    hidden.value = hashVal;
                    form.appendChild(hidden);
                    pw.removeAttribute('name');
                    pw.dataset.hashed = '1';
                }
                form.submit();
            } catch (err) {
                submitting = false;
                form.submit();
            }
        }
    });
});
})();

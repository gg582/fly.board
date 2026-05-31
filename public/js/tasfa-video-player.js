var activeModal = null;

function closeModal() {
    if (!activeModal) return;
    var overlay = activeModal;
    activeModal = null;
    var media = overlay.querySelector('video, audio');
    if (media) {
        try { media.pause(); media.src = ''; media.load(); } catch(e){}
    }
    overlay.style.animation = 'tasfa-modal-in 0.15s ease reverse forwards';
    setTimeout(function() { if (overlay.parentElement) overlay.remove(); }, 150);
}

function injectModalStyles() {
    if (document.getElementById('tasfa-video-modal-style')) return;
    var style = document.createElement('style');
    style.id = 'tasfa-video-modal-style';
    style.textContent = [
        '.tasfa-video-modal-overlay{',
        '  position:fixed;inset:0;z-index:9999;',
        '  display:flex;align-items:center;justify-content:center;',
        '  background:rgba(0,0,0,0.82);',
        '  animation:tasfa-modal-in 0.18s ease;',
        '}',
        '@keyframes tasfa-modal-in{from{opacity:0}to{opacity:1}}',
        '.tasfa-video-modal-box{',
        '  position:relative;width:min(92vw,880px);',
        '  background:#111;border-radius:10px;overflow:hidden;',
        '  box-shadow:0 24px 64px rgba(0,0,0,0.7);',
        '}',
        '.tasfa-video-modal-close{',
        '  position:absolute;top:8px;right:8px;z-index:10;',
        '  width:32px;height:32px;border-radius:50%;border:none;cursor:pointer;',
        '  background:rgba(255,255,255,0.12);color:#fff;font-size:18px;line-height:1;',
        '  display:flex;align-items:center;justify-content:center;',
        '  transition:background 0.15s;',
        '}',
        '.tasfa-video-modal-close:hover{background:rgba(255,255,255,0.25)}',
        '.tasfa-video-modal-title{',
        '  padding:10px 48px 10px 14px;font-size:13px;color:rgba(255,255,255,0.55);',
        '  white-space:nowrap;overflow:hidden;text-overflow:ellipsis;',
        '  border-bottom:1px solid rgba(255,255,255,0.07);',
        '}',
        '.tasfa-video-modal-box video,.tasfa-video-modal-box audio{',
        '  width:100%;display:block;background:#000;',
        '}'
    ].join('\n');
    document.head.appendChild(style);
}

function _openModal(blobUrl, title, isAudio) {
    if (!blobUrl) return;
    closeModal();
    injectModalStyles();

    var overlay = document.createElement('div');
    overlay.className = 'tasfa-video-modal-overlay';
    overlay.setAttribute('role', 'dialog');
    overlay.setAttribute('aria-modal', 'true');

    var box = document.createElement('div');
    box.className = 'tasfa-video-modal-box';

    var closeBtn = document.createElement('button');
    closeBtn.className = 'tasfa-video-modal-close';
    closeBtn.setAttribute('aria-label', isAudio ? 'Close audio' : 'Close video');
    closeBtn.innerHTML = '&times;';
    closeBtn.addEventListener('click', closeModal);

    if (title) {
        var titleEl = document.createElement('div');
        titleEl.className = 'tasfa-video-modal-title';
        titleEl.textContent = title;
        box.appendChild(titleEl);
    }

    var mediaEl = document.createElement(isAudio ? 'audio' : 'video');
    mediaEl.setAttribute('playsinline', '');
    mediaEl.setAttribute('controls', '');
    mediaEl.setAttribute('preload', 'auto');
    mediaEl.src = blobUrl;

    // If the video fails to decode or the blob is invalid, close the modal
    // immediately so the user never sees a broken player or icon.
    mediaEl.addEventListener('error', function() {
        closeModal();
    });

    box.appendChild(closeBtn);
    box.appendChild(mediaEl);
    overlay.appendChild(box);
    document.body.appendChild(overlay);
    activeModal = overlay;

    overlay.addEventListener('click', function(e) {
        if (e.target === overlay) closeModal();
    });

    function onKey(e) {
        if (e.key === 'Escape') { closeModal(); document.removeEventListener('keydown', onKey); }
    }
    document.addEventListener('keydown', onKey);

    // Try to autoplay once enough data is buffered.
    mediaEl.addEventListener('loadeddata', function() {
        try { mediaEl.play(); } catch(e) {}
    });
}

export function openTasfaVideoModal(url, title, isAudio) {
    if (!url) return;

    // For TASFA-protected videos, use progressive chunk-by-chunk streaming.
    // The first few chunks are fetched sequentially; once enough data is
    // buffered the player opens immediately, and the rest follows in the
    // background through the Service Worker stream.
    if (window.fetchVideoProgressive && /\/file\/download\/\d+/.test(url)) {
        window.fetchVideoProgressive(url, {
            onReady: function(streamUrl) {
                _openModal(streamUrl, title, isAudio);
            }
        }).catch(function() {
            // Failed — do not open a broken player.
        });
        return;
    }

    // For non-TASFA URLs (e.g. direct blob: URLs), open immediately.
    _openModal(url, title, isAudio);
}

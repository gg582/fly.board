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
        '}',
        '.tasfa-video-modal-loading{',
        '  padding:48px 20px;text-align:center;color:rgba(255,255,255,0.65);font-size:14px;letter-spacing:0.02em;',
        '}',
        '.tasfa-video-modal-loading::after{',
        "  content:'';display:block;width:28px;height:28px;margin:12px auto 0;border:2px solid rgba(255,255,255,0.15);border-top-color:rgba(255,255,255,0.7);border-radius:50%;animation:tasfa-spin 1s linear infinite;",
        '}',
        '@keyframes tasfa-spin{to{transform:rotate(360deg)}}'
    ].join('\n');
    document.head.appendChild(style);
}

function _openModal(blobUrl, title, isAudio, isLoading) {
    if (!blobUrl && !isLoading) return;
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
    mediaEl.setAttribute('crossorigin', 'use-credentials');
    if (blobUrl) {
        mediaEl.src = blobUrl;
    } else {
        mediaEl.style.display = 'none';
    }

    var loadingEl = document.createElement('div');
    loadingEl.className = 'tasfa-video-modal-loading';
    loadingEl.textContent = isLoading ? 'Buffering…' : '';
    if (!isLoading) loadingEl.style.display = 'none';

    // If the video fails to decode or the blob is invalid, close the modal
    // immediately so the user never sees a broken player or icon.
    mediaEl.addEventListener('error', function() {
        if (!isLoading) closeModal();
    });

    box.appendChild(closeBtn);
    box.appendChild(loadingEl);
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

    // For TASFA-protected videos/audio, use TASFA only for session gating and
    // let the browser media engine drive playback with native Range requests.
    if (window.fetchDownloadSession && /\/file\/download\/\d+/.test(url)) {
        _openModal(null, title, isAudio, true);
        window.fetchDownloadSession(url).then(function(session) {
            var loading = activeModal && activeModal.querySelector('.tasfa-video-modal-loading');
            function attachSource(streamUrl) {
                if (!streamUrl) return;
                var media = activeModal && activeModal.querySelector('video, audio');
                var currentLoading = activeModal && activeModal.querySelector('.tasfa-video-modal-loading');
                if (media) {
                    media.style.display = 'block';
                    media.src = streamUrl;
                    try { media.load(); } catch(e) {}
                    try { media.play(); } catch(e) {}
                }
                if (currentLoading) currentLoading.style.display = 'none';
            }
            var streamUrl = window.tasfaDirectMediaUrl ?
                window.tasfaDirectMediaUrl(url, session) :
                (url + '?session_id=' + encodeURIComponent(session.sessionId) +
                 '&session_token=' + encodeURIComponent(session.sessionToken));
            attachSource(streamUrl);
        }).catch(function() {
            var loading = activeModal && activeModal.querySelector('.tasfa-video-modal-loading');
            if (loading) {
                loading.textContent = 'Failed to load media';
                loading.style.color = '#ff6b6b';
            }
            setTimeout(closeModal, 2000);
        });
        return;
    }

    // For non-TASFA URLs (e.g. direct blob: URLs), open immediately.
    _openModal(url, title, isAudio);
}

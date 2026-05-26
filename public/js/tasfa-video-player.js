var plyrLoaded = false;
var plyrLoading = null;
var activeModal = null;

function loadPlyr() {
    if (plyrLoaded) return Promise.resolve();
    if (plyrLoading) return plyrLoading;
    plyrLoading = new Promise(function(resolve, reject) {
        // Load Plyr CSS
        if (!document.getElementById('tasfa-plyr-css')) {
            var link = document.createElement('link');
            link.id = 'tasfa-plyr-css';
            link.rel = 'stylesheet';
            link.href = 'https://cdn.plyr.io/3.7.8/plyr.css';
            document.head.appendChild(link);
        }
        // Load Plyr JS
        var script = document.createElement('script');
        script.src = 'https://cdn.plyr.io/3.7.8/plyr.polyfilled.js';
        script.onload = function() { plyrLoaded = true; resolve(); };
        script.onerror = reject;
        document.head.appendChild(script);
    });
    return plyrLoading;
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
        '  backdrop-filter:blur(6px);-webkit-backdrop-filter:blur(6px);',
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
        /* Plyr theme overrides */
        '.tasfa-video-modal-box .plyr--video{border-radius:0}',
        '.tasfa-video-modal-box .plyr__control--overlaid{',
        '  background:rgba(99,102,241,0.85)',
        '}',
        '.tasfa-video-modal-box .plyr--full-ui input[type=range]{color:#6366f1}',
        '.tasfa-video-modal-box .plyr__progress input[type=range]::-webkit-slider-thumb{',
        '  background:#6366f1',
        '}'
    ].join('\n');
    document.head.appendChild(style);
}

function closeModal() {
    if (!activeModal) return;
    var overlay = activeModal;
    activeModal = null;
    // Stop the video/audio to release resources
    var media = overlay.querySelector('video, audio');
    if (media) { try { media.pause(); media.src = ''; } catch(e){} }
    overlay.style.animation = 'tasfa-modal-in 0.15s ease reverse forwards';
    setTimeout(function() { if (overlay.parentElement) overlay.remove(); }, 150);
}

function _openModal(url, title, isAudio) {
    if (!url) return;
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
    mediaEl.preload = 'none';
    mediaEl.src = url;
    mediaEl.style.width = '100%';
    mediaEl.style.display = 'block';

    box.appendChild(closeBtn);
    box.appendChild(mediaEl);
    overlay.appendChild(box);
    document.body.appendChild(overlay);
    activeModal = overlay;

    // Close on backdrop click
    overlay.addEventListener('click', function(e) {
        if (e.target === overlay) closeModal();
    });

    // Close on Escape
    function onKey(e) {
        if (e.key === 'Escape') { closeModal(); document.removeEventListener('keydown', onKey); }
    }
    document.addEventListener('keydown', onKey);

    // Init Plyr after loading
    loadPlyr().then(function() {
        if (!overlay.parentElement) return; // closed before load
        if (window.Plyr) {
            new window.Plyr(mediaEl, {
                controls: ['play-large','play','progress','current-time','mute','volume','fullscreen'],
                hideControls: false
            });
        }
        try { mediaEl.play(); } catch(e) {}
    }).catch(function() {
        // Plyr failed to load — native controls still work
        try { mediaEl.play(); } catch(e) {}
    });
}

export function openTasfaVideoModal(url, title, isAudio) {
    if (!url) return;
    // If the URL is a TASFA-protected download link, fetch the blob first
    // because direct GET to /file/download/... returns 403 when TASFA is active.
    if (window.fetchBlobViaTasfa && /^https?:\/\/[^/]+\/file\/download\/\d+/.test(url)) {
        window.fetchBlobViaTasfa(url, { silent: false }).then(function(result) {
            var blobUrl = URL.createObjectURL(result.blob);
            _openModal(blobUrl, title, isAudio);
        }).catch(function() {
            // Fallback: try the original URL (will likely fail for non-image files,
            // but gives a chance for images or if TASFA is disabled).
            _openModal(url, title, isAudio);
        });
        return;
    }
    _openModal(url, title, isAudio);
}


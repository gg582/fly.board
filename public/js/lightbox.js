(function () {
    'use strict';

    if (window.__flyboardLightboxBound) return;
    window.__flyboardLightboxBound = true;

    var overlay = null;
    var overlayImg = null;
    var overlayCaption = null;
    var prevBodyOverflow = '';

    function ensureOverlay() {
        if (overlay) return;

        overlay = document.createElement('div');
        overlay.className = 'lightbox-overlay';
        overlay.setAttribute('role', 'dialog');
        overlay.setAttribute('aria-modal', 'true');

        overlayImg = document.createElement('img');
        overlayImg.alt = '';
        overlay.appendChild(overlayImg);

        overlayCaption = document.createElement('div');
        overlayCaption.className = 'lightbox-caption';
        overlayCaption.style.display = 'none';
        overlay.appendChild(overlayCaption);

        var closeBtn = document.createElement('button');
        closeBtn.type = 'button';
        closeBtn.className = 'lightbox-close';
        closeBtn.setAttribute('aria-label', 'Close');
        closeBtn.innerHTML = '&times;';
        closeBtn.addEventListener('click', function (e) {
            e.stopPropagation();
            closeLightbox();
        });
        overlay.appendChild(closeBtn);

        overlay.addEventListener('click', function (e) {
            if (e.target === overlay || e.target === overlayImg) {
                closeLightbox();
            }
        });

        document.body.appendChild(overlay);
    }

    function openLightbox(src, alt) {
        ensureOverlay();
        overlayImg.src = src;
        overlayImg.alt = alt || '';
        if (alt) {
            overlayCaption.textContent = alt;
            overlayCaption.style.display = '';
        } else {
            overlayCaption.textContent = '';
            overlayCaption.style.display = 'none';
        }
        prevBodyOverflow = document.body.style.overflow;
        document.body.style.overflow = 'hidden';
        overlay.style.display = 'flex';
    }

    function closeLightbox() {
        if (!overlay || overlay.style.display === 'none') return;
        overlay.style.display = 'none';
        overlayImg.src = '';
        document.body.style.overflow = prevBodyOverflow;
    }

    document.addEventListener('click', function (e) {
        var t = e.target;
        if (!t || t.tagName !== 'IMG') return;
        if (overlay && overlay.contains(t)) return;
        if (!t.closest('.markdown-body')) return;
        if (t.closest('a')) return;
        if (t.closest('.gif-video-container')) return;
        if (t.naturalWidth > 0 && t.naturalWidth < 64 && t.naturalHeight < 64) return;

        var src = t.getAttribute('data-tasfa-original') || t.currentSrc || t.src;
        if (!src) return;
        openLightbox(src, t.getAttribute('alt') || '');
    });

    document.addEventListener('keydown', function (e) {
        if (e.key === 'Escape' || e.keyCode === 27) {
            closeLightbox();
        }
    });
})();

(function() {
    function init() {
    var ta = document.getElementById('md-editor');
    var preview = document.getElementById('md-preview');
    var fileInput = document.getElementById('file-input');
    var uploadPreview = document.getElementById('upload-preview');
    var dropzone = document.getElementById('upload-dropzone');
    var fileRepoUploadButton = document.getElementById('file-repo-upload-btn');
    var form = ta ? ta.closest('form') : null;
    var isEditorMode = !!ta;
    var isFileRepoMode = !!document.getElementById('file-repo-upload-root');
    var titleInput = document.getElementById('post-title-input');
    var wordCount = document.getElementById('editor-word-count');
    var readingTime = document.getElementById('editor-reading-time');
    var syncStatus = document.getElementById('editor-sync-status');
    var summaryInput = document.getElementById('summary-input');
    var tabButtons = document.querySelectorAll('[data-editor-tab]');
    var panes = document.querySelectorAll('[data-editor-pane]');
    var toolbarButtons = document.querySelectorAll('.editor-tool');
    var submitButtons = form ? form.querySelectorAll("button[type='submit']") : [];
    var timer = null;
    var isSubmitting = false;
    var AssetRegistry = [];
    var UPLOAD_INIT_ENDPOINT = '/file/upload/init';
    var UPLOAD_STATUS_ENDPOINT = '/file/upload/status';
    var UPLOAD_RENEGOTIATE_ENDPOINT = '/file/upload/renegotiate';
    var UPLOAD_ENDPOINT = '/file/upload';
    var UPLOAD_COMPLETE_ENDPOINT = '/file/upload/complete';
    var UPLOAD_CANCEL_ENDPOINT = '/file/upload/cancel';
    var UPLOAD_WORKER_URL = '/assets/js/tasfa-upload-worker.js';
    var UPLOAD_CHUNK_SIZE = 16 * 1024 * 1024;
    var UPLOAD_DEFAULT_PARALLEL = 16;
    var UPLOAD_MAX_PARALLEL = 64;
    var UPLOAD_RECOVERY_BASE_DELAY = 400;
    var UPLOAD_RECOVERY_MAX_DELAY = 8000;
    var UPLOAD_SCHEDULER_TICK_MS = 20;
    var UPLOAD_STALL_TIMEOUT_MS = 5000;
    var UPLOAD_PROGRESS_STALL_TIMEOUT_MS = 8000;
    var UPLOAD_STARTUP_STALL_TIMEOUT_MS = 10000;
    var UPLOAD_SESSION_FETCH_TIMEOUT_MS = 8000;
    var UPLOAD_CHUNK_RETRY_LIMIT = 7;
    var UPLOAD_WORKER_POOL_LIMIT = 12;
    var TASFA_CLIENT_STRIPE_COUNT = 32;
    var uploadWorkerBridge = null;
    var FAST_RENEGOTIATE_DELAY_MS = 4000;
    var MIN_RENEGOTIATE_INTERVAL_MS = 50;
    var UPLOAD_ROLLOVER_RETRY_LIMIT = 24;
    var UPLOAD_STARTUP_ROLLOVER_RETRY_LIMIT = 96;
    var LINK_DEGRADE_RETRY_THRESHOLD = 18;
    var LINK_DEGRADE_TIMEOUT_THRESHOLD = 6;
    var LINK_RECENT_DECAY_SUCCESS_STEP = 6;
    var PREPARE_AHEAD_MULTIPLIER = 5;
    var PREPARE_BATCH_SIZE = 4;
    var UPLOAD_MEMORY_BUDGET_BYTES = 512 * 1024 * 1024;
    var UPLOAD_PREPARED_BUDGET_BYTES = 96 * 1024 * 1024;
    var UPLOAD_ACTIVE_BUDGET_BYTES = 128 * 1024 * 1024;
    function escapeHtml(value) {
        return String(value || '')
            .replace(/&/g, '&amp;')
            .replace(/</g, '&lt;')
            .replace(/>/g, '&gt;')
            .replace(/"/g, '&quot;')
            .replace(/'/g, '&#39;');
    }

    function escapeRegExp(value) {
        return value.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
    }

    function generateUUID() {
        return 'asset-' + Math.random().toString(36).slice(2) + Date.now().toString(36);
    }

    function lineIsTableSeparator(line) {
        return /^\|?[\s:-]+\|[\s|:-]*$/.test(line.trim());
    }

    function lineStartsBlock(line) {
        var trimmed = line.trim();
        return !trimmed ||
            /^#{1,6}\s+/.test(trimmed) ||
            /^(```|~~~)/.test(trimmed) ||
            /^>\s?/.test(trimmed) ||
            /^(-|\*|\+)\s+/.test(trimmed) ||
            /^\d+\.\s+/.test(trimmed) ||
            /^(-{3,}|\*{3,}|_{3,})$/.test(trimmed);
    }

    function splitTableRow(line) {
        var row = line.trim();
        if (row.charAt(0) === '|') row = row.slice(1);
        if (row.charAt(row.length - 1) === '|') row = row.slice(0, -1);
        return row.split('|').map(function(cell) {
            return applyInline(cell.trim());
        });
    }

    function applyInline(text) {
        var html = escapeHtml(text);
        html = html.replace(/!\[([^\]]*)\]\(([^)\s]+)(?:\s+"([^"]+)")?\)/g, function(_, alt, src, title) {
            var attrs = "src='" + escapeHtml(src) + "' alt='" + escapeHtml(alt) + "' loading='lazy'";
            if (title) attrs += " title='" + escapeHtml(title) + "'";
            return "<img " + attrs + ">";
        });
        html = html.replace(/\[([^\]]+)\]\(([^)\s]+)(?:\s+"([^"]+)")?\)/g, function(_, label, href, title) {
            var attrs = "href='" + escapeHtml(href) + "'";
            if (/^https?:\/\//.test(href)) attrs += " target='_blank' rel='noopener noreferrer'";
            if (title) attrs += " title='" + escapeHtml(title) + "'";
            return "<a " + attrs + ">" + label + "</a>";
        });
        html = html.replace(/`([^`]+)`/g, '<code>$1</code>');
        html = html.replace(/\*\*([^*]+)\*\*/g, '<strong>$1</strong>');
        html = html.replace(/\*([^*\n]+)\*/g, '<em>$1</em>');
        html = html.replace(/~~([^~]+)~~/g, '<del>$1</del>');
        return html;
    }

    function renderMarkdown(md) {
        if (!md || !md.trim()) {
            return "<p style='color:var(--muted)'>Nothing to preview yet.</p>";
        }

        var codeBlocks = [];
        var normalized = md.replace(/\r\n/g, '\n');
        normalized = normalized.replace(/```([\w-]*)\n([\s\S]*?)```/g, function(_, lang, code) {
            var token = '@@CODEBLOCK' + codeBlocks.length + '@@';
            var cls = lang ? " class='language-" + escapeHtml(lang) + "'" : '';
            codeBlocks.push("<pre><code" + cls + ">" + escapeHtml(code.replace(/\n$/, '')) + "</code></pre>");
            return token;
        });

        var lines = normalized.split('\n');
        var html = [];
        var i = 0;

        while (i < lines.length) {
            var line = lines[i];
            var trimmed = line.trim();

            if (!trimmed) {
                i += 1;
                continue;
            }

            if (/^@@CODEBLOCK\d+@@$/.test(trimmed)) {
                html.push(trimmed);
                i += 1;
                continue;
            }

            if (/^#{1,6}\s+/.test(trimmed)) {
                var level = trimmed.match(/^#+/)[0].length;
                html.push('<h' + level + '>' + applyInline(trimmed.slice(level).trim()) + '</h' + level + '>');
                i += 1;
                continue;
            }

            if (/^(-{3,}|\*{3,}|_{3,})$/.test(trimmed)) {
                html.push('<hr>');
                i += 1;
                continue;
            }

            if (trimmed.indexOf('|') !== -1 && i + 1 < lines.length && lineIsTableSeparator(lines[i + 1])) {
                var headers = splitTableRow(lines[i]);
                var table = ['<table><thead><tr>'];
                headers.forEach(function(cell) {
                    table.push('<th>' + cell + '</th>');
                });
                table.push('</tr></thead><tbody>');
                i += 2;
                while (i < lines.length && lines[i].trim().indexOf('|') !== -1) {
                    var cells = splitTableRow(lines[i]);
                    table.push('<tr>');
                    cells.forEach(function(cell) {
                        table.push('<td>' + cell + '</td>');
                    });
                    table.push('</tr>');
                    i += 1;
                }
                table.push('</tbody></table>');
                html.push(table.join(''));
                continue;
            }

            if (/^>\s?/.test(trimmed)) {
                var quoteLines = [];
                while (i < lines.length && /^>\s?/.test(lines[i].trim())) {
                    quoteLines.push(applyInline(lines[i].trim().replace(/^>\s?/, '')));
                    i += 1;
                }
                html.push('<blockquote><p>' + quoteLines.join('<br>') + '</p></blockquote>');
                continue;
            }

            if (/^(-|\*|\+)\s+/.test(trimmed) || /^\d+\.\s+/.test(trimmed)) {
                var ordered = /^\d+\.\s+/.test(trimmed);
                var tag = ordered ? 'ol' : 'ul';
                var list = ['<' + tag + '>'];
                while (i < lines.length) {
                    var candidate = lines[i].trim();
                    if (ordered && /^\d+\.\s+/.test(candidate)) {
                        list.push('<li>' + applyInline(candidate.replace(/^\d+\.\s+/, '')) + '</li>');
                        i += 1;
                        continue;
                    }
                    if (!ordered && /^(-|\*|\+)\s+/.test(candidate)) {
                        list.push('<li>' + applyInline(candidate.replace(/^(-|\*|\+)\s+/, '')) + '</li>');
                        i += 1;
                        continue;
                    }
                    break;
                }
                list.push('</' + tag + '>');
                html.push(list.join(''));
                continue;
            }

            var paragraph = [applyInline(trimmed)];
            i += 1;
            while (i < lines.length && !lineStartsBlock(lines[i]) && !/^@@CODEBLOCK\d+@@$/.test(lines[i].trim())) {
                paragraph.push(applyInline(lines[i].trim()));
                i += 1;
            }
            html.push('<p>' + paragraph.join('<br>') + '</p>');
        }

        var rendered = html.join('');
        return rendered.replace(/@@CODEBLOCK(\d+)@@/g, function(_, idx) {
            return codeBlocks[Number(idx)] || '';
        });
    }

    function insertAtCursor(text, selectOffset) {
        if (!ta) return;
        var start = ta.selectionStart;
        var end = ta.selectionEnd;
        ta.value = ta.value.slice(0, start) + text + ta.value.slice(end);
        var caret = start + (typeof selectOffset === 'number' ? selectOffset : text.length);
        ta.selectionStart = ta.selectionEnd = caret;
        ta.focus();
        schedulePreview();
    }

    function toggleWrap(wrap, placeholder) {
        if (!ta) return;
        var start = ta.selectionStart;
        var end = ta.selectionEnd;
        var selected = ta.value.slice(start, end) || placeholder || '';
        var wrapped = wrap + selected + wrap;
        ta.value = ta.value.slice(0, start) + wrapped + ta.value.slice(end);
        ta.selectionStart = start + wrap.length;
        ta.selectionEnd = ta.selectionStart + selected.length;
        ta.focus();
        schedulePreview();
    }

    function prependToSelection(prefix, placeholder) {
        if (!ta) return;
        var start = ta.selectionStart;
        var end = ta.selectionEnd;
        var selected = ta.value.slice(start, end) || placeholder || '';
        var lines = selected.split('\n').map(function(line) {
            return prefix + line;
        }).join('\n');
        ta.value = ta.value.slice(0, start) + lines + ta.value.slice(end);
        ta.selectionStart = start;
        ta.selectionEnd = start + lines.length;
        ta.focus();
        schedulePreview();
    }

    function removeMarkdownByUrl(url) {
        if (!ta || !url) return;
        var pattern = new RegExp('!?\\[[^\\]]*\\]\\(' + escapeRegExp(url) + '\\)\\n?', 'g');
        ta.value = ta.value.replace(pattern, '');
    }

    function replaceAllInEditor(fromValue, toValue) {
        if (!ta || !fromValue) return;
        ta.value = ta.value.split(fromValue).join(toValue);
    }

    function editorHasUrl(url) {
        return !!(ta && url && ta.value.indexOf(url) !== -1);
    }

    function insertAssetMarkdown(asset) {
        if (!asset.url) return;
        insertAtCursor('![' + asset.filename + '](' + asset.url + ')\n');
    }

    function switchTab(nextTab) {
        Array.prototype.forEach.call(tabButtons, function(button) {
            button.classList.toggle('is-active', button.getAttribute('data-editor-tab') === nextTab);
            button.className = button.getAttribute('data-editor-tab') === nextTab ? 'btn' : 'btn btn-outline';
        });
        Array.prototype.forEach.call(panes, function(pane) {
            var active = pane.getAttribute('data-editor-pane') === nextTab;
            pane.classList.toggle('is-active', active);
            pane.style.display = active ? 'block' : 'none';
        });
        if (nextTab === 'preview') updatePreview();
    }

    function updateMetrics() {
        if (!ta) return;
        var words = ta.value.trim() ? ta.value.trim().split(/\s+/).length : 0;
        var minutes = words ? Math.max(1, Math.ceil(words / 220)) : 0;
        if (wordCount) wordCount.textContent = words + ' words';
        if (readingTime) readingTime.textContent = minutes + ' min read';
    }

    function updatePreview() {
        if (!preview || !ta) return;
        preview.innerHTML = renderMarkdown(ta.value);
        if (typeof window.initMarkdownAffordances === 'function') {
            window.initMarkdownAffordances(preview);
        }
        updateMetrics();
        if (syncStatus) {
            syncStatus.textContent = 'Local preview updated';
        }
    }

    function schedulePreview() {
        if (syncStatus) syncStatus.textContent = 'Editing locally';
        clearTimeout(timer);
        timer = setTimeout(updatePreview, 120);
    }

    function syncMediaMeta() {
        var metaInput = document.getElementById('media-meta');
        if (!metaInput) return;
        metaInput.value = JSON.stringify(
            AssetRegistry
                .filter(function(asset) { return asset.fid !== null; })
                .map(function(asset) {
                    return { fid: asset.fid, mode: asset.mode, delete_pin: asset.deletePin || '' };
                })
        );
    }

    function removeAssetRecord(asset) {
        AssetRegistry = AssetRegistry.filter(function(item) {
            return item.client_uuid !== asset.client_uuid;
        });
        syncMediaMeta();
    }

    function setModeButtons(ui, mode) {
        ui.btnInline.className = mode === 'inline' ? 'btn media-inline-btn' : 'btn btn-outline media-inline-btn';
        ui.btnAttachment.className = mode === 'attachment' ? 'btn media-attachment-btn' : 'btn btn-outline media-attachment-btn';
    }

    function prependFileRepoCard(asset, response) {
        if (!isFileRepoMode) return;
        var list = document.getElementById('file-repo-list');
        var empty = document.getElementById('file-repo-empty');
        if (empty) empty.remove();
        if (!list) {
            list = document.createElement('div');
            list.className = 'post-grid';
            list.id = 'file-repo-list';
            var anchor = document.getElementById('file-repo-list-anchor');
            if (!anchor || !anchor.parentNode) return;
            anchor.parentNode.insertBefore(list, anchor.nextSibling);
        }

        var deletePinHtml = response.delete_pin
            ? "<div style='margin-top:10px;font-size:12px;color:var(--muted)'>Delete PIN: <code>" + escapeHtml(response.delete_pin) + "</code></div>"
            : "";
        var article = document.createElement('article');
        article.className = 'card';
        article.innerHTML =
            "<h4 style='margin-top:0'>" + escapeHtml(asset.filename) + "</h4>" +
            "<p style='color:var(--muted);font-size:13px'>" +
            escapeHtml(response.mime_type || 'unknown/mime') + " &middot; " +
            String(response.size || asset.fileSize || 0) + " bytes</p>" +
            "<div class='file-card-actions' style='display:flex;gap:10px;flex-wrap:wrap;margin-top:12px'>" +
            "<a href='/file/" + String(asset.fid) + "' class='btn btn-outline' style='font-size:12px;padding:4px 10px'>Open</a>" +
            "<a href='#' data-tasfa-download-link='" + escapeHtml(asset.url || ('/file/download/' + String(asset.fid))) + "' class='btn' style='font-size:12px;padding:4px 10px'>Download</a>" +
            "</div>" +
            deletePinHtml;
        list.insertBefore(article, list.firstChild);
    }

    function createMediaCard(filename, previewUrl, isImage) {
        var card = document.createElement('div');
        card.className = 'media-card';

        var thumb = document.createElement('div');
        thumb.className = 'media-thumb';
        if (isImage) {
            var img = document.createElement('img');
            img.src = previewUrl;
            img.style.display = 'block';
            img.style.width = '100%';
            img.style.height = '100%';
            img.style.maxWidth = 'none';
            img.style.objectFit = 'cover';
            img.style.filter = 'grayscale(1) contrast(1.08)';
            thumb.appendChild(img);
        } else {
            var icon = document.createElement('span');
            icon.style.fontSize = '22px';
            icon.textContent = 'FILE';
            thumb.appendChild(icon);
        }

        var info = document.createElement('div');
        info.className = 'media-info';

        var name = document.createElement('div');
        name.className = 'media-name';
        name.textContent = filename;

        var status = document.createElement('div');
        status.className = 'media-status';
        status.textContent = isEditorMode ? 'Queued [0%]' : 'Queued';

        var progress = document.createElement('div');
        progress.className = 'media-progress-bar';

        var progressInner = document.createElement('div');
        progressInner.className = 'media-progress-inner';
        progress.appendChild(progressInner);

        info.appendChild(name);
        info.appendChild(status);
        info.appendChild(progress);

        var actions = document.createElement('div');
        actions.className = 'media-actions';

        var btnInsert = document.createElement('button');
        btnInsert.type = 'button';
        btnInsert.className = 'btn btn-outline media-insert-btn';
        btnInsert.textContent = 'Insert';
        btnInsert.disabled = true;

        var btnInline = document.createElement('button');
        btnInline.type = 'button';
        btnInline.className = 'btn media-inline-btn';
        btnInline.textContent = 'Inline';
        btnInline.disabled = true;

        var btnAttachment = document.createElement('button');
        btnAttachment.type = 'button';
        btnAttachment.className = 'btn btn-outline media-attachment-btn';
        btnAttachment.textContent = 'Attachment';
        btnAttachment.disabled = true;

        var btnDelete = document.createElement('button');
        btnDelete.type = 'button';
        btnDelete.className = 'btn btn-outline media-delete-btn';
        btnDelete.textContent = 'Delete';

        actions.appendChild(btnInsert);
        actions.appendChild(btnInline);
        actions.appendChild(btnAttachment);
        actions.appendChild(btnDelete);

        card.appendChild(thumb);
        card.appendChild(info);
        card.appendChild(actions);

        return {
            el: card,
            thumbImg: thumb.querySelector('img'),
            status: status,
            progress: progress,
            progressInner: progressInner,
            btnInsert: btnInsert,
            btnInline: btnInline,
            btnAttachment: btnAttachment,
            btnDelete: btnDelete
        };
    }

    function bindAssetControls(asset) {
        var ui = asset.ui;

        ui.btnDelete.onclick = function() {
            clearRecoveryTimer(asset);
            if (asset.fid === null && asset.xhrs && asset.xhrs.length) {
                asset.isCancelling = true;
                asset.xhrs.slice().forEach(function(xhr) {
                    try { xhr.abort(); } catch (err) {}
                });
                if (asset.uploadId) {
                    fetch(UPLOAD_CANCEL_ENDPOINT, {
                        method: 'POST',
                        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
                        body: 'upload_ids=' + encodeURIComponent(JSON.stringify([asset.uploadId]))
                    });
                }
            } else if (asset.fid !== null) {
                fetch('/file/delete', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
                    body: 'id=' + encodeURIComponent(String(asset.fid)) +
                        '&delete_pin=' + encodeURIComponent(asset.deletePin || '')
                });
            }

            removeMarkdownByUrl(asset.url || asset.placeholderUrl);
            if (asset.blobUrl) {
                URL.revokeObjectURL(asset.blobUrl);
            }
            if (asset.ui && asset.ui.el) {
                asset.ui.el.remove();
            }
            removeAssetRecord(asset);
            updateFileRepoUploadButton();
            schedulePreview();
        };

        if (!isEditorMode) {
            ui.btnInsert.textContent = 'Open';
            ui.btnInsert.onclick = function() {
                if (asset.fid !== null) window.location.href = '/file/' + asset.fid;
            };
            ui.btnInline.textContent = 'Download';
            ui.btnInline.onclick = function() {
                if (asset.url && typeof window.openTasfaDownload === 'function') {
                    window.openTasfaDownload(asset.url);
                }
            };
            ui.btnAttachment.style.display = 'none';
            ui.btnInsert.disabled = asset.fid === null;
            ui.btnInline.disabled = asset.fid === null;
            return;
        }

        ui.btnInsert.onclick = function() {
            insertAssetMarkdown(asset);
        };

        ui.btnInline.onclick = function() {
            if (!asset.url) return;
            asset.mode = 'inline';
            if (!editorHasUrl(asset.url)) {
                insertAssetMarkdown(asset);
            } else {
                schedulePreview();
            }
            setModeButtons(ui, asset.mode);
            syncMediaMeta();
        };

        ui.btnAttachment.onclick = function() {
            if (!asset.url) return;
            asset.mode = 'attachment';
            removeMarkdownByUrl(asset.url);
            setModeButtons(ui, asset.mode);
            syncMediaMeta();
            schedulePreview();
        };

        setModeButtons(ui, asset.mode);
    }

    function finalizeUploadSuccess(asset, response) {
        asset.fid = response.fid || response.id;
        asset.url = response.url;
        asset.filename = response.filename || asset.filename;
        asset.deletePin = response.delete_pin || '';
        asset.isUploading = false;
        asset.failed = false;
        asset.isPaused = false;
        asset.recovering = false;
        asset.recoveryAttempts = 0;
        asset.activeRequests = 0;
        asset.lastVisualBytes = asset.fileSize || 0;
        clearRecoveryTimer(asset);
        clearSchedulerTimer(asset);
        resetAllInflightChunks(asset, true);
        replaceAllInEditor(asset.placeholderUrl, asset.url);
        asset.mode = isEditorMode ? 'inline' : 'attachment';
        asset.xhrs = [];
        asset.uploadToken = null;
        asset.uploadSecret = null;
        asset.ui.status.textContent = response.delete_pin
            ? ((isEditorMode ? 'Uploaded. Save delete PIN: ' : 'Uploaded. Save delete PIN: ') + response.delete_pin)
            : (isEditorMode ? 'Uploaded and available for inline placement' : 'Uploaded to file repository');
        if (asset.ui.progressInner) {
            if (asset.ui.progress) {
                asset.ui.progress.classList.remove('is-completing');
                asset.ui.progress.style.display = '';
            }
            asset.ui.progressInner.style.width = '100%';
            if (asset.ui.progress) {
                window.setTimeout(function() {
                    if (!asset.ui || !asset.ui.progress) return;
                    asset.ui.progress.classList.add('is-completing');
                    window.setTimeout(function() {
                        if (!asset.ui || !asset.ui.progress) return;
                        asset.ui.progress.style.display = 'none';
                        asset.ui.progress.classList.remove('is-completing');
                    }, 380);
                }, 120);
            }
        }
        asset.ui.btnInsert.disabled = false;
        asset.ui.btnInline.disabled = false;
        asset.ui.btnAttachment.disabled = !isEditorMode;
        if (asset.ui.thumbImg) {
            asset.ui.thumbImg.style.filter = 'none';
        }
        if (isEditorMode) setModeButtons(asset.ui, asset.mode);
        bindAssetControls(asset);
        syncMediaMeta();
        updateFileRepoUploadButton();
        updateSubmitButtons();
        schedulePreview();
        prependFileRepoCard(asset, response);
        if (asset.blobUrl) {
            URL.revokeObjectURL(asset.blobUrl);
            asset.blobUrl = null;
        }
    }

    function markUploadFailure(asset, message) {
        asset.isUploading = false;
        asset.failed = true;
        asset.recovering = false;
        asset.isPaused = false;
        clearRecoveryTimer(asset);
        clearSchedulerTimer(asset);
        resetAllInflightChunks(asset, false);
        asset.ui.status.textContent = message;
        asset.ui.btnDelete.disabled = false;
        asset.xhrs = [];
        updateFileRepoUploadButton();
        updateSubmitButtons();
    }

    function fileRepoPendingAssets() {
        return AssetRegistry.filter(function(asset) {
            return !asset.isExisting && asset.fid === null && !asset.isUploading && !asset.uploadId && !asset.failed;
        });
    }

    function fileRepoActiveUploads() {
        return AssetRegistry.filter(function(asset) {
            return !asset.isExisting && asset.fid === null && asset.isUploading;
        });
    }

    function hasActiveUploads() {
        return fileRepoActiveUploads().length > 0;
    }

    function updateSubmitButtons() {
        if (!submitButtons || !submitButtons.length) return;
        var locked = hasActiveUploads();
        Array.prototype.forEach.call(submitButtons, function(button) {
            button.disabled = locked;
            if (locked) {
                button.setAttribute('aria-disabled', 'true');
                button.title = 'Wait for uploads to finish';
            } else {
                button.removeAttribute('aria-disabled');
                button.removeAttribute('title');
            }
        });
        if (syncStatus && locked) syncStatus.textContent = 'Uploading files, save is locked';
    }

    function updateFileRepoUploadButton() {
        if (!isFileRepoMode || !fileRepoUploadButton) return;
        var pending = fileRepoPendingAssets().length;
        var active = fileRepoActiveUploads().length;
        fileRepoUploadButton.disabled = pending === 0 || active > 0;
        if (active > 0) {
            fileRepoUploadButton.textContent = 'Uploading...';
        } else if (pending > 0) {
            fileRepoUploadButton.textContent = 'Upload queued files (' + pending + ')';
        } else {
            fileRepoUploadButton.textContent = 'Upload queued files';
        }
        updateSubmitButtons();
    }

    function chunkByteRange(asset, chunkIndex) {
        var start = chunkIndex * asset.chunkSize;
        var end = Math.min(start + asset.chunkSize, asset.fileSize);
        return { start: start, end: end, size: Math.max(0, end - start) };
    }

    function countBitmapBits(bitmap) {
        if (!bitmap) return 0;
        var count = 0;
        for (var i = 0; i < bitmap.length; i += 1) {
            if (bitmap.charAt(i) === '1') count += 1;
        }
        return count;
    }

    function setBitmapBit(bitmap, index) {
        if (!bitmap || index < 0 || index >= bitmap.length) return bitmap || '';
        if (bitmap.charAt(index) === '1') return bitmap;
        return bitmap.substring(0, index) + '1' + bitmap.substring(index + 1);
    }

    function confirmedUploadBytes(asset) {
        if (!asset || !asset.receivedBitmap) return 0;
        var confirmed = 0;
        for (var i = 0; i < asset.totalChunks; i += 1) {
            if (asset.receivedBitmap.charAt(i) === '1') {
                confirmed += chunkByteRange(asset, i).size;
            }
        }
        return confirmed;
    }

    function contiguousConfirmedUploadBytes(asset) {
        if (!asset || !asset.receivedBitmap) return 0;
        var confirmed = 0;
        for (var i = 0; i < asset.totalChunks; i += 1) {
            if (asset.receivedBitmap.charAt(i) !== '1') break;
            confirmed += chunkByteRange(asset, i).size;
        }
        return confirmed;
    }

    function syncConfirmedProgress(asset) {
        if (!asset) return;
        asset.confirmedBytes = confirmedUploadBytes(asset);
        if (!asset.transferProgress || asset.transferProgress.length !== asset.totalChunks) {
            asset.transferProgress = new Array(asset.totalChunks).fill(0);
        }
    }

    function activeUploadBytes(asset) {
        if (!asset) return 0;
        var active = asset.confirmedBytes || 0;
        if (asset.transferProgress) {
            for (var i = 0; i < asset.transferProgress.length; i += 1) {
                active += Number(asset.transferProgress[i] || 0);
            }
        }
        return active;
    }

    function encodeFormBody(values) {
        return Object.keys(values).map(function(k) {
            return encodeURIComponent(k) + '=' + encodeURIComponent(values[k]);
        }).join('&');
    }

    function fetchWithTimeout(url, options, timeoutMs) {
        return new Promise(function(resolve, reject) {
            var controller = new AbortController();
            var tid = setTimeout(function() { controller.abort(); reject(new Error('timeout:fetch')); }, timeoutMs || 8000);
            fetch(url, Object.assign({}, options, { signal: controller.signal })).then(function(r) {
                clearTimeout(tid);
                resolve(r);
            }).catch(function(e) {
                clearTimeout(tid);
                reject(e);
            });
        });
    }

    function getConnectionInfo() {
        var conn = navigator.connection || navigator.mozConnection || navigator.webkitConnection;
        return {
            effectiveType: conn && conn.effectiveType ? conn.effectiveType : '',
            downlinkMbps: conn && typeof conn.downlink === 'number' ? conn.downlink : 0,
            rttMs: conn && typeof conn.rtt === 'number' ? conn.rtt : 0,
            saveData: !!(conn && conn.saveData)
        };
    }

    function ensureLinkState(asset) {
        if (!asset) return {};
        if (!asset.linkState) asset.linkState = { ewmaRttMs: 0, retries: 0, timeouts: 0, successes: 0, recentRetries: 0, recentTimeouts: 0 };
        return asset.linkState;
    }

    function recordLinkSuccess(asset, elapsedMs) {
        var state = ensureLinkState(asset);
        state.ewmaRttMs = state.ewmaRttMs > 0 ? (state.ewmaRttMs * 0.7 + elapsedMs * 0.3) : elapsedMs;
        state.successes = (state.successes || 0) + 1;
        state.recentRetries = Math.max(0, (state.recentRetries || 0) - 1);
        state.recentTimeouts = Math.max(0, (state.recentTimeouts || 0) - 1);
    }

    function recordLinkPenalty(asset, reason) {
        var state = ensureLinkState(asset);
        if (reason === 'retry') { state.retries = (state.retries || 0) + 1; state.recentRetries = (state.recentRetries || 0) + 1; }
        else if (reason === 'timeout') { state.timeouts = (state.timeouts || 0) + 1; state.recentTimeouts = (state.recentTimeouts || 0) + 1; }
    }

    function computeLinkStabilityScore(asset) {
        var conn = getConnectionInfo();
        var state = ensureLinkState(asset);
        var score = 55;
        if (conn.effectiveType === '4g') score += 24;
        else if (conn.effectiveType === '3g') score += 10;
        else if (conn.effectiveType === '2g' || conn.effectiveType === 'slow-2g') score -= 10;
        if (conn.downlinkMbps >= 30) score += 18;
        else if (conn.downlinkMbps >= 10) score += 12;
        else if (conn.downlinkMbps >= 3) score += 6;
        else if (conn.downlinkMbps > 0 && conn.downlinkMbps < 1.5) score -= 10;
        if (state.ewmaRttMs > 0) {
            if (state.ewmaRttMs <= 60) score += 16;
            else if (state.ewmaRttMs <= 120) score += 8;
            else if (state.ewmaRttMs <= 220) score += 2;
            else if (state.ewmaRttMs <= 450) score -= 10;
            else score -= 18;
        }
        score -= (state.recentRetries || 0) * 5;
        score -= (state.recentTimeouts || 0) * 12;
        if (conn.saveData) score -= 10;
        if (score < 10) score = 10;
        if (score > 100) score = 100;
        return score;
    }

    function buildLinkHintFields(asset) {
        var conn = getConnectionInfo();
        var state = ensureLinkState(asset);
        return {
            link_stability_score: computeLinkStabilityScore(asset),
            link_effective_type: conn.effectiveType || '',
            link_downlink_mbps: conn.downlinkMbps || '',
            link_rtt_ms: Math.round(state.ewmaRttMs || conn.rttMs || 0),
            link_retry_events: state.recentRetries || 0,
            link_timeout_events: state.recentTimeouts || 0,
            link_save_data: conn.saveData ? '1' : '0'
        };
    }

    function getUploadWorkerBridge() {
        if (uploadWorkerBridge) return uploadWorkerBridge;
        if (!window.Worker) return null;
        try {
            var workerCount = Math.max(2, Math.min(UPLOAD_WORKER_POOL_LIMIT, Math.max(2, Number(window.navigator && window.navigator.hardwareConcurrency) || 4) - 1));
            var seq = 0;
            var pending = {};
            var queue = [];
            var workers = [];
            function flushWorkerQueue() {
                for (var i = 0; i < workers.length && queue.length; i += 1) {
                    if (workers[i].busy) continue;
                    var next = queue.shift();
                    workers[i].busy = true;
                    workers[i].worker.postMessage(next.message, next.transferList || []);
                }
            }
            function attachWorker(workerState) {
                workerState.worker.onmessage = function(event) {
                    var data = event.data || {};
                    var job = pending[data.id];
                    workerState.busy = false;
                    if (!job) { flushWorkerQueue(); return; }
                    delete pending[data.id];
                    if (data.ok) job.resolve(data);
                    else job.reject(new Error(data.error || 'worker failed'));
                    flushWorkerQueue();
                };
                workerState.worker.onerror = function() { workerState.busy = false; flushWorkerQueue(); };
            }
            for (var i = 0; i < workerCount; i += 1) {
                var worker = new Worker(UPLOAD_WORKER_URL);
                var workerState = { worker: worker, busy: false };
                attachWorker(workerState);
                workers.push(workerState);
            }
            uploadWorkerBridge = {
                request: function(message, transferList) {
                    return new Promise(function(resolve, reject) {
                        seq += 1;
                        message.id = seq;
                        pending[seq] = { resolve: resolve, reject: reject };
                        queue.push({ message: message, transferList: transferList || [] });
                        flushWorkerQueue();
                    });
                }
            };
        } catch (err) { uploadWorkerBridge = null; }
        return uploadWorkerBridge;
    }

    function preprocessChunkPayload(asset, chunkIndex, blob) {
        return blob.arrayBuffer().then(function(buffer) {
            var bridge = getUploadWorkerBridge();
            if (bridge) {
                return bridge.request({
                    type: 'prepare-upload',
                    uploadId: asset.uploadId,
                    streamKeyHex: asset.streamKeyHex,
                    streamIvSeedHex: asset.streamIvSeedHex,
                    chunkIndex: chunkIndex,
                    chunkBuffer: buffer
                }, [buffer]);
            }
            return Promise.reject(new Error('worker unavailable'));
        });
    }

    function clearRecoveryTimer(asset) {
        if (asset && asset.recoveryTimer) { clearTimeout(asset.recoveryTimer); asset.recoveryTimer = null; }
    }

    function clearSchedulerTimer(asset) {
        if (asset && asset.schedulerTimer) { clearTimeout(asset.schedulerTimer); asset.schedulerTimer = null; }
    }

    function bumpUploadSessionGeneration(asset) {
        asset.sessionGeneration = (asset.sessionGeneration || 0) + 1;
        return asset.sessionGeneration;
    }

    function nextChunkRetryDelay(attempt) {
        return Math.min(UPLOAD_RECOVERY_MAX_DELAY, UPLOAD_RECOVERY_BASE_DELAY * Math.pow(1.6, Math.max(0, Number(attempt) || 0)));
    }

    function budgetParallelLimit(chunkSize) {
        return Math.max(2, Math.min(UPLOAD_MAX_PARALLEL, Math.floor(UPLOAD_MEMORY_BUDGET_BYTES / Math.max(1, chunkSize))));
    }

    function clampParallelToBudget(asset, value) {
        var limit = budgetParallelLimit(asset && asset.chunkSize ? asset.chunkSize : UPLOAD_CHUNK_SIZE);
        return Math.max(2, Math.min(limit, Math.max(2, Number(value) || 2)));
    }

    function markSchedulerActivity(asset) {
        if (asset) asset.lastSchedulerActivityAt = Date.now();
    }

    function finishNetworkAttempt(asset, chunkIndex) {
        if (!asset || !asset.inflightRequests) return;
        delete asset.inflightRequests[chunkIndex];
    }

    function resetInflightChunk(asset, chunkIndex, keepProgress) {
        if (!asset || !asset.inflightChunks) return;
        delete asset.inflightChunks[chunkIndex];
        asset.dispatchReservations = Math.max(0, Number(asset.dispatchReservations || 0) - 1);
        if (!keepProgress && asset.transferProgress) asset.transferProgress[chunkIndex] = 0;
    }

    function requeueChunk(asset, chunkIndex) {
        if (!asset || asset.receivedBitmap && asset.receivedBitmap.charAt(chunkIndex) === '1') return;
        if (!asset.pendingChunks) asset.pendingChunks = [];
        if (asset.pendingChunks.indexOf(chunkIndex) < 0) asset.pendingChunks.push(chunkIndex);
    }

    function recoverOrContinue(asset, file, chunkIndex, reason) {
        if (!asset || asset.failed || asset.isCancelling || asset.recovering) return;
        if (reason === 'timeout' || reason === 'slow') {
            recordLinkPenalty(asset, reason === 'slow' ? 'retry' : 'timeout');
        }
        resetInflightChunk(asset, chunkIndex, false);
        requeueChunk(asset, chunkIndex);
        updateAssetProgress(asset);
        fillChunkWindow(asset, file);
    }

    function resetAllInflightChunks(asset, keepProgress) {
        if (!asset) return;
        var inflight = asset.inflightChunks || {};
        Object.keys(inflight).forEach(function(key) { resetInflightChunk(asset, Number(key), keepProgress); });
        asset.inflightChunks = {};
        asset.inflightRequests = {};
        asset.chunkActivityAt = {};
        asset.dispatchReservations = 0;
        asset.activeRequests = 0;
    }

    function clearPreparedChunks(asset) {
        if (!asset) return;
        asset.preparedChunks = {};
        asset.prepareInflight = {};
        asset.prepareInflightCount = 0;
    }

    function abortInflightRequests(asset) {
        if (!asset || !asset.inflightRequests) return;
        asset.suppressAbortHandling = true;
        Object.keys(asset.inflightRequests).forEach(function(key) {
            var xhr = asset.inflightRequests[key];
            if (!xhr) return;
            try { xhr.abort(); } catch (err) {}
        });
        resetAllInflightChunks(asset, false);
        asset.xhrs = (asset.xhrs || []).filter(function(xhr) { return xhr && xhr._tasfaKind === 'finalize'; });
        asset.suppressAbortHandling = false;
        clearPreparedChunks(asset);
    }

    function startSchedulerLoop(asset) {
        if (!asset) return;
        clearSchedulerTimer(asset);
        function tick() {
            try {
                if (!asset || asset.failed || asset.isCancelling || asset.fid !== null || !asset.isUploading) {
                    clearSchedulerTimer(asset);
                    return;
                }
                if (!asset.recovering && !asset.isPaused && !asset.finalizing) {
                    var now = Date.now();
                    var chunkActivity = asset.chunkActivityAt || {};
                    var stalled = Object.keys(chunkActivity).some(function(key) {
                        var chunkIndex = Number(key);
                        var sentBytes = Number((asset.transferProgress && asset.transferProgress[chunkIndex]) || 0);
                        var hasConfirmedTraffic = Number(asset.lastChunkResponseAt || 0) > 0 || confirmedUploadBytes(asset) > 0 || Number(asset.completedChunks || 0) > 0;
                        var stallLimit = !hasConfirmedTraffic ? UPLOAD_STARTUP_STALL_TIMEOUT_MS : (sentBytes > 0 ? UPLOAD_PROGRESS_STALL_TIMEOUT_MS : UPLOAD_STALL_TIMEOUT_MS);
                        if (document.visibilityState === 'hidden') stallLimit = Math.max(stallLimit, 10000);
                        var linkState = ensureLinkState(asset);
                        var rttFloor = Math.max(stallLimit, Math.min(10000, Math.ceil(Number(linkState.ewmaRttMs || 0) * 3.5)));
                        return now - Number(chunkActivity[key] || 0) > rttFloor;
                    });
                    if (stalled) {
                        rolloverUploadSession(asset, 'timeout');
                        clearSchedulerTimer(asset);
                        return;
                    }
                    if (Number(asset.dispatchReservations || 0) === 0 && hasMissingChunks(asset) && asset.nextChunkCursor >= asset.pendingChunks.length) {
                        rebuildPendingChunks(asset);
                    }
                    if (Number(asset.dispatchReservations || 0) < asset.currentParallel || Number(asset.dispatchReservations || 0) === 0) {
                        fillChunkWindow(asset, asset.file);
                    }
                }
            } catch (err) { console.error('TASFA scheduler tick failed:', err); }
            asset.schedulerTimer = window.setTimeout(tick, UPLOAD_SCHEDULER_TICK_MS);
        }
        asset.schedulerTimer = window.setTimeout(tick, UPLOAD_SCHEDULER_TICK_MS);
        if (!asset._visibilityHandler) {
            asset._visibilityHandler = function() {
                if (document.visibilityState === 'visible' && asset.isUploading && !asset.schedulerTimer) startSchedulerLoop(asset);
            };
            document.addEventListener('visibilitychange', asset._visibilityHandler);
        }
    }

    function buildChunkMeta(asset, file, chunkIndex) {
        var range = chunkByteRange(asset, chunkIndex);
        var blob = file.slice(range.start, range.end);
        return { start: range.start, end: range.end, blob: blob, chunkIndex: chunkIndex };
    }

    function prepareAheadTarget(asset) {
        return Math.max(4, Math.min(32, asset.currentParallel || UPLOAD_DEFAULT_PARALLEL));
    }

    function schedulePrepareAhead(asset, file) {
        if (!asset || !file || asset.failed || asset.isCancelling || asset.recovering || asset.isPaused) return;
        if (!asset.preparedChunks) asset.preparedChunks = {};
        if (!asset.prepareInflight) asset.prepareInflight = {};
        if (!asset.prepareInflightCount) asset.prepareInflightCount = 0;
        var target = prepareAheadTarget(asset);
        var generation = Number(asset.sessionGeneration || 0);
        for (var i = asset.nextChunkCursor; i < asset.pendingChunks.length;) {
            if ((Object.keys(asset.preparedChunks).length + Number(asset.prepareInflightCount || 0)) >= target) break;
            var chunkIndex = asset.pendingChunks[i];
            i += 1;
            if (chunkIndex === null || chunkIndex === undefined) continue;
            if (asset.receivedBitmap && asset.receivedBitmap.charAt(chunkIndex) === '1') continue;
            if (asset.inflightChunks && asset.inflightChunks[chunkIndex]) continue;
            if (asset.preparedChunks[chunkIndex] || asset.prepareInflight[chunkIndex]) continue;
            asset.prepareInflight[chunkIndex] = true;
            asset.prepareInflightCount += 1;
            var meta = buildChunkMeta(asset, file, chunkIndex);
            (function(idx, b) {
                preprocessChunkPayload(asset, idx, b).then(function(prepared) {
                    if (generation !== Number(asset.sessionGeneration || 0)) return;
                    asset.preparedChunks[idx] = prepared;
                }).catch(function() {}).finally(function() {
                    if (asset.prepareInflight && asset.prepareInflight[idx]) {
                        delete asset.prepareInflight[idx];
                        asset.prepareInflightCount = Math.max(0, Number(asset.prepareInflightCount || 0) - 1);
                    }
                });
            })(chunkIndex, meta.blob);
        }
    }

    function rebuildPendingChunks(asset) {
        if (!asset || asset.rebuildingPending) return;
        asset.rebuildingPending = true;
        if (asset.pendingChunks && asset.pendingChunks.length > (asset.currentParallel * 2)) {
            asset.rebuildingPending = false;
            return;
        }
        setTimeout(function() {
            var pending = [];
            for (var i = 0; i < asset.totalChunks; i += 1) {
                if (!asset.receivedBitmap || asset.receivedBitmap.charAt(i) !== '1') pending.push(i);
            }
            if (Number(asset.resumeFromByte || 0) > 0) {
                var resumeChunk = Math.max(0, Math.floor(Number(asset.resumeFromByte || 0) / Math.max(1, Number(asset.chunkSize || UPLOAD_CHUNK_SIZE))));
                pending.sort(function(a, b) {
                    var aAfter = a >= resumeChunk ? 0 : 1;
                    var bAfter = b >= resumeChunk ? 0 : 1;
                    if (aAfter !== bAfter) return aAfter - bAfter;
                    return a - b;
                });
            }
            asset.pendingChunks = pending;
            asset.nextChunkCursor = 0;
            asset.rebuildingPending = false;
        }, 0);
    }

    function hasMissingChunks(asset) {
        if (!asset) return false;
        return confirmedUploadBytes(asset) < (asset.fileSize || 0);
    }

    function uploadRolloverDelay(asset) {
        var confirmed = confirmedUploadBytes(asset);
        if (confirmed > 0 || Number(asset && asset.completedChunks || 0) > 0) return 700;
        return 8000;
    }

    function uploadRolloverLimit(asset) {
        var confirmed = confirmedUploadBytes(asset);
        if (confirmed > 0 || Number(asset && asset.completedChunks || 0) > 0) return UPLOAD_ROLLOVER_RETRY_LIMIT;
        return UPLOAD_STARTUP_ROLLOVER_RETRY_LIMIT;
    }

    function applyServerSessionState(asset, payload) {
        if (!asset || !payload) return;
        asset.chunkSize = Number(payload.chunk_size || asset.chunkSize || UPLOAD_CHUNK_SIZE);
        asset.totalChunks = Number(payload.chunk_count || asset.totalChunks || Math.max(1, Math.ceil(asset.fileSize / asset.chunkSize)));
        asset.receivedBitmap = payload.received_bitmap || asset.receivedBitmap || '';
        asset.streamKeyHex = payload.stream_key_hex || asset.streamKeyHex || '';
        asset.streamIvSeedHex = payload.stream_iv_seed_hex || asset.streamIvSeedHex || '';
        asset.resumeFromByte = Number(payload.resume_from_byte || asset.resumeFromByte || 0);
        asset.completedChunks = Number(payload.received_chunks || 0);
        asset.currentParallel = clampParallelToBudget(asset, payload.current_parallel_chunks || payload.initial_parallel_chunks || asset.currentParallel || UPLOAD_DEFAULT_PARALLEL);
        asset.maxParallel = Math.max(asset.currentParallel, clampParallelToBudget(asset, payload.max_parallel_chunks || asset.maxParallel || UPLOAD_MAX_PARALLEL));
        asset.peakParallel = Math.max(Number(asset.peakParallel || 0), Number(asset.currentParallel || 0), Number(asset.maxParallel || 0));
        rebuildPendingChunks(asset);
        syncConfirmedProgress(asset);
        updateAssetProgress(asset);
    }

    function syncUploadSession(asset) {
        if (!asset || !asset.uploadId || !asset.uploadToken) return Promise.reject(new Error('upload session missing'));
        return fetchWithTimeout(UPLOAD_STATUS_ENDPOINT, {
            method: 'POST',
            headers: { 'Content-Type': 'application/x-www-form-urlencoded', 'Accept': 'application/json', 'X-Requested-With': 'XMLHttpRequest' },
            body: encodeFormBody({ upload_id: asset.uploadId, upload_token: asset.uploadToken })
        }, UPLOAD_SESSION_FETCH_TIMEOUT_MS).then(function(response) {
            if (!response.ok) throw new Error('status:' + response.status);
            return response.json();
        }).then(function(payload) {
            applyServerSessionState(asset, payload);
            return payload;
        });
    }

    function shouldAccelerateParallel(asset) {
        if (!asset || asset.failed || asset.isCancelling || asset.recovering || asset.isPaused) return false;
        if (!asset.uploadId || !asset.uploadToken) return false;
        if ((asset.currentParallel || 0) >= (asset.maxParallel || 0)) return false;
        if ((asset.activeRequests || 0) < Math.max(2, (asset.currentParallel || 0) - 1)) return false;
        if ((Date.now() - Number(asset.lastRenegotiateAt || 0)) < MIN_RENEGOTIATE_INTERVAL_MS) return false;
        var state = ensureLinkState(asset);
        if (Number(state.recentRetries || 0) > 0 || Number(state.recentTimeouts || 0) > 0) return false;
        if (Number(state.successes || 0) < Math.max(4, (asset.currentParallel || 0))) return false;
        return computeLinkStabilityScore(asset) >= 78;
    }

    function requestUploadAcceleration(asset) {
        if (!shouldAccelerateParallel(asset)) return Promise.resolve(false);
        asset.lastRenegotiateAt = Date.now();
        var suggested = Math.min(clampParallelToBudget(asset, Math.max(Number(asset.peakParallel || 0), (asset.currentParallel || UPLOAD_DEFAULT_PARALLEL) + Math.max(8, Math.ceil((asset.currentParallel || 0) * 0.5)))), asset.maxParallel || UPLOAD_MAX_PARALLEL);
        var linkHints = buildLinkHintFields(asset);
        return fetchWithTimeout(UPLOAD_RENEGOTIATE_ENDPOINT, {
            method: 'POST',
            headers: { 'Content-Type': 'application/x-www-form-urlencoded', 'Accept': 'application/json', 'X-Requested-With': 'XMLHttpRequest' },
            body: encodeFormBody({ upload_id: asset.uploadId, upload_token: asset.uploadToken, suggested_parallel: suggested, link_stability_score: linkHints.link_stability_score || '', link_effective_type: linkHints.link_effective_type || '', link_downlink_mbps: linkHints.link_downlink_mbps || '', link_rtt_ms: linkHints.link_rtt_ms || '', link_retry_events: linkHints.link_retry_events || 0, link_timeout_events: linkHints.link_timeout_events || 0, link_save_data: linkHints.link_save_data || '0' })
        }, UPLOAD_SESSION_FETCH_TIMEOUT_MS).then(function(response) {
            if (!response.ok) throw new Error('renegotiate:' + response.status);
            return response.json();
        }).then(function(payload) {
            applyServerSessionState(asset, payload);
            return true;
        }).catch(function() { return false; });
    }

    function queueNextChunkIndex(asset) {
        if (asset.nextChunkCursor >= asset.pendingChunks.length && hasMissingChunks(asset)) rebuildPendingChunks(asset);
        while (asset.nextChunkCursor < asset.pendingChunks.length) {
            var nextIndex = asset.pendingChunks[asset.nextChunkCursor];
            asset.nextChunkCursor += 1;
            if (!asset.inflightChunks[nextIndex]) return nextIndex;
        }
        return null;
    }

    function updateAssetProgress(asset) {
        var confirmed = confirmedUploadBytes(asset);
        var total = activeUploadBytes(asset);
        if (asset.recovering || asset.isPaused) total = confirmed;
        asset.lastVisualBytes = Math.max(confirmed, Math.max(asset.lastVisualBytes || 0, total));
        if (asset.completedChunks === asset.totalChunks && confirmed >= (asset.fileSize || 0)) asset.lastVisualBytes = asset.fileSize;
        var visual = Math.min(asset.fileSize || 0, asset.lastVisualBytes || 0);
        var percent = asset.fileSize ? Math.min(99, Math.round((visual / asset.fileSize) * 100)) : 0;
        if (asset.completedChunks === asset.totalChunks && confirmed >= (asset.fileSize || 0)) percent = 100;
        asset.ui.status.textContent = percent >= 100 ? 'Uploaded [100%]' : ('Uploading [' + percent + '%]');
        asset.ui.progressInner.style.width = percent + '%';
    }

    function finalizeChunkedUpload(asset) {
        var xhr = new XMLHttpRequest();
        var sessionGeneration = Number(asset.sessionGeneration || 0);
        xhr._tasfaKind = 'finalize';
        asset.xhrs.push(xhr);
        xhr.open('POST', UPLOAD_COMPLETE_ENDPOINT, true);
        xhr.setRequestHeader('Content-Type', 'application/x-www-form-urlencoded');
        xhr.setRequestHeader('Accept', 'application/json');
        xhr.setRequestHeader('X-Requested-With', 'XMLHttpRequest');
        xhr.onload = function() {
            asset.xhrs = asset.xhrs.filter(function(item) { return item !== xhr; });
            if (sessionGeneration !== Number(asset.sessionGeneration || 0)) return;
            if (xhr.status !== 200) {
                if (xhr.status === 404 || xhr.status === 403) { asset.finalizing = false; rolloverUploadSession(asset, 'finalize'); return; }
                if (xhr.status === 429 || xhr.status >= 500) { asset.finalizing = false; rolloverUploadSession(asset, 'finalize'); return; }
                markUploadFailure(asset, 'Upload failed [' + xhr.status + ']');
                return;
            }
            var payload = null;
            try { payload = JSON.parse(xhr.responseText); } catch (err) { markUploadFailure(asset, 'Upload failed [invalid JSON]'); return; }
            if (!payload || payload.ok === false || !payload.url) {
                if (payload && (payload.error === 'upload session not found' || payload.error === 'upload session expired')) { asset.finalizing = false; rolloverUploadSession(asset, 'finalize'); return; }
                markUploadFailure(asset, 'Upload failed [' + ((payload && payload.error) || 'unknown error') + ']');
                return;
            }
            finalizeUploadSuccess(asset, payload);
        };
        xhr.onerror = xhr.ontimeout = function() {
            asset.xhrs = asset.xhrs.filter(function(item) { return item !== xhr; });
            if (sessionGeneration !== Number(asset.sessionGeneration || 0)) return;
            asset.ui.status.textContent = 'Rolling over upload session';
            syncUploadSession(asset).then(function() {
                if (sessionGeneration !== Number(asset.sessionGeneration || 0)) return;
                if (asset.completedChunks === asset.totalChunks) { asset.finalizing = false; finalizeChunkedUpload(asset); return; }
                asset.finalizing = false; fillChunkWindow(asset, asset.file);
            }).catch(function() {
                if (sessionGeneration !== Number(asset.sessionGeneration || 0)) return;
                asset.finalizing = false; rolloverUploadSession(asset, 'finalize');
            });
        };
        xhr.timeout = 15000;
        xhr.send(encodeFormBody({ upload_id: asset.uploadId, upload_token: asset.uploadToken }));
    }

    function maybeFinalizeChunkUpload(asset) {
        if (asset.failed || asset.isCancelling || asset.finalizing || Number(asset.dispatchReservations || 0) > 0) return;
        if (asset.completedChunks !== asset.totalChunks) return;
        if (hasMissingChunks(asset)) return;
        asset.finalizing = true;
        asset.ui.status.textContent = 'Verifying uploaded chunks';
        syncUploadSession(asset).then(function() {
            if (asset.failed || asset.isCancelling) { asset.finalizing = false; return; }
            if (asset.completedChunks !== asset.totalChunks || hasMissingChunks(asset)) { asset.finalizing = false; asset.ui.status.textContent = 'Rolling over remaining chunks'; fillChunkWindow(asset, asset.file); return; }
            asset.ui.status.textContent = 'Finalizing upload on server';
            finalizeChunkedUpload(asset);
        }).catch(function() {
            if (asset.failed || asset.isCancelling) { asset.finalizing = false; return; }
            asset.finalizing = false; rolloverUploadSession(asset, 'finalize');
        });
    }

    function rolloverUploadSession(asset, reason) {
        if (!asset || asset.isCancelling || !asset.file) return;
        var rolloverState = {
            uploadId: asset.uploadId, uploadToken: asset.uploadToken,
            confirmedBytes: contiguousConfirmedUploadBytes(asset),
            receivedBitmap: String(asset.receivedBitmap || '')
        };
        clearRecoveryTimer(asset); clearSchedulerTimer(asset); abortInflightRequests(asset);
        asset.recovering = true; asset.isPaused = true;
        asset.dispatchReservations = 0; asset.activeRequests = 0;
        asset.inflightChunks = {}; asset.chunkNetworkStarted = {}; asset.finalizing = false;
        asset.recoveryEpoch = Number(asset.recoveryEpoch || 0) + 1;
        asset.transferProgress = new Array(asset.totalChunks || 0).fill(0);
        syncConfirmedProgress(asset);
        asset.ui.status.textContent = reason === 'offline' ? 'Waiting for network' : 'Rolling over upload session';
        if (!navigator.onLine) return;
        initChunkedUpload(asset, asset.file, 'rollover', rolloverState);
    }

    function dispatchChunk(asset, file, chunkIndex, retryCount) {
        if (asset.failed || asset.isCancelling || asset.isPaused) return;
        var sessionGeneration = Number(asset.sessionGeneration || 0);
        var preparedState = asset.preparedChunks && asset.preparedChunks[chunkIndex] ? asset.preparedChunks[chunkIndex] : null;
        var range = chunkByteRange(asset, chunkIndex);
        var start = preparedState ? preparedState.start : range.start;
        var end = preparedState ? preparedState.end : range.end;
        var blob = file.slice(start, end);
        asset.inflightChunks[chunkIndex] = true;
        asset.dispatchReservations = Math.max(0, Number(asset.dispatchReservations || 0)) + 1;
        markSchedulerActivity(asset);
        if (preparedState && asset.preparedChunks) delete asset.preparedChunks[chunkIndex];
        schedulePrepareAhead(asset, file);

        function attemptUpload(attempt) {
            var xhr = new XMLHttpRequest();
            xhr._tasfaKind = 'chunk';
            asset.xhrs.push(xhr);
            asset.transferProgress[chunkIndex] = 0;
            asset.inflightRequests[chunkIndex] = xhr;
            asset.chunkActivityAt[chunkIndex] = Date.now();

            xhr.open('POST', UPLOAD_ENDPOINT, true);
            xhr.timeout = 15000;
            xhr.setRequestHeader('Accept', 'application/json');
            xhr.setRequestHeader('X-Requested-With', 'XMLHttpRequest');

            xhr.upload.onprogress = function(event) {
                if (!event.lengthComputable || asset.failed || asset.isCancelling || sessionGeneration !== Number(asset.sessionGeneration || 0)) return;
                asset.transferProgress[chunkIndex] = event.loaded;
                asset.chunkActivityAt[chunkIndex] = Date.now();
                markSchedulerActivity(asset);
                updateAssetProgress(asset);
            };

            xhr.onload = function() {
                asset.xhrs = asset.xhrs.filter(function(item) { return item !== xhr; });
                if (sessionGeneration !== Number(asset.sessionGeneration || 0)) return;
                if (asset.failed || asset.isCancelling || asset.recovering) return;
                finishNetworkAttempt(asset, chunkIndex);
                if (xhr.status !== 200 && xhr.status !== 204) {
                    if ((xhr.status === 429 || xhr.status >= 500) && attempt < retryCount) {
                        recordLinkPenalty(asset, xhr.status === 429 ? 'retry' : 'timeout');
                        setTimeout(function() { attemptUpload(attempt + 1); }, nextChunkRetryDelay(attempt));
                        return;
                    }
                    recordLinkPenalty(asset, xhr.status === 429 ? 'retry' : 'timeout');
                    recoverOrContinue(asset, file, chunkIndex, xhr.status === 429 ? 'slow' : 'timeout');
                    return;
                }
                recordLinkSuccess(asset, Date.now() - Number(xhr._tasfaStartedAt || Date.now()));
                asset.lastChunkResponseAt = Date.now();
                if (xhr.status === 204) {
                    asset.transferProgress[chunkIndex] = blob.size;
                    asset.receivedBitmap = setBitmapBit(asset.receivedBitmap, chunkIndex);
                    asset.completedChunks = countBitmapBits(asset.receivedBitmap);
                    asset.confirmedBytes = confirmedUploadBytes(asset);
                    resetInflightChunk(asset, chunkIndex, true);
                    markSchedulerActivity(asset);
                    updateAssetProgress(asset);
                    requestUploadAcceleration(asset);
                    fillChunkWindow(asset, file);
                    maybeFinalizeChunkUpload(asset);
                    return;
                }
                var payload = null;
                try { payload = JSON.parse(xhr.responseText); } catch (err) {
                    recoverOrContinue(asset, file, chunkIndex, 'recover');
                    return;
                }
                if (!payload || payload.ok === false) {
                    recoverOrContinue(asset, file, chunkIndex, 'recover');
                    return;
                }
                if (payload.accepted === false && payload.recoverable) {
                    asset.transferProgress[chunkIndex] = 0;
                    asset.receivedBitmap = payload.received_bitmap || asset.receivedBitmap;
                    asset.confirmedBytes = confirmedUploadBytes(asset);
                    asset.resumeFromByte = Math.max(Number(asset.resumeFromByte || 0), contiguousConfirmedUploadBytes(asset));
                    asset.completedChunks = Number(payload.received_chunks || asset.completedChunks);
                    rebuildPendingChunks(asset);
                    resetInflightChunk(asset, chunkIndex, false);
                    markSchedulerActivity(asset);
                    updateAssetProgress(asset);
                    fillChunkWindow(asset, file);
                    return;
                }
                asset.transferProgress[chunkIndex] = blob.size;
                asset.receivedBitmap = setBitmapBit(asset.receivedBitmap, chunkIndex);
                asset.completedChunks = countBitmapBits(asset.receivedBitmap);
                asset.confirmedBytes = confirmedUploadBytes(asset);
                asset.resumeFromByte = Math.max(Number(asset.resumeFromByte || 0), contiguousConfirmedUploadBytes(asset));
                rebuildPendingChunks(asset);
                resetInflightChunk(asset, chunkIndex, true);
                markSchedulerActivity(asset);
                updateAssetProgress(asset);
                requestUploadAcceleration(asset);
                fillChunkWindow(asset, file);
                maybeFinalizeChunkUpload(asset);
            };

            xhr.onerror = xhr.ontimeout = function() {
                asset.xhrs = asset.xhrs.filter(function(item) { return item !== xhr; });
                if (sessionGeneration !== Number(asset.sessionGeneration || 0)) return;
                if (asset.failed || asset.isCancelling || asset.recovering) return;
                finishNetworkAttempt(asset, chunkIndex);
                if (attempt < retryCount) {
                    recordLinkPenalty(asset, 'timeout');
                    setTimeout(function() { attemptUpload(attempt + 1); }, nextChunkRetryDelay(attempt));
                    return;
                }
                recordLinkPenalty(asset, 'timeout');
                recoverOrContinue(asset, file, chunkIndex, 'timeout');
            };

            xhr.onabort = function() {
                asset.xhrs = asset.xhrs.filter(function(item) { return item !== xhr; });
                if (sessionGeneration !== Number(asset.sessionGeneration || 0)) return;
                if (asset.suppressAbortHandling || asset.recovering) return;
                finishNetworkAttempt(asset, chunkIndex);
                if (!asset.isCancelling && !asset.failed) {
                    recordLinkPenalty(asset, navigator.onLine ? 'retry' : 'timeout');
                    recoverOrContinue(asset, file, chunkIndex, navigator.onLine ? 'recover' : 'offline');
                }
            };

            xhr._tasfaStartedAt = Date.now();
            var dispatchMeta = preparedState ? Promise.resolve(preparedState) : (function() {
                return preprocessChunkPayload(asset, chunkIndex, blob).then(function(prepared) {
                    prepared.start = range.start;
                    prepared.blobSize = blob.size;
                    prepared.chunkIndex = chunkIndex;
                    return prepared;
                });
            })();
            dispatchMeta.then(function(prepared) {
                if (sessionGeneration !== Number(asset.sessionGeneration || 0)) return;
                if (!asset.chunkNetworkStarted) asset.chunkNetworkStarted = {};
                if (!asset.chunkNetworkStarted[chunkIndex]) {
                    asset.chunkNetworkStarted[chunkIndex] = true;
                    asset.activeRequests = Math.max(0, Number(asset.activeRequests || 0)) + 1;
                }
                xhr.setRequestHeader('Content-Type', 'application/octet-stream');
                xhr.setRequestHeader('X-TASFA-Upload-ID', asset.uploadId);
                xhr.setRequestHeader('X-TASFA-Upload-Token', asset.uploadToken);
                xhr.setRequestHeader('X-TASFA-Chunk-Index', String(chunkIndex));
                xhr.setRequestHeader('X-TASFA-Stream-Mode', 'aes-256-gcm');
                xhr.send(prepared.payloadBuffer);
            }).catch(function() {
                if (sessionGeneration !== Number(asset.sessionGeneration || 0)) return;
                delete asset.inflightChunks[chunkIndex];
                asset.dispatchReservations = Math.max(0, Number(asset.dispatchReservations || 0) - 1);
                markUploadFailure(asset, 'Browser crypto unavailable');
            });
        }
        attemptUpload(0);
    }

    function fillChunkWindow(asset, file) {
        if (!asset || asset.failed || asset.isCancelling || asset.isPaused || asset.recovering) return;
        if (Number(asset.dispatchReservations || 0) === 0 && hasMissingChunks(asset) && asset.nextChunkCursor >= asset.pendingChunks.length) {
            rebuildPendingChunks(asset);
        }
        while (Number(asset.dispatchReservations || 0) < asset.currentParallel) {
            var nextChunk = queueNextChunkIndex(asset);
            if (nextChunk === null) break;
            dispatchChunk(asset, file, nextChunk, UPLOAD_CHUNK_RETRY_LIMIT);
        }
        markSchedulerActivity(asset);
        maybeFinalizeChunkUpload(asset);
    }

    function beginChunkUpload(asset, file) {
        asset.isUploading = true;
        clearRecoveryTimer(asset); clearSchedulerTimer(asset);
        asset.file = file;
        asset.chunkSize = asset.chunkSize || UPLOAD_CHUNK_SIZE;
        asset.totalChunks = Math.max(1, Math.ceil(file.size / asset.chunkSize));
        asset.resumeFromByte = Math.max(Number(asset.resumeFromByte || 0), contiguousConfirmedUploadBytes(asset));
        syncConfirmedProgress(asset);
        if (!asset.pendingChunks || !asset.pendingChunks.length) rebuildPendingChunks(asset);
        else asset.nextChunkCursor = 0;
        asset.finalizing = false; asset.failed = false; asset.recovering = false; asset.isPaused = false;
        asset.dispatchReservations = 0; asset.activeRequests = 0;
        asset.inflightChunks = {}; asset.inflightRequests = {}; asset.chunkNetworkStarted = {}; asset.chunkActivityAt = {};
        asset.xhrs = [];
        asset.lastVisualBytes = Math.max(asset.lastVisualBytes || 0, confirmedUploadBytes(asset));
        asset.lastSchedulerActivityAt = Date.now();
        asset.currentParallel = clampParallelToBudget(asset, Math.min(asset.maxParallel || UPLOAD_MAX_PARALLEL, asset.currentParallel || UPLOAD_DEFAULT_PARALLEL));
        clearPreparedChunks(asset);
        startSchedulerLoop(asset);
        schedulePrepareAhead(asset, file);
        fillChunkWindow(asset, file);
        updateFileRepoUploadButton();
        updateSubmitButtons();
    }

    function initChunkedUpload(asset, file, reason, rolloverState) {
        var sessionGeneration = bumpUploadSessionGeneration(asset);
        asset.isUploading = true; asset.failed = false; asset.isPaused = false; asset.recovering = false;
        updateFileRepoUploadButton(); updateSubmitButtons();
        var chunkCount = Math.max(1, Math.ceil(file.size / UPLOAD_CHUNK_SIZE));
        var linkHints = buildLinkHintFields(asset);
        var suggestedParallel = reason === 'rollover' ? UPLOAD_DEFAULT_PARALLEL : Math.max(Number(asset.peakParallel || 0), Number(asset.currentParallel || UPLOAD_DEFAULT_PARALLEL));
        var body = 'filename=' + encodeURIComponent(file.name) +
            '&total_size=' + encodeURIComponent(String(file.size)) +
            '&chunk_count=' + encodeURIComponent(String(chunkCount)) +
            '&post_id=' + encodeURIComponent(window.POST_ID || 0) +
            '&session_id=' + encodeURIComponent(asset.client_uuid || '') +
            '&suggested_parallel=' + encodeURIComponent(String(suggestedParallel)) +
            '&link_stability_score=' + encodeURIComponent(String(linkHints.link_stability_score || '')) +
            '&link_effective_type=' + encodeURIComponent(linkHints.link_effective_type || '') +
            '&link_downlink_mbps=' + encodeURIComponent(String(linkHints.link_downlink_mbps || '')) +
            '&link_rtt_ms=' + encodeURIComponent(String(linkHints.link_rtt_ms || '')) +
            '&link_retry_events=' + encodeURIComponent(String(linkHints.link_retry_events || '')) +
            '&link_timeout_events=' + encodeURIComponent(String(linkHints.link_timeout_events || '')) +
            '&link_save_data=' + encodeURIComponent(linkHints.link_save_data || '0');
        if (rolloverState && rolloverState.uploadId && rolloverState.uploadToken) {
            body += '&rollover_upload_id=' + encodeURIComponent(rolloverState.uploadId) +
                '&rollover_upload_token=' + encodeURIComponent(rolloverState.uploadToken) +
                '&client_confirmed_bytes=' + encodeURIComponent(String(rolloverState.confirmedBytes || 0));
        }
        fetchWithTimeout(UPLOAD_INIT_ENDPOINT, {
            method: 'POST',
            headers: { 'Content-Type': 'application/x-www-form-urlencoded', 'Accept': 'application/json', 'X-Requested-With': 'XMLHttpRequest' },
            body: body
        }, UPLOAD_SESSION_FETCH_TIMEOUT_MS).then(function(response) {
            if (!response.ok) throw new Error('init:' + response.status);
            return response.json();
        }).then(function(payload) {
            if (sessionGeneration !== Number(asset.sessionGeneration || 0)) return;
            if (payload && payload.already_done) {
                asset.fid = payload.file_id;
                asset.url = payload.url || ('/file/download/' + String(payload.file_id || ''));
                finalizeUploadSuccess(asset, payload);
                return;
            }
            if (!payload || payload.ok === false || !payload.upload_id || !payload.upload_token || !payload.stream_key_hex || !payload.stream_iv_seed_hex) {
                throw new Error((payload && payload.error) || 'upload init failed');
            }
            asset.uploadId = payload.upload_id;
            asset.uploadToken = payload.upload_token;
            asset.uploadSecret = payload.upload_secret;
            asset.streamKeyHex = payload.stream_key_hex;
            asset.streamIvSeedHex = payload.stream_iv_seed_hex;
            asset.chunkSize = Number(payload.chunk_size || UPLOAD_CHUNK_SIZE);
            asset.currentParallel = clampParallelToBudget(asset, payload.initial_parallel_chunks || UPLOAD_DEFAULT_PARALLEL);
            asset.maxParallel = Math.max(asset.currentParallel, clampParallelToBudget(asset, payload.max_parallel_chunks || UPLOAD_MAX_PARALLEL));
            asset.receivedBitmap = payload.received_bitmap || '';
            asset.resumeFromByte = Number(payload.resume_from_byte || 0);
            asset.completedChunks = Number(payload.received_chunks || 0);
            asset.recoveryAttempts = 0;
            syncConfirmedProgress(asset);
            asset.ui.status.textContent = (rolloverState && rolloverState.uploadId) || payload.resumed ? 'Rolled over upload session' : 'Negotiated upload session';
            beginChunkUpload(asset, file);
        }).catch(function(error) {
            if (sessionGeneration !== Number(asset.sessionGeneration || 0)) return;
            var message = error && error.message ? error.message : '';
            if (message.indexOf('init:4') === 0) { markUploadFailure(asset, 'Upload failed [' + message + ']'); return; }
            var nextRecoveryAttempts = Number(asset.recoveryAttempts || 0) + 1;
            asset.recoveryAttempts = nextRecoveryAttempts;
            asset.recovering = true; asset.isPaused = true;
            if (nextRecoveryAttempts > uploadRolloverLimit(asset)) { markUploadFailure(asset, 'Upload failed [rollover limit]'); return; }
            asset.ui.status.textContent = navigator.onLine ? 'Rolling over upload session' : 'Waiting for network';
            clearRecoveryTimer(asset);
            asset.recoveryTimer = window.setTimeout(function() {
                if (sessionGeneration !== Number(asset.sessionGeneration || 0)) return;
                if (!navigator.onLine) { rolloverUploadSession(asset, 'offline'); return; }
                initChunkedUpload(asset, file, reason || 'rollover', rolloverState || { uploadId: asset.uploadId, uploadToken: asset.uploadToken, confirmedBytes: contiguousConfirmedUploadBytes(asset) });
            }, uploadRolloverDelay(asset));
        });
    }

    function startQueuedAssetUpload(asset) {
        if (!asset || asset.isExisting || asset.fid !== null || asset.uploadId || asset.isUploading || asset.failed) return;
        asset.ui.status.textContent = 'Preparing upload session';
        initChunkedUpload(asset, asset.file);
    }

    function flushQueuedFileRepoUploads() {
        if (!isFileRepoMode) return;
        fileRepoPendingAssets().forEach(startQueuedAssetUpload);
        updateFileRepoUploadButton();
    }

    function uploadFile(file) {
        if (!uploadPreview) return;
        var clientUuid = generateUUID();
        var blobUrl = URL.createObjectURL(file);
        var placeholderUrl = isEditorMode ? (blobUrl + '#' + clientUuid) : null;
        var ui = createMediaCard(file.name, blobUrl, /^image\//.test(file.type || ''));
        var asset = {
            client_uuid: clientUuid,
            fid: null,
            filename: file.name,
            file: file,
            blobUrl: blobUrl,
            url: null,
            placeholderUrl: placeholderUrl,
            mode: isEditorMode ? 'inline' : 'attachment',
            xhrs: [],
            uploadId: null,
            uploadToken: null,
            uploadSecret: null,
            streamKeyHex: '',
            streamIvSeedHex: '',
            fileSize: file.size,
            chunkSize: UPLOAD_CHUNK_SIZE,
            transferProgress: [],
            confirmedBytes: 0,
            resumeFromByte: 0,
            receivedBitmap: '',
            completedChunks: 0,
            pendingChunks: [],
            nextChunkCursor: 0,
            totalChunks: 0,
            currentParallel: UPLOAD_DEFAULT_PARALLEL,
            maxParallel: UPLOAD_MAX_PARALLEL,
            peakParallel: UPLOAD_DEFAULT_PARALLEL,
            dispatchReservations: 0,
            activeRequests: 0,
            lastRenegotiateAt: 0,
            inflightChunks: {},
            inflightRequests: {},
            chunkNetworkStarted: {},
            chunkActivityAt: {},
            lastVisualBytes: 0,
            lastSchedulerActivityAt: 0,
            lastChunkResponseAt: 0,
            recoveryEpoch: 0,
            recoveryAttempts: 0,
            recoveryTimer: null,
            schedulerTimer: null,
            sessionGeneration: 0,
            isUploading: false,
            isPaused: false,
            recovering: false,
            finalizing: false,
            failed: false,
            isCancelling: false,
            ui: ui
        };
        AssetRegistry.push(asset);
        bindAssetControls(asset);
        updateFileRepoUploadButton();
        updateSubmitButtons();
        if (isEditorMode) { initChunkedUpload(asset, file); }
    }

    function bootstrapExistingAssets() {
        if (!isEditorMode || !uploadPreview) return;
        var cards = uploadPreview.querySelectorAll('.existing-media');
        Array.prototype.forEach.call(cards, function(card, index) {
            var fid = Number(card.getAttribute('data-fid'));
            var filename = card.getAttribute('data-filename') || 'Attachment';
            var url = card.getAttribute('data-url') || ('/file/download/' + fid);
            var mode = card.getAttribute('data-mode') || 'attachment';
            var btnInsert = document.createElement('button');
            btnInsert.type = 'button';
            btnInsert.className = 'btn btn-outline media-insert-btn';
            btnInsert.textContent = 'Insert';
            var actions = card.querySelector('.media-actions');
            if (actions) actions.insertBefore(btnInsert, actions.firstChild);

            var asset = {
                client_uuid: 'existing-' + fid + '-' + index,
                fid: fid,
                filename: filename,
                blobUrl: null,
                url: url,
                placeholderUrl: null,
                mode: mode,
                xhr: null,
                isExisting: true,
                ui: {
                    el: card,
                    thumbImg: card.querySelector('img'),
                    status: card.querySelector('.media-status'),
                    progress: card.querySelector('.media-progress-bar'),
                    progressInner: card.querySelector('.media-progress-inner'),
                    btnInsert: btnInsert,
                    btnInline: card.querySelector('.media-inline-btn'),
                    btnAttachment: card.querySelector('.media-attachment-btn'),
                    btnDelete: card.querySelector('.media-delete-btn')
                }
            };
            if (asset.ui.progress) {
                asset.ui.progress.style.display = 'none';
            }
            if (asset.ui.progressInner) {
                asset.ui.progressInner.style.width = '0%';
            }
            AssetRegistry.push(asset);
            bindAssetControls(asset);
        });
        syncMediaMeta();
    }

    function onFileBatch(files) {
        if (!files) return;
        for (var i = 0; i < files.length; i += 1) {
            uploadFile(files[i]);
        }
    }

    if (dropzone) {
        dropzone.addEventListener('dragover', function(event) {
            event.preventDefault();
            dropzone.classList.add('is-dragging');
        });
        dropzone.addEventListener('dragleave', function(event) {
            event.preventDefault();
            dropzone.classList.remove('is-dragging');
        });
        dropzone.addEventListener('drop', function(event) {
            event.preventDefault();
            dropzone.classList.remove('is-dragging');
            if (!event.dataTransfer || !event.dataTransfer.files) return;
            onFileBatch(event.dataTransfer.files);
        });
    }

    if (fileInput) {
        fileInput.addEventListener('change', function(event) {
            onFileBatch(event.target.files);
            fileInput.value = '';
        });
    }

    if (fileRepoUploadButton) {
        fileRepoUploadButton.addEventListener('click', flushQueuedFileRepoUploads);
        updateFileRepoUploadButton();
    }

    if (ta) {
        ta.addEventListener('input', schedulePreview);
        ta.addEventListener('paste', function(event) {
            if (!event.clipboardData || !event.clipboardData.files || !event.clipboardData.files.length) return;
            event.preventDefault();
            onFileBatch(event.clipboardData.files);
        });
    }

    Array.prototype.forEach.call(tabButtons, function(button) {
        button.addEventListener('click', function() {
            switchTab(button.getAttribute('data-editor-tab'));
        });
    });

    Array.prototype.forEach.call(toolbarButtons, function(button) {
        button.addEventListener('click', function() {
            var wrap = button.getAttribute('data-md-wrap');
            var prefix = button.getAttribute('data-md-prefix');
            var block = button.getAttribute('data-md-block');
            var placeholder = button.getAttribute('data-md-placeholder') || '';
            if (wrap) {
                toggleWrap(wrap, placeholder);
            } else if (prefix) {
                prependToSelection(prefix, placeholder);
            } else if (block) {
                insertAtCursor(block + '\n');
            }
        });
    });

    if (form) {
        form.addEventListener('submit', function(event) {
            if (hasActiveUploads()) {
                event.preventDefault();
                updateSubmitButtons();
                return;
            }
            isSubmitting = true;
            syncMediaMeta();
        });
    }

    window.addEventListener('offline', function() {
        AssetRegistry.forEach(function(asset) {
            if (!asset || asset.isExisting || asset.fid !== null || !asset.isUploading) return;
            asset.isPaused = true;
            asset.ui.status.textContent = 'Waiting for network';
            abortInflightRequests(asset);
        });
    });

    window.addEventListener('online', function() {
        AssetRegistry.forEach(function(asset) {
            if (!asset || asset.isExisting || asset.fid !== null || !asset.uploadId || asset.finalizing) return;
            asset.ui.status.textContent = 'Network restored, rolling over';
            rolloverUploadSession(asset, 'offline');
        });
    });

    window.addEventListener('beforeunload', function() {
        if (isSubmitting) return;
        var transientFids = AssetRegistry
            .filter(function(asset) { return !asset.isExisting && asset.fid !== null; })
            .map(function(asset) { return asset.fid; });
        if (!transientFids.length || !navigator.sendBeacon) return;
        var formData = new FormData();
        if (transientFids.length) formData.append('fids', JSON.stringify(transientFids));
        navigator.sendBeacon(UPLOAD_CANCEL_ENDPOINT, formData);
    });

    window.cleanupUploadedFiles = function(event) {
        var transientFids = AssetRegistry
            .filter(function(asset) { return !asset.isExisting && asset.fid !== null; })
            .map(function(asset) { return asset.fid; });
        var transientUploadIds = AssetRegistry
            .filter(function(asset) { return !asset.isExisting && asset.fid === null && asset.uploadId; })
            .map(function(asset) { return asset.uploadId; });
        if (!transientFids.length && !transientUploadIds.length) return;
        event.preventDefault();
        var targetUrl = event.currentTarget.href;
        fetch(UPLOAD_CANCEL_ENDPOINT, {
            method: 'POST',
            headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
            body: 'fids=' + encodeURIComponent(JSON.stringify(transientFids)) +
                '&upload_ids=' + encodeURIComponent(JSON.stringify(transientUploadIds))
        }).finally(function() {
            window.location.href = targetUrl;
        });
    };

    if (titleInput) {
        titleInput.addEventListener('keydown', function(event) {
            if (event.key === 'Enter') {
                event.preventDefault();
                ta.focus();
            }
        });
    }

    if (summaryInput) {
        summaryInput.addEventListener('input', function() {
            if (syncStatus) syncStatus.textContent = 'Publish rail updated';
        });
    }

    bootstrapExistingAssets();
    updateSubmitButtons();
    if (isEditorMode) {
        switchTab('write');
        updatePreview();
    }
    }

    if (document.readyState === 'loading') {
        document.addEventListener('DOMContentLoaded', init, { once: true });
    } else {
        init();
    }
})();

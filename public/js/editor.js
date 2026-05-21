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
    var USE_TASFA = window.BLOG_USE_TASFA !== false;
    var PLAIN_UPLOAD_ENDPOINT = '/api/upload';
    var UPLOAD_INIT_ENDPOINT = '/file/upload/init';
    var UPLOAD_COMPLETE_ENDPOINT = '/file/upload/complete';
    var UPLOAD_CANCEL_ENDPOINT = '/file/upload/cancel';
    var UPLOAD_STATUS_ENDPOINT = '/file/upload/status';
    var UPLOAD_ENDPOINT = '/file/upload';
    var UPLOAD_CHUNK_SIZE = 8 * 1024 * 1024;
    var UPLOAD_DEFAULT_PARALLEL = 4;
    var TASFA_MIN_SIZE_BYTES = 4 * 1024 * 1024;
    var FileUploadQueue = [];
    var isFileUploadRunning = false;

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
        var url = asset.url;
        var isMedia = /^image\//.test(asset.mime_type || '') || /^video\//.test(asset.mime_type || '') || /^audio\//.test(asset.mime_type || '');
        if (isMedia) {
            insertAtCursor('![' + asset.filename + '](' + url + ')\n');
        } else {
            insertAtCursor('[' + asset.filename + '](' + url + ')\n');
        }
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
        FileUploadQueue = FileUploadQueue.filter(function(uuid) {
            return uuid !== asset.client_uuid;
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

    function createMediaCard(filename, previewUrl, mediaType) {
        var card = document.createElement('div');
        card.className = 'media-card';

        var thumb = document.createElement('div');
        thumb.className = 'media-thumb';
        if (mediaType === 'image') {
            var img = document.createElement('img');
            img.src = previewUrl;
            img.style.display = 'block';
            img.style.width = '100%';
            img.style.height = '100%';
            img.style.maxWidth = 'none';
            img.style.objectFit = 'cover';
            img.style.filter = 'grayscale(1) contrast(1.08)';
            thumb.appendChild(img);
        } else if (mediaType === 'video') {
            var vid = document.createElement('video');
            vid.src = previewUrl;
            vid.muted = true;
            vid.playsInline = true;
            vid.preload = 'metadata';
            vid.style.display = 'block';
            vid.style.width = '100%';
            vid.style.height = '100%';
            vid.style.objectFit = 'cover';
            vid.style.filter = 'grayscale(1) contrast(1.08)';
            thumb.appendChild(vid);
        } else if (mediaType === 'audio') {
            var aud = document.createElement('audio');
            aud.src = previewUrl;
            aud.controls = true;
            aud.preload = 'metadata';
            aud.style.width = '100%';
            aud.style.height = '28px';
            aud.style.alignSelf = 'center';
            thumb.appendChild(aud);
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

        var btnUpload = document.createElement('button');
        btnUpload.type = 'button';
        btnUpload.className = 'btn media-upload-btn';
        btnUpload.textContent = 'Upload';
        btnUpload.style.display = 'none';

        var btnCancel = document.createElement('button');
        btnCancel.type = 'button';
        btnCancel.className = 'btn btn-outline media-cancel-btn';
        btnCancel.textContent = 'Cancel';
        btnCancel.style.display = 'none';

        var btnRemove = document.createElement('button');
        btnRemove.type = 'button';
        btnRemove.className = 'btn btn-outline media-remove-btn';
        btnRemove.textContent = 'Remove';
        btnRemove.style.display = 'none';

        var btnRetry = document.createElement('button');
        btnRetry.type = 'button';
        btnRetry.className = 'btn media-retry-btn';
        btnRetry.textContent = 'Retry';
        btnRetry.style.display = 'none';

        var btnDelete = document.createElement('button');
        btnDelete.type = 'button';
        btnDelete.className = 'btn btn-outline media-delete-btn';
        btnDelete.textContent = 'Delete';
        btnDelete.style.display = 'none';

        actions.appendChild(btnInsert);
        actions.appendChild(btnInline);
        actions.appendChild(btnAttachment);
        actions.appendChild(btnUpload);
        actions.appendChild(btnCancel);
        actions.appendChild(btnRemove);
        actions.appendChild(btnRetry);
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
            btnUpload: btnUpload,
            btnCancel: btnCancel,
            btnRemove: btnRemove,
            btnRetry: btnRetry,
            btnDelete: btnDelete
        };
    }

    function enqueueFileUpload(asset) {
        if (!asset || asset.isUploading || asset.fid !== null || asset.failed) return;
        if (FileUploadQueue.indexOf(asset.client_uuid) !== -1) return;
        FileUploadQueue.push(asset.client_uuid);
        if (asset.ui && asset.ui.status) asset.ui.status.textContent = 'Waiting...';
        if (asset.ui && asset.ui.btnUpload) asset.ui.btnUpload.disabled = true;
        updateMediaCardUI(asset);
    }

    function updateMediaCardUI(asset) {
        if (!asset || !asset.ui) return;
        var ui = asset.ui;
        var isQueued = FileUploadQueue.indexOf(asset.client_uuid) !== -1;
        var isUploading = asset.isUploading && !asset.failed && asset.fid === null && !asset.isCancelling;
        var isDone = asset.fid !== null && !asset.failed;
        var isFailed = asset.failed;
        var isPending = !asset.isExisting && asset.fid === null && !asset.isUploading && !asset.uploadId && !asset.failed;

        function hide(el) { if (el) el.style.display = 'none'; }
        function show(el) { if (el) el.style.display = ''; }

        hide(ui.btnInsert);
        hide(ui.btnInline);
        hide(ui.btnAttachment);
        hide(ui.btnUpload);
        hide(ui.btnDelete);
        hide(ui.btnCancel);
        hide(ui.btnRemove);
        hide(ui.btnRetry);

        if (isFailed) {
            if (!asset.tooLarge) show(ui.btnRetry);
            show(ui.btnRemove);
        } else if (isUploading) {
            show(ui.btnCancel);
        } else if (isPending || isQueued) {
            if (!isEditorMode) {
                show(ui.btnUpload);
            }
            show(ui.btnRemove);
        } else if (isDone) {
            if (isEditorMode) {
                show(ui.btnInsert);
                show(ui.btnInline);
                show(ui.btnAttachment);
            } else {
                show(ui.btnInsert);
                show(ui.btnInline);
            }
            show(ui.btnDelete);
        }

        if (isDone) {
            if (ui.btnInsert) ui.btnInsert.disabled = false;
            if (ui.btnInline) ui.btnInline.disabled = false;
            if (ui.btnAttachment) ui.btnAttachment.disabled = !isEditorMode;
        }
    }

    function processFileUploadQueue() {
        if (isFileUploadRunning) return;
        var nextUuid = FileUploadQueue.shift();
        if (!nextUuid) return;
        var asset = AssetRegistry.find(function(a) { return a.client_uuid === nextUuid; });
        if (!asset || asset.fid !== null || asset.isUploading || asset.failed) {
            processFileUploadQueue();
            return;
        }
        isFileUploadRunning = true;
        startQueuedAssetUpload(asset);
    }

    function bindAssetControls(asset) {
        var ui = asset.ui;

        function cleanupAssetUI() {
            removeMarkdownByUrl(asset.url || asset.placeholderUrl);
            if (asset.blobUrl) {
                URL.revokeObjectURL(asset.blobUrl);
                asset.blobUrl = null;
            }
            if (asset.ui && asset.ui.el) {
                asset.ui.el.remove();
            }
            removeAssetRecord(asset);
            updateFileRepoUploadButton();
            schedulePreview();
        }

        ui.btnCancel.onclick = function() {
            asset.isCancelling = true;
            if (asset.xhrs && asset.xhrs.length) {
                asset.xhrs.slice().forEach(function(xhr) {
                    try { xhr.abort(); } catch (err) {}
                });
            }
            if (asset.uploadId) {
                fetch(UPLOAD_CANCEL_ENDPOINT, {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
                    body: 'upload_ids=' + encodeURIComponent(JSON.stringify([asset.uploadId]))
                });
            }
            asset.isUploading = false;
            isFileUploadRunning = false;
            processFileUploadQueue();
            cleanupAssetUI();
        };

        ui.btnRemove.onclick = function() {
            if (asset.isUploading && !asset.isCancelling) {
                asset.isCancelling = true;
                if (asset.xhrs && asset.xhrs.length) {
                    asset.xhrs.slice().forEach(function(xhr) {
                        try { xhr.abort(); } catch (err) {}
                    });
                }
                if (asset.uploadId) {
                    fetch(UPLOAD_CANCEL_ENDPOINT, {
                        method: 'POST',
                        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
                        body: 'upload_ids=' + encodeURIComponent(JSON.stringify([asset.uploadId]))
                    });
                }
                asset.isUploading = false;
                isFileUploadRunning = false;
                processFileUploadQueue();
            }
            cleanupAssetUI();
        };

        ui.btnRetry.onclick = function() {
            if (!asset.file || asset.fid !== null) return;
            asset.failed = false;
            asset.isUploading = false;
            asset.isCancelling = false;
            asset.xhrs = [];
            if (asset.ui && asset.ui.progressInner) {
                asset.ui.progressInner.style.width = '0%';
            }
            if (asset.ui && asset.ui.progress) {
                asset.ui.progress.style.display = '';
            }
            if (asset.uploadId && asset.uploadToken && asset.isSessionExpired) {
                asset.isSessionExpired = false;
                verifyAllChunksBeforeComplete(asset, asset.file);
                return;
            }
            asset.uploadId = null;
            asset.uploadToken = null;
            if (isEditorMode) {
                startTasfaUpload(asset, asset.file);
            } else {
                enqueueFileUpload(asset);
                processFileUploadQueue();
            }
        };

        ui.btnDelete.onclick = function() {
            if (asset.fid !== null) {
                fetch('/file/delete', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
                    body: 'id=' + encodeURIComponent(String(asset.fid)) +
                        '&delete_pin=' + encodeURIComponent(asset.deletePin || '')
                });
            }
            cleanupAssetUI();
        };

        if (!isEditorMode) {
            ui.btnUpload.onclick = function() {
                enqueueFileUpload(asset);
                processFileUploadQueue();
            };
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
        } else {
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

        updateMediaCardUI(asset);
    }

    function finalizeUploadSuccess(asset, response) {
        asset.fid = response.fid || response.id || asset.fid;
        asset.url = response.url;
        asset.blob_url = response.url || '';
        asset.filename = response.filename || asset.filename;
        asset.mime_type = response.mime_type || asset.mime_type || '';
        asset.deletePin = response.delete_pin || '';
        asset.isUploading = false;
        asset.failed = false;
        asset.isCancelling = false;
        asset.xhrs = [];
        asset.mode = isEditorMode ? 'inline' : 'attachment';
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
        if (isEditorMode) {
            setModeButtons(asset.ui, asset.mode);
            if (asset.placeholderUrl) {
                replaceAllInEditor(asset.placeholderUrl, asset.url);
                asset.placeholderUrl = null;
            }
            var finalUrl = asset.url;
            var isValidUrl = finalUrl && (
                finalUrl.indexOf('/file/') === 0 ||
                /^https?:\/\//.test(finalUrl)
            );
            if (isValidUrl && !editorHasUrl(finalUrl) && !editorHasUrl('/file/download/' + asset.fid)) {
                insertAssetMarkdown(asset);
            }
        }
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
        isFileUploadRunning = false;
        processFileUploadQueue();
    }

    function markUploadFailure(asset, message) {
        asset.isUploading = false;
        asset.failed = true;
        asset.ui.status.textContent = message;
        asset.ui.btnDelete.disabled = false;
        asset.xhrs = [];
        updateFileRepoUploadButton();
        updateSubmitButtons();
        isFileUploadRunning = false;
        processFileUploadQueue();
        updateMediaCardUI(asset);
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
        fileRepoUploadButton.disabled = pending === 0 && FileUploadQueue.length === 0;
        if (isFileUploadRunning) {
            fileRepoUploadButton.textContent = 'Uploading...';
        } else {
            fileRepoUploadButton.textContent = 'Upload queued files';
        }
        updateSubmitButtons();
    }

    function updateAssetProgress(asset) {
        var totalTransferred = asset.confirmedBytes || 0;
        if (asset.inflightBytes) {
            for (var i = 0; i < asset.inflightBytes.length; i++) {
                totalTransferred += Number(asset.inflightBytes[i] || 0);
            }
        }
        var percent = asset.fileSize ? Math.min(100, Math.round((totalTransferred / asset.fileSize) * 100)) : 0;
        asset.ui.status.textContent = percent >= 100 ? 'Uploaded [100%]' : ('Uploading [' + percent + '%]');
        asset.ui.progressInner.style.width = percent + '%';
    }

    function uploadFilePlain(asset, file) {
        asset.isUploading = true;
        asset.failed = false;
        asset.ui.status.textContent = 'Uploading...';
        updateFileRepoUploadButton();
        updateSubmitButtons();

        var xhr = new XMLHttpRequest();
        asset.xhrs.push(xhr);

        var formData = new FormData();
        formData.append('file', file);

        xhr.open('POST', PLAIN_UPLOAD_ENDPOINT + '?post_id=' + encodeURIComponent(window.POST_ID || 0), true);
        xhr.setRequestHeader('X-Requested-With', 'XMLHttpRequest');

        xhr.upload.onprogress = function(event) {
            if (!event.lengthComputable) return;
            asset.confirmedBytes = event.loaded;
            updateAssetProgress(asset);
        };

        xhr.onload = function() {
            asset.xhrs = asset.xhrs.filter(function(x) { return x !== xhr; });
            if (xhr.status === 200) {
                var payload = null;
                try { payload = JSON.parse(xhr.responseText); } catch (e) {}
                if (payload && payload.ok) {
                    asset.fid = payload.file_id !== undefined ? payload.file_id : (payload.id !== undefined ? payload.id : asset.fid);
                    asset.url = payload.url;
                    asset.mime_type = payload.mime_type || asset.mime_type || '';
                    asset.confirmedBytes = file.size;
                    finalizeUploadSuccess(asset, payload);
                    if (isFileRepoMode) {
                        isFileUploadRunning = false;
                        processFileUploadQueue();
                        updateFileRepoUploadButton();
                    }
                    return;
                }
            }
            markUploadFailure(asset, 'Upload failed [' + xhr.status + ']');
            if (isFileRepoMode) {
                isFileUploadRunning = false;
                processFileUploadQueue();
                updateFileRepoUploadButton();
            }
        };

        xhr.onerror = xhr.ontimeout = function() {
            asset.xhrs = asset.xhrs.filter(function(x) { return x !== xhr; });
            markUploadFailure(asset, 'Upload failed [network]');
            if (isFileRepoMode) {
                isFileUploadRunning = false;
                processFileUploadQueue();
                updateFileRepoUploadButton();
            }
        };

        xhr.send(formData);
    }

    function encodeFormBody(values) {
        return Object.keys(values).map(function(k) {
            return encodeURIComponent(k) + '=' + encodeURIComponent(values[k]);
        }).join('&');
    }

    function hexToBytes(hex) {
        if (!hex || hex.length % 2 !== 0) return null;
        var out = new Uint8Array(hex.length / 2);
        for (var i = 0; i < out.length; i++) {
            var value = parseInt(hex.substr(i * 2, 2), 16);
            if (!isFinite(value)) return null;
            out[i] = value;
        }
        return out;
    }

    function bytesToHex(bytes) {
        var out = '';
        for (var i = 0; i < bytes.length; i++) {
            out += bytes[i].toString(16).padStart(2, '0');
        }
        return out;
    }

    function deriveStreamIv(seedBytes, chunkIndex) {
        var iv = new Uint8Array(seedBytes);
        iv[8] ^= (chunkIndex >> 24) & 0xff;
        iv[9] ^= (chunkIndex >> 16) & 0xff;
        iv[10] ^= (chunkIndex >> 8) & 0xff;
        iv[11] ^= chunkIndex & 0xff;
        return iv;
    }

    function firstEightBytesToBigInt(bytes) {
        var value = 0n;
        for (var i = 0; i < 8 && i < bytes.length; i++) {
            value = (value << 8n) + BigInt(bytes[i]);
        }
        return value;
    }

    function positiveMod(value, modulus) {
        if (!modulus || modulus <= 0n) return 0n;
        var out = value % modulus;
        return out < 0n ? out + modulus : out;
    }

    function uploadModulus(asset) {
        var n = Number(asset.modulusM || 0);
        if (!isFinite(n) || n <= 0) return 1n;
        return BigInt(Math.floor(n));
    }

    function ensureHtpGroup(asset, file, groupIndex) {
        if (!window.crypto || !crypto.subtle || typeof BigInt === 'undefined') {
            return Promise.resolve(null);
        }
        asset.htpGroups = asset.htpGroups || {};
        if (asset.htpGroups[groupIndex]) return asset.htpGroups[groupIndex];

        asset.htpGroups[groupIndex] = (async function() {
            var modulus = uploadModulus(asset);
            var groupStart = groupIndex * 6;
            var groupEnd = Math.min(groupStart + 6, asset.totalChunks);
            var scalars = [0n, 0n, 0n, 0n, 0n, 0n];
            var tags = ['', '', '', '', '', ''];
            for (var ci = groupStart; ci < groupEnd; ci++) {
                var start = ci * asset.chunkSize;
                var end = Math.min(start + asset.chunkSize, file.size);
                var data = await file.slice(start, end).arrayBuffer();
                var digest = new Uint8Array(await crypto.subtle.digest('SHA-512', data));
                tags[ci - groupStart] = bytesToHex(digest);
                scalars[ci - groupStart] = positiveMod(firstEightBytesToBigInt(digest), modulus);
            }

            var l1 = positiveMod(scalars[0] + scalars[1] + scalars[2], modulus);
            var l2 = positiveMod(scalars[2] + scalars[3] + scalars[4], modulus);
            var l3 = positiveMod(scalars[4] + scalars[5] + scalars[0], modulus);
            if (groupStart + 3 < asset.totalChunks) {
                scalars[3] = positiveMod(scalars[3] + (l1 - l2), modulus);
            }
            if (groupStart + 5 < asset.totalChunks) {
                scalars[5] = positiveMod(scalars[5] + (l1 - l3), modulus);
            }

            var result = {};
            for (var cj = groupStart; cj < groupEnd; cj++) {
                var slot = cj - groupStart;
                result[cj] = {
                    hashTag: tags[slot],
                    magicScalar: scalars[slot].toString(10)
                };
            }
            return result;
        })().catch(function() {
            return null;
        });
        return asset.htpGroups[groupIndex];
    }

    function getHtpHeaders(asset, file, chunkIndex) {
        return ensureHtpGroup(asset, file, Math.floor(chunkIndex / 6)).then(function(group) {
            return group && group[chunkIndex] ? group[chunkIndex] : null;
        });
    }

    function importUploadStreamKey(asset) {
        if (asset.streamCryptoKey) return Promise.resolve(asset.streamCryptoKey);
        if (!window.crypto || !crypto.subtle) return Promise.reject(new Error('crypto unavailable'));
        var keyBytes = hexToBytes(asset.streamKeyHex || '');
        if (!keyBytes || keyBytes.length !== 32) return Promise.reject(new Error('stream key unavailable'));
        return crypto.subtle.importKey('raw', keyBytes, { name: 'AES-GCM' }, false, ['encrypt']).then(function(key) {
            asset.streamCryptoKey = key;
            return key;
        });
    }

    function startTasfaUpload(asset, file) {
        asset.isUploading = true;
        asset.failed = false;
        asset.isCancelling = false;
        asset.xhrs = asset.xhrs || [];
        asset.confirmedBytes = 0;
        asset.ui.status.textContent = 'Preparing upload session';
        updateFileRepoUploadButton();
        updateSubmitButtons();

        if (asset.uploadId && asset.uploadToken) {
            resumeTasfaUpload(asset, file);
            return;
        }

        var chunkCount = Math.max(1, Math.ceil(file.size / asset.chunkSize));
        var body = 'filename=' + encodeURIComponent(file.name) +
            '&total_size=' + encodeURIComponent(String(file.size)) +
            '&chunk_count=' + encodeURIComponent(String(chunkCount)) +
            '&chunk_size=' + encodeURIComponent(String(asset.chunkSize)) +
            '&post_id=' + encodeURIComponent(window.POST_ID || 0) +
            '&session_id=' + encodeURIComponent(asset.client_uuid || '');

        function doInit(retries) {
            retries = retries || 0;
            var controller = new AbortController();
            var timeoutId = setTimeout(function() { controller.abort(); }, 30000);
            fetch(UPLOAD_INIT_ENDPOINT, {
                method: 'POST',
                headers: { 'Content-Type': 'application/x-www-form-urlencoded', 'Accept': 'application/json', 'X-Requested-With': 'XMLHttpRequest' },
                body: body,
                signal: controller.signal
            }).then(function(response) {
                clearTimeout(timeoutId);
                if (response.status === 429 && retries < 10) {
                    return response.json().then(function(payload) {
                        asset.ui.status.textContent = (payload && payload.error) || 'Wait a second...';
                        var delay = (payload.retry_after || 5) * 1000;
                        setTimeout(function() { doInit(retries + 1); }, delay);
                    }).catch(function() {
                        setTimeout(function() { doInit(retries + 1); }, 5000);
                    });
                }
                if (!response.ok) throw new Error('init:' + response.status);
                return response.json();
            }).then(function(payload) {
                if (!payload || payload.queued) return;
                if (payload && payload.already_done) {
                    asset.fid = payload.file_id;
                    asset.url = payload.url || ('/file/download/' + String(payload.file_id || ''));
                    finalizeUploadSuccess(asset, payload);
                    return;
                }
                if (!payload || payload.ok === false || !payload.upload_id || !payload.upload_token) {
                    throw new Error((payload && payload.error) || 'upload init failed');
                }
                asset.uploadId = payload.upload_id;
                asset.uploadToken = payload.upload_token;
                asset.streamKeyHex = payload.stream_key_hex || '';
                asset.streamIvSeedHex = payload.stream_iv_seed_hex || '';
                asset.modulusM = payload.modulus_M || 1;
                asset.chunkSize = Number(payload.chunk_size || asset.chunkSize || UPLOAD_CHUNK_SIZE);
                asset.totalChunks = Math.max(1, Math.ceil(file.size / asset.chunkSize));
                asset.maxParallel = Math.max(1, Math.min(Number(payload.max_parallel_chunks) || UPLOAD_DEFAULT_PARALLEL, asset.totalChunks));
                asset.inflightBytes = new Array(asset.totalChunks).fill(0);
                asset.retryCounts = new Array(asset.totalChunks).fill(0);
                asset.completedChunks = 0;
                asset.ui.status.textContent = 'Uploading...';
                runSimpleChunkUpload(asset, file);
            }).catch(function(error) {
                clearTimeout(timeoutId);
                if (error && error.message && error.message.indexOf('init:429') !== -1) return;
                if (error && error.name === 'AbortError') {
                    if (retries < 3) {
                        asset.ui.status.textContent = 'Retrying session...';
                        setTimeout(function() { doInit(retries + 1); }, 2000);
                        return;
                    }
                }
                var message = error && error.message ? error.message : 'Upload failed';
                markUploadFailure(asset, message);
            });
        }
        doInit();
    }

    function resumeTasfaUpload(asset, file) {
        asset.ui.status.textContent = 'Resuming upload...';
        var body = 'upload_id=' + encodeURIComponent(asset.uploadId) +
            '&upload_token=' + encodeURIComponent(asset.uploadToken);
        var controller = new AbortController();
        var timeoutId = setTimeout(function() { controller.abort(); }, 30000);
        fetch(UPLOAD_STATUS_ENDPOINT, {
            method: 'POST',
            headers: { 'Content-Type': 'application/x-www-form-urlencoded', 'Accept': 'application/json', 'X-Requested-With': 'XMLHttpRequest' },
            body: body,
            signal: controller.signal
        }).then(function(response) {
            clearTimeout(timeoutId);
            if (!response.ok) throw new Error('status:' + response.status);
            return response.json();
        }).then(function(payload) {
            if (!payload || payload.ok === false) throw new Error((payload && payload.error) || 'resume failed');
            var bitmap = payload.received_bitmap || '';
            asset.chunkSize = Number(payload.chunk_size || asset.chunkSize || UPLOAD_CHUNK_SIZE);
            asset.totalChunks = Math.max(1, Math.ceil(file.size / asset.chunkSize));
            asset.streamKeyHex = payload.stream_key_hex || asset.streamKeyHex || '';
            asset.streamIvSeedHex = payload.stream_iv_seed_hex || asset.streamIvSeedHex || '';
            asset.modulusM = payload.modulus_M || asset.modulusM || 1;
            asset.maxParallel = Math.max(1, Math.min(Number(payload.max_parallel_chunks) || UPLOAD_DEFAULT_PARALLEL, asset.totalChunks));
            asset.inflightBytes = new Array(asset.totalChunks).fill(0);
            asset.retryCounts = new Array(asset.totalChunks).fill(0);
            asset.completedChunks = 0;
            asset.confirmedBytes = 0;
            var pending = [];
            for (var i = 0; i < asset.totalChunks; i++) {
                if (i < bitmap.length && bitmap[i] === '1') {
                    asset.completedChunks += 1;
                    asset.confirmedBytes += Math.min(asset.chunkSize, file.size - (i * asset.chunkSize));
                } else {
                    pending.push(i);
                }
            }
            asset.pendingChunks = pending;
            asset.ui.status.textContent = 'Uploading...';
            runSimpleChunkUpload(asset, file);
        }).catch(function(error) {
            clearTimeout(timeoutId);
            var message = error && error.message ? error.message : 'Resume failed';
            markUploadFailure(asset, message);
        });
    }

    function runSimpleChunkUpload(asset, file) {
        var pending = asset.pendingChunks;
        if (!pending || !pending.length) {
            pending = [];
            for (var i = 0; i < asset.totalChunks; i++) pending.push(i);
        }
        asset.pendingChunks = null;
        var poolFailed = false;

        function postChunk(chunkIndex) {
            return getHtpHeaders(asset, file, chunkIndex).then(function(htp) {
            return new Promise(function(resolve, reject) {
                var start = chunkIndex * asset.chunkSize;
                var end = Math.min(start + asset.chunkSize, file.size);
                var blob = file.slice(start, end);
                var size = end - start;
                var xhr = new XMLHttpRequest();
                xhr._tasfaChunkIndex = chunkIndex;
                asset.xhrs.push(xhr);
                asset.inflightBytes[chunkIndex] = 0;

                xhr.open('POST', UPLOAD_ENDPOINT, true);
                xhr.setRequestHeader('Accept', 'application/json');
                xhr.setRequestHeader('X-Requested-With', 'XMLHttpRequest');
                xhr.setRequestHeader('X-TASFA-Upload-ID', asset.uploadId);
                xhr.setRequestHeader('X-TASFA-Upload-Token', asset.uploadToken);
                xhr.setRequestHeader('X-TASFA-Chunk-Index', String(chunkIndex));
                if (htp && htp.hashTag) xhr.setRequestHeader('X-TASFA-Hash-Tag', htp.hashTag);
                if (htp && htp.magicScalar) xhr.setRequestHeader('X-TASFA-Magic-Scalar', htp.magicScalar);

                xhr.timeout = 120000;

                xhr.upload.onprogress = function(event) {
                    if (!event.lengthComputable) return;
                    asset.inflightBytes[chunkIndex] = event.loaded;
                    updateAssetProgress(asset);
                };

                xhr.onload = function() {
                    asset.xhrs = asset.xhrs.filter(function(x) { return x !== xhr; });
                    asset.inflightBytes[chunkIndex] = 0;
                    if (xhr.status === 200 || xhr.status === 204) {
                        asset.confirmedBytes += size;
                        resolve({ ok: true, chunkIndex: chunkIndex });
                    } else if (xhr.status === 429) {
                        var delay = 3000;
                        var waitMessage = 'Wait a second...';
                        try {
                            var resp = JSON.parse(xhr.responseText);
                            if (resp.retry_after) delay = resp.retry_after * 1000;
                            if (resp.error) waitMessage = resp.error;
                        } catch(e) {}
                        asset.ui.status.textContent = waitMessage;
                        resolve({ retry: true, chunkIndex: chunkIndex, delay: delay });
                    } else {
                        reject(new Error('status:' + xhr.status));
                    }
                };

                xhr.onerror = xhr.ontimeout = function() {
                    asset.xhrs = asset.xhrs.filter(function(x) { return x !== xhr; });
                    asset.inflightBytes[chunkIndex] = 0;
                    reject(new Error('network'));
                };

                xhr.onabort = function() {
                    asset.xhrs = asset.xhrs.filter(function(x) { return x !== xhr; });
                    asset.inflightBytes[chunkIndex] = 0;
                    reject(new Error('abort'));
                };

                xhr.send(blob);
            });
            });
        }

        function postEncryptedChunk(chunkIndex) {
            return Promise.all([
                importUploadStreamKey(asset),
                getHtpHeaders(asset, file, chunkIndex)
            ]).then(function(values) {
                var key = values[0];
                var htp = values[1];
                var start = chunkIndex * asset.chunkSize;
                var end = Math.min(start + asset.chunkSize, file.size);
                var blob = file.slice(start, end);
                var size = end - start;
                var seed = hexToBytes(asset.streamIvSeedHex || '');
                if (!seed || seed.length !== 12) throw new Error('stream iv unavailable');
                var iv = deriveStreamIv(seed, chunkIndex);
                var aad = new TextEncoder().encode((asset.uploadId || '') + ':' + String(chunkIndex));
                return blob.arrayBuffer().then(function(plain) {
                    return crypto.subtle.encrypt({
                        name: 'AES-GCM',
                        iv: iv,
                        additionalData: aad,
                        tagLength: 128
                    }, key, plain);
                }).then(function(cipher) {
                    return new Promise(function(resolve, reject) {
                        var xhr = new XMLHttpRequest();
                        xhr._tasfaChunkIndex = chunkIndex;
                        asset.xhrs.push(xhr);
                        asset.inflightBytes[chunkIndex] = 0;
                        xhr.open('POST', UPLOAD_ENDPOINT, true);
                        xhr.setRequestHeader('Accept', 'application/json');
                        xhr.setRequestHeader('Content-Type', 'application/octet-stream');
                        xhr.setRequestHeader('X-Requested-With', 'XMLHttpRequest');
                        xhr.setRequestHeader('X-TASFA-Upload-ID', asset.uploadId);
                        xhr.setRequestHeader('X-TASFA-Upload-Token', asset.uploadToken);
                        xhr.setRequestHeader('X-TASFA-Chunk-Index', String(chunkIndex));
                        xhr.setRequestHeader('X-TASFA-Stream-Mode', 'aes-256-gcm');
                        if (htp && htp.hashTag) xhr.setRequestHeader('X-TASFA-Hash-Tag', htp.hashTag);
                        if (htp && htp.magicScalar) xhr.setRequestHeader('X-TASFA-Magic-Scalar', htp.magicScalar);
                        xhr.timeout = 120000;
                        xhr.onload = function() {
                            asset.xhrs = asset.xhrs.filter(function(x) { return x !== xhr; });
                            asset.inflightBytes[chunkIndex] = 0;
                            if (xhr.status === 200 || xhr.status === 204) {
                                asset.confirmedBytes += size;
                                resolve({ ok: true, chunkIndex: chunkIndex });
                            } else {
                                reject(new Error('fallback:' + xhr.status));
                            }
                        };
                        xhr.onerror = xhr.ontimeout = function() {
                            asset.xhrs = asset.xhrs.filter(function(x) { return x !== xhr; });
                            asset.inflightBytes[chunkIndex] = 0;
                            reject(new Error('fallback:network'));
                        };
                        xhr.onabort = function() {
                            asset.xhrs = asset.xhrs.filter(function(x) { return x !== xhr; });
                            asset.inflightBytes[chunkIndex] = 0;
                            reject(new Error('fallback:abort'));
                        };
                        xhr.send(cipher);
                    });
                });
            });
        }

        function postEncryptedChunkSerial(chunkIndex) {
            asset.fallbackChain = (asset.fallbackChain || Promise.resolve()).catch(function() {}).then(function() {
                asset.ui.status.textContent = 'Uploading fallback...';
                return postEncryptedChunk(chunkIndex);
            });
            return asset.fallbackChain;
        }

        function worker() {
            return new Promise(function(resolve) {
                function next() {
                    if (poolFailed || asset.isCancelling || pending.length === 0) {
                        resolve();
                        return;
                    }
                    if (asset.isBackgroundPaused) {
                        setTimeout(next, 1000);
                        return;
                    }
                    var chunkIndex = pending.pop();
                    postChunk(chunkIndex).then(function(result) {
                        if (result && result.retry) {
                            asset.retryCounts[chunkIndex] = (asset.retryCounts[chunkIndex] || 0) + 1;
                            if (asset.retryCounts[chunkIndex] < 10) {
                                setTimeout(function() {
                                    pending.push(chunkIndex);
                                    next();
                                }, result.delay || 3000);
                                return;
                            }
                            postEncryptedChunkSerial(chunkIndex).then(function() {
                                asset.completedChunks += 1;
                                updateAssetProgress(asset);
                                next();
                            }).catch(function() {
                                poolFailed = true;
                                resolve();
                            });
                            return;
                        }
                        asset.completedChunks += 1;
                        updateAssetProgress(asset);
                        next();
                    }).catch(function(err) {
                        asset.retryCounts[chunkIndex] = (asset.retryCounts[chunkIndex] || 0) + 1;
                        if (asset.retryCounts[chunkIndex] < 5) {
                            pending.push(chunkIndex);
                            next();
                        } else {
                            postEncryptedChunkSerial(chunkIndex).then(function() {
                                asset.completedChunks += 1;
                                updateAssetProgress(asset);
                                next();
                            }).catch(function() {
                                poolFailed = true;
                                resolve();
                            });
                        }
                    });
                }
                next();
            });
        }

        var workers = [];
        for (var i = 0; i < asset.maxParallel; i++) {
            workers.push(worker());
        }
        Promise.all(workers).then(function() {
            if (asset.isCancelling) return;
            verifyAllChunksBeforeComplete(asset, asset.file);
        });
    }

    function verifyAllChunksBeforeComplete(asset, file) {
        asset.serverVerifyRounds = (asset.serverVerifyRounds || 0) + 1;
        if (asset.serverVerifyRounds > 10) {
            markUploadFailure(asset, 'Upload failed [too many verify rounds]');
            return;
        }
        asset.ui.status.textContent = 'Verifying chunks on server (round ' + asset.serverVerifyRounds + ')...';
        var body = 'upload_id=' + encodeURIComponent(asset.uploadId) +
            '&upload_token=' + encodeURIComponent(asset.uploadToken);
        var controller = new AbortController();
        var timeoutId = setTimeout(function() { controller.abort(); }, 30000);
        fetch(UPLOAD_STATUS_ENDPOINT, {
            method: 'POST',
            headers: { 'Content-Type': 'application/x-www-form-urlencoded', 'Accept': 'application/json', 'X-Requested-With': 'XMLHttpRequest' },
            body: body,
            signal: controller.signal
        }).then(function(response) {
            clearTimeout(timeoutId);
            if (!response.ok) throw new Error('status:' + response.status);
            return response.json();
        }).then(function(payload) {
            if (!payload || payload.ok === false) throw new Error((payload && payload.error) || 'verify failed');
            var bitmap = payload.received_bitmap || '';
            var pending = [];
            asset.confirmedBytes = 0;
            asset.completedChunks = 0;
            for (var i = 0; i < asset.totalChunks; i++) {
                if (i < bitmap.length && bitmap[i] === '1') {
                    asset.completedChunks += 1;
                    asset.confirmedBytes += Math.min(asset.chunkSize, file.size - (i * asset.chunkSize));
                } else {
                    pending.push(i);
                }
            }
            if (pending.length > 0) {
                asset.ui.status.textContent = 'Filling ' + pending.length + ' missing chunk(s)...';
                asset.pendingChunks = pending;
                asset.retryCounts = new Array(asset.totalChunks).fill(0);
                runSimpleChunkUpload(asset, file);
            } else {
                updateAssetProgress(asset);
                completeTasfaUpload(asset);
            }
        }).catch(function(error) {
            clearTimeout(timeoutId);
            var msg = error && error.message ? error.message : 'verify failed';
            if (msg.indexOf('status:401') !== -1) {
                asset.ui.status.textContent = 'Auth check during verify, retrying...';
                setTimeout(function() { verifyAllChunksBeforeComplete(asset, file); }, 2000);
                return;
            }
            if (msg.indexOf('status:') === 0) {
                markUploadFailure(asset, 'Upload failed [' + msg.split(':')[1] + ']');
            } else {
                markUploadFailure(asset, 'Upload failed [' + msg + ']');
            }
        });
    }

    function completeTasfaUpload(asset, attempt) {
        attempt = attempt || 1;
        asset.ui.status.textContent = 'Finalizing upload on server (attempt ' + attempt + ')';
        var xhr = new XMLHttpRequest();
        asset.xhrs.push(xhr);
        xhr.open('POST', UPLOAD_COMPLETE_ENDPOINT, true);
        xhr.setRequestHeader('Content-Type', 'application/x-www-form-urlencoded');
        xhr.setRequestHeader('Accept', 'application/json');
        xhr.setRequestHeader('X-Requested-With', 'XMLHttpRequest');
        xhr.onload = function() {
            asset.xhrs = asset.xhrs.filter(function(x) { return x !== xhr; });
            if (xhr.status === 200) {
                var payload = null;
                try { payload = JSON.parse(xhr.responseText); } catch (e) {}
                if (payload && payload.ok && payload.url) {
                    finalizeUploadSuccess(asset, payload);
                    return;
                }
            }
            if (xhr.status === 401) {
                if (attempt < 5) {
                    var delay = Math.min(30000, Math.pow(2, attempt) * 1000);
                    asset.ui.status.textContent = 'Finalize auth error, retrying in ' + (delay / 1000) + 's...';
                    setTimeout(function() { completeTasfaUpload(asset, attempt + 1); }, delay);
                    return;
                }
            }
            if (xhr.status === 409) {
                var payload = null;
                try { payload = JSON.parse(xhr.responseText); } catch (e) {}
                if (payload && payload.received_bitmap) {
                    asset.htpRetryCount = (asset.htpRetryCount || 0) + 1;
                    if (asset.htpRetryCount > 5) {
                        markUploadFailure(asset, 'Upload failed [integrity retry limit]');
                        return;
                    }
                    asset.ui.status.textContent = 'Server integrity check failed, re-uploading affected chunks...';
                    var bitmap = payload.received_bitmap;
                    var pending = [];
                    asset.confirmedBytes = 0;
                    asset.completedChunks = 0;
                    for (var i = 0; i < asset.totalChunks; i++) {
                        if (i < bitmap.length && bitmap[i] === '1') {
                            asset.completedChunks += 1;
                            asset.confirmedBytes += Math.min(asset.chunkSize, asset.file.size - (i * asset.chunkSize));
                        } else {
                            pending.push(i);
                        }
                    }
                    asset.pendingChunks = pending;
                    runSimpleChunkUpload(asset, asset.file);
                    return;
                }
            }
            if (attempt < 5 && (xhr.status >= 500 || xhr.status === 0)) {
                var delay = Math.min(30000, Math.pow(2, attempt) * 1000);
                asset.ui.status.textContent = 'Finalize error ' + xhr.status + ', retrying in ' + (delay / 1000) + 's...';
                setTimeout(function() { completeTasfaUpload(asset, attempt + 1); }, delay);
                return;
            }
            markUploadFailure(asset, 'Upload failed [' + xhr.status + ']');
        };
        xhr.onerror = xhr.ontimeout = function() {
            asset.xhrs = asset.xhrs.filter(function(x) { return x !== xhr; });
            if (attempt < 5) {
                var delay = Math.min(30000, Math.pow(2, attempt) * 1000);
                asset.ui.status.textContent = 'Finalize network error, retrying in ' + (delay / 1000) + 's...';
                setTimeout(function() { completeTasfaUpload(asset, attempt + 1); }, delay);
                return;
            }
            markUploadFailure(asset, 'Upload failed [network]');
        };
        xhr.send(encodeFormBody({ upload_id: asset.uploadId, upload_token: asset.uploadToken }));
    }

    function startQueuedAssetUpload(asset) {
        if (!asset || asset.isExisting || asset.fid !== null || asset.isUploading || asset.failed) return;
        if (asset.uploadMethod === 'plain') {
            uploadFilePlain(asset, asset.file);
            return;
        }
        startTasfaUpload(asset, asset.file);
    }

    function flushQueuedFileRepoUploads() {
        if (!isFileRepoMode) return;
        fileRepoPendingAssets().forEach(enqueueFileUpload);
        processFileUploadQueue();
        updateFileRepoUploadButton();
    }

    function uploadFile(file) {
        if (!uploadPreview) return;
        var useTasfa = USE_TASFA && file.size > TASFA_MIN_SIZE_BYTES;
        var clientUuid = generateUUID();
        var blobUrl = URL.createObjectURL(file);
        var placeholderUrl = isEditorMode ? (blobUrl + '#' + clientUuid) : null;
        var mediaType = /^image\//.test(file.type || '') ? 'image' : /^video\//.test(file.type || '') ? 'video' : /^audio\//.test(file.type || '') ? 'audio' : 'file';
        var ui = createMediaCard(file.name, blobUrl, mediaType);
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
            streamKeyHex: '',
            streamIvSeedHex: '',
            streamCryptoKey: null,
            modulusM: 1,
            htpGroups: {},
            fallbackChain: Promise.resolve(),
            fileSize: file.size,
            chunkSize: UPLOAD_CHUNK_SIZE,
            confirmedBytes: 0,
            totalChunks: 0,
            maxParallel: UPLOAD_DEFAULT_PARALLEL,
            inflightBytes: [],
            retryCounts: [],
            completedChunks: 0,
            isUploading: false,
            failed: false,
            isCancelling: false,
            uploadMethod: useTasfa ? 'tasfa' : 'plain',
            ui: ui
        };
        AssetRegistry.push(asset);
        if (uploadPreview) uploadPreview.appendChild(ui.el);
        bindAssetControls(asset);
        updateFileRepoUploadButton();
        updateSubmitButtons();
        var maxUploadSize = Number(window.BLOG_MAX_UPLOAD_SIZE || 0);
        if (maxUploadSize > 0 && file.size > maxUploadSize) {
            asset.tooLarge = true;
            asset.failed = true;
            asset.ui.status.textContent = 'Upload too large';
            asset.xhrs = [];
            updateFileRepoUploadButton();
            updateSubmitButtons();
            updateMediaCardUI(asset);
            return;
        }
        if (isEditorMode) {
            if (useTasfa) { startTasfaUpload(asset, file); }
            else { uploadFilePlain(asset, file); }
        }
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
            asset.ui.status.textContent = 'Waiting for network';
            if (asset.xhrs && asset.xhrs.length) {
                asset.xhrs.slice().forEach(function(xhr) {
                    try { xhr.abort(); } catch (err) {}
                });
            }
        });
    });

    window.addEventListener('online', function() {
        AssetRegistry.forEach(function(asset) {
            if (!asset || asset.isExisting || asset.fid !== null || !asset.uploadId || asset.failed) return;
            if (!asset.isUploading && asset.uploadMethod === 'tasfa') {
                asset.failed = false;
                startTasfaUpload(asset, asset.file);
            }
        });
    });

    document.addEventListener('visibilitychange', function() {
        if (document.visibilityState === 'hidden') {
            AssetRegistry.forEach(function(asset) {
                if (!asset || asset.isExisting || asset.fid !== null || !asset.isUploading || asset.failed || asset.isCancelling) return;
                asset.isBackgroundPaused = true;
            });
        } else {
            AssetRegistry.forEach(function(asset) {
                if (!asset || asset.isExisting || asset.fid !== null || !asset.uploadId || asset.failed) return;
                asset.isBackgroundPaused = false;
                if (asset.uploadMethod === 'tasfa') {
                    asset.failed = false;
                    asset.isUploading = false;
                    startTasfaUpload(asset, asset.file);
                }
            });
        }
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

    document.addEventListener('submit', function(event) {
        var form = event.target;
        if (!form || !form.classList.contains('file-delete-form')) return;
        event.preventDefault();
        var btn = form.querySelector('button[type="submit"]');
        if (btn) btn.disabled = true;
        fetch(form.action || '/file/delete', {
            method: 'POST',
            headers: { 'Content-Type': 'application/x-www-form-urlencoded', 'X-Requested-With': 'XMLHttpRequest' },
            body: new URLSearchParams(new FormData(form))
        }).then(function(response) {
            if (!response.ok) throw new Error('status:' + response.status);
            var card = form.closest('article');
            if (card && card.parentNode) {
                card.remove();
                var list = document.getElementById('file-repo-list');
                if (list && !list.children.length) {
                    var emptyCard = document.createElement('div');
                    emptyCard.className = 'card';
                    emptyCard.style.cssText = 'text-align:center;padding:40px 20px;color:var(--muted);';
                    emptyCard.textContent = 'No files uploaded yet.';
                    list.parentNode.insertBefore(emptyCard, list.nextSibling);
                    list.remove();
                    var h3 = document.querySelector('h3');
                    if (h3) {
                        var match = h3.textContent.match(/Files\s*\(/);
                        if (match) h3.textContent = 'Files';
                    }
                }
            } else {
                window.location.reload();
            }
        }).catch(function(err) {
            console.error('Delete failed:', err);
            if (btn) btn.disabled = false;
            alert('Failed to delete file.');
        });
    });

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

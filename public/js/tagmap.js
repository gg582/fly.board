/**
 * @file tagmap.js
 * @brief Interactive Tag World Map for /archive.
 *
 * Loads /api/tag-graph, runs the CWIST v4 WASM layout/BFS kernel
 * (public/js/tagmap-kernel.js + public/js/tagmap.wasm) using zero-copy
 * TypedArray views (Module.cwistView.f64 / i32), and renders a zoomable,
 * pannable world map of tags with:
 *   - Continent regions (grouped by primary board)
 *   - Mouse wheel zoom (centered on cursor), click-drag pan, click/double-click
 *   - Mobile touch gestures (1-finger drag pan, 2-finger pinch zoom + pan, tap)
 *   - Semantic zoom LOD with collision-free label placement
 *   - Search box with autocomplete dropdown and camera fly-to animation
 *   - 1-hop / 2-hop neighborhood inspector panel with direct /tag/<name> links
 */
(function () {
    'use strict';

    var root = document.getElementById('tag-map-section');
    var canvas = document.getElementById('tag-map-canvas');
    if (!root || !canvas) return;

    var ctx = canvas.getContext('2d');
    var searchInput = document.getElementById('tag-map-search');
    var searchResults = document.getElementById('tag-map-search-results');
    var boardSelect = document.getElementById('tag-map-board');
    var btnZoomIn = document.getElementById('tag-map-zoom-in');
    var btnZoomOut = document.getElementById('tag-map-zoom-out');
    var btnReset = document.getElementById('tag-map-reset');
    var statusEl = document.getElementById('tag-map-status');
    var panelEl = document.getElementById('tag-map-panel');

    var wasmMod = null;
    var graph = { nodes: [], edges: [], boards: [] };
    var idToIdx = {};
    var adj = [];

    /* TypedArray views or fallback arrays populated after layout */
    var posX = null;
    var posY = null;
    var posR = null;
    var nodeRank = null;
    var nodeHop = null;
    var nodeCont = null;
    var contX = null;
    var contY = null;
    var contBoard = null;

    /* Camera state: screenX = (worldX - cam.x) * cam.z + viewW / 2 */
    var cam = { x: 0, y: 0, z: 1, tx: 0, ty: 0, tz: 1 };
    var viewW = 800;
    var viewH = 540;
    var dpr = 1;
    var animFrame = 0;

    var focusIdx = -1;
    var hoverIdx = -1;
    var searchQuery = '';
    var searchMatches = [];

    /* Distinct palette for continents (boards) in light and dark modes */
    var PALETTE_LIGHT = [
        '#4f46e5', '#0891b2', '#059669', '#d97706', '#dc2626',
        '#7c3aed', '#db2777', '#2563eb', '#65a30d', '#ea580c'
    ];
    var PALETTE_DARK = [
        '#818cf8', '#22d3ee', '#34d399', '#fbbf24', '#f87171',
        '#a78bfa', '#f472b6', '#60a5fa', '#a3e635', '#fb923c'
    ];

    function isDarkTheme() {
        try {
            var bg = getComputedStyle(document.documentElement).getPropertyValue('--bg').trim() ||
                     getComputedStyle(document.body).backgroundColor;
            if (bg) {
                if (bg.charAt(0) === '#') {
                    var hex = bg.slice(1);
                    if (hex.length === 3) hex = hex[0] + hex[0] + hex[1] + hex[1] + hex[2] + hex[2];
                    var r = parseInt(hex.slice(0, 2), 16);
                    var g = parseInt(hex.slice(2, 4), 16);
                    var b = parseInt(hex.slice(4, 6), 16);
                    if (!isNaN(r) && !isNaN(g) && !isNaN(b)) {
                        return (0.299 * r + 0.587 * g + 0.114 * b) < 128;
                    }
                }
                var m = bg.match(/rgba?\((\d+),\s*(\d+),\s*(\d+)/);
                if (m) {
                    return (0.299 * (+m[1]) + 0.587 * (+m[2]) + 0.114 * (+m[3])) < 128;
                }
            }
        } catch (e) {}
        var hl = document.getElementById('hl-theme');
        if (hl && hl.dataset && hl.dataset.active) {
            return hl.dataset.active !== 'light';
        }
        return document.cookie.indexOf('theme=dark') !== -1 ||
               document.cookie.indexOf('theme=ocean') !== -1 ||
               document.cookie.indexOf('theme=forest') !== -1;
    }

    function encodeTagPath(name) {
        return '/tag/' + encodeURIComponent(name);
    }

    function escapeHtml(s) {
        return String(s || '')
            .replace(/&/g, '&amp;')
            .replace(/</g, '&lt;')
            .replace(/>/g, '&gt;')
            .replace(/"/g, '&quot;')
            .replace(/'/g, '&#39;');
    }

    function loadWasmKernel() {
        if (wasmMod) return Promise.resolve(wasmMod);
        if (typeof WebAssembly === 'undefined') return Promise.resolve(null);
        return new Promise(function (resolve) {
            if (typeof window.createTagmapModule === 'function') {
                window.createTagmapModule({
                    locateFile: function (p) {
                        return p.endsWith('.wasm') ? '/assets/js/tagmap.wasm?v=2' : p;
                    }
                }).then(function (m) {
                    wasmMod = m;
                    resolve(m);
                }).catch(function () { resolve(null); });
                return;
            }
            var s = document.createElement('script');
            s.src = '/assets/js/tagmap-kernel.js?v=2';
            s.onload = function () {
                if (typeof window.createTagmapModule !== 'function') {
                    resolve(null);
                    return;
                }
                window.createTagmapModule({
                    locateFile: function (p) {
                        return p.endsWith('.wasm') ? '/assets/js/tagmap.wasm?v=2' : p;
                    }
                }).then(function (m) {
                    wasmMod = m;
                    resolve(m);
                }).catch(function () { resolve(null); });
            };
            s.onerror = function () { resolve(null); };
            document.head.appendChild(s);
        });
    }

    function runLayout() {
        var nodes = graph.nodes || [];
        var edges = graph.edges || [];
        var n = nodes.length;
        idToIdx = {};
        adj = new Array(n);
        for (var i = 0; i < n; i++) {
            idToIdx[nodes[i].id] = i;
            adj[i] = [];
        }

        var validEdges = [];
        for (var e = 0; e < edges.length; e++) {
            var ia = idToIdx[edges[e].a];
            var ib = idToIdx[edges[e].b];
            var w = edges[e].w || 1;
            if (ia !== undefined && ib !== undefined && ia !== ib) {
                validEdges.push({ a: ia, b: ib, w: w });
                adj[ia].push({ to: ib, w: w });
                adj[ib].push({ to: ia, w: w });
            }
        }
        for (var k = 0; k < n; k++) {
            adj[k].sort(function (x, y) {
                if (y.w !== x.w) return y.w - x.w;
                return (nodes[y.to].n || 0) - (nodes[x.to].n || 0);
            });
        }
        graph.resolvedEdges = validEdges;

        if (wasmMod && typeof wasmMod._fb_tagmap_layout === 'function') {
            wasmMod._fb_tagmap_init();
            var nPtr = wasmMod._malloc(Math.max(4, n * 3 * 4));
            var ePtr = wasmMod._malloc(Math.max(4, validEdges.length * 3 * 4));

            var nView = wasmMod.cwistView.i32(nPtr, n * 3);
            for (var ni = 0; ni < n; ni++) {
                nView[ni * 3 + 0] = nodes[ni].id | 0;
                nView[ni * 3 + 1] = nodes[ni].n | 0;
                nView[ni * 3 + 2] = (nodes[ni].board_id || 0) | 0;
            }
            var eView = wasmMod.cwistView.i32(ePtr, validEdges.length * 3);
            for (var ei = 0; ei < validEdges.length; ei++) {
                eView[ei * 3 + 0] = validEdges[ei].a | 0;
                eView[ei * 3 + 1] = validEdges[ei].b | 0;
                eView[ei * 3 + 2] = validEdges[ei].w | 0;
            }

            wasmMod._fb_tagmap_layout(nPtr, n, ePtr, validEdges.length, 130);
            wasmMod._free(nPtr);
            wasmMod._free(ePtr);

            posX = wasmMod.cwistView.f64(wasmMod._fb_tagmap_x_ptr(), wasmMod._fb_tagmap_x_len());
            posY = wasmMod.cwistView.f64(wasmMod._fb_tagmap_y_ptr(), wasmMod._fb_tagmap_y_len());
            posR = wasmMod.cwistView.f64(wasmMod._fb_tagmap_r_ptr(), wasmMod._fb_tagmap_r_len());
            nodeRank = wasmMod.cwistView.i32(wasmMod._fb_tagmap_rank_ptr(), wasmMod._fb_tagmap_rank_len());
            nodeHop = wasmMod.cwistView.i32(wasmMod._fb_tagmap_hop_ptr(), wasmMod._fb_tagmap_hop_len());
            nodeCont = wasmMod.cwistView.i32(wasmMod._fb_tagmap_continent_ptr(), wasmMod._fb_tagmap_continent_len());
            contX = wasmMod.cwistView.f64(wasmMod._fb_tagmap_cx_ptr(), wasmMod._fb_tagmap_cx_len());
            contY = wasmMod.cwistView.f64(wasmMod._fb_tagmap_cy_ptr(), wasmMod._fb_tagmap_cy_len());
            contBoard = wasmMod.cwistView.i32(wasmMod._fb_tagmap_cboard_ptr(), wasmMod._fb_tagmap_cboard_len());
        }

        fitWorldToScreen(false);
        setFocusNode(-1);
        updateStatusText();
        requestRender();
    }

    function updateStatusText() {
        if (!statusEl) return;
        var n = (graph.nodes || []).length;
        var e = (graph.resolvedEdges || []).length;
        statusEl.textContent = n + ' tags · ' + e + ' links (CWIST WASM Kernel)';
    }

    function fitWorldToScreen(animate) {
        var n = (graph.nodes || []).length;
        if (n === 0 || !posX) {
            cam.tx = 0; cam.ty = 0; cam.tz = 1;
            if (!animate) { cam.x = 0; cam.y = 0; cam.z = 1; }
            return;
        }
        var minX = Infinity, maxX = -Infinity, minY = Infinity, maxY = -Infinity;
        for (var i = 0; i < n; i++) {
            var r = posR[i] + 24;
            if (posX[i] - r < minX) minX = posX[i] - r;
            if (posX[i] + r > maxX) maxX = posX[i] + r;
            if (posY[i] - r < minY) minY = posY[i] - r;
            if (posY[i] + r > maxY) maxY = posY[i] + r;
        }
        var spanX = Math.max(120, maxX - minX);
        var spanY = Math.max(120, maxY - minY);
        var pad = 72;
        var zx = Math.max(200, viewW - pad * 2) / spanX;
        var zy = Math.max(200, viewH - pad * 2) / spanY;
        var z = Math.max(0.22, Math.min(2.2, Math.min(zx, zy)));

        cam.tx = (minX + maxX) * 0.5;
        cam.ty = (minY + maxY) * 0.5;
        cam.tz = z;
        if (!animate) {
            cam.x = cam.tx;
            cam.y = cam.ty;
            cam.z = cam.tz;
        }
        requestRender();
    }

    function flyToNode(idx, targetZoom) {
        if (idx < 0 || idx >= graph.nodes.length || !posX) return;
        cam.tx = posX[idx];
        cam.ty = posY[idx];
        if (targetZoom) {
            cam.tz = Math.max(cam.tz, targetZoom);
        } else if (cam.tz < 1.15) {
            cam.tz = 1.35;
        }
        requestRender();
    }

    function setFocusNode(idx) {
        focusIdx = idx;
        if (wasmMod && typeof wasmMod._fb_tagmap_bfs === 'function') {
            wasmMod._fb_tagmap_bfs(idx);
        }
        renderInspectorPanel();
        requestRender();
    }

    function renderInspectorPanel() {
        if (!panelEl) return;
        if (focusIdx < 0 || focusIdx >= graph.nodes.length) {
            panelEl.style.display = 'none';
            panelEl.innerHTML = '';
            return;
        }
        var node = graph.nodes[focusIdx];
        var hop1 = [];
        var hop2 = [];
        var n = graph.nodes.length;

        /* Collect 1-hop with shared post weights, and 2-hop tags */
        var n1Edges = adj[focusIdx] || [];
        var wMap = {};
        for (var i = 0; i < n1Edges.length; i++) {
            wMap[n1Edges[i].to] = n1Edges[i].w;
        }
        for (var j = 0; j < n; j++) {
            var h = nodeHop[j];
            if (h === 1) {
                hop1.push({ idx: j, name: graph.nodes[j].name, n: graph.nodes[j].n, w: wMap[j] || 1 });
            } else if (h === 2) {
                hop2.push({ idx: j, name: graph.nodes[j].name, n: graph.nodes[j].n });
            }
        }
        hop1.sort(function (a, b) { return (b.w - a.w) || (b.n - a.n); });
        hop2.sort(function (a, b) { return b.n - a.n; });

        var html = "<div style='display:flex;flex-wrap:wrap;justify-content:space-between;align-items:center;gap:10px;margin-bottom:10px'>" +
            "<div><strong style='font-size:17px'>#" + escapeHtml(node.name) + "</strong> " +
            "<span style='color:var(--muted);font-size:13px'>(" + (node.n || 0) + " post" + ((node.n === 1) ? "" : "s") +
            (node.board_name ? " &middot; " + escapeHtml(node.board_name) : "") + ")</span></div>" +
            "<div style='display:flex;gap:8px;align-items:center'>" +
            "<a class='btn' style='padding:5px 12px;font-size:13px;text-decoration:none' href='" + escapeHtml(encodeTagPath(node.name)) + "'>View Posts &rarr;</a>" +
            "<button type='button' id='tag-map-panel-close' class='btn btn-outline' style='padding:4px 10px;font-size:12px'>Clear</button>" +
            "</div></div>";

        if (hop1.length > 0) {
            html += "<div style='margin-top:6px;font-size:13px'><span style='color:var(--muted);font-weight:600'>1-hop co-occurring (" + hop1.length + "):</span> ";
            for (var k = 0; k < Math.min(hop1.length, 18); k++) {
                var h1 = hop1[k];
                html += "<button type='button' data-focus-idx='" + h1.idx + "' class='tag' style='margin:2px 4px 2px 0;cursor:pointer;border:1px solid var(--border);background:var(--card-bg);color:var(--fg);padding:2px 8px;border-radius:999px;font-size:12px'>" +
                    "#" + escapeHtml(h1.name) + " <span style='opacity:.65'>&times;" + h1.w + "</span></button>";
            }
            html += "</div>";
        } else {
            html += "<div style='margin-top:4px;font-size:13px;color:var(--muted)'>No co-occurring tags on the same post.</div>";
        }

        if (hop2.length > 0) {
            html += "<div style='margin-top:6px;font-size:13px'><span style='color:var(--muted);font-weight:600'>2-hop indirect (" + hop2.length + "):</span> ";
            for (var m = 0; m < Math.min(hop2.length, 16); m++) {
                var h2 = hop2[m];
                html += "<button type='button' data-focus-idx='" + h2.idx + "' class='tag' style='margin:2px 4px 2px 0;cursor:pointer;border:1px dashed var(--border);background:transparent;color:var(--muted);padding:2px 8px;border-radius:999px;font-size:12px'>" +
                    "#" + escapeHtml(h2.name) + "</button>";
            }
            if (hop2.length > 16) {
                html += "<span style='color:var(--muted);font-size:12px'>+" + (hop2.length - 16) + " more</span>";
            }
            html += "</div>";
        }

        panelEl.innerHTML = html;
        panelEl.style.display = 'block';

        var closeBtn = document.getElementById('tag-map-panel-close');
        if (closeBtn) {
            closeBtn.onclick = function () { setFocusNode(-1); };
        }
        var hopBtns = panelEl.querySelectorAll('[data-focus-idx]');
        for (var b = 0; b < hopBtns.length; b++) {
            hopBtns[b].onclick = function () {
                var target = parseInt(this.getAttribute('data-focus-idx'), 10);
                if (!isNaN(target)) {
                    setFocusNode(target);
                    flyToNode(target);
                }
            };
        }
    }

    function worldToScreenX(wx) { return (wx - cam.x) * cam.z + viewW * 0.5; }
    function worldToScreenY(wy) { return (wy - cam.y) * cam.z + viewH * 0.5; }
    function screenToWorldX(sx) { return (sx - viewW * 0.5) / cam.z + cam.x; }
    function screenToWorldY(sy) { return (sy - viewH * 0.5) / cam.z + cam.y; }

    function resizeCanvas() {
        var rect = canvas.parentElement.getBoundingClientRect();
        viewW = Math.max(280, Math.floor(rect.width));
        viewH = Math.max(360, Math.min(620, Math.floor(window.innerHeight * 0.62)));
        if (window.innerWidth <= 640) {
            viewH = Math.max(340, Math.min(480, Math.floor(window.innerHeight * 0.56)));
        }
        dpr = Math.min(window.devicePixelRatio || 1, 2);
        canvas.width = Math.floor(viewW * dpr);
        canvas.height = Math.floor(viewH * dpr);
        canvas.style.width = viewW + 'px';
        canvas.style.height = viewH + 'px';
        requestRender();
    }

    function requestRender() {
        if (!animFrame) {
            animFrame = requestAnimationFrame(renderFrame);
        }
    }

    function getBoardNameById(bid) {
        if (!bid) return '';
        var bs = graph.boards || [];
        for (var i = 0; i < bs.length; i++) {
            if (bs[i].id === bid) return bs[i].name || '';
        }
        return '';
    }

    function renderFrame() {
        animFrame = 0;
        var dark = isDarkTheme();
        var palette = dark ? PALETTE_DARK : PALETTE_LIGHT;

        /* Smooth camera interpolation */
        var dx = cam.tx - cam.x;
        var dy = cam.ty - cam.y;
        var dz = cam.tz - cam.z;
        var moving = (Math.abs(dx) > 0.08 || Math.abs(dy) > 0.08 || Math.abs(dz) > 0.0008);
        if (moving) {
            cam.x += dx * 0.22;
            cam.y += dy * 0.22;
            cam.z += dz * 0.22;
        } else {
            cam.x = cam.tx;
            cam.y = cam.ty;
            cam.z = cam.tz;
        }

        ctx.save();
        ctx.scale(dpr, dpr);

        /* Map ocean/cartographic background */
        ctx.fillStyle = dark ? '#0d131f' : '#f4f6fb';
        ctx.fillRect(0, 0, viewW, viewH);

        /* Cartographic latitude/longitude grid lines */
        var gridStep = 120;
        if (cam.z < 0.45) gridStep = 240;
        else if (cam.z > 1.8) gridStep = 60;

        var startWX = Math.floor(screenToWorldX(0) / gridStep) * gridStep;
        var endWX = screenToWorldX(viewW);
        var startWY = Math.floor(screenToWorldY(0) / gridStep) * gridStep;
        var endWY = screenToWorldY(viewH);

        ctx.strokeStyle = dark ? 'rgba(148,163,184,0.06)' : 'rgba(71,85,105,0.06)';
        ctx.lineWidth = 1;
        ctx.beginPath();
        for (var gx = startWX; gx <= endWX; gx += gridStep) {
            var sx = Math.round(worldToScreenX(gx)) + 0.5;
            ctx.moveTo(sx, 0);
            ctx.lineTo(sx, viewH);
        }
        for (var gy = startWY; gy <= endWY; gy += gridStep) {
            var sy = Math.round(worldToScreenY(gy)) + 0.5;
            ctx.moveTo(0, sy);
            ctx.lineTo(viewW, sy);
        }
        ctx.stroke();

        var nodes = graph.nodes || [];
        var n = nodes.length;
        if (n === 0 || !posX) {
            ctx.fillStyle = dark ? '#94a3b8' : '#64748b';
            ctx.font = '14px sans-serif';
            ctx.textAlign = 'center';
            ctx.fillText('No tags found.', viewW * 0.5, viewH * 0.5);
            ctx.restore();
            return;
        }

        /* 1. Draw continent territories (board clusters) */
        if (contX && contBoard && contBoard.length > 0) {
            for (var c = 0; c < contBoard.length; c++) {
                var bname = getBoardNameById(contBoard[c]);
                if (!bname) continue;
                var csx = worldToScreenX(contX[c]);
                var csy = worldToScreenY(contY[c]);
                var col = palette[c % palette.length];
                var haloR = Math.max(45, Math.min(260, 115 * cam.z));

                var grad = ctx.createRadialGradient(csx, csy, haloR * 0.1, csx, csy, haloR);
                grad.addColorStop(0, dark ? col + '18' : col + '14');
                grad.addColorStop(1, 'transparent');
                ctx.fillStyle = grad;
                ctx.beginPath();
                ctx.arc(csx, csy, haloR, 0, Math.PI * 2);
                ctx.fill();

                /* Continent watermark title */
                if (cam.z < 1.65) {
                    ctx.save();
                    ctx.font = '700 ' + Math.round(Math.max(11, Math.min(18, 14 * Math.sqrt(cam.z)))) + 'px sans-serif';
                    ctx.fillStyle = dark ? 'rgba(226,232,240,0.22)' : 'rgba(30,41,59,0.20)';
                    ctx.textAlign = 'center';
                    ctx.fillText(bname.toUpperCase(), csx, csy - haloR * 0.55);
                    ctx.restore();
                }
            }
        }

        /* 2. Draw co-occurrence edges */
        var edges = graph.resolvedEdges || [];
        var hasFocus = (focusIdx >= 0 && focusIdx < n);
        var hasSearch = searchQuery.length > 0;

        for (var e = 0; e < edges.length; e++) {
            var ed = edges[e];
            var a = ed.a, b = ed.b, w = ed.w;

            var ha = hasFocus ? nodeHop[a] : -1;
            var hb = hasFocus ? nodeHop[b] : -1;

            var isHop1Edge = hasFocus && ((ha === 0 && hb === 1) || (hb === 0 && ha === 1));
            var isHop2Edge = hasFocus && !isHop1Edge &&
                             ((ha === 1 && (hb === 1 || hb === 2)) || (hb === 1 && (ha === 1 || ha === 2)));

            if (!hasFocus) {
                /* Declutter edges at low zoom: only draw stronger edges or edges between top-ranked nodes */
                if (cam.z < 0.7 && w < 2 && nodeRank[a] > 28 && nodeRank[b] > 28) continue;
                if (cam.z < 1.1 && w < 2 && nodeRank[a] > 65 && nodeRank[b] > 65) continue;
            } else if (!isHop1Edge && !isHop2Edge) {
                /* Dim unrelated edges heavily when a tag is focused */
                if (w < 2) continue;
            }

            var x1 = worldToScreenX(posX[a]);
            var y1 = worldToScreenY(posY[a]);
            var x2 = worldToScreenX(posX[b]);
            var y2 = worldToScreenY(posY[b]);

            if ((x1 < -50 && x2 < -50) || (x1 > viewW + 50 && x2 > viewW + 50) ||
                (y1 < -50 && y2 < -50) || (y1 > viewH + 50 && y2 > viewH + 50)) {
                continue;
            }

            ctx.beginPath();
            ctx.moveTo(x1, y1);
            ctx.lineTo(x2, y2);

            if (isHop1Edge) {
                ctx.strokeStyle = dark ? 'rgba(129,140,248,0.88)' : 'rgba(79,70,229,0.82)';
                ctx.lineWidth = Math.min(4.5, 1.6 + Math.log(1 + w));
            } else if (isHop2Edge) {
                ctx.strokeStyle = dark ? 'rgba(56,189,248,0.48)' : 'rgba(8,145,178,0.45)';
                ctx.lineWidth = 1.2;
            } else if (hasFocus || hasSearch) {
                ctx.strokeStyle = dark ? 'rgba(148,163,184,0.07)' : 'rgba(100,116,139,0.08)';
                ctx.lineWidth = 0.8;
            } else {
                var alpha = Math.min(0.45, 0.12 + 0.08 * w);
                ctx.strokeStyle = dark ? 'rgba(148,163,184,' + alpha + ')' : 'rgba(100,116,139,' + alpha + ')';
                ctx.lineWidth = Math.min(3.0, 0.8 + 0.45 * Math.log(1 + w));
            }
            ctx.stroke();
        }

        /* 3. Draw nodes and collect label candidates in rank order */
        var matchSet = {};
        if (hasSearch) {
            for (var m = 0; m < searchMatches.length; m++) {
                matchSet[searchMatches[m]] = true;
            }
        }

        /* Build index list sorted by rank (0 = most important) */
        var byRank = new Array(n);
        for (var i = 0; i < n; i++) byRank[i] = i;
        byRank.sort(function (u, v) { return nodeRank[u] - nodeRank[v]; });

        /* Draw nodes from least important to most important so hubs sit on top */
        for (var ri = n - 1; ri >= 0; ri--) {
            var idx = byRank[ri];
            var sxNode = worldToScreenX(posX[idx]);
            var syNode = worldToScreenY(posY[idx]);
            var sr = Math.max(3.2, Math.min(28.0, posR[idx] * Math.pow(cam.z, 0.65)));

            if (sxNode + sr < -20 || sxNode - sr > viewW + 20 ||
                syNode + sr < -20 || syNode - sr > viewH + 20) {
                continue;
            }

            var cIdx = nodeCont ? nodeCont[idx] : 0;
            var baseCol = palette[cIdx % palette.length];
            var hop = hasFocus ? nodeHop[idx] : -1;
            var isMatch = hasSearch && !!matchSet[idx];
            var isHover = (idx === hoverIdx);

            var dimmed = (hasFocus && hop < 0) || (hasSearch && !isMatch);
            ctx.save();
            if (dimmed && !isHover) {
                ctx.globalAlpha = 0.20;
            }

            /* Outer glow for focus, 1-hop, or search match */
            if (hop === 0 || isMatch || isHover) {
                ctx.beginPath();
                ctx.arc(sxNode, syNode, sr + 5, 0, Math.PI * 2);
                ctx.fillStyle = (hop === 0) ? (dark ? 'rgba(250,204,21,0.32)' : 'rgba(234,179,8,0.32)')
                                            : (dark ? 'rgba(129,140,248,0.32)' : 'rgba(79,70,229,0.28)');
                ctx.fill();
            }

            ctx.beginPath();
            ctx.arc(sxNode, syNode, sr, 0, Math.PI * 2);
            ctx.fillStyle = baseCol;
            ctx.fill();

            ctx.lineWidth = (hop === 0 || isHover || isMatch) ? 2.4 : (hop === 1 ? 1.8 : 1.1);
            ctx.strokeStyle = (hop === 0)
                ? (dark ? '#fde047' : '#ca8a04')
                : (dark ? '#0f172a' : '#ffffff');
            ctx.stroke();
            ctx.restore();
        }

        /* 4. Semantic-zoom label placement with bounding-box collision avoidance */
        var placedBoxes = [];
        var maxLabels = cam.z < 0.55 ? 16 : (cam.z < 0.9 ? 34 : (cam.z < 1.45 ? 75 : 240));

        /* Priority pass: always try to place focus, hover, search matches, and 1-hop first */
        var labelOrder = [];
        var seenOrder = {};
        function pushCandidate(k) {
            if (k >= 0 && k < n && !seenOrder[k]) {
                seenOrder[k] = true;
                labelOrder.push(k);
            }
        }
        if (focusIdx >= 0) pushCandidate(focusIdx);
        if (hoverIdx >= 0) pushCandidate(hoverIdx);
        if (hasSearch) {
            for (var sm = 0; sm < searchMatches.length; sm++) pushCandidate(searchMatches[sm]);
        }
        if (hasFocus) {
            for (var h1i = 0; h1i < n; h1i++) {
                if (nodeHop[h1i] === 1) pushCandidate(h1i);
            }
            for (var h2i = 0; h2i < n; h2i++) {
                if (nodeHop[h2i] === 2) pushCandidate(h2i);
            }
        }
        for (var rj = 0; rj < n; rj++) {
            pushCandidate(byRank[rj]);
        }

        ctx.textAlign = 'center';
        ctx.textBaseline = 'middle';

        var placedCount = 0;
        for (var li = 0; li < labelOrder.length; li++) {
            var kIdx = labelOrder[li];
            var hopK = hasFocus ? nodeHop[kIdx] : -1;
            var matchK = hasSearch && !!matchSet[kIdx];
            var priority = (kIdx === focusIdx || kIdx === hoverIdx || matchK || hopK === 1 || hopK === 2);

            if (!priority) {
                if (hasFocus) continue; /* When inspecting a node, only label its 1-hop/2-hop neighborhood */
                if (placedCount >= maxLabels) continue;
            }

            var sxL = worldToScreenX(posX[kIdx]);
            var syL = worldToScreenY(posY[kIdx]);
            var srL = Math.max(3.2, Math.min(28.0, posR[kIdx] * Math.pow(cam.z, 0.65)));

            if (sxL < 10 || sxL > viewW - 10 || syL < 10 || syL > viewH - 10) continue;

            var isHub = nodeRank[kIdx] < 12 || hopK === 0;
            var fontSize = isHub ? 13 : (hopK === 1 || matchK ? 12 : 11);
            ctx.font = (isHub || hopK === 0 || matchK ? '600 ' : '500 ') + fontSize + 'px sans-serif';

            var text = '#' + nodes[kIdx].name;
            var tw = ctx.measureText(text).width + 10;
            var th = fontSize + 6;
            var bx = sxL - tw * 0.5;
            var by = syL + srL + 3;

            var collides = false;
            if (kIdx !== focusIdx && kIdx !== hoverIdx) {
                for (var bi = 0; bi < placedBoxes.length; bi++) {
                    var pb = placedBoxes[bi];
                    if (bx < pb.x + pb.w && bx + tw > pb.x && by < pb.y + pb.h && by + th > pb.y) {
                        collides = true;
                        break;
                    }
                }
            }
            if (collides) continue;

            placedBoxes.push({ x: bx, y: by, w: tw, h: th });
            placedCount++;

            ctx.fillStyle = dark ? 'rgba(15,23,42,0.82)' : 'rgba(255,255,255,0.86)';
            ctx.beginPath();
            if (ctx.roundRect) {
                ctx.roundRect(bx, by, tw, th, 4);
            } else {
                ctx.rect(bx, by, tw, th);
            }
            ctx.fill();

            if (hopK === 0 || matchK || kIdx === hoverIdx) {
                ctx.strokeStyle = dark ? '#818cf8' : '#4f46e5';
                ctx.lineWidth = 1.2;
                ctx.stroke();
            }

            ctx.fillStyle = (hopK === 2)
                ? (dark ? '#94a3b8' : '#64748b')
                : (dark ? '#f1f5f9' : '#0f172a');
            ctx.fillText(text, sxL, by + th * 0.5);
        }

        ctx.restore();

        if (moving) {
            requestRender();
        }
    }

    function hitTestNode(sx, sy) {
        if (wasmMod && typeof wasmMod._fb_tagmap_hit_test === 'function') {
            return wasmMod._fb_tagmap_hit_test(sx, sy, cam.x, cam.y, cam.z, viewW, viewH);
        }
        return -1;
    }

    function zoomAtScreenPoint(factor, sx, sy) {
        var wx = screenToWorldX(sx);
        var wy = screenToWorldY(sy);
        var nextZ = Math.max(0.18, Math.min(5.0, cam.z * factor));
        cam.z = nextZ;
        cam.tz = nextZ;
        cam.x = wx - (sx - viewW * 0.5) / nextZ;
        cam.y = wy - (sy - viewH * 0.5) / nextZ;
        cam.tx = cam.x;
        cam.ty = cam.y;
        requestRender();
    }

    /* ---- Mouse and Touch Gestures ---- */
    var isDragging = false;
    var dragMoved = false;
    var dragStartX = 0, dragStartY = 0;
    var camStartX = 0, camStartY = 0;
    var lastTapTime = 0;
    var lastTapIdx = -1;

    canvas.addEventListener('mousedown', function (e) {
        if (e.button !== 0) return;
        isDragging = true;
        dragMoved = false;
        dragStartX = e.clientX;
        dragStartY = e.clientY;
        camStartX = cam.x;
        camStartY = cam.y;
        canvas.style.cursor = 'grabbing';
    });

    window.addEventListener('mousemove', function (e) {
        var rect = canvas.getBoundingClientRect();
        var sx = e.clientX - rect.left;
        var sy = e.clientY - rect.top;

        if (isDragging) {
            var dx = e.clientX - dragStartX;
            var dy = e.clientY - dragStartY;
            if (Math.abs(dx) > 4 || Math.abs(dy) > 4) dragMoved = true;
            cam.x = camStartX - dx / cam.z;
            cam.y = camStartY - dy / cam.z;
            cam.tx = cam.x;
            cam.ty = cam.y;
            requestRender();
            return;
        }

        if (sx >= 0 && sx <= viewW && sy >= 0 && sy <= viewH) {
            var hit = hitTestNode(sx, sy);
            if (hit !== hoverIdx) {
                hoverIdx = hit;
                canvas.style.cursor = hit >= 0 ? 'pointer' : 'grab';
                requestRender();
            }
        } else if (hoverIdx !== -1) {
            hoverIdx = -1;
            requestRender();
        }
    });

    window.addEventListener('mouseup', function (e) {
        if (!isDragging) return;
        isDragging = false;
        canvas.style.cursor = hoverIdx >= 0 ? 'pointer' : 'grab';
        if (!dragMoved) {
            var rect = canvas.getBoundingClientRect();
            var sx = e.clientX - rect.left;
            var sy = e.clientY - rect.top;
            var hit = hitTestNode(sx, sy);
            var now = Date.now();
            if (hit >= 0 && hit === lastTapIdx && (now - lastTapTime) < 360) {
                window.location.href = encodeTagPath(graph.nodes[hit].name);
                return;
            }
            lastTapTime = now;
            lastTapIdx = hit;
            if (hit >= 0) {
                setFocusNode(hit);
                flyToNode(hit);
            } else {
                setFocusNode(-1);
            }
        }
    });

    canvas.addEventListener('wheel', function (e) {
        e.preventDefault();
        var rect = canvas.getBoundingClientRect();
        var sx = e.clientX - rect.left;
        var sy = e.clientY - rect.top;
        var factor = e.deltaY < 0 ? 1.16 : (1 / 1.16);
        zoomAtScreenPoint(factor, sx, sy);
    }, { passive: false });

    /* Touch support: 1-finger pan/tap, 2-finger pinch-zoom + pan */
    var touchMode = 0; /* 0=none, 1=pan, 2=pinch */
    var touchMoved = false;
    var tStartX = 0, tStartY = 0;
    var pinchStartDist = 0;
    var pinchStartZ = 1;
    var pinchWorldX = 0, pinchWorldY = 0;

    function touchDist(t0, t1) {
        var dx = t0.clientX - t1.clientX;
        var dy = t0.clientY - t1.clientY;
        return Math.sqrt(dx * dx + dy * dy);
    }

    canvas.addEventListener('touchstart', function (e) {
        if (e.touches.length === 1) {
            touchMode = 1;
            touchMoved = false;
            tStartX = e.touches[0].clientX;
            tStartY = e.touches[0].clientY;
            camStartX = cam.x;
            camStartY = cam.y;
        } else if (e.touches.length >= 2) {
            touchMode = 2;
            touchMoved = true;
            var rect = canvas.getBoundingClientRect();
            var mx = (e.touches[0].clientX + e.touches[1].clientX) * 0.5 - rect.left;
            var my = (e.touches[0].clientY + e.touches[1].clientY) * 0.5 - rect.top;
            pinchStartDist = Math.max(10, touchDist(e.touches[0], e.touches[1]));
            pinchStartZ = cam.z;
            pinchWorldX = screenToWorldX(mx);
            pinchWorldY = screenToWorldY(my);
        }
    }, { passive: false });

    canvas.addEventListener('touchmove', function (e) {
        e.preventDefault();
        var rect = canvas.getBoundingClientRect();
        if (touchMode === 1 && e.touches.length === 1) {
            var dx = e.touches[0].clientX - tStartX;
            var dy = e.touches[0].clientY - tStartY;
            if (Math.abs(dx) > 6 || Math.abs(dy) > 6) touchMoved = true;
            cam.x = camStartX - dx / cam.z;
            cam.y = camStartY - dy / cam.z;
            cam.tx = cam.x;
            cam.ty = cam.y;
            requestRender();
        } else if (e.touches.length >= 2) {
            touchMode = 2;
            var dist = Math.max(10, touchDist(e.touches[0], e.touches[1]));
            var mx = (e.touches[0].clientX + e.touches[1].clientX) * 0.5 - rect.left;
            var my = (e.touches[0].clientY + e.touches[1].clientY) * 0.5 - rect.top;
            var nextZ = Math.max(0.18, Math.min(5.0, pinchStartZ * (dist / pinchStartDist)));
            cam.z = nextZ;
            cam.tz = nextZ;
            cam.x = pinchWorldX - (mx - viewW * 0.5) / nextZ;
            cam.y = pinchWorldY - (my - viewH * 0.5) / nextZ;
            cam.tx = cam.x;
            cam.ty = cam.y;
            requestRender();
        }
    }, { passive: false });

    canvas.addEventListener('touchend', function (e) {
        if (touchMode === 1 && !touchMoved && e.changedTouches.length > 0) {
            var rect = canvas.getBoundingClientRect();
            var sx = e.changedTouches[0].clientX - rect.left;
            var sy = e.changedTouches[0].clientY - rect.top;
            var hit = hitTestNode(sx, sy);
            var now = Date.now();
            if (hit >= 0 && hit === lastTapIdx && (now - lastTapTime) < 380) {
                window.location.href = encodeTagPath(graph.nodes[hit].name);
                touchMode = 0;
                return;
            }
            lastTapTime = now;
            lastTapIdx = hit;
            if (hit >= 0) {
                setFocusNode(hit);
                flyToNode(hit);
            } else {
                setFocusNode(-1);
            }
        }
        if (e.touches.length === 0) touchMode = 0;
    });

    /* ---- Search & Controls ---- */
    function updateSearch(q) {
        searchQuery = String(q || '').trim().toLowerCase();
        if (searchQuery.charAt(0) === '#') searchQuery = searchQuery.slice(1).trim();
        searchMatches = [];
        var nodes = graph.nodes || [];
        if (searchQuery.length > 0) {
            for (var i = 0; i < nodes.length; i++) {
                var nm = (nodes[i].name || '').toLowerCase();
                if (nm.indexOf(searchQuery) !== -1) {
                    searchMatches.push(i);
                }
            }
            searchMatches.sort(function (a, b) {
                var na = (nodes[a].name || '').toLowerCase();
                var nb = (nodes[b].name || '').toLowerCase();
                var exactA = (na === searchQuery) ? 1 : 0;
                var exactB = (nb === searchQuery) ? 1 : 0;
                if (exactA !== exactB) return exactB - exactA;
                var prefA = (na.indexOf(searchQuery) === 0) ? 1 : 0;
                var prefB = (nb.indexOf(searchQuery) === 0) ? 1 : 0;
                if (prefA !== prefB) return prefB - prefA;
                return (nodes[b].n || 0) - (nodes[a].n || 0);
            });
        }
        renderSearchDropdown();
        requestRender();
    }

    function renderSearchDropdown() {
        if (!searchResults) return;
        if (!searchQuery || searchMatches.length === 0) {
            searchResults.style.display = 'none';
            searchResults.innerHTML = '';
            return;
        }
        var html = '';
        var limit = Math.min(searchMatches.length, 8);
        for (var i = 0; i < limit; i++) {
            var idx = searchMatches[i];
            var nd = graph.nodes[idx];
            html += "<div data-search-idx='" + idx + "' style='padding:7px 12px;cursor:pointer;display:flex;justify-content:space-between;align-items:center;border-bottom:1px solid var(--border);font-size:13px'>" +
                "<span><strong>#" + escapeHtml(nd.name) + "</strong>" +
                (nd.board_name ? " <span style='color:var(--muted);font-size:11px'>&middot; " + escapeHtml(nd.board_name) + "</span>" : "") +
                "</span><span style='color:var(--muted);font-size:12px'>" + (nd.n || 0) + "</span></div>";
        }
        searchResults.innerHTML = html;
        searchResults.style.display = 'block';

        var items = searchResults.querySelectorAll('[data-search-idx]');
        for (var j = 0; j < items.length; j++) {
            items[j].onclick = function () {
                var target = parseInt(this.getAttribute('data-search-idx'), 10);
                if (!isNaN(target)) {
                    searchResults.style.display = 'none';
                    setFocusNode(target);
                    flyToNode(target, 1.55);
                }
            };
        }
    }

    if (searchInput) {
        searchInput.addEventListener('input', function () {
            updateSearch(this.value);
        });
        searchInput.addEventListener('keydown', function (e) {
            if (e.key === 'Enter') {
                e.preventDefault();
                if (searchMatches.length > 0) {
                    var best = searchMatches[0];
                    if (searchResults) searchResults.style.display = 'none';
                    setFocusNode(best);
                    flyToNode(best, 1.55);
                }
            } else if (e.key === 'Escape') {
                this.value = '';
                updateSearch('');
                setFocusNode(-1);
            }
        });
    }

    document.addEventListener('click', function (e) {
        if (searchResults && searchInput && !searchResults.contains(e.target) && e.target !== searchInput) {
            searchResults.style.display = 'none';
        }
    });

    if (btnZoomIn) {
        btnZoomIn.onclick = function () { zoomAtScreenPoint(1.28, viewW * 0.5, viewH * 0.5); };
    }
    if (btnZoomOut) {
        btnZoomOut.onclick = function () { zoomAtScreenPoint(1 / 1.28, viewW * 0.5, viewH * 0.5); };
    }
    if (btnReset) {
        btnReset.onclick = function () {
            if (searchInput) searchInput.value = '';
            updateSearch('');
            setFocusNode(-1);
            fitWorldToScreen(true);
        };
    }

    function populateBoardsDropdown(boards) {
        if (!boardSelect || boardSelect.options.length > 1) return;
        for (var i = 0; i < boards.length; i++) {
            var b = boards[i];
            var opt = document.createElement('option');
            opt.value = String(b.id);
            opt.textContent = b.name + ' (' + b.tag_count + ' tags)';
            boardSelect.appendChild(opt);
        }
    }

    function fetchGraph(boardId) {
        var url = '/api/tag-graph' + (boardId > 0 ? '?board=' + encodeURIComponent(boardId) : '');
        if (statusEl) statusEl.textContent = 'Loading tag map...';
        fetch(url, { credentials: 'same-origin' })
            .then(function (r) { return r.json(); })
            .then(function (data) {
                graph = data || { nodes: [], edges: [], boards: [] };
                populateBoardsDropdown(graph.boards || []);
                runLayout();
            })
            .catch(function () {
                if (statusEl) statusEl.textContent = 'Failed to load tag graph.';
            });
    }

    if (boardSelect) {
        boardSelect.addEventListener('change', function () {
            var bid = parseInt(this.value || '0', 10) || 0;
            fetchGraph(bid);
        });
    }

    window.addEventListener('resize', resizeCanvas);
    resizeCanvas();

    var dynTheme = document.getElementById('dyn-theme');
    if (dynTheme && typeof MutationObserver !== 'undefined') {
        new MutationObserver(function () { requestRender(); })
            .observe(dynTheme, { childList: true, characterData: true, subtree: true });
    }

    loadWasmKernel().then(function () {
        fetchGraph(0);
    });
})();

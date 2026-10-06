/* Tag chips for the post editor.
 *
 * Enhances <input name="tags" data-tag-input> (a comma-separated list, which
 * is what the server reads and what works without JavaScript) into chips:
 * Enter, comma or Tab adds the typed tag, Backspace on an empty field removes
 * the last one, and existing tags from /api/tags are suggested as you type.
 * Normalization mirrors the server (src/db/tag.c), so a chip shows the tag as
 * it will be stored. */
(function () {
    'use strict';

    var MAX_TAGS = 10;
    var MAX_BYTES = 47;
    var MAX_SUGGESTIONS = 8;

    function utf8Length(s) {
        return new TextEncoder().encode(s).length;
    }

    function normalize(raw) {
        var s = String(raw || '').replace(/^[#\s]+/, '');
        s = s.replace(/[\/?#%&<>"'\\`\u0000-\u001f]/g, '');
        s = s.replace(/\s+/g, ' ').trim();
        s = s.replace(/[A-Z]/g, function (c) { return c.toLowerCase(); });
        while (s && utf8Length(s) > MAX_BYTES) s = Array.from(s).slice(0, -1).join('');
        return s.trim();
    }

    function enhance(source) {
        var tags = [];
        var known = null;
        var active = -1;

        var box = document.createElement('div');
        box.className = 'tag-editor';
        var list = document.createElement('ul');
        list.className = 'tag-editor-chips';
        list.setAttribute('aria-label', 'Tags');
        var field = document.createElement('input');
        field.type = 'text';
        field.className = 'tag-editor-field';
        field.id = source.id;
        field.setAttribute('autocomplete', 'off');
        field.setAttribute('role', 'combobox');
        field.setAttribute('aria-autocomplete', 'list');
        field.setAttribute('aria-expanded', 'false');
        field.placeholder = 'Add a tag';
        var menu = document.createElement('ul');
        menu.className = 'tag-editor-menu';
        menu.id = source.id + '-suggestions';
        menu.setAttribute('role', 'listbox');
        menu.hidden = true;
        field.setAttribute('aria-controls', menu.id);
        var status = document.createElement('p');
        status.className = 'tag-editor-hint';
        status.setAttribute('aria-live', 'polite');

        /* The original input keeps the name and carries the value. */
        source.removeAttribute('id');
        source.type = 'hidden';
        source.parentNode.insertBefore(box, source);
        box.appendChild(list);
        box.appendChild(field);
        box.appendChild(menu);
        box.parentNode.insertBefore(status, box.nextSibling);
        box.appendChild(source);

        function sync() {
            source.value = tags.join(', ');
            field.placeholder = tags.length ? '' : 'Add a tag';
            status.textContent = tags.length >= MAX_TAGS
                ? 'Up to ' + MAX_TAGS + ' tags.'
                : 'Enter or comma adds a tag; Backspace removes the last one.';
        }

        function render() {
            list.textContent = '';
            tags.forEach(function (tag, i) {
                var li = document.createElement('li');
                li.className = 'tag tag-editor-chip';
                var label = document.createElement('span');
                label.textContent = '#' + tag;
                var remove = document.createElement('button');
                remove.type = 'button';
                remove.className = 'tag-editor-remove';
                remove.setAttribute('aria-label', 'Remove tag ' + tag);
                remove.textContent = '\u00d7';
                remove.addEventListener('click', function () {
                    tags.splice(i, 1);
                    render();
                    field.focus();
                });
                li.appendChild(label);
                li.appendChild(remove);
                list.appendChild(li);
            });
            sync();
        }

        function add(raw) {
            var added = false;
            String(raw || '').split(',').forEach(function (part) {
                var tag = normalize(part);
                if (!tag || tags.indexOf(tag) !== -1 || tags.length >= MAX_TAGS) return;
                tags.push(tag);
                added = true;
            });
            if (added) render();
            return added;
        }

        function closeMenu() {
            menu.hidden = true;
            menu.textContent = '';
            active = -1;
            field.setAttribute('aria-expanded', 'false');
            field.removeAttribute('aria-activedescendant');
        }

        function highlight(i) {
            var items = menu.querySelectorAll('[role=option]');
            if (!items.length) return;
            active = (i + items.length) % items.length;
            items.forEach(function (el, j) { el.setAttribute('aria-selected', j === active ? 'true' : 'false'); });
            field.setAttribute('aria-activedescendant', items[active].id);
        }

        function openMenu() {
            if (!known || tags.length >= MAX_TAGS) { closeMenu(); return; }
            var q = normalize(field.value);
            var matches = known.filter(function (t) {
                return tags.indexOf(t.name) === -1 && (!q || t.name.indexOf(q) !== -1);
            });
            /* Prefix matches first, then by use. */
            matches.sort(function (a, b) {
                var pa = a.name.indexOf(q) === 0 ? 0 : 1, pb = b.name.indexOf(q) === 0 ? 0 : 1;
                return pa - pb || b.n - a.n || (a.name < b.name ? -1 : 1);
            });
            matches = matches.slice(0, MAX_SUGGESTIONS);
            menu.textContent = '';
            active = -1;
            if (!matches.length) { closeMenu(); return; }
            matches.forEach(function (t, i) {
                var li = document.createElement('li');
                li.id = menu.id + '-' + i;
                li.setAttribute('role', 'option');
                li.setAttribute('aria-selected', 'false');
                li.className = 'tag-editor-option';
                li.setAttribute('data-tag', t.name);
                var name = document.createElement('span');
                name.textContent = '#' + t.name;
                var count = document.createElement('span');
                count.className = 'tag-editor-count';
                count.textContent = t.n;
                li.appendChild(name);
                li.appendChild(count);
                /* mousedown, not click: keep focus in the field. */
                li.addEventListener('mousedown', function (e) {
                    e.preventDefault();
                    add(t.name);
                    field.value = '';
                    openMenu();
                });
                menu.appendChild(li);
            });
            menu.hidden = false;
            field.setAttribute('aria-expanded', 'true');
        }

        function loadKnown() {
            if (known) return;
            known = [];
            fetch('/api/tags', { credentials: 'same-origin' })
                .then(function (r) { return r.ok ? r.json() : []; })
                .then(function (rows) {
                    known = Array.isArray(rows) ? rows.filter(function (t) { return t && t.name; }) : [];
                    if (document.activeElement === field) openMenu();
                })
                .catch(function () {});
        }

        field.addEventListener('focus', function () { loadKnown(); openMenu(); });
        field.addEventListener('blur', function () {
            /* A half-typed tag still counts when the form is submitted. */
            if (field.value.trim()) { add(field.value); field.value = ''; }
            closeMenu();
        });
        field.addEventListener('input', function () {
            if (field.value.indexOf(',') !== -1) {
                add(field.value);
                field.value = '';
            }
            openMenu();
        });
        field.addEventListener('keydown', function (e) {
            var options = menu.hidden ? [] : menu.querySelectorAll('[role=option]');
            if (e.key === 'ArrowDown' && options.length) { e.preventDefault(); highlight(active + 1); return; }
            if (e.key === 'ArrowUp' && options.length) { e.preventDefault(); highlight(active - 1); return; }
            if (e.key === 'Escape') { closeMenu(); return; }
            if (e.key === 'Enter' || (e.key === 'Tab' && field.value.trim()) || e.key === ',') {
                if (e.isComposing) return; /* Hangul/IME composition in progress */
                var pick = active >= 0 && options[active] ? options[active].getAttribute('data-tag') : '';
                if (pick || field.value.trim()) {
                    e.preventDefault();
                    add(pick || field.value);
                    field.value = '';
                    openMenu();
                } else if (e.key === 'Enter') {
                    e.preventDefault(); /* never submit the post from the tag field */
                }
                return;
            }
            if (e.key === 'Backspace' && !field.value && tags.length) {
                tags.pop();
                render();
                openMenu();
            }
        });
        field.addEventListener('paste', function (e) {
            var text = (e.clipboardData || window.clipboardData).getData('text');
            if (text && /[,\n]/.test(text)) {
                e.preventDefault();
                add(text.replace(/\n/g, ','));
                openMenu();
            }
        });
        box.addEventListener('click', function (e) {
            if (e.target === box || e.target === list) field.focus();
        });

        add(source.value);
        render();
    }

    function init() {
        document.querySelectorAll('input[data-tag-input]').forEach(enhance);
    }
    if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', init);
    else init();
})();

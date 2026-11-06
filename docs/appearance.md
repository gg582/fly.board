# Appearance and live preview editor

Administrators can open **Dashboard > Appearance** to select the site structure, light and dark themes, and typography independently. Selecting a control immediately redraws the embedded blog preview; visitors see no change until **Save appearance** succeeds.

## Layout and navigation

The layout controls are independent of the color theme and font configuration:

- **Desktop navigation:** a visible top menu or a hamburger drawer. Mobile uses the drawer in either mode.
- **Site character:** blog/series presentation with editorial hierarchy, or web community presentation with compact, scan-friendly headings.
- **Post list:** feed with excerpts, separated cards, or compact rows without excerpts.
- **Density:** compact, comfortable (the existing spacing), or spacious.
- **Content width:** narrow, standard (the existing responsive maximum), or wide.
- **Menu panel:** square (the existing appearance), round, or bubble. Width ranges from 240 to 520 px.
- **Panel position:** open the hamburger preview and drag its grip. X/Y offsets range from -600 to 600 px and can also be edited numerically. Position and geometry apply to the desktop drawer; mobile retains the full-width responsive menu.
- **Panel color:** follow the active light/dark theme by default, or override it with an independent RGB color and 0-100% alpha channel. The saved CSS value uses eight-digit `#RRGGBBAA` notation.

The preview can switch between a representative post list and an article, light and dark selections, and desktop and mobile viewport widths. In hamburger mode the preview drawer can be opened, closed, and dragged. Defaults preserve the previous responsive navigation, square theme-colored panel, and editorial feed appearance.

Settings are parsed from `layout.settings`. Unknown values are ignored on startup and rejected by the editor. The server adds explicit navigation, content style, density, list style, width, and panel style classes to the body and emits layout overrides separately from the replaceable color-theme CSS.

## Custom CWIST themes

Upload a UTF-8 JSON document of at most 512 KiB with the same shape as `/themes.json`:

```json
{
  "name": "my-theme",
  "mode": "dark",
  "vars": {
    "--bg": "#101218",
    "--fg": "#f8fafc",
    "--panel": "#181b24",
    "--accent": "#a5b4fc"
  },
  "rules": [
    {
      "sel": "body",
      "decls": {
        "background": "var(--bg)",
        "color": "var(--fg)"
      }
    }
  ]
}
```

`mode` is optional and defaults to the light color family for wallpaper and syntax highlighting. The uploaded name is normalized to `custom`. Variables and every rule's `sel` and `decls` are validated. CSS braces, markup delimiters, non-string declarations, and at-rule selectors other than the existing `@keyframes` form are rejected. The theme is stored in `data/custom-theme.json` and becomes available as **Custom** in both theme groups.

A custom theme may replace all generated rules. Font declarations from the appearance editor are emitted separately after the custom theme so typography remains independently configurable.

## Font sources

Fonts and themes are saved independently. The editor supports:

- TTF, OTF, WOFF, and WOFF2 uploads up to 12 MiB. The administrator supplies the family name; that family is prepended to the six configured stacks.
- A CSS webfont URL through `font_import_url`.
- A JavaScript webfont URL through `font_script_url`, or a JavaScript upload up to 256 KiB. Uploaded scripts are served from `/assets/js/appearance-webfont.js`.

Webfont JavaScript executes with page privileges. Install only trusted code. External URLs also need to be permitted by the site's Content Security Policy; uploaded files use the same origin and need no CSP exception.

The body, heading, UI, code, blockquote, and display stacks can include separate Korean, Latin, and Cyrillic fallbacks. Fine typography controls cover body text, page headings H1-H6, the top bar, buttons, board titles, hero title and subtitle, post title, and Markdown H1-H3. Weights range from 100 to 900. Spacing accepts `em`, `rem`, `px`, or `normal`.

Settings are written atomically to `layout.settings`, `fonts.settings`, `blog.settings`, and `data/custom-theme.json`. Uploaded binary files are stored under `public/fonts` and `public/js`. Existing files and settings remain unchanged when form validation fails.

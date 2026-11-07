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

## Built-in themes

Light, Dark, Ocean, Forest, Sepia, Paper, Glacier, Rose, Midnight, Plum, and Ember are built in. Paper, Glacier, and Rose use the light wallpaper and syntax-highlighting family; Midnight, Plum, and Ember use the dark family. Radio cards show background, accent, and foreground swatches. Preview JSON always contains the actual base palettes, independent of the currently saved selection.

A successful save advances a shared appearance version. Every serving process reloads the selected light/dark modes and uploaded custom theme before its next response, and the HTML version invalidates both browser HTTP and localStorage theme caches. Visitors therefore do not need to wait for the former five-minute cache expiry or restart the server.

## Transparent SVG images

**Invert transparent SVG images in dark mode** is enabled by default. The browser draws each content SVG to a small canvas and checks alpha values along its outer edge. Images with a transparent background receive a hue-preserving inversion in dark-family themes; opaque images and every image in light-family themes keep their original colors. Theme switches update the filter without reloading the page. Uploaded SVGs rendered through `/file/preview/<id>` are also detected from their original filename, including when the preview itself is a WebP image with preserved transparency.

The setting is stored as `invert_transparent_svg=true|false` in `layout.settings`. Existing files without this key retain the enabled default. SVGs that cannot be drawn without cross-origin canvas restrictions are left unchanged. Hero backgrounds and logos with an explicit light/dark variant keep their existing inversion behavior.

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

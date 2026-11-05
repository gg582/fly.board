# Appearance and live preview editor

Administrators can open **Dashboard > Appearance** to select the light and dark themes independently. Selecting a radio button immediately redraws the embedded blog preview; visitors see no change until **Save appearance** succeeds.

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

Settings are written atomically to `fonts.settings`, `blog.settings`, and `data/custom-theme.json`. Uploaded binary files are stored under `public/fonts` and `public/js`. Existing files and settings remain unchanged when form validation fails.

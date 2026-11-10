#include "render.h"
#include "theme.h"
#include "config/config.h"
#include <cwist/core/sstring/sstring.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void append_input(cwist_sstring *b, const char *name, const char *value, const char *label) {
    cwist_sstring_append(b, "<label class='appearance-field'><span>");
    cwist_sstring_append(b, label);
    cwist_sstring_append(b, "</span><input name='");
    cwist_sstring_append(b, name);
    cwist_sstring_append(b, "' value='");
    cwist_sstring_append_escaped(b, value);
    cwist_sstring_append(b, "' data-appearance-setting></label>");
}

static bool transparent_format_selected(const char *format) {
    const char *formats = g_layout_settings.transparent_image_formats;
    if (strcmp(formats, "all") == 0) return true;
    size_t wanted = strlen(format);
    const char *p = formats;
    while (*p) {
        const char *end = strchr(p, ',');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (len == wanted && strncmp(p, format, wanted) == 0) return true;
        if (!end) break;
        p = end + 1;
    }
    return false;
}

static void append_transparent_format(cwist_sstring *b, const char *format, const char *label, bool all) {
    cwist_sstring_append(b, "<label class='check-row'><input type='checkbox' name='transparent_format_");
    cwist_sstring_append(b, all ? "all" : format);
    cwist_sstring_append(b, "' value='");
    cwist_sstring_append(b, all ? "all" : format);
    cwist_sstring_append(b, all ? "' data-transparent-format-all" : "' data-transparent-format");
    cwist_sstring_append(b, " data-appearance-setting");
    if (transparent_format_selected(format)) cwist_sstring_append(b, " checked");
    cwist_sstring_append(b, "> ");
    cwist_sstring_append(b, label);
    cwist_sstring_append(b, "</label>");
}

static void append_layout_choice(cwist_sstring *b, const char *group, const char *value,
                                 const char *current, const char *label, const char *detail) {
    cwist_sstring_append(b, "<label class='appearance-theme-choice appearance-layout-choice'><input type='radio' name='");
    cwist_sstring_append(b, group);
    cwist_sstring_append(b, "' value='");
    cwist_sstring_append(b, value);
    cwist_sstring_append(b, "' data-appearance-setting");
    if (strcmp(value, current) == 0) cwist_sstring_append(b, " checked");
    cwist_sstring_append(b, "><span><strong>");
    cwist_sstring_append(b, label);
    cwist_sstring_append(b, "</strong><small>");
    cwist_sstring_append(b, detail);
    cwist_sstring_append(b, "</small></span></label>");
}

static void append_theme_choice(cwist_sstring *b, const char *group, const char *value,
                                const char *current, const char *label) {
    cwist_sstring_append(b, "<label class='appearance-theme-choice'><input type='radio' name='");
    cwist_sstring_append(b, group);
    cwist_sstring_append(b, "' value='");
    cwist_sstring_append(b, value);
    cwist_sstring_append(b, "' data-theme-radio");
    if (strcmp(value, "custom") == 0) cwist_sstring_append(b, " data-custom-choice");
    if (strcmp(value, "custom") == 0 && !theme_custom_available()) cwist_sstring_append(b, " disabled");
    if (strcmp(value, current) == 0) cwist_sstring_append(b, " checked");
    cwist_sstring_append(b, "><span class='appearance-theme-name'>");
    cwist_sstring_append(b, label);
    cwist_sstring_append(b, "</span>");
    if (strcmp(value, "custom") != 0) {
        theme_color_t *palette = theme_builtin_by_name(value);
        cwist_sstring_append(b, "<span class='appearance-theme-swatches' aria-hidden='true'><i style='background:");
        cwist_sstring_append(b, palette->bg);
        cwist_sstring_append(b, "'></i><i style='background:");
        cwist_sstring_append(b, palette->accent);
        cwist_sstring_append(b, "'></i><i style='background:");
        cwist_sstring_append(b, palette->fg);
        cwist_sstring_append(b, "'></i></span>");
    }
    cwist_sstring_append(b, "</label>");
}

static void append_theme_group(cwist_sstring *b, const char *group, const char *current) {
    append_theme_choice(b, group, "light", current, "Light");
    append_theme_choice(b, group, "dark", current, "Dark");
    append_theme_choice(b, group, "ocean", current, "Ocean");
    append_theme_choice(b, group, "forest", current, "Forest");
    append_theme_choice(b, group, "sepia", current, "Sepia");
    append_theme_choice(b, group, "paper", current, "Paper");
    append_theme_choice(b, group, "glacier", current, "Glacier");
    append_theme_choice(b, group, "rose", current, "Rose");
    append_theme_choice(b, group, "midnight", current, "Midnight");
    append_theme_choice(b, group, "plum", current, "Plum");
    append_theme_choice(b, group, "ember", current, "Ember");
    append_theme_choice(b, group, "custom", current, "Custom");
}

static void append_code_theme_select(cwist_sstring *b, const char *name, const char *label, const char *current) {
    cwist_sstring_append(b, "<label class='appearance-field'><span>");
    cwist_sstring_append(b, label);
    cwist_sstring_append(b, "</span><select name='");
    cwist_sstring_append(b, name);
    cwist_sstring_append(b, "' data-appearance-setting data-code-theme>");
    for (int dark = 0; dark < 2; dark++) {
        cwist_sstring_append(b, dark ? "<optgroup label='Dark backgrounds'>" : "<optgroup label='Light backgrounds'>");
        for (size_t i = 0; i < g_code_theme_count; i++) {
            const code_theme_t *t = &g_code_themes[i];
            if ((int)t->dark != dark) continue;
            char href[160];
            code_theme_href(t->id, href, sizeof(href));
            cwist_sstring_append(b, "<option value='");
            cwist_sstring_append(b, t->id);
            cwist_sstring_append(b, "' data-href='");
            cwist_sstring_append_escaped(b, href);
            cwist_sstring_append(b, "'");
            if (strcmp(t->id, current) == 0) cwist_sstring_append(b, " selected");
            cwist_sstring_append(b, ">");
            cwist_sstring_append_escaped(b, t->label);
            cwist_sstring_append(b, "</option>");
        }
        cwist_sstring_append(b, "</optgroup>");
    }
    cwist_sstring_append(b, "</select></label>");
}

cwist_sstring *render_admin_appearance(bool dark, const char *profile_pic, bool is_mobile, const char *msg) {
    cwist_sstring *b = cwist_sstring_create();
    cwist_sstring_assign(b, "<div class='hero'><h1>Appearance</h1><p>Choose the site structure, navigation, list presentation, theme, and typography in a live preview.</p></div>");
    if (msg && strcmp(msg, "saved") == 0) cwist_sstring_append(b, "<div class='alert'>Appearance settings saved.</div>");
    else if (msg && strcmp(msg, "upload-expired") == 0) cwist_sstring_append(b, "<div class='alert'>Settings were not saved. An uploaded file expired or did not finish uploading; choose it again and save.</div>");
    else if (msg && msg[0]) cwist_sstring_append(b, "<div class='alert'>Settings were not saved. Check the uploaded files and field values.</div>");
    cwist_sstring_append(b, "<style>.appearance-layout{display:grid;grid-template-columns:minmax(300px,440px) minmax(0,1fr);gap:20px;align-items:start}.appearance-controls{max-height:calc(100vh - 110px);overflow:auto;padding-right:8px}.appearance-preview{position:sticky;top:16px}.appearance-frame-wrap{display:flex;justify-content:center}.appearance-preview iframe{display:block;width:100%;height:calc(100vh - 170px);min-height:560px;border:1px solid var(--border);border-radius:18px;background:var(--panel);transition:width .2s}.appearance-preview iframe.appearance-preview-mobile{width:390px;max-width:100%}.appearance-theme-grid,.appearance-option-grid{display:grid;grid-template-columns:repeat(3,1fr);gap:8px}.appearance-option-grid{grid-template-columns:repeat(2,minmax(0,1fr))}.appearance-theme-choice{display:flex;gap:6px;align-items:center;justify-content:space-between;padding:8px;border:1px solid var(--border);border-radius:10px}.appearance-theme-swatches{display:flex;gap:3px}.appearance-theme-swatches i{display:block;width:11px;height:11px;border:1px solid var(--border);border-radius:50%}.appearance-layout-choice{align-items:flex-start}.appearance-layout-choice span{display:grid;gap:3px}.appearance-layout-choice small{color:var(--muted);font-size:11px;line-height:1.35}.appearance-field{display:grid;gap:5px;margin:10px 0}.appearance-field input,.appearance-field select{width:100%;padding:8px;background:var(--panel);color:var(--fg);border:1px solid var(--border);border-radius:8px}.appearance-tune{display:grid;grid-template-columns:1fr 110px 90px;gap:8px;align-items:end}.appearance-actions{display:flex;gap:10px;align-items:center;margin:16px 0}.appearance-preview-tools{flex-wrap:wrap}.appearance-preview-tools select{width:auto;min-width:0;padding:6px}.appearance-note{font-size:13px;color:var(--muted)}.appearance-font-role{border:1px solid var(--border);border-radius:10px;margin:10px 0;padding:6px 12px}.appearance-font-role legend{font-weight:600;padding:0 4px}@media(max-width:1000px){.appearance-layout{grid-template-columns:1fr}.appearance-controls{max-height:none}.appearance-preview{position:static}.appearance-preview iframe{height:640px;min-height:0}}</style>");
    char site_roundness[16];
    snprintf(site_roundness, sizeof(site_roundness), "%.2f", g_config.roundness);
    cwist_sstring_append(b, "<form action='/admin/appearance' method='post' enctype='multipart/form-data' id='appearance-form' data-site-roundness='");
    cwist_sstring_append(b, site_roundness);
    cwist_sstring_append(b, "'><div class='appearance-layout'><div class='appearance-controls'>");
    cwist_sstring_append(b, "<section class='card'><h2>Layout and navigation</h2><p class='appearance-note'>These controls change the actual page structure and reading rhythm, not just its colors. Mobile always keeps the drawer menu.</p><h3>Desktop navigation</h3><div class='appearance-option-grid'>");
    append_layout_choice(b, "navigation", "topbar", g_layout_settings.navigation, "Top menu", "Visible links across the header");
    append_layout_choice(b, "navigation", "hamburger", g_layout_settings.navigation, "Hamburger", "A drawer on desktop and mobile");
    cwist_sstring_append(b, "</div><h3>Menu panel</h3><p class='appearance-note'>The hamburger and its panel share a side. In the preview, drag the panel's inner edge to change its width.</p><div class='appearance-option-grid'>");
    append_layout_choice(b, "panel_side", "left", g_layout_settings.panel_side, "Left", "Hamburger and panel on the left");
    append_layout_choice(b, "panel_side", "right", g_layout_settings.panel_side, "Right", "Hamburger and panel on the right");
    cwist_sstring_append(b, "</div><div class='appearance-option-grid'>");
    append_layout_choice(b, "panel_style", "square", g_layout_settings.panel_style, "Square panel", "Current square corners");
    append_layout_choice(b, "panel_style", "round", g_layout_settings.panel_style, "Round panel", "Soft rounded corners");
    append_layout_choice(b, "panel_style", "bubble", g_layout_settings.panel_style, "Bubble panel", "Floating pill-like glass and shadow");
    cwist_sstring_append(b, "</div>");
    char panel_width[16];
    snprintf(panel_width, sizeof(panel_width), "%d", g_layout_settings.panel_width);
    cwist_sstring_append(b, "<input type='hidden' name='panel_width' value='");
    cwist_sstring_append(b, panel_width);
    cwist_sstring_append(b, "' data-appearance-setting data-panel-width><p class='appearance-note'>Panel width: <output id='appearance-panel-width-value'>");
    cwist_sstring_append(b, panel_width);
    cwist_sstring_append(b, "</output> px (240&ndash;520)</p>");

    /* Corner radius: follow the style preset, the site roundness, or a value. */
    const char *radius = g_layout_settings.panel_radius;
    bool custom_radius = strcmp(radius, "auto") != 0 && strcmp(radius, "site") != 0;
    char radius_px[8];
    snprintf(radius_px, sizeof(radius_px), "%d", custom_radius ? atoi(radius) : 18);
    cwist_sstring_append(b, "<h3>Panel roundness</h3><div class='appearance-option-grid'>");
    append_layout_choice(b, "panel_radius", "auto", custom_radius ? "custom" : radius, "Match style", "Square, round, or bubble corners");
    append_layout_choice(b, "panel_radius", "site", custom_radius ? "custom" : radius, "Site roundness", "Same roundness as the rest of the site");
    append_layout_choice(b, "panel_radius", "custom", custom_radius ? "custom" : radius, "Custom", "Pick the corner radius below");
    cwist_sstring_append(b, "</div><label class='appearance-field'><span>Corner radius: <output id='appearance-panel-radius-value'>");
    cwist_sstring_append(b, radius_px);
    cwist_sstring_append(b, "</output> px</span><input type='range' name='panel_radius_px' min='0' max='48' step='1' value='");
    cwist_sstring_append(b, radius_px);
    cwist_sstring_append(b, "' data-appearance-setting data-panel-radius");
    if (!custom_radius) cwist_sstring_append(b, " disabled");
    cwist_sstring_append(b, "></label>");
    cwist_sstring_append(b, "<input type='hidden' name='panel_color' id='appearance-panel-color' value='");
    cwist_sstring_append_escaped(b, g_layout_settings.panel_color);
    cwist_sstring_append(b, "' data-appearance-setting><label class='check-row'><input type='checkbox' id='appearance-panel-color-enabled'");
    if (g_layout_settings.panel_color[0]) cwist_sstring_append(b, " checked");
    cwist_sstring_append(b, "> Override panel color independently of light/dark theme</label><div class='appearance-color-controls'><input type='color' id='appearance-panel-color-picker' value='");
    char panel_rgb[8] = "#ffffff";
    if (strlen(g_layout_settings.panel_color) == 9) memcpy(panel_rgb, g_layout_settings.panel_color, 7);
    cwist_sstring_append(b, panel_rgb);
    cwist_sstring_append(b, "' aria-label='Panel color'><label>Alpha <input type='range' id='appearance-panel-alpha' min='0' max='100' value='");
    char alpha[8] = "85";
    if (strlen(g_layout_settings.panel_color) == 9) snprintf(alpha, sizeof(alpha), "%d", (int)strtol(g_layout_settings.panel_color + 7, NULL, 16) * 100 / 255);
    cwist_sstring_append(b, alpha);
    cwist_sstring_append(b, "'></label></div><h3>Site character</h3><div class='appearance-option-grid'>");
    append_layout_choice(b, "content_style", "blog", g_layout_settings.content_style, "Blog / series", "Editorial headings and prominent stories");
    append_layout_choice(b, "content_style", "community", g_layout_settings.content_style, "Web community", "Compact headings and scan-friendly posts");
    cwist_sstring_append(b, "</div><h3>Post list</h3><div class='appearance-option-grid'>");
    append_layout_choice(b, "list_style", "feed", g_layout_settings.list_style, "Feed", "Titles, excerpts, and metadata");
    append_layout_choice(b, "list_style", "cards", g_layout_settings.list_style, "Cards", "Separated glass panels");
    append_layout_choice(b, "list_style", "table", g_layout_settings.list_style, "Compact rows", "Titles and metadata without excerpts");
    cwist_sstring_append(b, "</div><h3>Density</h3><div class='appearance-option-grid'>");
    append_layout_choice(b, "density", "compact", g_layout_settings.density, "Compact", "More posts per screen");
    append_layout_choice(b, "density", "comfortable", g_layout_settings.density, "Comfortable", "Current balanced spacing");
    append_layout_choice(b, "density", "spacious", g_layout_settings.density, "Spacious", "Generous editorial whitespace");
    cwist_sstring_append(b, "</div><h3>Content width</h3><div class='appearance-option-grid'>");
    append_layout_choice(b, "content_width", "narrow", g_layout_settings.content_width, "Narrow", "Focused reading column");
    append_layout_choice(b, "content_width", "standard", g_layout_settings.content_width, "Standard", "Current responsive width");
    append_layout_choice(b, "content_width", "wide", g_layout_settings.content_width, "Wide", "More room for large displays");
    cwist_sstring_append(b, "</div></section>");
    cwist_sstring_append(b, "<section class='card'><h2>Sections</h2><p class='appearance-note'>Unchecked sections are switched off for everyone: their pages answer 404 and their links disappear from the menu and pages. Posts and data are kept; checking a section again brings it back.</p><input type='hidden' name='sections_form' value='1'><div class='appearance-option-grid'>");
    for (size_t i = 0; i < g_site_section_count; i++) {
        const site_section_t *sec = &g_site_sections[i];
        cwist_sstring_append(b, "<label class='check-row appearance-section-toggle'><input type='checkbox' name='section_");
        cwist_sstring_append(b, sec->id);
        cwist_sstring_append(b, "' value='on'");
        if (site_section_enabled(sec->id)) cwist_sstring_append(b, " checked");
        cwist_sstring_append(b, "><span><strong>");
        cwist_sstring_append(b, sec->label);
        cwist_sstring_append(b, "</strong> <small>");
        cwist_sstring_append(b, sec->detail);
        cwist_sstring_append(b, "</small></span></label>");
    }
    cwist_sstring_append(b, "</div></section>");
    cwist_sstring_append(b, "<section class='card'><h2>Theme</h2><p class='appearance-note'>Selecting a radio updates the preview immediately. Light and dark choices are saved independently.</p><h3>Default mode</h3><div class='appearance-option-grid'>");
    append_layout_choice(b, "default_mode", "light", g_layout_settings.default_mode, "Light first", "New visitors start in the light-mode theme; the toggle switches to dark");
    append_layout_choice(b, "default_mode", "dark", g_layout_settings.default_mode, "Dark first", "New visitors start in the dark-mode theme; the toggle switches to light");
    cwist_sstring_append(b, "</div><p class='appearance-note'>Applies to visitors who have not used the theme toggle yet. A visitor's own choice is kept.</p><h3>Light mode</h3><div class='appearance-theme-grid'>");
    char light[64], dark_theme[64];
    get_special_themes(light, dark_theme);
    append_theme_group(b, "light_theme", light);
    cwist_sstring_append(b, "</div><h3>Dark mode</h3><div class='appearance-theme-grid'>");
    append_theme_group(b, "dark_theme", dark_theme);
    cwist_sstring_append(b, "</div><label class='check-row'><input type='checkbox' name='invert_transparent_images' value='true' data-appearance-setting");
    if (g_layout_settings.invert_transparent_images) cwist_sstring_append(b, " checked");
    cwist_sstring_append(b, "> Invert transparent images in dark mode</label><div class='appearance-field'><span>Formats to invert</span><div class='appearance-option-grid'>");
    append_transparent_format(b, "all", "All transparent formats", true);
    append_transparent_format(b, "svg", "SVG", false);
    append_transparent_format(b, "png", "PNG", false);
    append_transparent_format(b, "webp", "WebP", false);
    append_transparent_format(b, "gif", "GIF", false);
    append_transparent_format(b, "avif", "AVIF", false);
    append_transparent_format(b, "jpg", "JPEG", false);
    append_transparent_format(b, "bmp", "BMP", false);
    append_transparent_format(b, "ico", "ICO", false);
    cwist_sstring_append(b, "</div></div><p class='appearance-note'>Only selected formats with transparent backgrounds are changed. Opaque images and all light-mode images keep their original colors. The default is SVG only.</p><label class='appearance-field'><span>Custom CWIST theme JSON</span><input type='file' name='theme_file' accept='.json,application/json' data-preview-theme data-stage></label>");
    cwist_sstring_append(b, "<p class='appearance-note'>The file uses the same <code>name</code>, <code>vars</code>, and <code>rules</code> structure as <code>/themes.json</code>. Add <code>mode: \"light\"</code> or <code>\"dark\"</code>. Maximum 512 KiB.</p></section>");
    cwist_sstring_append(b, "<section class='card'><h2>Code highlighting</h2><p class='appearance-note'>Syntax colors for code blocks. Each site mode uses its own choice; open an Article in the preview to compare.</p>");
    append_code_theme_select(b, "code_theme_light", "Light mode", g_layout_settings.code_theme_light);
    append_code_theme_select(b, "code_theme_dark", "Dark mode", g_layout_settings.code_theme_dark);
    cwist_sstring_append(b, "</section>");

    static const char *role_labels[FONT_ROLE_COUNT] = {"Body", "Headings", "UI, navigation, buttons", "Code", "Blockquotes", "Display titles"};
    cwist_sstring_append(b, "<section class='card'><h2>Font sources</h2><p class='appearance-note'>Upload a font for each role separately (TTF, OTF, WOFF, or WOFF2, up to 12 MiB). An uploaded font is used only by its own role and goes to the front of that role's stack below.</p>");
    for (int r = 0; r < FONT_ROLE_COUNT; r++) {
        const char *role = g_font_roles[r];
        bool has = g_font_settings.role_src[r][0] != '\0';
        cwist_sstring_append(b, "<fieldset class='appearance-font-role' data-font-role='");
        cwist_sstring_append(b, role);
        cwist_sstring_append(b, "'");
        if (has) {
            cwist_sstring_append(b, " data-font-src=\"");
            cwist_sstring_append_escaped(b, g_font_settings.role_src[r]);
            cwist_sstring_append(b, "\"");
        }
        cwist_sstring_append(b, "><legend>");
        cwist_sstring_append(b, role_labels[r]);
        cwist_sstring_append(b, "</legend><label class='appearance-field'><span>Font file</span><input type='file' name='font_file_");
        cwist_sstring_append(b, role);
        cwist_sstring_append(b, "' accept='.ttf,.otf,.woff,.woff2' data-preview-font data-stage></label><label class='appearance-field'><span>Family name</span><input name='font_face_family_");
        cwist_sstring_append(b, role);
        cwist_sstring_append(b, "' value='");
        cwist_sstring_append_escaped(b, g_font_settings.role_family[r]);
        cwist_sstring_append(b, "' placeholder='Custom ");
        cwist_sstring_append(b, role);
        cwist_sstring_append(b, "' data-appearance-setting></label>");
        if (has) {
            cwist_sstring_append(b, "<label class='check-row'><input type='checkbox' name='font_clear_");
            cwist_sstring_append(b, role);
            cwist_sstring_append(b, "' data-appearance-setting> Remove the uploaded font</label>");
        }
        cwist_sstring_append(b, "</fieldset>");
    }
    if (g_font_settings.face_src[0]) {
        cwist_sstring_append(b, "<label class='check-row'><input type='checkbox' name='font_clear_legacy'> Remove the earlier font upload applied to every role (");
        cwist_sstring_append_escaped(b, g_font_settings.face_family);
        cwist_sstring_append(b, ")</label>");
    }
    append_input(b, "font_import_url", g_font_settings.import_url, "Webfont CSS URL (optional)");
    append_input(b, "font_script_url", g_font_settings.script_url, "Webfont JS URL (optional)");
    cwist_sstring_append(b, "<label class='appearance-field'><span>Webfont JavaScript file (optional)</span><input type='file' name='font_script_file' accept='.js,text/javascript' data-preview-script data-stage></label><p class='appearance-note'>Uploading a file replaces its corresponding URL. JavaScript runs with page privileges; upload only code you trust.</p></section>");

    cwist_sstring_append(b, "<section class='card'><h2>Font stacks</h2>");
    append_input(b, "font_body", g_font_settings.body, "Body and Korean/Latin/Cyrillic fallback");
    append_input(b, "font_heading", g_font_settings.heading, "Headings");
    append_input(b, "font_ui", g_font_settings.ui, "UI, navigation, buttons, metadata");
    append_input(b, "font_code", g_font_settings.code, "Code");
    append_input(b, "font_blockquote", g_font_settings.blockquote, "Blockquotes");
    append_input(b, "font_display", g_font_settings.display, "Display titles");
    cwist_sstring_append(b, "</section>");

    cwist_sstring_append(b, "<section class='card'><h2>Fine typography</h2><p class='appearance-note'>Each row controls the named page area. Weights accept 100 through 900; spacing accepts em, rem, px, or normal.</p>");
    struct { const char *label; const char *spacing_name; const char *spacing; const char *weight_name; const char *weight; } rows[] = {
        {"Body", "letter_spacing_body", g_font_settings.letter_spacing_body, "font_weight_body", g_font_settings.font_weight_body},
        {"Page H1", "letter_spacing_h1", g_font_settings.letter_spacing_h1, "font_weight_h1", g_font_settings.font_weight_h1},
        {"Page H2", "letter_spacing_h2", g_font_settings.letter_spacing_h2, "font_weight_h2", g_font_settings.font_weight_h2},
        {"Page H3", "letter_spacing_h3", g_font_settings.letter_spacing_h3, "font_weight_h3", g_font_settings.font_weight_h3},
        {"Page H4", "letter_spacing_h4", g_font_settings.letter_spacing_h4, "font_weight_h4", g_font_settings.font_weight_h4},
        {"H5 and H6", "letter_spacing_h5h6", g_font_settings.letter_spacing_h5h6, "font_weight_h5h6", g_font_settings.font_weight_h5h6},
        {"Top bar", "letter_spacing_topbar_title", g_font_settings.letter_spacing_topbar_title, "font_weight_topbar_title", g_font_settings.font_weight_topbar_title},
        {"Buttons", "letter_spacing_btn", g_font_settings.letter_spacing_btn, "font_weight_btn", g_font_settings.font_weight_btn},
        {"Board titles", "letter_spacing_board_line_title", g_font_settings.letter_spacing_board_line_title, "font_weight_board_line_title", g_font_settings.font_weight_board_line_title},
        {"Hero title", "letter_spacing_hero_h1", g_font_settings.letter_spacing_hero_h1, "font_weight_hero_h1", g_font_settings.font_weight_hero_h1},
        {"Post H1", "letter_spacing_post_h1", g_font_settings.letter_spacing_post_h1, "font_weight_post_h1", g_font_settings.font_weight_post_h1},
        {"Markdown H1", "letter_spacing_md_h1", g_font_settings.letter_spacing_md_h1, "font_weight_md_h1", g_font_settings.font_weight_md_h1},
        {"Markdown H2", "letter_spacing_md_h2", g_font_settings.letter_spacing_md_h2, "font_weight_md_h2", g_font_settings.font_weight_md_h2},
        {"Markdown H3", "letter_spacing_md_h3", g_font_settings.letter_spacing_md_h3, "font_weight_md_h3", g_font_settings.font_weight_md_h3}
    };
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        cwist_sstring_append(b, "<div class='appearance-tune'><span>");
        cwist_sstring_append(b, rows[i].label);
        cwist_sstring_append(b, "</span>");
        append_input(b, rows[i].spacing_name, rows[i].spacing, "Spacing");
        append_input(b, rows[i].weight_name, rows[i].weight, "Weight");
        cwist_sstring_append(b, "</div>");
    }
    append_input(b, "letter_spacing_hero_p", g_font_settings.letter_spacing_hero_p, "Hero subtitle spacing");
    cwist_sstring_append(b, "</section><div class='appearance-actions'><button type='submit' class='btn'>Save appearance</button><button type='reset' class='btn btn-outline' id='appearance-reset'>Reset preview</button><a href='/admin/dashboard' class='btn btn-outline'>Dashboard</a></div>");
    cwist_sstring_append(b, "</div><aside class='appearance-preview'><div class='appearance-actions appearance-preview-tools'><strong>Live preview</strong><select id='appearance-preview-page' aria-label='Preview page'><option value='list'>Post list</option><option value='article'>Article</option></select><select id='appearance-preview-mode' aria-label='Preview mode'><option value='light'>Light</option><option value='dark'>Dark</option></select><select id='appearance-preview-size' aria-label='Preview viewport'><option value='desktop'>Desktop</option><option value='mobile'>Mobile</option></select></div><div class='appearance-frame-wrap'><iframe id='appearance-preview' title='Live blog appearance preview' sandbox='allow-scripts allow-same-origin'></iframe></div><p class='appearance-note'>Nothing is applied to visitors until Save appearance is pressed.</p></aside></div></form><script src='/assets/js/appearance-editor.js?v=6'></script>");
    cwist_sstring *page = render_page("Appearance", b->data, dark, "admin", profile_pic, is_mobile);
    cwist_sstring_destroy(b);
    return page;
}

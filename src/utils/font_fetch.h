#ifndef FLY_FONT_FETCH_H
#define FLY_FONT_FETCH_H

/* Download font files declared as font_file=<filename> <url> entries in
 * fonts.settings into public/fonts/ when they are missing locally.
 * Called once at startup after the font settings have been loaded. */
void font_files_ensure_downloaded(void);

#endif

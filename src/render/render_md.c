#define _POSIX_C_SOURCE 200809L
#include "render.h"
#include "cwist/image_size.h"
#include <cwist/core/sstring/sstring.h>
#include <md4c-html.h>
#include <md4c.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    char **expressions;
    int count;
    int capacity;
} math_registry_t;

/* Pasted LaTeX occasionally contains a dragged-out equals or minus sign,
 * including mixed Unicode minus (U+2212) and ASCII hyphens.  Consecutive
 * copies have no mathematical meaning here and can make KaTeX reject the
 * expression, so retain one canonical ASCII operator. */
static size_t normalize_operator_runs(const char *expr, size_t len, char *copy) {
    static const char unicode_minus[] = "\xE2\x88\x92";
    size_t read = 0;
    size_t written = 0;

    while (read < len) {
        /* Fix missing double backslash line break in LaTeX environments (e.g. \begin{array}).
         * If a single '\' occurs at the end of a line (followed by optional spaces/tabs then '\n' or '\r'),
         * and is NOT preceded by another '\', convert it to '\\' for valid KaTeX row breaks. */
        if (expr[read] == '\\') {
            bool prev_was_slash = (read > 0 && expr[read - 1] == '\\');
            bool next_is_slash = (read + 1 < len && expr[read + 1] == '\\');
            if (!prev_was_slash && !next_is_slash) {
                size_t p = read + 1;
                while (p < len && (expr[p] == ' ' || expr[p] == '\t')) p++;
                if (p < len && (expr[p] == '\n' || expr[p] == '\r')) {
                    copy[written++] = '\\';
                    copy[written++] = '\\';
                    read = p;
                    continue;
                }
            }
        }

        if (expr[read] == '=') {
            size_t end = read + 1;
            while (end < len && expr[end] == '=') end++;
            copy[written++] = '=';
            read = end;
            continue;
        }

        if (expr[read] == '-' ||
            (read + 3 <= len && memcmp(expr + read, unicode_minus, 3) == 0)) {
            size_t end = read;
            size_t operators = 0;
            while (end < len) {
                if (expr[end] == '-') {
                    end++;
                    operators++;
                } else if (end + 3 <= len && memcmp(expr + end, unicode_minus, 3) == 0) {
                    end += 3;
                    operators++;
                } else {
                    break;
                }
            }
            if (operators >= 2) {
                copy[written++] = '-';
            } else {
                memcpy(copy + written, expr + read, end - read);
                written += end - read;
            }
            read = end;
            continue;
        }

        copy[written++] = expr[read++];
    }

    return written;
}

static void math_registry_init(math_registry_t *reg) {
    reg->expressions = NULL;
    reg->count = 0;
    reg->capacity = 0;
}

static void math_registry_add(math_registry_t *reg, const char *expr, size_t len) {
    while (len > 0 && (expr[0] == ' ' || expr[0] == '\t' || expr[0] == '\n' || expr[0] == '\r')) {
        expr++; len--;
    }
    while (len > 0 && (expr[len - 1] == ' ' || expr[len - 1] == '\t' || expr[len - 1] == '\n' || expr[len - 1] == '\r')) {
        len--;
    }
    if (len == 0) {
        expr = " ";
        len = 1;
    }
    if (reg->count >= reg->capacity) {
        reg->capacity = reg->capacity ? reg->capacity * 2 : 16;
        reg->expressions = (char **)realloc(reg->expressions, sizeof(char *) * reg->capacity);
    }
    char *copy = (char *)malloc(len + 1);
    size_t written = normalize_operator_runs(expr, len, copy);
    copy[written] = '\0';
    reg->expressions[reg->count++] = copy;
}

static void math_registry_free(math_registry_t *reg) {
    for (int i = 0; i < reg->count; i++) free(reg->expressions[i]);
    free(reg->expressions);
    reg->expressions = NULL;
    reg->count = 0;
    reg->capacity = 0;
}

static void append_escaped_math(cwist_sstring *out, const char *text) {
    while (*text) {
        if (*text == '<') cwist_sstring_append(out, "&lt;");
        else if (*text == '>') cwist_sstring_append(out, "&gt;");
        else if (*text == '&') cwist_sstring_append(out, "&amp;");
        else if (*text == '"') cwist_sstring_append(out, "&quot;");
        else cwist_sstring_append_len(out, text, 1);
        text++;
    }
}

/* TikZ is rendered by TikZJax in the browser.  It must not be inserted into
 * the Markdown input directly: MD_FLAG_NOHTML would escape the wrapper and
 * leave the diagram as text.  Keep the source in a registry, emit a harmless
 * placeholder for md4c, then restore an escaped wrapper in the HTML output. */
static void add_tikz_placeholder(cwist_sstring *out, math_registry_t *tikz,
                                 const char *code, size_t len) {
    math_registry_add(tikz, code, len);
    char placeholder[64];
    snprintf(placeholder, sizeof(placeholder), "@@TIKZ_BLOCK_%d@@", tikz->count - 1);
    cwist_sstring_append(out, placeholder);
}

static bool is_line_start(const char *s, size_t pos) {
    return pos == 0 || s[pos - 1] == '\n';
}

/* A block-level placeholder must land in its own paragraph. If one is glued
 * to a neighboring text line, the restored block element (a TikZ <div> or a
 * display-math span) ends up inside that <p>, producing invalid HTML and
 * swallowing the surrounding text. Pad the protected stream with a blank
 * line before the placeholder. Call only in a block position, where the
 * output currently ends at a line boundary. */
static void begin_block_placeholder(cwist_sstring *out) {
    if (!out->data) return; /* nothing emitted yet: placeholder starts the document */
    size_t n = strlen(out->data);
    if (n == 0) return;
    if (out->data[n - 1] != '\n') return;
    if (n >= 2 && out->data[n - 2] == '\n') return;
    cwist_sstring_append(out, "\n");
}

/* After a block placeholder the line must end; when the next source line
 * holds text directly (no blank line in between), add one more newline so
 * that text starts a fresh paragraph. `next` is the first unconsumed source
 * index. */
static void end_block_placeholder(cwist_sstring *out, const char *md, size_t len, size_t next) {
    cwist_sstring_append(out, "\n");
    if (next < len && md[next] != '\n') cwist_sstring_append(out, "\n");
}

/* True when a match starting at `start` begins at a line start and its end
 * `end` is followed by nothing but whitespace up to the line end, i.e. the
 * match occupies whole lines and behaves as a block rather than inline text
 * (a $$...$$ share a line with prose, or a tikzpicture inside a table row,
 * must stay inline). On success `after_line` receives the index just past
 * the terminating newline. */
static bool is_block_position(const char *md, size_t len, size_t start, size_t end, size_t *after_line) {
    if (!is_line_start(md, start)) return false;
    size_t p = end;
    while (p < len && (md[p] == ' ' || md[p] == '\t' || md[p] == '\r')) p++;
    if (p < len && md[p] != '\n') return false;
    *after_line = p < len ? p + 1 : p;
    return true;
}

/* A dollar preceded by an odd number of backslashes is escaped.  md4c's
 * LaTeX span parser is intentionally conservative, but that also means that
 * otherwise valid expressions can be left as plain text.  We protect dollar
 * spans ourselves so the server and KaTeX see the exact expression. */
static bool is_escaped(const char *s, size_t pos) {
    size_t backslashes = 0;
    while (pos > 0 && s[pos - 1] == '\\') {
        backslashes++;
        pos--;
    }
    return (backslashes & 1) != 0;
}

static size_t find_math_delimiter(const char *s, size_t len, size_t start,
                                  size_t delimiter_len, bool allow_newline) {
    for (size_t i = start; i + delimiter_len <= len; i++) {
        if (!allow_newline && s[i] == '\n') return SIZE_MAX;
        if (s[i] == '$' && !is_escaped(s, i)) {
            if (delimiter_len == 2) {
                if (i + 1 < len && s[i + 1] == '$') return i;
            } else if ((i + 1 >= len || s[i + 1] != '$') &&
                       (i + 1 >= len || !((s[i + 1] >= '0' && s[i + 1] <= '9') ||
                                           (s[i + 1] >= 'A' && s[i + 1] <= 'Z') ||
                                           (s[i + 1] >= 'a' && s[i + 1] <= 'z') ||
                                           s[i + 1] == '_'))) {
                return i;
            }
        }
    }
    return SIZE_MAX;
}

static size_t find_latex_delimiter(const char *s, size_t len, size_t start,
                                   char closing) {
    for (size_t i = start; i + 1 < len; i++) {
        if (s[i] == '\\' && s[i + 1] == closing && !is_escaped(s, i)) return i;
    }
    return SIZE_MAX;
}

/* Accept a display expression wrapped in bare square-bracket lines as a
 * convenient dollar-free form:
 *
 *   [
 *   \frac{a}{b}
 *   ]
 *
 * Restrict it to whole lines and require TeX-like punctuation in the body so
 * ordinary Markdown reference-style text is not mistaken for mathematics. */
static bool is_bare_bracket_line(const char *s, size_t len, size_t pos, char bracket,
                                 size_t *out_after) {
    if (!is_line_start(s, pos) || pos >= len || s[pos] != bracket) return false;
    size_t p = pos + 1;
    while (p < len && (s[p] == ' ' || s[p] == '\t' || s[p] == '\r')) p++;
    if (p < len && s[p] != '\n') return false;
    *out_after = p < len ? p + 1 : p;
    return true;
}

static bool looks_like_latex(const char *s, size_t len) {
    if (len == 0) return false;
    /* Check for key TeX characters */
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '\\' || s[i] == '^' || s[i] == '_' || s[i] == '{' ||
            s[i] == '}' || s[i] == '=' || s[i] == '+' || s[i] == '*' ||
            s[i] == '&') return true;
    }
    /* Check for TeX math keywords */
    if (memmem(s, len, "\\binom", 6) || memmem(s, len, "\\frac", 5) ||
        memmem(s, len, "\\begin", 6) || memmem(s, len, "\\end", 4) ||
        memmem(s, len, "\\matrix", 7) || memmem(s, len, "\\array", 6) ||
        memmem(s, len, "\\left", 5) || memmem(s, len, "\\right", 6) ||
        memmem(s, len, "\\partial", 8) || memmem(s, len, "\\sum", 4) ||
        memmem(s, len, "\\prod", 5) || memmem(s, len, "\\int", 4) ||
        memmem(s, len, "\\alpha", 6) || memmem(s, len, "\\beta", 5) ||
        memmem(s, len, "\\gamma", 6) || memmem(s, len, "\\delta", 6) ||
        memmem(s, len, "\\theta", 6) || memmem(s, len, "\\lambda", 7) ||
        memmem(s, len, "\\mu", 3) || memmem(s, len, "\\pi", 3) ||
        memmem(s, len, "\\sigma", 6) || memmem(s, len, "\\phi", 4) ||
        memmem(s, len, "\\omega", 6) || memmem(s, len, "\\Delta", 6) ||
        memmem(s, len, "\\mathbf", 7) || memmem(s, len, "\\mathbb", 7) ||
        memmem(s, len, "\\mathcal", 8) || memmem(s, len, "\\mathrm", 7) ||
        memmem(s, len, "\\text", 5) || memmem(s, len, "\\over", 5)) {
        return true;
    }
    return false;
}

/* Authors often paste a complete LaTeX expression on its own line or block without
 * wrapping it in $...$. Treat mathematical lines and environment blocks as display math. */
static bool looks_like_bare_latex_expression(const char *s, size_t len) {
    bool has_command = false;
    bool has_math_syntax = false;

    while (len > 0 && (*s == ' ' || *s == '\t' || *s == '\r')) {
        s++;
        len--;
    }
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\r')) len--;
    if (len == 0) return false;

    /* If it starts with a LaTeX math environment like \begin{array}, \begin{matrix}, etc. */
    if (len >= 12 && strncmp(s, "\\begin{", 7) == 0) {
        if (strstr(s, "array") || strstr(s, "matrix") || strstr(s, "aligned") ||
            strstr(s, "cases") || strstr(s, "equation") || strstr(s, "gather")) {
            return true;
        }
    }

    for (size_t i = 0; i < len; i++) {
        /* Preserve Markdown table cells and explicitly delimited math for
         * their dedicated parsers below. */
        if (s[i] == '|' || s[i] == '$') return false;
        if (s[i] == '\\' && i + 1 < len &&
            ((s[i + 1] >= 'A' && s[i + 1] <= 'Z') || (s[i + 1] >= 'a' && s[i + 1] <= 'z'))) {
            has_command = true;
        }
        if (s[i] == '=' || s[i] == '^' || s[i] == '_' || s[i] == '{' || s[i] == '}' ||
            s[i] == '&' || s[i] == '+' || s[i] == '*' || s[i] == '/' || s[i] == '-') {
            has_math_syntax = true;
        }
    }
    return has_command && has_math_syntax;
}

static bool find_bare_bracket_math(const char *s, size_t len, size_t start,
                                   size_t *out_close, size_t *out_after) {
    for (size_t p = start; p < len; p++) {
        size_t after = 0;
        if (is_bare_bracket_line(s, len, p, ']', &after)) {
            if (!looks_like_latex(s + start, p - start)) return false;
            *out_close = p;
            *out_after = after;
            return true;
        }
    }
    return false;
}

/* Recognize an indented fenced TikZ block while retaining the opening fence
 * length. A four-backtick block may legally contain three backticks, and the
 * previous parser prematurely terminated it. */
static bool tikz_fence_open(const char *s, size_t pos, size_t len,
                            char *out_char, size_t *out_fence_len,
                            size_t *out_content_start) {
    if (!is_line_start(s, pos)) return false;
    size_t p = pos, indent = 0;
    while (p < len && s[p] == ' ' && indent < 4) { p++; indent++; }
    if (indent > 3 || p >= len || (s[p] != '`' && s[p] != '~')) return false;
    char fence = s[p];
    size_t run = 0;
    while (p + run < len && s[p + run] == fence) run++;
    if (run < 3) return false;
    size_t info = p + run;
    while (info < len && (s[info] == ' ' || s[info] == '\t')) info++;
    if (info + 4 > len || strncasecmp(s + info, "tikz", 4) != 0) return false;
    info += 4;
    if (info < len && s[info] != ' ' && s[info] != '\t' && s[info] != '\r' && s[info] != '\n') return false;
    while (info < len && s[info] != '\n') info++;
    if (info < len) info++;
    *out_char = fence;
    *out_fence_len = run;
    *out_content_start = info;
    return true;
}

static bool tikz_fence_close(const char *s, size_t pos, size_t len,
                             char fence, size_t min_fence_len, size_t *out_after) {
    if (!is_line_start(s, pos)) return false;
    size_t p = pos, indent = 0;
    while (p < len && s[p] == ' ' && indent < 4) { p++; indent++; }
    if (indent > 3 || p >= len || s[p] != fence) return false;
    size_t run = 0;
    while (p + run < len && s[p + run] == fence) run++;
    if (run < min_fence_len) return false;
    p += run;
    while (p < len && (s[p] == ' ' || s[p] == '\t' || s[p] == '\r')) p++;
    if (p < len && s[p] != '\n') return false;
    *out_after = p < len ? p + 1 : p;
    return true;
}

/* Case-insensitive search for a "</tag" style needle, used to find the end of
 * a raw <video>/<audio> block before md4c runs with MD_FLAG_NOHTML. */
static const char *find_closing_tag(const char *hay, size_t hay_len, const char *needle, size_t needle_len) {
    if (needle_len == 0 || hay_len < needle_len) return NULL;
    for (size_t i = 0; i + needle_len <= hay_len; i++) {
        if (strncasecmp(hay + i, needle, needle_len) == 0) return hay + i;
    }
    return NULL;
}

static bool media_attr_allowed(const char *name, size_t len) {
    static const char *const allowed[] = {
        "src", "poster", "controls", "autoplay", "loop", "muted", "playsinline",
        "preload", "width", "height", "style", "class", "id", "title", "crossorigin"
    };
    if (len >= 5 && strncasecmp(name, "data-", 5) == 0) return true;
    for (size_t k = 0; k < sizeof(allowed) / sizeof(allowed[0]); k++) {
        if (strlen(allowed[k]) == len && strncasecmp(name, allowed[k], len) == 0) return true;
    }
    return false;
}

/* Rebuild a raw <video>/<audio> snippet keeping only whitelisted attributes.
 * Event handlers (on*) and everything not on the list are dropped so that
 * restoring the tag cannot smuggle script past MD_FLAG_NOHTML.
 * Inner content and the closing tag are discarded; the editor never emits any. */
static void sanitize_media_tag(cwist_sstring *out, const char *snippet, size_t len, const char *tag_name) {
    cwist_sstring_append(out, "<");
    cwist_sstring_append(out, tag_name);
    size_t i = strlen(tag_name) + 1; /* skip "<video" / "<audio" */
    while (i < len && snippet[i] != '>') {
        while (i < len && (snippet[i] == ' ' || snippet[i] == '\t' || snippet[i] == '\n' || snippet[i] == '\r' || snippet[i] == '/')) i++;
        if (i >= len || snippet[i] == '>') break;
        size_t name_start = i;
        while (i < len && snippet[i] != '=' && snippet[i] != '>' && snippet[i] != ' ' &&
               snippet[i] != '\t' && snippet[i] != '\n' && snippet[i] != '\r' && snippet[i] != '/') i++;
        size_t name_len = i - name_start;
        if (name_len == 0) { i++; continue; }
        while (i < len && (snippet[i] == ' ' || snippet[i] == '\t' || snippet[i] == '\n' || snippet[i] == '\r')) i++;
        const char *value = NULL;
        size_t value_len = 0;
        if (i < len && snippet[i] == '=') {
            i++;
            while (i < len && (snippet[i] == ' ' || snippet[i] == '\t' || snippet[i] == '\n' || snippet[i] == '\r')) i++;
            if (i < len && (snippet[i] == '"' || snippet[i] == '\'')) {
                char quote = snippet[i++];
                value = snippet + i;
                while (i < len && snippet[i] != quote) i++;
                value_len = (size_t)(snippet + i - value);
                if (i < len) i++; /* skip closing quote */
            } else {
                value = snippet + i;
                while (i < len && snippet[i] != '>' && snippet[i] != ' ' && snippet[i] != '\t' &&
                       snippet[i] != '\n' && snippet[i] != '\r') i++;
                value_len = (size_t)(snippet + i - value);
            }
        }
        if (!media_attr_allowed(snippet + name_start, name_len)) continue;
        cwist_sstring_append(out, " ");
        cwist_sstring_append_len(out, snippet + name_start, name_len);
        if (value) {
            cwist_sstring_append(out, "=\"");
            for (size_t v = 0; v < value_len; v++) {
                if (value[v] == '"') cwist_sstring_append(out, "&quot;");
                else cwist_sstring_append_len(out, value + v, 1);
            }
            cwist_sstring_append(out, "\"");
        }
    }
    cwist_sstring_append(out, "></");
    cwist_sstring_append(out, tag_name);
    cwist_sstring_append(out, ">");
}

static bool is_code_fence_line(const char *s, size_t pos, size_t len, int *out_len) {
    size_t k = pos;
    while (k < len && (s[k] == ' ' || s[k] == '\t')) k++;
    if (k + 3 > len) return false;
    int backticks = 0;
    while (k + backticks < len && s[k + backticks] == '`') backticks++;
    if (backticks >= 3) {
        if (out_len) *out_len = backticks;
        return true;
    }
    int tildes = 0;
    while (k + tildes < len && s[k + tildes] == '~') tildes++;
    if (tildes >= 3) {
        if (out_len) *out_len = tildes;
        return true;
    }
    return false;
}

static char *protect_math(const char *md, math_registry_t *blocks,
                          math_registry_t *inlines, math_registry_t *media,
                          math_registry_t *tikz) {
    size_t len = strlen(md);
    cwist_sstring *out = cwist_sstring_create();
    size_t i = 0;
    int in_code_block = 0;
    int current_fence_len = 0;

    while (i < len) {
        /* Handle TikZ before generic fence tracking.  Otherwise this opening
         * line would put us in a code block and the specialized branch below
         * would never be reached. */
        char fence_char = 0;
        size_t opening_fence_len = 0, line_end = 0;
        if (tikz_fence_open(md, i, len, &fence_char, &opening_fence_len, &line_end)) {
            const char *closing = NULL;
            size_t after_closing = 0;
            for (size_t j = line_end; j < len; j++) {
                size_t candidate_after = 0;
                if (tikz_fence_close(md, j, len, fence_char, opening_fence_len, &candidate_after)) {
                    closing = md + j;
                    after_closing = candidate_after;
                    break;
                }
            }
            if (closing) {
                size_t code_len = (size_t)(closing - (md + line_end));
                begin_block_placeholder(out);
                add_tikz_placeholder(out, tikz, md + line_end, code_len);
                i = after_closing;
                end_block_placeholder(out, md, len, i);
                continue;
            }
        }

        /* Track fenced code blocks with proper backtick/tilde counts */
        int fence_len = 0;
        if (is_line_start(md, i) && is_code_fence_line(md, i, len, &fence_len)) {
            if (in_code_block) {
                if (fence_len >= current_fence_len) {
                    in_code_block = 0;
                    current_fence_len = 0;
                    /* Consume the rest of the closing fence line so backticks
                       are not misinterpreted as inline code spans. */
                    while (i < len && md[i] != '\n') {
                        cwist_sstring_append_len(out, md + i, 1);
                        i++;
                    }
                    if (i < len) {
                        cwist_sstring_append_len(out, md + i, 1);
                        i++;
                    }
                    continue;
                }
            } else {
                in_code_block = 1;
                current_fence_len = fence_len;
            }
        }

        if (in_code_block) {
            /* md4c's LaTeX math extension parses $ even inside fenced code
             * blocks. Mask dollars so code examples containing $ stay literal. */
            if (md[i] == '$') {
                cwist_sstring_append(out, "@@MATHDOLLAR@@");
            } else {
                cwist_sstring_append_len(out, md + i, 1);
            }
            i++;
            continue;
        }

        /* Skip inline code spans (backticks), masking any $ inside so inline
         * code like `$var` is not turned into math. The closing run must be
         * exactly as long as the opening run, so `` ` `` style spans with a
         * lone backtick inside are handled as well. */
        if (md[i] == '`') {
            size_t run = 1;
            while (i + run < len && md[i + run] == '`') run++;
            size_t j = i + run;
            size_t close_end = SIZE_MAX;
            while (j < len) {
                if (md[j] != '`') { j++; continue; }
                size_t crun = 0;
                while (j + crun < len && md[j + crun] == '`') crun++;
                if (crun == run) { close_end = j + crun; break; }
                j += crun;
            }
            if (close_end != SIZE_MAX) {
                for (size_t k = i; k < close_end; k++) {
                    if (md[k] == '$') cwist_sstring_append(out, "@@MATHDOLLAR@@");
                    else cwist_sstring_append_len(out, md + k, 1);
                }
                i = close_end;
                continue;
            }
        }

        /* Raw <video>/<audio> tags inserted by the editor. md4c runs with
         * MD_FLAG_NOHTML and would escape them, so swap them for placeholders
         * here and restore a sanitized version after rendering. */
        if (md[i] == '<' && media) {
            const char *tag_name = NULL;
            size_t name_len = 0;
            if (i + 6 <= len && strncasecmp(md + i, "<video", 6) == 0) { tag_name = "video"; name_len = 6; }
            else if (i + 6 <= len && strncasecmp(md + i, "<audio", 6) == 0) { tag_name = "audio"; name_len = 6; }
            if (tag_name && (i + name_len >= len || md[i + name_len] == ' ' || md[i + name_len] == '\t' ||
                             md[i + name_len] == '\n' || md[i + name_len] == '\r' ||
                             md[i + name_len] == '>' || md[i + name_len] == '/')) {
                size_t tag_end = i + name_len;
                while (tag_end < len && md[tag_end] != '>') tag_end++;
                if (tag_end < len) {
                    char closing[16];
                    snprintf(closing, sizeof(closing), "</%s", tag_name);
                    size_t block_end = tag_end + 1;
                    const char *close = find_closing_tag(md + tag_end + 1, len - (tag_end + 1), closing, strlen(closing));
                    if (close) {
                        const char *close_gt = memchr(close, '>', len - (size_t)(close - md));
                        if (close_gt) block_end = (size_t)(close_gt - md) + 1;
                    }
                    if (block_end - i <= 4096) {
                        cwist_sstring *sanitized = cwist_sstring_create();
                        if (sanitized) {
                            sanitize_media_tag(sanitized, md + i, tag_end + 1 - i, tag_name);
                            math_registry_add(media, sanitized->data, strlen(sanitized->data));
                            cwist_sstring_destroy(sanitized);
                            char placeholder[64];
                            snprintf(placeholder, sizeof(placeholder), "@@RAW_MEDIA_%d@@", media->count - 1);
                            cwist_sstring_append(out, placeholder);
                            i = block_end;
                            continue;
                        }
                    }
                }
            }
        }

        /* TikZ environments. tikzcd uses the same browser renderer but had
         * previously fallen through to Markdown, where its line breaks and
         * special characters could be reinterpreted. */
        static const char tikzpicture_begin[] = "\\begin{tikzpicture}";
        static const char tikzpicture_end[] = "\\end{tikzpicture}";
        static const char tikzcd_begin[] = "\\begin{tikzcd}";
        static const char tikzcd_end[] = "\\end{tikzcd}";
        const char *tikz_begin = NULL;
        const char *tikz_end = NULL;
        if (i + sizeof(tikzpicture_begin) - 1 <= len &&
            strncmp(md + i, tikzpicture_begin, sizeof(tikzpicture_begin) - 1) == 0) {
            tikz_begin = tikzpicture_begin;
            tikz_end = tikzpicture_end;
        } else if (i + sizeof(tikzcd_begin) - 1 <= len &&
                   strncmp(md + i, tikzcd_begin, sizeof(tikzcd_begin) - 1) == 0) {
            tikz_begin = tikzcd_begin;
            tikz_end = tikzcd_end;
        }
        if (tikz_begin) {
            size_t tikz_begin_len = strlen(tikz_begin);
            size_t tikz_end_len = strlen(tikz_end);
            const char *end_tag = strstr(md + i + tikz_begin_len, tikz_end);
            if (end_tag) {
                size_t total_len = (size_t)(end_tag + tikz_end_len - (md + i));
                size_t after = 0;
                if (is_block_position(md, len, i, i + total_len, &after)) {
                    begin_block_placeholder(out);
                    add_tikz_placeholder(out, tikz, md + i, total_len);
                    end_block_placeholder(out, md, len, after);
                    i = after;
                } else {
                    /* Inline context (e.g. a table cell): keep the
                     * placeholder on the current line. */
                    add_tikz_placeholder(out, tikz, md + i, total_len);
                    i += total_len;
                }
                continue;
            }
        }

        /* A multiline or single-line LaTeX environment block (e.g., \begin{array} ... \end{array}, \begin{matrix} ... \end{matrix}) */
        if (is_line_start(md, i) && i + 7 <= len && strncmp(md + i, "\\begin{", 7) == 0) {
            const char *env_name_start = md + i + 7;
            const char *env_name_end = strchr(env_name_start, '}');
            if (env_name_end && (size_t)(env_name_end - env_name_start) < 64) {
                char env_name[64];
                size_t name_len = (size_t)(env_name_end - env_name_start);
                memcpy(env_name, env_name_start, name_len);
                env_name[name_len] = '\0';

                /* Ignore tikzpicture and tikzcd as they are handled in their dedicated TikZ parser block above */
                if (strcmp(env_name, "tikzpicture") != 0 && strcmp(env_name, "tikzcd") != 0) {
                    char closing_tag[80];
                    snprintf(closing_tag, sizeof(closing_tag), "\\end{%s}", env_name);
                    const char *end_match = strstr(env_name_end + 1, closing_tag);
                    if (end_match) {
                        size_t block_len = (size_t)(end_match + strlen(closing_tag) - (md + i));
                        size_t after = (size_t)(end_match + strlen(closing_tag) - md);
                        while (after < len && (md[after] == ' ' || md[after] == '\t' || md[after] == '\r')) after++;
                        if (after < len && md[after] == '\n') after++;
                        begin_block_placeholder(out);
                        math_registry_add(blocks, md + i, block_len);
                        char placeholder[64];
                        snprintf(placeholder, sizeof(placeholder), "@@MATH_BLOCK_%d@@", blocks->count - 1);
                        cwist_sstring_append(out, placeholder);
                        end_block_placeholder(out, md, len, after);
                        i = after;
                        continue;
                    }
                }
            }
        }

        /* A complete, delimiter-free LaTeX expression on its own line is a
         * common paste format. Run this after TikZ recognition, otherwise a
         * \begin{tikzpicture} line would be mistaken for display math. */
        if (is_line_start(md, i)) {
            size_t line_end = i;
            while (line_end < len && md[line_end] != '\n') line_end++;
            if (looks_like_bare_latex_expression(md + i, line_end - i)) {
                begin_block_placeholder(out);
                math_registry_add(blocks, md + i, line_end - i);
                char placeholder[64];
                snprintf(placeholder, sizeof(placeholder), "@@MATH_BLOCK_%d@@", blocks->count - 1);
                cwist_sstring_append(out, placeholder);
                size_t after = line_end < len ? line_end + 1 : line_end;
                end_block_placeholder(out, md, len, after);
                i = after;
                continue;
            }
        }

        /* Dollar-free display math. A bare [ ... ] pair is recognized only
         * when each delimiter owns its line, preserving normal Markdown links
         * and letting long divider-like lines remain LaTeX source. */
        size_t bracket_content = 0;
        if (is_bare_bracket_line(md, len, i, '[', &bracket_content)) {
            size_t close = 0, after = 0;
            if (find_bare_bracket_math(md, len, bracket_content, &close, &after)) {
                begin_block_placeholder(out);
                math_registry_add(blocks, md + bracket_content, close - bracket_content);
                char placeholder[64];
                snprintf(placeholder, sizeof(placeholder), "@@MATH_BLOCK_%d@@", blocks->count - 1);
                cwist_sstring_append(out, placeholder);
                end_block_placeholder(out, md, len, after);
                i = after;
                continue;
            }
        }

        /* Block math: \[...\]
         * $$...$$ and $...$ are handled natively by md4c when
         * MD_FLAG_LATEXMATHSPANS is enabled; we only protect the LaTeX-style
         * delimiters that md4c does not understand. */
        if (i + 1 < len && md[i] == '\\' && md[i + 1] == '[') {
            size_t j = find_latex_delimiter(md, len, i + 2, ']');
            if (j != SIZE_MAX) {
                size_t expr_len = j - (i + 2);
                size_t after = 0;
                if (is_block_position(md, len, i, j + 2, &after)) {
                    begin_block_placeholder(out);
                    math_registry_add(blocks, md + i + 2, expr_len);
                    char placeholder[64];
                    snprintf(placeholder, sizeof(placeholder), "@@MATH_BLOCK_%d@@", blocks->count - 1);
                    cwist_sstring_append(out, placeholder);
                    end_block_placeholder(out, md, len, after);
                    i = after;
                } else {
                    math_registry_add(blocks, md + i + 2, expr_len);
                    char placeholder[64];
                    snprintf(placeholder, sizeof(placeholder), "@@MATH_BLOCK_%d@@", blocks->count - 1);
                    cwist_sstring_append(out, placeholder);
                    i = j + 2;
                }
                continue;
            }
        }

        /* Protect dollar-delimited math before md4c sees it.  This handles
         * display math ($$...$$), inline math ($...$), escaped dollars, and
         * multiline display expressions consistently. */

        if (md[i] == '$' && !is_escaped(md, i)) {
            size_t delimiter_len = (i + 1 < len && md[i + 1] == '$') ? 2 : 1;
            size_t expr_start = i + delimiter_len;
            size_t close = find_math_delimiter(md, len, expr_start,
                                               delimiter_len, delimiter_len == 2);
            if (close != SIZE_MAX && close > expr_start) {
                math_registry_t *registry = delimiter_len == 2 ? blocks : inlines;
                size_t after = 0;
                bool block_position = delimiter_len == 2 &&
                    is_block_position(md, len, i, close + delimiter_len, &after);
                if (block_position) begin_block_placeholder(out);
                math_registry_add(registry, md + expr_start, close - expr_start);
                char placeholder[64];
                if (delimiter_len == 2) {
                    snprintf(placeholder, sizeof(placeholder), "@@MATH_BLOCK_%d@@", registry->count - 1);
                } else {
                    snprintf(placeholder, sizeof(placeholder), "@@MATH_INLINE_%d@@", registry->count - 1);
                }
                cwist_sstring_append(out, placeholder);
                if (block_position) {
                    end_block_placeholder(out, md, len, after);
                    i = after;
                } else {
                    i = close + delimiter_len;
                }
                continue;
            }
        }

        /* Inline math: \(...\) */
        if (i + 1 < len && md[i] == '\\' && md[i + 1] == '(') {
            size_t j = find_latex_delimiter(md, len, i + 2, ')');
            if (j != SIZE_MAX) {
                size_t expr_len = j - (i + 2);
                if (expr_len > 0) {
                    math_registry_add(inlines, md + i + 2, expr_len);
                    char placeholder[64];
                    snprintf(placeholder, sizeof(placeholder), "@@MATH_INLINE_%d@@", inlines->count - 1);
                    cwist_sstring_append(out, placeholder);
                    i = j + 2;
                    continue;
                }
            }
        }

        cwist_sstring_append_len(out, md + i, 1);
        i++;
    }

    char *result = strdup(out->data);
    cwist_sstring_destroy(out);
    return result;
}

static void restore_math(cwist_sstring *html, const math_registry_t *blocks, const math_registry_t *inlines) {
    const char *data = html->data;
    size_t len = strlen(data);
    cwist_sstring *out = cwist_sstring_create();
    size_t i = 0;

    while (i < len) {
        if (i + 13 <= len && strncmp(data + i, "@@MATH_BLOCK_", 13) == 0) {
            size_t j = i + 13;
            int idx = 0;
            while (j < len && data[j] >= '0' && data[j] <= '9') {
                idx = idx * 10 + (data[j] - '0');
                j++;
            }
            if (j + 2 <= len && strncmp(data + j, "@@", 2) == 0) {
                cwist_sstring_append(out, "<span class=\"math-block\">");
                if (idx >= 0 && idx < blocks->count) {
                    append_escaped_math(out, blocks->expressions[idx]);
                }
                cwist_sstring_append(out, "</span>");
                i = j + 2;
                continue;
            }
        } else if (i + 14 <= len && strncmp(data + i, "@@MATHDOLLAR@@", 14) == 0) {
            cwist_sstring_append(out, "$");
            i += 14;
            continue;
        } else if (i + 14 <= len && strncmp(data + i, "@@MATH_INLINE_", 14) == 0) {
            size_t j = i + 14;
            int idx = 0;
            while (j < len && data[j] >= '0' && data[j] <= '9') {
                idx = idx * 10 + (data[j] - '0');
                j++;
            }
            if (j + 2 <= len && strncmp(data + j, "@@", 2) == 0) {
                cwist_sstring_append(out, "<span class=\"math-inline\">");
                if (idx >= 0 && idx < inlines->count) {
                    append_escaped_math(out, inlines->expressions[idx]);
                }
                cwist_sstring_append(out, "</span>");
                i = j + 2;
                continue;
            }
        }
        cwist_sstring_append_len(out, data + i, 1);
        i++;
    }

    cwist_sstring_assign(html, out->data);
    cwist_sstring_destroy(out);
}

/* Swap the @@RAW_MEDIA_N@@ placeholders back for the sanitized <video>/<audio>
 * markup captured by protect_math. */
static void restore_media(cwist_sstring *html, const math_registry_t *media) {
    if (!media || media->count == 0) return;
    const char *data = html->data;
    size_t len = strlen(data);
    cwist_sstring *out = cwist_sstring_create();
    size_t i = 0;

    while (i < len) {
        if (i + 12 <= len && strncmp(data + i, "@@RAW_MEDIA_", 12) == 0) {
            size_t j = i + 12;
            int idx = 0;
            while (j < len && data[j] >= '0' && data[j] <= '9') {
                idx = idx * 10 + (data[j] - '0');
                j++;
            }
            if (j + 2 <= len && strncmp(data + j, "@@", 2) == 0) {
                if (idx >= 0 && idx < media->count) {
                    cwist_sstring_append(out, media->expressions[idx]);
                }
                i = j + 2;
                continue;
            }
        }
        cwist_sstring_append_len(out, data + i, 1);
        i++;
    }

    cwist_sstring_assign(html, out->data);
    cwist_sstring_destroy(out);
}

static bool parse_tikz_placeholder(const char *data, size_t len, size_t pos,
                                   size_t *after, int *index) {
    static const char prefix[] = "@@TIKZ_BLOCK_";
    const size_t prefix_len = sizeof(prefix) - 1;
    if (pos + prefix_len > len || strncmp(data + pos, prefix, prefix_len) != 0) return false;

    size_t j = pos + prefix_len;
    int idx = 0;
    bool has_digit = false;
    while (j < len && data[j] >= '0' && data[j] <= '9') {
        has_digit = true;
        idx = idx * 10 + (data[j] - '0');
        j++;
    }
    if (!has_digit || j + 2 > len || strncmp(data + j, "@@", 2) != 0) return false;
    *after = j + 2;
    *index = idx;
    return true;
}

static void append_tikz_block(cwist_sstring *out, const char *code) {
    bool has_packages = strstr(code, "\\usetikzlibrary") != NULL ||
                        strstr(code, "\\usepackage") != NULL;
    cwist_sstring_append(out, "<div class=\"tikz-block\"");
    if (has_packages) cwist_sstring_append(out, " data-has-packages=\"true\"");
    cwist_sstring_append(out, ">\n");
    append_escaped_math(out, code);
    cwist_sstring_append(out, "\n</div>");
}

/* Replace paragraph-wrapped placeholders with a block-level TikZ container.
 * The source is HTML-escaped so untrusted post content cannot break out before
 * TikZJax reads it through textContent. */
static void restore_tikz(cwist_sstring *html, const math_registry_t *tikz) {
    if (!tikz || tikz->count == 0) return;
    const char *data = html->data;
    size_t len = strlen(data);
    cwist_sstring *out = cwist_sstring_create();
    if (!out) return;

    size_t i = 0;
    while (i < len) {
        size_t placeholder_start = i;
        size_t after = 0;
        int index = -1;
        bool paragraph = i + 3 <= len && strncmp(data + i, "<p>", 3) == 0;
        if (paragraph) placeholder_start += 3;
        if (parse_tikz_placeholder(data, len, placeholder_start, &after, &index) &&
            (!paragraph || (after + 4 <= len && strncmp(data + after, "</p>", 4) == 0))) {
            if (index >= 0 && index < tikz->count) {
                append_tikz_block(out, tikz->expressions[index]);
            }
            i = paragraph ? after + 4 : after;
            continue;
        }
        cwist_sstring_append_len(out, data + i, 1);
        i++;
    }

    cwist_sstring_assign(html, out->data);
    cwist_sstring_destroy(out);
}

/* md4c emits <x-equation> tags for $...$ and $$...$$ when MD_FLAG_LATEXMATHSPANS
 * is enabled. Convert them to the spans the client-side KaTeX renderer expects. */
static void rewrite_x_equation(cwist_sstring *html) {
    const char *data = html->data;
    size_t len = strlen(data);
    cwist_sstring *out = cwist_sstring_create();
    if (!out) return;
    size_t i = 0;

    while (i < len) {
        if (i + 27 <= len && strncmp(data + i, "<x-equation type=\"display\">", 27) == 0) {
            cwist_sstring_append(out, "<span class=\"math-block\">");
            i += 27;
        } else if (i + 12 <= len && strncmp(data + i, "<x-equation>", 12) == 0) {
            cwist_sstring_append(out, "<span class=\"math-inline\">");
            i += 12;
        } else if (i + 13 <= len && strncmp(data + i, "</x-equation>", 13) == 0) {
            cwist_sstring_append(out, "</span>");
            i += 13;
        } else {
            cwist_sstring_append_len(out, data + i, 1);
            i++;
        }
    }

    cwist_sstring_assign(html, out->data);
    cwist_sstring_destroy(out);
}

static void md_output_cb(const MD_CHAR *data, MD_SIZE size, void *userdata) {
    cwist_sstring *str = (cwist_sstring *)userdata;
    cwist_error_t err = cwist_sstring_append_len(str, data, size);
    if (err.errtype != CWIST_ERR_INT8 || err.error.err_i8 != ERR_SSTRING_OKAY) {
        size_t new_size = (size_t)(str->size * 1.25f);
        if (new_size < str->size + size) new_size = str->size + size;
        cwist_sstring_change_size(str, new_size, false);
        cwist_sstring_append_len(str, data, size);
    }
}

static void rewrite_tasfa_bootstrap(cwist_sstring *html) {
    const char *data = html->data;
    size_t len = strlen(data);
    cwist_sstring *out = cwist_sstring_create();
    size_t i = 0;
    while (i < len) {
        if (data[i] == '<') {
            const char *tag_name = NULL;
            bool is_media = false;
            bool is_link = false;
            if (i + 2 <= len && strncmp(data + i, "<a", 2) == 0) { tag_name = "a"; is_link = true; }
            else if (i + 4 <= len && strncmp(data + i, "<img", 4) == 0) { tag_name = "img"; is_media = true; }
            else if (i + 6 <= len && strncmp(data + i, "<video", 6) == 0) { tag_name = "video"; is_media = true; }
            else if (i + 6 <= len && strncmp(data + i, "<audio", 6) == 0) { tag_name = "audio"; is_media = true; }
            if (tag_name) {
                size_t j = i;
                while (j < len && data[j] != '>') j++;
                if (j < len) j++;
                size_t tag_len = j - i;
                char tag[2048];
                if (tag_len >= sizeof(tag)) tag_len = sizeof(tag) - 1;
                memcpy(tag, data + i, tag_len);
                tag[tag_len] = '\0';
                if (is_media) {
                    const char *src = strstr(tag, "src=\"");
                    if (src) {
                        src += 5;
                        if (strncmp(src, "/file/download/", 15) == 0) {
                            char rewritten[3072];
                            const char *value_end = strchr(src, '"');
                            size_t path_len = value_end ? (size_t)(value_end - src) : strlen(src);
                            char download_path[1024] = {0};
                            if (path_len < sizeof(download_path)) {
                                strncpy(download_path, src, path_len);
                            }
                            char *id_end = NULL;
                            long file_id = strtol(download_path + 15, &id_end, 10);
                            if (strcmp(tag_name, "img") == 0 && file_id > 0 && id_end && (*id_end == '\0' || *id_end == '?')) {
                                snprintf(rewritten, sizeof(rewritten),
                                         "<%s src=\"/file/preview/%ld?poster=1\" data-tasfa-src=\"/file/preview/%ld?poster=1\" "
                                         "data-tasfa-original=\"%s\" data-tasfa-animation-url=\"/file/download/%ld?preview=1&tasfa_fallback=1\" data-tasfa-fixed-preview=\"1\"",
                                         tag_name, file_id, file_id, download_path, file_id);
                            } else {
                                snprintf(rewritten, sizeof(rewritten), "<%s data-tasfa-download=\"%s\"", tag_name, download_path);
                            }
                            cwist_sstring_append(out, rewritten);
                            const char *after_name = data + i + strlen(tag_name) + 1;
                            const char *src_pos = strstr(after_name, "src=\"");
                            if (src_pos && src_pos < data + j) {
                                const char *before_src_end = src_pos;
                                cwist_sstring_append_len(out, after_name, (size_t)(before_src_end - after_name));
                                const char *src_end = strchr(src_pos + 5, '"');
                                if (src_end && src_end < data + j) {
                                    src_end++;
                                    cwist_sstring_append_len(out, src_end, (size_t)((data + j) - src_end));
                                }
                            } else {
                                cwist_sstring_append_len(out, data + i + strlen(tag_name) + 1, j - i - strlen(tag_name) - 1);
                            }
                            i = j;
                            continue;
                        }
                    }
                } else if (is_link) {
                    const char *href = strstr(tag, "href=\"");
                    if (href) {
                        href += 6;
                        const char *target = NULL;
                        if (strncmp(href, "/file/download/", 15) == 0) {
                            target = href;
                        } else if (strncmp(href, "https://oborona.zip/file/download/", 34) == 0) {
                            target = href + 19;
                        } else if (strncmp(href, "http://oborona.zip/file/download/", 33) == 0) {
                            target = href + 18;
                        }
                        if (target) {
                            cwist_sstring_append(out, "<a href=\"#\" data-tasfa-download-link=\"");
                            const char *value_end = strchr(target, '"');
                            if (value_end) cwist_sstring_append_len(out, target, (size_t)(value_end - target));
                            cwist_sstring_append(out, "\"");
                            const char *after_name = data + i + 2;
                            const char *href_pos = strstr(after_name, "href=\"");
                            if (href_pos && href_pos < data + j) {
                                cwist_sstring_append_len(out, after_name, (size_t)(href_pos - after_name));
                                const char *href_end = strchr(href_pos + 6, '"');
                                if (href_end && href_end < data + j) {
                                    href_end++;
                                    cwist_sstring_append_len(out, href_end, (size_t)((data + j) - href_end));
                                }
                            } else {
                                cwist_sstring_append_len(out, data + i + 2, j - i - 2);
                            }
                            i = j;
                            continue;
                        }
                    }
                }
            }
        }
        cwist_sstring_append_len(out, data + i, 1);
        i++;
    }
    cwist_sstring_assign(html, out->data);
    cwist_sstring_destroy(out);
}

static void inject_img_attrs(cwist_sstring *html) {
    const char *data = html->data;
    size_t len = strlen(data);
    if (len == 0) return;

    cwist_sstring *out = cwist_sstring_create();
    size_t i = 0;
    int img_count = 0;

    while (i < len) {
        if (i + 4 <= len && strncmp(data + i, "<img", 4) == 0) {
            size_t tag_start = i;
            size_t j = i + 4;
            while (j < len && data[j] != '>') j++;
            if (j < len) j++; /* include '>' */

            /* Check if already has width/height inside this tag */
            bool has_width = false, has_height = false;
            const char *scan = data + tag_start;
            const char *tag_end_ptr = data + j;
            while (scan < tag_end_ptr - 6) {
                if (strncmp(scan, "width=", 6) == 0) has_width = true;
                if (strncmp(scan, "height=", 7) == 0) has_height = true;
                scan++;
            }

            /* Extract src */
            char src[512] = {0};
            const char *src_attr = strstr(data + tag_start, "src=\"");
            if (src_attr && src_attr < data + j) {
                src_attr += 5;
                const char *src_end = strchr(src_attr, '"');
                if (src_end && src_end < data + j) {
                    size_t src_len = (size_t)(src_end - src_attr);
                    if (src_len >= sizeof(src)) src_len = sizeof(src) - 1;
                    strncpy(src, src_attr, src_len);
                    src[src_len] = '\0';
                }
            }

            /* Copy tag content without the closing '>' */
            size_t close_pos = j - 1;
            cwist_sstring_append_len(out, data + tag_start, close_pos - tag_start);

            if (src[0] && !has_width && !has_height) {
                char path[512] = {0};
                if (strncmp(src, "/assets/img/", 12) == 0) {
                    snprintf(path, sizeof(path), "public/img/%s", src + 12);
                } else if (strncmp(src, "/assets/uploads/", 16) == 0) {
                    snprintf(path, sizeof(path), "public/uploads/%s", src + 16);
                } else if (strncmp(src, "/assets/profile/", 16) == 0) {
                    snprintf(path, sizeof(path), "public/profile/%s", src + 16);
                }

                if (path[0]) {
                    int w, h;
                    if (get_image_dimensions(path, &w, &h)) {
                        char attrs[256];
                        snprintf(attrs, sizeof(attrs),
                                 " width=\"%d\" height=\"%d\" style=\"aspect-ratio:%d/%d;background:var(--hover)\"",
                                 w, h, w, h);
                        cwist_sstring_append(out, attrs);
                    }
                }
            }

            /* loading / decoding */
            bool has_loading = false, has_decoding = false;
            scan = data + tag_start;
            while (scan < tag_end_ptr - 9) {
                if (strncmp(scan, "loading=", 8) == 0) has_loading = true;
                if (strncmp(scan, "decoding=", 9) == 0) has_decoding = true;
                scan++;
            }

            if (!has_loading) {
                if (img_count == 0) {
                    cwist_sstring_append(out, " loading=\"eager\" fetchpriority=\"high\"");
                } else {
                    cwist_sstring_append(out, " loading=\"lazy\"");
                }
            }
            if (!has_decoding) {
                cwist_sstring_append(out, " decoding=\"async\"");
            }

            cwist_sstring_append(out, ">"); /* close tag */
            img_count++;
            i = j;
        } else {
            cwist_sstring_append_len(out, data + i, 1);
            i++;
        }
    }

    cwist_sstring_assign(html, out->data);
    cwist_sstring_destroy(out);
}

cwist_sstring *render_markdown_to_html(const char *md) {
    math_registry_t blocks = {0}, inlines = {0}, media = {0}, tikz = {0};
    math_registry_init(&blocks);
    math_registry_init(&inlines);
    math_registry_init(&media);
    math_registry_init(&tikz);
    char *protected = protect_math(md, &blocks, &inlines, &media, &tikz);

    cwist_sstring *html = cwist_sstring_create();
    if (!html) {
        free(protected);
        math_registry_free(&blocks);
        math_registry_free(&inlines);
        math_registry_free(&media);
        math_registry_free(&tikz);
        return NULL;
    }
    cwist_sstring_assign(html, "");
    unsigned flags = MD_DIALECT_GITHUB | MD_FLAG_TABLES | MD_FLAG_PERMISSIVEAUTOLINKS | MD_FLAG_STRIKETHROUGH | MD_FLAG_TASKLISTS | MD_FLAG_LATEXMATHSPANS | MD_FLAG_NOHTML;
    int rc = md_html(protected, (MD_SIZE)strlen(protected), md_output_cb, html, flags, 0);
    free(protected);
    if (rc != 0) {
        cwist_sstring_destroy(html);
        math_registry_free(&blocks);
        math_registry_free(&inlines);
        math_registry_free(&media);
        math_registry_free(&tikz);
        return NULL;
    }
    restore_math(html, &blocks, &inlines);
    restore_media(html, &media);
    restore_tikz(html, &tikz);
    math_registry_free(&blocks);
    math_registry_free(&inlines);
    math_registry_free(&media);
    math_registry_free(&tikz);
    rewrite_x_equation(html);
    inject_img_attrs(html);
    rewrite_tasfa_bootstrap(html);
    return html;
}

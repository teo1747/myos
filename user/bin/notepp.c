/* user/bin/notepp.c -- Note++, the OS's own code editor.
 *
 * user/bin/edit.c is the one-file editor: a buffer, a Save button, the path
 * Files handed it. This is the other thing -- several files at once, a caret
 * you can drive from the keyboard, selection, undo, find and replace, and the
 * code coloured the way this OS's own compiler reads it.
 *
 * WHAT IS HERE AND WHAT IS NOT. Everything that happens to TEXT lives in
 * user/note/edit.c and is covered by 45 host tests, because an editor is
 * mostly edge cases and none of them are worth discovering on a screen. This
 * file is the part that cannot be tested that way: pixels, keys and files.
 *
 * It draws its own text rather than using the toolkit's TextEditor, which
 * cannot show a selection, a current line or a match, and owns the keyboard in
 * a way an editor's shortcuts cannot share.
 *
 * The chrome is the shape Vellum arrived at: the window's lights share the tab
 * strip, because a title bar has nothing to say that the tab does not.
 * Monospace throughout, chosen the way term.c chooses it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "embk.h"
#include "ui.h"
#include "em.h"
#include "theme.h"
#include "syntax.h"
#include "doc.h"
#include "edit.h"

/* ---- palette ------------------------------------------------------------ */

static Color rgb(unsigned v) {
    Color c;
    c.r = (float)((v >> 16) & 0xFF) / 255.0f;
    c.g = (float)((v >>  8) & 0xFF) / 255.0f;
    c.b = (float)( v        & 0xFF) / 255.0f;
    c.a = (float)((v >> 24) & 0xFF) / 255.0f;
    return c;
}

/* One colour per syntax role. Stated here rather than taken from the theme: a
 * syntax palette has to keep seven things apart at body size on one
 * background, which is a different problem from making a button look pressed. */
static Color role_color(int role) {
    switch (role) {
        case SYN_KEYWORD: return rgb(0xFFFF7AB2);
        case SYN_TYPE:    return rgb(0xFF6BDFFF);
        case SYN_STRING:  return rgb(0xFFFF8170);
        case SYN_COMMENT: return rgb(0xFF7E8B99);
        case SYN_NUMBER:  return rgb(0xFFD9C97C);
        case SYN_PREPROC: return rgb(0xFFB281EB);
        case SYN_PUNCT:   return rgb(0xFFA6B0BB);
        default:          return rgb(0xFFE6E9ED);
    }
}
#define C_GUTTER      rgb(0xFF4E5967)
#define C_GUTTER_CUR  rgb(0xFFB9C4D0)
#define C_CURLINE     rgb(0x24303C4A)
#define C_SEL         rgb(0x593B6FB5)
#define C_CARET       rgb(0xFF7FB2FF)
#define C_EDITOR_BG   rgb(0xFF16181D)
#define C_MATCH       rgb(0x66FFC24B)   /* the bracket partner */
#define C_FIND        rgb(0x4CE0A33A)   /* every other search hit */
#define TAB_ON        rgb(0xFF3A3A3C)
#define TAB_OFF       rgb(0x00000000)
#define TAB_TEXT_ON   rgb(0xFFF2F2F7)
#define TAB_TEXT_OFF  rgb(0xFF98989E)

/* ---- state --------------------------------------------------------------- */

/* The height of one line of code, everywhere: the row, the scroller, and the
 * page keys all have to agree or the caret leaves the screen. */
#define NOTE_LINE_H 20.0f
/* How much of one line the highlighter is given. Beyond this a line is not
 * coloured past the cap -- it is still all there, still editable, still saved. */
#define NOTE_LINE_MAX 4096

static struct editor ED;          /* the engine, re-pointed at the current doc */
static int   g_bound = -1;        /* which doc slot ED currently wraps         */
static char  g_msg[96];
static char  g_path_field[DOC_PATH_MAX];
static int   g_want_open, g_want_new, g_switch_to = -1, g_close = -1;
static int   g_last_doc = -1;

/* find / replace */
static int   g_find_open, g_replace_open, g_find_icase = 1;
static char  g_find_buf[80], g_repl_buf[80];

static float g_scroll;            /* first visible line                        */
static int   g_hscroll;           /* first visible COLUMN                      */
static int   g_cols = 80;         /* columns that fit, recomputed from the box */
static int   g_rows = 20;         /* visible lines, recomputed from the window */
static int   g_match = -1;        /* bracket matching the caret's, or -1       */
static float g_doc_top;           /* screen y of the first drawn line          */
static float g_char_w;            /* one monospace advance, measured once      */
static float g_gutter_w;          /* the number column, in pixels              */
static int   g_dragging;          /* a selection drag is in progress           */
static int   g_confirm_close = -1;/* a modified document asking before it goes */
static uint64_t g_last_click_ms;  /* for double / triple click                 */
static int   g_click_streak;
static int   g_last_click_off = -1;


/* ---- files --------------------------------------------------------------- */

static long file_read(const char *path, char *buf, size_t cap) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    size_t n = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[n] = 0;
    return (long)n;
}

/* Point the engine at whichever document is current. The engine holds a
 * pointer INTO the document's buffer rather than a copy, so switching tabs is
 * re-binding and never a save. */
static void bind_current(void) {
    struct doc *d = doc_cur();
    if (!d) return;
    /* Already bound: leave it alone. Copying the document's cursor back in here
     * looked harmless and was not -- the key hook runs BETWEEN frames, so every
     * keystroke moved the engine's caret and the next frame put it back where
     * the document last remembered it. Typing "abc" left the caret at zero with
     * a two-character selection behind it. While a document is bound the ENGINE
     * owns the caret; the document only needs it when switching away. */
    if (g_bound == doc_current()) return;
    ed_init(&ED, d->text, DOC_CAP);
    ED.cursor = ED.anchor = d->cursor <= ED.len ? d->cursor : 0;
    g_bound = doc_current();
}
static void sync_out(void) {
    struct doc *d = doc_cur();
    if (d) d->cursor = ED.cursor;
}

static void session_save(void);   /* defined below; save records the session */

static void save_current(void) {
    struct doc *d = doc_cur();
    if (!d) return;
    if (!d->path[0]) { snprintf(g_msg, sizeof g_msg, "No name -- type a path above"); return; }
    if (d->truncated) {
        snprintf(g_msg, sizeof g_msg, "REFUSED: opened truncated at %d KB", DOC_CAP / 1024);
        return;
    }
    FILE *f = fopen(d->path, "w");
    if (!f) { snprintf(g_msg, sizeof g_msg, "Cannot write %.60s", d->path); return; }
    size_t len = strlen(d->text), w = fwrite(d->text, 1, len, f);
    fclose(f);
    if (w == len) { doc_mark_saved(doc_current());
                    snprintf(g_msg, sizeof g_msg, "Saved %lu bytes", (unsigned long)w);
                    session_save(); }
    else snprintf(g_msg, sizeof g_msg, "PARTIAL WRITE %lu of %lu",
                  (unsigned long)w, (unsigned long)len);
}

/* The token that starts a line comment, per language. Markdown and plain text
 * have none, and a comment toggle that invents one would corrupt the file. */
static const char *comment_tok(enum syn_lang l) {
    switch (l) {
        case SYN_C: case SYN_JS: return "//";
        case SYN_PY: case SYN_SH: return "#";
        default: return 0;
    }
}

static enum syn_lang d_lang(void) {
    struct doc *d = doc_cur();
    return d ? d->lang : SYN_PLAIN;
}

/* ---- the file picker -----------------------------------------------------
 * A path field alone means the only way to find out where you may write is to
 * type somewhere and read the error. This app can name exactly two subtrees --
 * $HOME to read and write, /system to read (notepp.ns) -- and a list is a far
 * better way to say that than a refusal after the fact. */
#define PICK_MAX 64
static int  g_pick_open;
static char g_pick_dir[DOC_PATH_MAX];
static struct embk_dirent g_pick[PICK_MAX];
static int  g_pick_n;
static float g_pick_scroll;
static int  g_pick_writable;      /* is the listed directory writable BY US? */

/* CAN WE WRITE HERE? Asked by trying, not by consulting a copy of the manifest.
 * The app's authority is declared in notepp.ns, which the app cannot read (its
 * namespace does not include /data/apps), and a second list of the same fact
 * kept here would be a list that drifts. Creating a file and removing it again
 * asks the kernel the real question and gets the real answer, including for a
 * directory that is writable for a reason nobody wrote down.
 *
 * Once per directory listed, not once per entry: the point is which PLACES
 * accept a save, and every entry in one directory shares that answer. */
static int dir_writable(const char *dir) {
    char probe[DOC_PATH_MAX + 24];
    snprintf(probe, sizeof probe, "%s%s.notepp-probe", dir,
             strcmp(dir, "/") ? "/" : "");
    FILE *f = fopen(probe, "w");
    if (!f) return 0;
    fclose(f);
    embk_unlink(probe);
    return 1;
}

static void pick_scan(const char *dir) {
    snprintf(g_pick_dir, sizeof g_pick_dir, "%s", dir);
    int64_t n = embk_readdir(g_pick_dir, g_pick, PICK_MAX);
    g_pick_n = n < 0 ? 0 : (int)n;
    g_pick_writable = dir_writable(g_pick_dir);
    g_pick_scroll = 0;
}

static void pick_begin(void) {
    const char *h = getenv("HOME");
    /* Start where the caret's file lives, else at home -- the two places a
     * person is most likely to mean. */
    struct doc *d = doc_cur();
    char start[DOC_PATH_MAX];
    if (d && d->path[0]) {
        snprintf(start, sizeof start, "%s", d->path);
        char *slash = start;
        for (char *q = start; *q; q++) if (*q == '/') slash = q;
        if (slash != start) *slash = 0; else start[1] = 0;
    } else snprintf(start, sizeof start, "%s", (h && h[0]) ? h : "/");
    pick_scan(start);
    g_pick_open = 1;
}

/* ---- the session ---------------------------------------------------------
 * Which files were open, and where the caret was in each. Reopening an editor
 * to an empty Untitled after it was full of work is the friction you meet
 * every single launch, and it is a dozen lines to not have.
 *
 * A flat text file under $HOME rather than anything structured: it is a list of
 * paths, a person may want to read or delete it, and a format nobody can
 * inspect is a format nobody can fix. Written whenever the set of documents
 * changes -- not on every keystroke, which would be a disk write per character
 * for a fact that has not changed. */
static void session_path(char *out, size_t cap) {
    const char *h = getenv("HOME");
    snprintf(out, cap, "%s/.notepp-session", (h && h[0]) ? h : "/data");
}

static void session_save(void) {
    char sp[256]; session_path(sp, sizeof sp);
    FILE *f = fopen(sp, "w");
    if (!f) return;
    for (int i = 0; i < DOC_MAX; i++) {
        struct doc *d = doc_at(i);
        if (!d || !d->path[0]) continue;        /* an unnamed buffer has nowhere to come back from */
        fprintf(f, "%d %d %s\n", i == doc_current(), d->cursor, d->path);
    }
    fclose(f);
}

static void session_load(void) {
    char sp[256]; session_path(sp, sizeof sp);
    FILE *f = fopen(sp, "r");
    if (!f) return;
    char line[400];
    int want = -1;
    while (fgets(line, sizeof line, f)) {
        int cur = 0, off = 0;
        char path[DOC_PATH_MAX];
        if (sscanf(line, "%d %d %255[^\n]", &cur, &off, path) != 3) continue;
        int i = doc_open(path, file_read);
        if (i < 0) continue;                    /* out of slots: keep what loaded */
        struct doc *d = doc_at(i);
        /* The file may have changed since -- shorter, or gone. Clamp rather
         * than restore a caret past the end of it. */
        if (d && off >= 0 && off <= (int)strlen(d->text)) d->cursor = off;
        if (cur) want = i;
    }
    fclose(f);
    if (want >= 0) doc_select(want);
}

/* ---- scrolling ----------------------------------------------------------- */

/* Keep the caret on screen. Called after anything that moves it -- which is
 * why it lives in one place rather than at every call site. */
static void scroll_to_caret(void) {
    int line = ed_line_of(&ED, ED.cursor);
    int first = (int)g_scroll;
    if (line < first)                first = line;
    else if (line >= first + g_rows) first = line - g_rows + 1;
    if (first < 0) first = 0;
    g_scroll = (float)first;

    /* ...and sideways. Without this a caret driven past the right edge simply
     * stopped being visible, which is the same class of lie as a line whose
     * tail is silently clipped: the editor knows where the caret is and the
     * screen does not say. A margin of a few columns keeps some context ahead
     * of it rather than pinning it to the very edge. */
    int col = ed_col_of(&ED, ED.cursor);
    if (col < g_hscroll + 4)            g_hscroll = col - 4;
    else if (col >= g_hscroll + g_cols - 4) g_hscroll = col - g_cols + 5;
    if (g_hscroll < 0) g_hscroll = 0;
}

/* ---- keyboard ------------------------------------------------------------ */

/* The editor owns the keyboard, so every shortcut is decided here. Returning 1
 * means "handled" and stops the toolkit seeing it -- which is how Ctrl+A can
 * mean select-all in the document and still type an 'a' into the find field. */
static int on_key(int ch) {
    int mods  = embk_key_mods();
    int ctrl  = (mods & 0x02) != 0;
    int shift = (mods & 0x01) != 0;

    /* A TEXT FIELD HAS THE KEYS WHEN IT HAS FOCUS. The hook sees every key in
     * the window, and an editor's hook handles nearly all of them -- so
     * without this the path field and the find box could not be typed into at
     * all: every character went into the document behind them. Save is the one
     * exception, because Ctrl+S means the same thing wherever you are. */
    if (ui_any_focus()) {
        if (ctrl && ch == 19) { save_current(); return 1; }   /* Ctrl+S */
        if (ch == 27) { g_find_open = g_replace_open = 0; return 0; }
        return 0;
    }

    /* PASTE, taken raw. The runtime would otherwise replay the clipboard
     * through this hook with newlines flattened to spaces, which is the right
     * rule for a terminal and turns a pasted function into one long line here.
     * Taking 0x16 ourselves keeps the line breaks. */
    if (ch == 0x16) {
        static char clip[8192];
        int64_t n = embk_clip_get(clip, sizeof clip - 1);
        if (n > 0) {
            clip[n] = 0;
            ed_insert_text(&ED, clip, (int)n);
            scroll_to_caret();
            snprintf(g_msg, sizeof g_msg, "%d bytes pasted", (int)n);
        }
        return 1;
    }

    /* A PRINTABLE CHARACTER IS NEVER A SHORTCUT, whatever the modifier keys say.
     * Ctrl+letter arrives as a control code 1..26; a plain byte with Ctrl still
     * physically held is the RUNTIME REPLAYING A PASTE through this hook. Those
     * were being read as shortcuts, so pasting any text containing an 'l' ran
     * delete-line and pasting a 'd' duplicated one -- the paste vanished and
     * the file lost a line instead. */
    if (ctrl && ch >= 32 && ch < 127) ctrl = 0;

    if (ctrl) {
        /* Ctrl+letter arrives as a CONTROL CODE, 1..26 -- which is why the
         * runtime tests for 0x16 to catch Ctrl+V. Masking with |32 turns
         * Ctrl+Z (0x1A) into ':' and matches nothing, so every shortcut here
         * silently did nothing. Decode it back to the letter. */
        int key = (ch >= 1 && ch <= 26) ? ('a' + ch - 1) : (ch | 32);
        switch (key) {
            case 's': save_current(); return 1;
            case 'n': g_want_new = 1; return 1;
            case 'o': g_want_open = 1; return 1;
            case 'w': g_close = doc_current(); return 1;
            case 'a': ed_select_all(&ED); return 1;
            case 'z': if (shift) ed_redo(&ED); else ed_undo(&ED); scroll_to_caret(); return 1;
            case 'y': ed_redo(&ED); scroll_to_caret(); return 1;
            case 'f': g_find_open = 1; g_replace_open = 0; return 1;
            case 'h': g_find_open = g_replace_open = 1; return 1;
            case 'd': ed_duplicate_line(&ED); scroll_to_caret(); return 1;
            case 'l': ed_delete_line(&ED); scroll_to_caret(); return 1;
            case '/': { const char *tk = comment_tok(d_lang());
                        if (tk) ed_toggle_comment(&ED, tk);
                        scroll_to_caret(); return 1; }
            case 'g': { /* go to line: the number is typed into the find field */
                        g_find_open = 1;
                        snprintf(g_msg, sizeof g_msg, "Type a line number, then Enter");
                        return 1; }
            case 'c': case 'x': {
                char tmp[4096];
                int n = ed_copy(&ED, tmp, sizeof tmp);
                if (n) { embk_clip_set(tmp, (size_t)n);
                         if (key == 'x') ed_delete_sel(&ED);
                         snprintf(g_msg, sizeof g_msg, "%d bytes %s", n,
                                  key == 'x' ? "cut" : "copied"); }
                return 1; }
            /* 'v' never reaches here: Ctrl+V arrives as 0x16 and is taken
             * above, raw, so the line breaks survive. */
            default: break;
        }
        /* Ctrl + arrows: by word, and by line for up/down. */
        if (ch == 0x11) { ed_move_word(&ED, -1, shift); scroll_to_caret(); return 1; }
        if (ch == 0x12) { ed_move_word(&ED, +1, shift); scroll_to_caret(); return 1; }
        if (ch == 0x13) { ed_move_line(&ED, -1); scroll_to_caret(); return 1; }
        if (ch == 0x14) { ed_move_line(&ED, +1); scroll_to_caret(); return 1; }
        if (ch == 0x02) { ed_doc_start(&ED, shift); scroll_to_caret(); return 1; }
        if (ch == 0x05) { ed_doc_end(&ED, shift);   scroll_to_caret(); return 1; }
        return 1;                      /* swallow unknown Ctrl combinations */
    }

    switch (ch) {
        case 0x11: ed_move(&ED, -1, shift); break;
        case 0x12: ed_move(&ED, +1, shift); break;
        case 0x13: ed_move_line_v(&ED, -1, shift); break;
        case 0x14: ed_move_line_v(&ED, +1, shift); break;
        case 0x02: ed_home(&ED, shift); break;
        case 0x05: ed_end(&ED, shift); break;
        case 0x7F: ed_delete(&ED); break;
        case '\b': ed_backspace(&ED); break;
        case '\n': case '\r': ed_newline(&ED); break;
        case '\t': if (shift) ed_outdent(&ED); else ed_indent(&ED); break;
        case 27:   if (g_find_open) { g_find_open = g_replace_open = 0; break; }
                   return 0;           /* Esc with no find bar quits the app */
        default:
            if (ch >= 32 && ch < 127) {
                /* AUTO-CLOSE. An opener brings its partner and leaves the caret
                 * between them. Typing the CLOSER when it is already the next
                 * character just steps over it, so the ordinary case of typing
                 * a whole call never leaves a stray one behind. */
                char close = ed_auto_close((char)ch);
                int over = (ED.cursor < ED.len && ED.buf[ED.cursor] == (char)ch &&
                            (ch == ')' || ch == ']' || ch == '}' ||
                             ch == '"' || ch == '\''));
                if (over)                      ed_move(&ED, +1, 0);
                else if (close && !ed_has_sel(&ED)) {
                    char pair[2] = { (char)ch, close };
                    ed_insert_text(&ED, pair, 2);
                    ED.cursor = ED.anchor = ED.cursor - 1;
                } else                          ed_insert_char(&ED, (char)ch);
            }
            else return 0;
            break;
    }
    scroll_to_caret();
    return 1;
}

/* ---- pointer -> caret ---------------------------------------------------- *
 * The whole reason em_text_width exists: turning a click into a position in
 * the text needs the width of the text, and only the font knows it. Monospace
 * makes the column arithmetic exact rather than a search -- one advance, one
 * character -- which is another reason this editor is monospace. */

static int offset_at_point(float px, float py) {
    if (g_char_w <= 0.0f) return ED.cursor;
    int line = (int)g_scroll + (int)((py - g_doc_top) / NOTE_LINE_H);
    int last = ed_line_count(&ED) - 1;
    if (line < 0) line = 0;
    if (line > last) line = last;
    int ls = ed_offset_of_line(&ED, line), le = ed_line_end(&ED, ls);
    /* Round to the NEAREST gap between characters, not the one to the left:
     * clicking the right half of a glyph must put the caret after it, which is
     * the difference between a caret that lands where you pointed and one that
     * is always one character early. */
    int col = g_hscroll + (int)(((px - g_gutter_w) / g_char_w) + 0.5f);
    if (col < 0) col = 0;
    if (ls + col > le) col = le - ls;
    return ls + col;
}

/* ---- the document view --------------------------------------------------- */

/* One line: gutter number, then the text as coloured spans, with the selection
 * behind it and the caret dropped in at the right offset. */
static void draw_line(int line, int ls, int le, int syn_state_in, enum syn_lang lang,
                      int cur_line, int sel_lo, int sel_hi) {
    char tmp[256];
    /* The line's full length is what the HIGHLIGHTER needs -- a string opened
     * near the start decides the colour of everything after it -- but only the
     * visible columns are ever drawn. That is what removes the old 512-byte
     * draw cap: a long line is no longer truncated, it is WINDOWED, and the
     * part off the right edge is reached by scrolling rather than lost. */
    int n = le - ls;
    if (n > NOTE_LINE_MAX) n = NOTE_LINE_MAX;
    int c0 = g_hscroll, c1 = g_hscroll + g_cols;

    /* EmZero, not 0. Zero means "unset" in EmProps and the theme then supplies
     * its own padding -- which on a row of code is a third of a line of air
     * between every line, and is why this looked like a form rather than a
     * file. */
    /* A FIXED ROW HEIGHT, because a line of code is a line of code. Left to
     * size itself the row took the theme's idea of a comfortable control and
     * came out at double the type's line height -- a file with a blank line
     * between every line of it. This is also the number the scroller and
     * PgUp/PgDn count in, so it is stated once, here. */
    HStack(.spacing = EmZero, .align = Center, .px = EmZero, .py = EmZero,
           .height = NOTE_LINE_H, .minh = NOTE_LINE_H, .maxh = NOTE_LINE_H,
           .background = (line == cur_line) ? C_CURLINE : rgb(0)) {
        /* EVERY Text IS FLUSHED IMMEDIATELY, and that is not a style choice.
         * The DSL STAGES a leaf and emits it on the next one, so a Text() given
         * a stack buffer keeps a POINTER to it -- and the next loop iteration
         * overwrites that buffer before the emit happens. Every line drew
         * whatever happened to be in `tmp` at flush time: line numbers came out
         * as boxes and words appeared on the wrong lines. */
        { char num[16];
          snprintf(num, sizeof num, "%4d ", line + 1);
          Text(num).font(Body).color(line == cur_line ? C_GUTTER_CUR : C_GUTTER);
          em_flush(); }

        /* EVERY MATCH ON THIS LINE, not just the one the caret is on. A find that
         * shows you one hit at a time makes you press Next to discover whether
         * there are others; showing them all answers that by looking. The current
         * hit is the selection, so it stays the brighter of the two. */
        int mlo[48], mhi[48], mn = 0;
        if (g_find_open && g_find_buf[0]) {
            int nl = (int)strlen(g_find_buf);
            for (int m = ed_find(&ED, g_find_buf, ls, g_find_icase, 0);
                 m >= 0 && m < le && mn < 48;
                 m = ed_find(&ED, g_find_buf, m + nl, g_find_icase, 0)) {
                if (m + nl > le) break;
                mlo[mn] = m; mhi[mn] = m + nl; mn++;
            }
        }

        struct syn_span sp[128];
        int st = syn_state_in;
        int ns = (lang == SYN_PLAIN || n <= 0) ? 0
               : syn_line(lang, ED.buf + ls, n, sp, 128, &st);
        if (ns <= 0) { sp[0].len = n; sp[0].role = SYN_TEXT; ns = n > 0 ? 1 : 0; }

        int off = 0;
        for (int k = 0; k < ns; k++) {
            int sl = sp[k].len;
            if (off + sl > n) sl = n - off;
            if (sl <= 0) continue;
            /* Split the span wherever the selection or the caret begins or ends
             * inside it, so both land between characters rather than being
             * rounded to a span boundary. */
            int i = 0;
            while (i < sl) {
                int col = off + i;                    /* monospace: column == byte */
                if (col >= c1) break;                 /* past the right edge */
                int abs = ls + col;
                int in_sel = (sel_lo < sel_hi && abs >= sel_lo && abs < sel_hi);
                int in_hit = 0;
                for (int m = 0; m < mn; m++)
                    if (abs >= mlo[m] && abs < mhi[m]) { in_hit = 1; break; }
                int run = 1;
                while (i + run < sl && col + run < c1) {
                    int a2 = ls + off + i + run;
                    int s2 = (sel_lo < sel_hi && a2 >= sel_lo && a2 < sel_hi);
                    if (s2 != in_sel) break;
                    int h2 = 0;
                    for (int m = 0; m < mn; m++)
                        if (a2 >= mlo[m] && a2 < mhi[m]) { h2 = 1; break; }
                    if (h2 != in_hit) break;
                    if (a2 == ED.cursor) break;
                    run++;
                }
                if (col + run <= c0) { i += run; continue; }   /* left of the window */
                int skip = (col < c0) ? c0 - col : 0;          /* partly left of it */
                if (abs + skip == ED.cursor) { Text("|").font(Body).color(C_CARET); em_flush(); }
                int keep = run - skip;
                if (keep > (int)sizeof tmp - 1) keep = (int)sizeof tmp - 1;
                memcpy(tmp, ED.buf + abs + skip, (size_t)keep); tmp[keep] = 0;
                run = skip + keep;
                Color fg = role_color(sp[k].role);
                /* A run that carries a BACKGROUND is given its exact width. Left to
                 * size itself the tinted box grew to the next sibling, so a
                 * highlighted word painted a bar reaching across the rest of the
                 * line. Monospace makes the right number exact rather than a
                 * measurement: one advance per character. */
                float w = (float)keep * g_char_w;
                if (g_match >= 0 && abs <= g_match && g_match < abs + run)
                    Text(tmp).font(Body).color(fg).bg(C_MATCH).width(w);
                else if (in_sel)
                    Text(tmp).font(Body).color(fg).bg(C_SEL).width(w);
                else if (in_hit)
                    Text(tmp).font(Body).color(fg).bg(C_FIND).width(w);
                else
                    Text(tmp).font(Body).color(fg);
                em_flush();                       /* tmp is about to change */
                i += run;
            }
            off += sl;
        }
        { int ecol = le - ls;
          if (ED.cursor == le && ecol >= c0 && ecol <= c1)
              { Text("|").font(Body).color(C_CARET); em_flush(); } }
        if (!n && ED.cursor != le) { Text(" ").font(Body); em_flush(); }
        Spacer();
    }
}

static void document_view(float height) {
    struct doc *d = doc_cur();
    if (!d) return;
    enum syn_lang lang = d->lang;

    /* How many lines fit. Recomputed every frame so a resize is not a special
     * case, and used by PgUp/PgDn as well as by the scroller. */
    g_rows = (int)(height / NOTE_LINE_H);
    if (g_char_w > 0.0f) {
        g_cols = (int)((em_viewport_width() - g_gutter_w - 12.0f) / g_char_w);
        if (g_cols < 8) g_cols = 8;
    }
    if (g_rows < 1) g_rows = 1;

    int total = ed_line_count(&ED);
    int first = (int)g_scroll;
    if (first > total - 1) first = total - 1;
    if (first < 0) first = 0;

    int sel_lo, sel_hi; ed_sel_range(&ED, &sel_lo, &sel_hi);
    int cur_line = ed_line_of(&ED, ED.cursor);

    /* The highlighter carries state across lines, so a screen that starts in
     * the middle of a file has to know whether it starts inside a block
     * comment. Re-scan from the top: at the sizes this edits, walking the
     * lines above the viewport costs less than a frame and is always right. */
    int st = SYN_ST_NONE;
    int off = 0;
    for (int l = 0; l < first && off < ED.len; l++) {
        int le = ed_line_end(&ED, off);
        struct syn_span sp[128];
        if (lang != SYN_PLAIN) syn_line(lang, ED.buf + off, le - off, sp, 128, &st);
        off = le + 1;
    }

    /* Measured once, from the font actually in use, rather than assumed. */
    if (g_char_w <= 0.0f) {
        g_char_w   = em_text_width("M", 15.0f);
        g_gutter_w = em_text_width("    0 ", 15.0f);
    }

    /* THE TRACK, sized from what is on screen. Without it the only clue to
     * where you are in a file is the line number, which tells you the position
     * and not the PROPORTION -- line 400 means nothing until you know whether
     * the file has 420 lines or 40000. */
    float thumb = height * (float)g_rows / (float)(total > 0 ? total : 1);
    if (thumb < 24.0f) thumb = 24.0f;
    if (thumb > height) thumb = height;
    float above = (total > g_rows)
                ? (height - thumb) * (float)first / (float)(total - g_rows) : 0.0f;

    HStack(.spacing = EmZero, .align = Fill, .padding = EmZero,
           .height = height, .key = "docrow") {
    VStack(.spacing = EmZero, .align = Fill, .padding = EmZero, .grow = 1,
           .background = C_EDITOR_BG, .clip = 1,
           .height = height, .key = "doc") {
        /* THE WHEEL scrolls the view; the box only takes it while the pointer
         * is over it, so a wheel meant for something else is left alone. */
        float w = ui_take_wheel();
        if (w != 0.0f) {
            g_scroll -= w * 3.0f;
            if (g_scroll < 0) g_scroll = 0;
            if (g_scroll > (float)(total - 1)) g_scroll = (float)(total - 1);
            first = (int)g_scroll;
        }
        /* CLICK places the caret, DRAG extends the selection. ui_is_active is
         * true while the button is held on this box, which is exactly the span
         * of a drag -- so press, move and release need no state of their own
         * beyond remembering that one began. */
        { float px, py; ui_pointer_pos(&px, &py);
          int at = offset_at_point(px, py);

          /* DRAGGING is sampled -- it is a state, true for as long as the
           * button is down on this box. */
          int shift_held = (embk_key_mods() & 0x01) != 0;
          if (ui_is_active()) {
              /* SHIFT-CLICK EXTENDS from where the selection already starts,
               * rather than beginning a new one -- the standard way to grow a
               * selection past the edge of the window without dragging. */
              if (!g_dragging) {
                  g_dragging = 1;
                  if (shift_held) ED.cursor = at;
                  else            ED.anchor = ED.cursor = at;
              }
              else if (g_click_streak <= 1) ED.cursor = at;
              em_request_frame();
          } else if (g_dragging) {
              g_dragging = 0;
          }

          /* COUNTING CLICKS is an EDGE, and the toolkit counts them now --
           * ui_take_press_edges. Neither ui_is_active (a state, sampled once a
           * frame) nor ui_consume_click (which remembers only the LAST press)
           * could tell one click from three: three presses fit easily between
           * two frames under an emulator, and the earlier ones were simply
           * never seen. */
          uint64_t press_ms = 0;
          int presses = em_take_clicks(&press_ms);
          if (presses) {
              /* THE PRESSES ARRIVE IN BATCHES, and that is the whole bug. The
               * loop hands over however many edges happened since it last
               * looked -- under an emulator a double click is routinely BOTH
               * presses in one frame -- and adding one per frame turned three
               * rapid clicks into a streak of one, every time.
               *
               * Count what arrived, not how many times we looked. Two edges in
               * one frame IS a double click: they cannot have been further
               * apart than a frame. */
              uint64_t now = press_ms;   /* the PRESS's time, not this frame's */
              int near = (g_last_click_off >= 0 && at >= g_last_click_off - 1
                                                && at <= g_last_click_off + 1);
              if (near && now - g_last_click_ms < 500) g_click_streak += presses;
              else                                     g_click_streak  = presses;
              g_last_click_ms = now;
              g_last_click_off = at;
              /* A shifted click is an extension, never a double click: two of
               * them in a row mean two extensions, not a word. */
              if (shift_held)               g_click_streak = 1;
              else if (g_click_streak >= 3) ed_select_line(&ED, at);
              else if (g_click_streak == 2) ed_select_word(&ED, at);
              em_request_frame();
          }
        }
        g_doc_top = em_viewport_height() - height - 26.0f;   /* the rows start here */
        for (int l = first; l < total && l < first + g_rows; l++) {
            int ls = off;
            int le = ed_line_end(&ED, ls);
            int st_in = st;
            if (lang != SYN_PLAIN) {
                struct syn_span sp[128];
                syn_line(lang, ED.buf + ls, le - ls, sp, 128, &st);
            }
            draw_line(l, ls, le, st_in, lang, cur_line, sel_lo, sel_hi);
            off = le + 1;
            if (le >= ED.len) break;
        }
        Spacer();
    }
    /* Only when there is something to scroll. A full-height thumb on a short
     * file is a control that says nothing and takes width to say it. */
    if (total > g_rows) {
        VStack(.spacing = EmZero, .align = Fill, .padding = EmZero,
               .width = 8, .height = height, .background = C_EDITOR_BG,
               .key = "track") {
            if (above > 0) { VStack(.height = above, .key = "above") { } }
            VStack(.height = thumb, .corner = 3, .background = C_GUTTER,
                   .key = "thumb") { }
            Spacer();
        }
    }
    }
}

/* ---- the app ------------------------------------------------------------- */

static void app(void) {
    /* The editor owns the keyboard; installed once, on the first frame, the
     * way vellum installs its own. */
    static int wired = 0;
    if (!wired) {
        wired = 1;
        em_set_key_hook(on_key);
        doc_init();
        session_load();          /* before the first bind, so it opens onto a real file */
        g_bound = -1;
    }
    doc_init();
    bind_current();

    if (g_want_new)  { g_want_new = 0; doc_new(); g_path_field[0] = 0; g_bound = -1; session_save(); }
    if (g_want_open) {
        g_want_open = 0;
        int i = doc_open(g_path_field, file_read);
        if (i < 0) snprintf(g_msg, sizeof g_msg, "No room for another file");
        else { struct doc *dd = doc_at(i);
               snprintf(g_msg, sizeof g_msg, "%s%s", dd->text[0] ? "Opened" : "New file",
                        dd->truncated ? " (TRUNCATED -- will not save)" : "");
               g_bound = -1; session_save(); }
    }
    if (g_switch_to >= 0) { sync_out(); doc_select(g_switch_to); g_switch_to = -1; g_bound = -1; session_save(); }
    if (g_close >= 0) {
        /* ASK BEFORE LOSING EDITS. Closing a modified document used to just
         * take it, and that is the only item on this app's list that costs
         * something you cannot get back. */
        if (doc_dirty(g_close)) { g_confirm_close = g_close; g_close = -1; }
        else { doc_close(g_close); g_close = -1; g_bound = -1; session_save(); }
    }
    bind_current();

    struct doc *d = doc_cur();
    if (doc_current() != g_last_doc) {
        g_last_doc = doc_current();
        snprintf(g_path_field, sizeof g_path_field, "%s", d ? d->path : "");
        g_msg[0] = 0;
        g_scroll = 0;
    }
    g_match = ed_match_bracket(&ED, ED.cursor);
    sync_out();

    float chrome = 96.0f + (g_find_open ? 34.0f : 0.0f) + (g_replace_open ? 30.0f : 0.0f);

    Window("Note++") {
        /* ROW 1 -- the open files, and the window's lights. */
        HStack(.spacing = 3, .align = Center, .px = 8, .py = 4, .key = "tabs") {
            CloseButton(); MinimizeButton();
            static const char *KEY[DOC_MAX]  = { "d0","d1","d2","d3","d4","d5","d6","d7" };
            static const char *KEYL[DOC_MAX] = { "l0","l1","l2","l3","l4","l5","l6","l7" };
            static const char *KEYX[DOC_MAX] = { "x0","x1","x2","x3","x4","x5","x6","x7" };
            for (int i = 0; i < DOC_MAX; i++) {
                if (!doc_at(i)) continue;
                int cur = (i == doc_current());
                HStack(.spacing = 0, .align = Center, .px = 3, .corner = 8,
                       .background = cur ? TAB_ON : TAB_OFF, .key = KEY[i]) {
                    EmV tb = Button(doc_label(i)).ghost().font(Caption).py(3).px(7).id(KEYL[i]);
                    tb.color(cur ? TAB_TEXT_ON : TAB_TEXT_OFF);
                    if (tb.clicked() && !cur) g_switch_to = i;
                    if (cur && doc_count() > 1 &&
                        Button("\xc3\x97").ghost().font(Caption).py(3).px(5)
                            .color(TAB_TEXT_OFF).id(KEYX[i]).clicked())
                        g_close = i;
                }
            }
            if (Button("+").ghost().font(Caption).py(3).px(7).id("dnew").clicked()) g_want_new = 1;
            DragHandle(.key = "tabdrag") { }
        }
        Divider("tabsep");

        /* ROW 2 -- the path and what can be done with it. */
        HStack(.spacing = 6, .align = Center, .px = 8, .py = 5, .key = "pathrow") {
            if (TextField(g_path_field, sizeof g_path_field, "Path to open or save as").submitted())
                g_want_open = 1;
            if (Button("Open").ghost().font(Caption).py(2).clicked()) g_want_open = 1;
            if (Button("Browse").ghost().font(Caption).py(2).clicked()) pick_begin();
            if (Button("Save").primary().font(Caption).py(2).clicked()) {
                if (d && strcmp(d->path, g_path_field)) {
                    snprintf(d->path, sizeof d->path, "%s", g_path_field);
                    d->lang = syn_lang_of(d->path);
                }
                save_current();
            }
        }

        /* THE FIND BAR, only when it is open -- an editor that always shows one
         * has given a row of the file to something you use occasionally. */
        if (g_find_open) {
            Divider("findsep");
            HStack(.spacing = 6, .align = Center, .px = 8, .py = 3, .key = "findrow") {
                Text("Find").caption().tertiary();
                /* Return in the find box means "find the next one" -- the gesture
                 * everybody already has in their fingers. */
                if (TextField(g_find_buf, sizeof g_find_buf, "text to find").submitted()) {
                    int at = ed_find(&ED, g_find_buf, ED.cursor + 1, g_find_icase, 1);
                    if (at >= 0) { ED.anchor = at; ED.cursor = at + (int)strlen(g_find_buf);
                                   scroll_to_caret(); }
                    else snprintf(g_msg, sizeof g_msg, "Not found: %.40s", g_find_buf);
                }
                if (Button("Prev").ghost().font(Caption).py(2).clicked()) {
                    int at = ed_find_prev(&ED, g_find_buf, ED.cursor - 1, g_find_icase);
                    if (at >= 0) { ED.anchor = at; ED.cursor = at + (int)strlen(g_find_buf); scroll_to_caret(); }
                }
                if (Button("Next").ghost().font(Caption).py(2).clicked()) {
                    int at = ed_find(&ED, g_find_buf, ED.cursor + 1, g_find_icase, 1);
                    if (at >= 0) { ED.anchor = at; ED.cursor = at + (int)strlen(g_find_buf); scroll_to_caret(); }
                }
                { char cnt[32];
                  int nm = ed_count(&ED, g_find_buf, g_find_icase);
                  int ix = ed_match_index(&ED, g_find_buf, g_find_icase);
                  if (!g_find_buf[0]) cnt[0] = 0;
                  else if (!nm)       snprintf(cnt, sizeof cnt, "none");
                  else                snprintf(cnt, sizeof cnt, "%d of %d", ix, nm);
                  if (cnt[0]) { Text(cnt).caption().tertiary(); em_flush(); } }
                if (Button(g_find_icase ? "Aa off" : "Aa on").ghost().font(Caption).py(2).clicked())
                    g_find_icase = !g_find_icase;
                if (Button("Done").ghost().font(Caption).py(2).clicked())
                    { g_find_open = g_replace_open = 0; }
            }
        }
        if (g_replace_open) {
            HStack(.spacing = 6, .align = Center, .px = 8, .py = 3, .key = "replrow") {
                Text("With").caption().tertiary();
                TextField(g_repl_buf, sizeof g_repl_buf, "replacement");
                if (Button("Replace").ghost().font(Caption).py(2).clicked())
                    ed_replace(&ED, g_find_buf, g_repl_buf, g_find_icase);
                if (Button("All").ghost().font(Caption).py(2).clicked()) {
                    int n = ed_replace_all(&ED, g_find_buf, g_repl_buf, g_find_icase);
                    snprintf(g_msg, sizeof g_msg, "%d replaced", n);
                }
            }
        }
        Divider("editsep");

        document_view(em_viewport_height() - chrome);

        if (g_pick_open) {
            Overlay() {
                Dialog(.width = 460, .spacing = 10, .padding = 18) {
                    HStack(.align = Center, .spacing = 8) {
                        Text(g_pick_dir).caption().secondary(); em_flush();
                        /* Say it here, once, rather than letting a save find out
                         * later: this is the question the picker exists to
                         * answer. */
                        Text(g_pick_writable ? "writable" : "read-only")
                            .caption().color(g_pick_writable ? rgb(0xFF7FC98A) : rgb(0xFFD08A6A));
                        em_flush();
                        Spacer();
                        if (Button("Close").ghost().font(Caption).py(2).clicked())
                            g_pick_open = 0;
                    }
                    ScrollView(&g_pick_scroll, 300, .key = "picklist") {
                        VStack(.spacing = 2, .align = Fill) {
                            /* Up first, always -- a picker you cannot leave is a
                             * trap, and the root has no parent to offer. */
                            if (strcmp(g_pick_dir, "/") != 0 &&
                                Button("..").ghost().font(Caption).py(3).leading()
                                    .id("pkup").clicked()) {
                                char up[DOC_PATH_MAX];
                                snprintf(up, sizeof up, "%s", g_pick_dir);
                                char *slash = up;
                                for (char *q = up; *q; q++) if (*q == '/') slash = q;
                                if (slash == up) up[1] = 0; else *slash = 0;
                                pick_scan(up[0] ? up : "/");
                            }
                            static const char *PK[PICK_MAX];
                            for (int i = 0; i < g_pick_n; i++) {
                                if (g_pick[i].name[0] == '.') continue;
                                int isdir = (g_pick[i].type == EMBK_DT_DIR);
                                char lbl[160];
                                snprintf(lbl, sizeof lbl, "%.150s%s", g_pick[i].name, isdir ? "/" : "");
                                PK[i] = g_pick[i].name;
                                if (Button(lbl).ghost().font(Caption).py(3).leading()
                                        .color(isdir ? TAB_TEXT_ON : TAB_TEXT_OFF)
                                        .id(PK[i]).clicked()) {
                                    /* Bounded on BOTH parts. A path assembled from
                                     * a directory that is already near the cap plus
                                     * a long name would otherwise be silently cut,
                                     * and a truncated path is a different file. */
                                    char full[DOC_PATH_MAX];
                                    int fn = snprintf(full, sizeof full, "%s%s%s", g_pick_dir,
                                                      strcmp(g_pick_dir, "/") ? "/" : "",
                                                      g_pick[i].name);
                                    if (fn < 0 || fn >= (int)sizeof full) {
                                        snprintf(g_msg, sizeof g_msg, "Path too long");
                                        g_pick_open = 0;
                                        break;
                                    }
                                    if (isdir) pick_scan(full);
                                    else {
                                        snprintf(g_path_field, sizeof g_path_field, "%s", full);
                                        g_want_open = 1;
                                        g_pick_open = 0;
                                    }
                                    break;      /* the list just changed under us */
                                }
                            }
                        }
                    }
                }
            }
            if (OverlayDismissed()) g_pick_open = 0;
        }

        /* The one question this app asks: a modified document is not closed out
         * from under the person who modified it. */
        if (g_confirm_close >= 0) {
            Overlay() {
                Dialog(.width = 380, .spacing = 14, .padding = 20) {
                    char q[96];
                    snprintf(q, sizeof q, "Close %.32s?", doc_label(g_confirm_close));
                    Text(q).heading(); em_flush();
                    Text("It has unsaved changes.").caption().secondary();
                    HStack(.spacing = 8, .align = Center) {
                        Spacer();
                        if (Button("Cancel").ghost().clicked()) g_confirm_close = -1;
                        if (Button("Save").clicked()) {
                            doc_select(g_confirm_close); g_bound = -1; bind_current();
                            save_current();
                            if (!doc_dirty(g_confirm_close)) doc_close(g_confirm_close);
                            g_confirm_close = -1; g_bound = -1;
                        }
                        if (Button("Discard").destructive().clicked()) {
                            doc_close(g_confirm_close);
                            g_confirm_close = -1; g_bound = -1;
                        }
                    }
                }
            }
        }

        Divider("statussep");
        HStack(.spacing = 14, .align = Center, .px = 12, .py = 4, .key = "status") {
            char pos[64];
            snprintf(pos, sizeof pos, "Ln %d, Col %d",
                     ed_line_of(&ED, ED.cursor) + 1, ed_col_of(&ED, ED.cursor) + 1);
            Text(pos).caption().tertiary();
            char meta[64];
            int lo, hi; ed_sel_range(&ED, &lo, &hi);
            snprintf(meta, sizeof meta, "%d lines", ed_line_count(&ED));
            Text(meta).caption().tertiary();
            if (hi > lo) { char s[32]; snprintf(s, sizeof s, "%d selected", hi - lo);
                           Text(s).caption().tertiary(); }
            Text(syn_lang_name(d ? d->lang : SYN_PLAIN)).caption().tertiary();
            Text(ED.use_tabs ? "Tabs" : "Spaces").caption().tertiary();
            if (d && doc_dirty(doc_current())) Text("modified").caption().tertiary();
            Spacer();
            if (g_msg[0]) Text(g_msg).caption().tertiary();
        }
    }
}

EM_APPLICATION {
    .title  = "Note++",
    .size   = { 900, 620 },
    .theme  = Dark,
    .chrome = Chromeless,
    .resize = Resizable,
    .font   = "/system/fonts/mono.ttf",
    .view   = app,
};

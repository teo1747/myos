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
#define C_MATCH       rgb(0x66FFC24B)
#define TAB_ON        rgb(0xFF3A3A3C)
#define TAB_OFF       rgb(0x00000000)
#define TAB_TEXT_ON   rgb(0xFFF2F2F7)
#define TAB_TEXT_OFF  rgb(0xFF98989E)

/* ---- state --------------------------------------------------------------- */

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
static int   g_rows = 20;         /* visible lines, recomputed from the window */
static int   g_match = -1;        /* bracket matching the caret's, or -1       */

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
                    snprintf(g_msg, sizeof g_msg, "Saved %lu bytes", (unsigned long)w); }
    else snprintf(g_msg, sizeof g_msg, "PARTIAL WRITE %lu of %lu",
                  (unsigned long)w, (unsigned long)len);
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
        if (ctrl && (ch | 32) == 's') { save_current(); return 1; }
        if (ch == 27) { g_find_open = g_replace_open = 0; return 0; }
        return 0;
    }

    if (ctrl) {
        switch (ch | 32) {
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
            case 'g': { /* go to line: the number is typed into the find field */
                        g_find_open = 1;
                        snprintf(g_msg, sizeof g_msg, "Type a line number, then Enter");
                        return 1; }
            case 'c': case 'x': {
                char tmp[4096];
                int n = ed_copy(&ED, tmp, sizeof tmp);
                if (n) { embk_clip_set(tmp, (size_t)n);
                         if ((ch | 32) == 'x') ed_delete_sel(&ED);
                         snprintf(g_msg, sizeof g_msg, "%d bytes %s", n,
                                  (ch | 32) == 'x' ? "cut" : "copied"); }
                return 1; }
            case 'v': {
                char tmp[4096];
                int64_t n = embk_clip_get(tmp, sizeof tmp - 1);
                if (n > 0) { tmp[n] = 0; ed_insert_text(&ED, tmp, (int)n); scroll_to_caret(); }
                return 1; }
            case 0x11 | 32: break;    /* fall through for arrows below */
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
            if (ch >= 32 && ch < 127) ed_insert_char(&ED, (char)ch);
            else return 0;
            break;
    }
    scroll_to_caret();
    return 1;
}

/* ---- the document view --------------------------------------------------- */

/* One line: gutter number, then the text as coloured spans, with the selection
 * behind it and the caret dropped in at the right offset. */
static void draw_line(int line, int ls, int le, int syn_state_in, enum syn_lang lang,
                      int cur_line, int sel_lo, int sel_hi) {
    char tmp[512];
    int n = le - ls;
    if (n > (int)sizeof tmp - 1) n = (int)sizeof tmp - 1;

    HStack(.spacing = 0, .align = Center, .px = 0, .py = 0,
           .background = (line == cur_line) ? C_CURLINE : rgb(0)) {
        { char num[16];
          snprintf(num, sizeof num, "%4d ", line + 1);
          Text(num).font(Body).color(line == cur_line ? C_GUTTER_CUR : C_GUTTER); }

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
            /* Split this span wherever the selection or the caret starts or
             * ends inside it, so both land between characters rather than
             * being approximated to a span boundary. */
            int i = 0;
            while (i < sl) {
                int abs = ls + off + i;
                int in_sel = (sel_lo < sel_hi && abs >= sel_lo && abs < sel_hi);
                int run = 1;
                while (i + run < sl) {
                    int a2 = ls + off + i + run;
                    int s2 = (sel_lo < sel_hi && a2 >= sel_lo && a2 < sel_hi);
                    if (s2 != in_sel) break;
                    if (a2 == ED.cursor) break;
                    run++;
                }
                if (abs == ED.cursor) {
                    Text("|").font(Body).color(C_CARET);
                }
                memcpy(tmp, ED.buf + abs, (size_t)run); tmp[run] = 0;
                Color fg = role_color(sp[k].role);
                if (g_match >= 0 && abs <= g_match && g_match < abs + run)
                    Text(tmp).font(Body).color(fg).bg(C_MATCH);
                else if (in_sel)
                    Text(tmp).font(Body).color(fg).bg(C_SEL);
                else
                    Text(tmp).font(Body).color(fg);
                i += run;
            }
            off += sl;
        }
        if (ED.cursor == le) Text("|").font(Body).color(C_CARET);
        if (!n && ED.cursor != le) Text(" ").font(Body);
        Spacer();
    }
}

static void document_view(float height) {
    struct doc *d = doc_cur();
    if (!d) return;
    enum syn_lang lang = d->lang;

    /* How many lines fit. Recomputed every frame so a resize is not a special
     * case, and used by PgUp/PgDn as well as by the scroller. */
    float lh = 21.0f;
    g_rows = (int)(height / lh);
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

    VStack(.spacing = 0, .align = Fill, .padding = 0, .background = C_EDITOR_BG,
           .corner = 8, .clip = 1, .height = height, .key = "doc") {
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
}

/* ---- the app ------------------------------------------------------------- */

static void app(void) {
    /* The editor owns the keyboard; installed once, on the first frame, the
     * way vellum installs its own. */
    static int wired = 0;
    if (!wired) { wired = 1; em_set_key_hook(on_key); }
    doc_init();
    bind_current();

    if (g_want_new)  { g_want_new = 0; doc_new(); g_path_field[0] = 0; g_bound = -1; }
    if (g_want_open) {
        g_want_open = 0;
        int i = doc_open(g_path_field, file_read);
        if (i < 0) snprintf(g_msg, sizeof g_msg, "No room for another file");
        else { struct doc *dd = doc_at(i);
               snprintf(g_msg, sizeof g_msg, "%s%s", dd->text[0] ? "Opened" : "New file",
                        dd->truncated ? " (TRUNCATED -- will not save)" : "");
               g_bound = -1; }
    }
    if (g_switch_to >= 0) { sync_out(); doc_select(g_switch_to); g_switch_to = -1; g_bound = -1; }
    if (g_close >= 0)     { doc_close(g_close); g_close = -1; g_bound = -1; }
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

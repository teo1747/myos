/* user/bin/notepp.c -- Note++, the OS's own code editor.
 *
 * user/bin/edit.c is the one-file editor: a buffer, a Save button, and the path
 * handed to it by Files. This is the other thing -- several files open at once,
 * line numbers, and the code coloured the way this OS's own compiler reads it.
 * Neither replaces the other; edit is what Files opens a text file with, and
 * this is what you write a program in.
 *
 * The chrome is the shape Vellum arrived at: the window's lights share the tab
 * strip (a title bar has nothing to say that the tab does not), and one row
 * below it holds the path and the actions that operate on it. Two rows, and the
 * rest of the window is the file.
 *
 * The whole app is monospace, chosen the way term.c chooses it -- a font at the
 * application level rather than a role in the theme, because a code editor has
 * no text that should be proportional and a theme with a mono role would have
 * to be threaded through every widget to get here.
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

/* ---- the file I/O the document model is handed --------------------------- */

static long file_read(const char *path, char *buf, size_t cap) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    size_t n = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[n] = 0;
    return (long)n;
}

static char g_msg[96] = "";

static void save_current(void) {
    struct doc *d = doc_cur();
    if (!d) return;
    if (!d->path[0]) { snprintf(g_msg, sizeof g_msg, "No name -- type a path above"); return; }
    if (d->truncated) {
        /* Refuse. The buffer holds a PREFIX of the file, and writing it back
         * deletes everything that did not fit -- the one mistake an editor
         * must never make quietly. */
        snprintf(g_msg, sizeof g_msg, "REFUSED: file was truncated at %d KB on open", DOC_CAP / 1024);
        return;
    }
    FILE *f = fopen(d->path, "w");
    if (!f) { snprintf(g_msg, sizeof g_msg, "Cannot write %.70s", d->path); return; }
    size_t len = strlen(d->text);
    size_t w = fwrite(d->text, 1, len, f);
    fclose(f);
    if (w == len) {
        doc_mark_saved(doc_current());
        snprintf(g_msg, sizeof g_msg, "Saved %lu bytes", (unsigned long)w);
    } else {
        snprintf(g_msg, sizeof g_msg, "PARTIAL WRITE %lu of %lu",
                 (unsigned long)w, (unsigned long)len);
    }
}

/* ---- colours ------------------------------------------------------------- */

static Color rgb(unsigned v) {
    Color c;
    c.r = (float)((v >> 16) & 0xFF) / 255.0f;
    c.g = (float)((v >>  8) & 0xFF) / 255.0f;
    c.b = (float)( v        & 0xFF) / 255.0f;
    c.a = (float)((v >> 24) & 0xFF) / 255.0f;
    return c;
}

/* One colour per role. Stated here rather than derived from the theme: a
 * syntax palette is not a UI palette -- it has to keep seven things apart at
 * body size on one background, which is a different problem from making a
 * button look pressed. */
static Color role_color(int role) {
    switch (role) {
        case SYN_KEYWORD: return rgb(0xFFFF7AB2);   /* pink   */
        case SYN_TYPE:    return rgb(0xFF6BDFFF);   /* cyan   */
        case SYN_STRING:  return rgb(0xFFFF8170);   /* coral  */
        case SYN_COMMENT: return rgb(0xFF7E8B99);   /* grey   */
        case SYN_NUMBER:  return rgb(0xFFD9C97C);   /* sand   */
        case SYN_PREPROC: return rgb(0xFFB281EB);   /* violet */
        case SYN_PUNCT:   return rgb(0xFFA6B0BB);
        default:          return rgb(0xFFE6E9ED);
    }
}

#define TAB_ON       rgb(0xFF3A3A3C)
#define TAB_OFF      rgb(0x00000000)
#define TAB_TEXT_ON  rgb(0xFFF2F2F7)
#define TAB_TEXT_OFF rgb(0xFF98989E)

/* ---- the highlighter, as the toolkit wants it ---------------------------- */

/* The editor draws lines in order from the top, so the block-comment state
 * threads through this callback the way it would through a loop. It is reset
 * when the document changes -- see the view. */
static int  g_syn_state;
static enum syn_lang g_syn_lang;

static int syn_cb(const char *line, int n, struct em_span *out, int max, void *ud) {
    (void)ud;
    struct syn_span sp[128];
    int cap = max < 128 ? max : 128;
    int ns = syn_line(g_syn_lang, line, n, sp, cap, &g_syn_state);
    for (int i = 0; i < ns; i++) {
        out[i].len = sp[i].len;
        out[i].color = role_color(sp[i].role);
    }
    return ns;
}

/* ---- the view ------------------------------------------------------------ */

static char g_path_field[DOC_PATH_MAX];
static int  g_want_open = 0, g_want_new = 0, g_switch_to = -1, g_close = -1;
static int  g_last_doc = -1;

static void app(void) {
    doc_init();

    /* Deferred from last frame's click: opening a file replaces the document
     * the view is being built from, so it happens between frames. */
    if (g_want_new)   { g_want_new = 0; doc_new(); g_path_field[0] = 0; }
    if (g_want_open)  {
        g_want_open = 0;
        int i = doc_open(g_path_field, file_read);
        if (i < 0) snprintf(g_msg, sizeof g_msg, "No room for another file");
        else {
            struct doc *d = doc_at(i);
            snprintf(g_msg, sizeof g_msg, "%s%s", d->text[0] ? "Opened" : "New file",
                     d->truncated ? " (TRUNCATED -- will not save)" : "");
        }
    }
    if (g_switch_to >= 0) { doc_select(g_switch_to); g_switch_to = -1; }
    if (g_close >= 0)     { doc_close(g_close); g_close = -1; }

    struct doc *d = doc_cur();
    /* The path field follows the document, except while you are typing a new
     * one into it -- so switching tabs shows you where you are. */
    if (doc_current() != g_last_doc) {
        g_last_doc = doc_current();
        snprintf(g_path_field, sizeof g_path_field, "%s", d ? d->path : "");
        g_msg[0] = 0;
    }

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
                    EmV tb = Button(doc_label(i)).ghost().font(Caption)
                                 .py(3).px(7).id(KEYL[i]);
                    tb.color(cur ? TAB_TEXT_ON : TAB_TEXT_OFF);
                    if (tb.clicked() && !cur) g_switch_to = i;
                    if (cur && doc_count() > 1 &&
                        Button("\xc3\x97").ghost().font(Caption).py(3).px(5)
                            .color(TAB_TEXT_OFF).id(KEYX[i]).clicked())
                        g_close = i;
                }
            }
            if (Button("+").ghost().font(Caption).py(3).px(7).id("dnew").clicked())
                g_want_new = 1;
            DragHandle(.key = "tabdrag") { }
        }
        Divider("tabsep");

        /* ROW 2 -- the path, and what can be done with it. Return in the field
         * opens, the same gesture the browser's address bar uses. */
        HStack(.spacing = 6, .align = Center, .px = 8, .py = 5, .key = "pathrow") {
            if (TextField(g_path_field, sizeof g_path_field, "Path to open or save as").submitted())
                g_want_open = 1;
            if (Button("Open").ghost().font(Caption).py(2).clicked()) g_want_open = 1;
            if (Button("Save").primary().font(Caption).py(2).clicked()) {
                if (d && strcmp(d->path, g_path_field))
                    snprintf(d->path, sizeof d->path, "%s", g_path_field);
                if (d) d->lang = syn_lang_of(d->path);
                save_current();
            }
        }
        Divider("pathsep");

        /* THE FILE. The gutter and the colours are set for the next editor
         * only, so they are stated here every frame. */
        if (d) {
            g_syn_lang  = d->lang;
            g_syn_state = SYN_ST_NONE;      /* the top of the file is not inside anything */
            em_editor_syntax(d->lang == SYN_PLAIN ? 0 : syn_cb, 0);
            em_editor_gutter(1);
            TextEditor(d->text, DOC_CAP, &d->cursor, em_viewport_height() - 132.0f);
            em_editor_syntax(0, 0);
        }

        Divider("statussep");
        HStack(.spacing = 12, .align = Center, .px = 12, .py = 4, .key = "status") {
            char pos[48];
            int line = 1, col = 1;
            doc_line_col(d, &line, &col);
            snprintf(pos, sizeof pos, "Ln %d, Col %d", line, col);
            Text(pos).caption().tertiary();
            Text(syn_lang_name(d ? d->lang : SYN_PLAIN)).caption().tertiary();
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
    /* Monospace throughout -- see the note at the top. */
    .font   = "/system/fonts/mono.ttf",
    .view   = app,
};

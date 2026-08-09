/* user/web/select.c -- see select.h.
 *
 * Everything here happens AFTER layout, and that is the design. The selection
 * never asks the renderer to cooperate: it reads the laid-out scene to find out
 * where the words are, and writes back a background on the ones that are
 * selected. render.c does not know this file exists.
 *
 * That matters because render.c emits text from several places -- words, image
 * alt text, list bullets, error strings -- and a scheme where each emitter had
 * to remember to announce itself would break silently the first time one of
 * them was forgotten, with the highlight landing on the wrong word. Reading the
 * scene cannot be forgotten.
 */
#include <string.h>
#include <stddef.h>

#include "select.h"
#include "ui.h"
#include "scene.h"
#include "theme.h"

/* A document's worth of words. Past this the tail is simply not selectable,
 * which is a bounded failure rather than a heap the page controls -- but it
 * was also a SILENT one, and 4096 is not a document: Wikipedia's flex article
 * draws 10477 runs, so Ctrl+A copied the first 24 KB of it, stopped mid-word,
 * and said nothing. Raised to cover a real encyclopedia page, and the overflow
 * is now reportable so a caller can tell a short copy from a complete one. */
#define WORD_MAX 12288

struct word {
    struct node_handle nh;
    float x, y, w, hgt;
    const char *text;
    unsigned char mark;      /* 0 none, 1 a find match, 2 the current one */
};

static struct word g_word[WORD_MAX];
static int   g_nword;
static int   g_overflow;   /* the document had more runs than the index holds */
static int   g_lo = -1, g_hi = -1;    /* inclusive index range; -1 = none */
static int   g_anchor = -1;           /* where the drag started */
static int   g_dragging;

/* find.c's chance to re-mark, after the runs are collected and before they are
 * painted. A callback rather than a call INTO find.c, so this module still
 * knows nothing about it. */
static void (*g_mark_cb)(void);
void vsel_set_mark_hook(void (*fn)(void)) { g_mark_cb = fn; }

int vsel_run_count(void) { return g_nword; }

const char *vsel_run_text(int i) {
    return (i >= 0 && i < g_nword) ? g_word[i].text : 0;
}

int vsel_run_rect(int i, float *x, float *y, float *w, float *h) {
    if (i < 0 || i >= g_nword) return -1;
    if (x) *x = g_word[i].x;
    if (y) *y = g_word[i].y;
    if (w) *w = g_word[i].w;
    if (h) *h = g_word[i].hgt;
    return 0;
}

void vsel_run_mark(int i, int kind) {
    if (i >= 0 && i < g_nword) g_word[i].mark = (unsigned char)kind;
}

void vsel_reset(void) { g_nword = 0; g_overflow = 0; g_lo = g_hi = g_anchor = -1; g_dragging = 0; }
int  vsel_active(void) { return g_lo >= 0 && g_hi >= g_lo; }
int  vsel_overflowed(void) { return g_overflow; }

int vsel_clear(void) {
    if (!vsel_active()) return 0;
    g_lo = g_hi = -1;
    return 1;
}

/* --- reading the laid-out scene ----------------------------------------- */

/* Depth-first in document order, counting only what is INSIDE a clipping node.
 * The clip is the ScrollView holding the document; without that test the walk
 * also picks up the address bar and the status line, and dragging across the
 * page would select the chrome. */
static void collect(struct scene_arena *a, struct node_handle h,
                    float ox, float oy, int inside) {
    struct scene_node *n = scene_resolve(a, h);
    if (!n) return;
    if (g_nword >= WORD_MAX) { g_overflow = 1; return; }
    float x = ox + n->tx, y = oy + n->ty;
    if (n->clip_children) inside = 1;
    if (inside && n->kind == SCENE_NODE_TEXT && n->data.text.utf8) {
        struct word *w = &g_word[g_nword++];
        w->nh = h; w->x = x; w->y = y; w->w = n->width; w->hgt = n->height;
        w->text = n->data.text.utf8;
    }
    for (struct node_handle c = n->first_child; !node_handle_is_null(c);) {
        struct scene_node *cn = scene_resolve(a, c);
        if (!cn) break;
        collect(a, c, x, y, inside);
        c = cn->next_sibling;
    }
}

void vsel_sync_geometry(void) {
    struct scene_arena *a = ui_scene_arena();
    g_nword = 0; g_overflow = 0;
    collect(a, ui_scene_of(ui_root()), 0, 0, 0);

    if (g_hi >= g_nword) g_hi = g_nword - 1;
    if (g_lo > g_hi) g_lo = g_hi = -1;

    /* Find marks are re-applied by find.c each frame, exactly as the selection
     * is -- so clearing them here is what makes a stale highlight impossible. */
    for (int i = 0; i < g_nword; i++) g_word[i].mark = 0;
    if (g_mark_cb) g_mark_cb();

    /* Paint the range and UNPAINT everything else, every frame.
     *
     * Every word, not just the selected ones, because this is the only pass
     * that knows what is no longer selected -- the declarative layer reuses
     * instances across frames, so a run keeps whatever background it was last
     * given until someone takes it away. scene_set_text_bg is a no-op when the
     * colour already matches, so in the steady state this walk marks nothing
     * dirty and costs one comparison per word. */
    struct color sel = ui_theme()->accent, none = { 0, 0, 0, 0 };
    sel.a = 0.30f;                     /* the glyphs must stay readable through it */
    /* A find match is a different colour from a selection, and the CURRENT
     * match different again -- otherwise "next" moves an indicator you cannot
     * see, which is the one thing find-in-page has to show. */
    struct color hit = { 0.95f, 0.75f, 0.20f, 0.35f };
    struct color cur = { 1.00f, 0.55f, 0.10f, 0.75f };
    int lo = vsel_active() ? g_lo : -1, hi = vsel_active() ? g_hi : -2;
    for (int i = 0; i < g_nword; i++) {
        struct color c = none;
        if (i >= lo && i <= hi)      c = sel;
        else if (g_word[i].mark == 2) c = cur;
        else if (g_word[i].mark == 1) c = hit;
        scene_set_text_bg(a, g_word[i].nh, c);
    }
}

/* --- hit testing --------------------------------------------------------- */

/* The word at (x,y), or the nearest one on the nearest row.
 *
 * "Nearest", not "under": a person dragging a selection does not keep the
 * pointer inside the glyphs, they sweep through the whitespace between lines
 * and past the end of a paragraph. A hit test that only answers when the
 * pointer is literally on a word makes a selection stop dead in the gaps. */
static int word_at(float x, float y) {
    if (g_nword <= 0) return -1;
    int best = -1;
    float best_dy = 0, best_dx = 0;
    for (int i = 0; i < g_nword; i++) {
        struct word *w = &g_word[i];
        float dy = (y < w->y) ? w->y - y : (y > w->y + w->hgt) ? y - (w->y + w->hgt) : 0;
        float dx = (x < w->x) ? w->x - x : (x > w->x + w->w) ? x - (w->x + w->w) : 0;
        /* row first, then horizontal position within that row */
        if (best < 0 || dy < best_dy - 0.01f ||
            (dy <= best_dy + 0.01f && dx < best_dx)) {
            best = i; best_dy = dy; best_dx = dx;
        }
    }
    return best;
}

int vsel_pointer(float x, float y, int down, int held) {
    if (down) {
        int i = word_at(x, y);
        g_anchor = i;
        g_dragging = 1;
        /* A press with no drag yet selects nothing -- otherwise every click on
         * the page would leave one word highlighted, including the click that
         * follows a link. */
        int had = vsel_active();
        g_lo = g_hi = -1;
        return had;
    }
    if (!held) { g_dragging = 0; return 0; }
    if (!g_dragging || g_anchor < 0) return 0;

    int i = word_at(x, y);
    if (i < 0) return 0;
    int lo = i < g_anchor ? i : g_anchor;
    int hi = i < g_anchor ? g_anchor : i;
    if (lo == g_lo && hi == g_hi) return 0;
    g_lo = lo; g_hi = hi;
    return 1;
}

int vsel_all(void) {
    if (g_nword <= 0) return 0;
    if (g_lo == 0 && g_hi == g_nword - 1) return 0;
    g_lo = 0; g_hi = g_nword - 1;
    return 1;
}

/* --- taking it away ------------------------------------------------------ */

/* Punctuation that belongs to the word BEFORE it. A comma after a <code> span
 * is its own word here -- the renderer splits on whitespace, and the markup put
 * a tag boundary between them -- so joining with a space would copy "EMBKFS ,"
 * out of a page that plainly reads "EMBKFS,". */
static int binds_left(char c) {
    return c == ',' || c == '.' || c == ';' || c == ':' || c == '!' ||
           c == '?' || c == ')' || c == ']' || c == '}' || c == '\'';
}

size_t vsel_copy_text(char *buf, size_t cap) {
    if (!vsel_active() || !buf || cap < 2) return 0;
    size_t n = 0;
    float last_y = g_word[g_lo].y;
    for (int i = g_lo; i <= g_hi && n + 2 < cap; i++) {
        const char *t = g_word[i].text;
        if (!t) continue;
        if (i > g_lo) {
            /* A new row becomes a newline, so a copied paragraph pastes as a
             * paragraph rather than as one endless line. Layout is the only
             * thing that knows where the lines broke.
             *
             * A word KEEPS the space the source put after it -- that is what
             * stops "and" welding onto an <i> that follows it -- so joining
             * with another space unconditionally gives "This  page  is". Add a
             * separator only where there is not one already. */
            int newrow = g_word[i].y > last_y + 1.0f;
            int have = n > 0 && (buf[n - 1] == ' ' || buf[n - 1] == '\n');
            if (newrow) {
                if (have) n--;                 /* the row break replaces it */
                buf[n++] = '\n';
            } else if (!have && t[0] != ' ' && !binds_left(t[0])) {
                buf[n++] = ' ';
            }
            last_y = g_word[i].y;
        }
        size_t l = strlen(t);
        if (n + l >= cap) l = cap - n - 1;
        memcpy(buf + n, t, l);
        n += l;
    }
    buf[n] = 0;
    return n;
}

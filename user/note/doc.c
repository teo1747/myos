/* user/note/doc.c -- see doc.h. Bookkeeping, one file's worth. */

#include "doc.h"
#include <string.h>
#include <stdio.h>

/* The buffers, reserved up front. Two megabytes of editor is a deliberate
 * number: it is what eight quarter-megabyte files cost, and growing them on
 * demand would mean a document could fail to open at the moment you typed into
 * it rather than at the moment you asked for it. */
static char        g_text[DOC_MAX][DOC_CAP];
static struct doc  g_doc[DOC_MAX];
static int         g_cur;
static int         g_inited;

/* FNV-1a. Not a security hash -- this answers "are these the same bytes I last
 * wrote", and the cost of a collision is a Save button that looks disabled. */
unsigned long doc_hash(const char *s) {
    unsigned long h = 1469598103934665603UL;
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 1099511628211UL; }
    return h;
}

void doc_init(void) {
    if (g_inited) return;
    g_inited = 1;
    memset(g_doc, 0, sizeof g_doc);
    for (int i = 0; i < DOC_MAX; i++) g_doc[i].text = g_text[i];
    doc_new();
}

int doc_count(void) {
    int n = 0;
    for (int i = 0; i < DOC_MAX; i++) if (g_doc[i].open) n++;
    return n;
}
int         doc_current(void) { return g_cur; }
struct doc *doc_at(int i) {
    if (i < 0 || i >= DOC_MAX || !g_doc[i].open) return 0;
    return &g_doc[i];
}
struct doc *doc_cur(void) { return doc_at(g_cur); }

static int free_slot(void) {
    for (int i = 0; i < DOC_MAX; i++) if (!g_doc[i].open) return i;
    return -1;
}

int doc_new(void) {
    int i = free_slot();
    if (i < 0) return -1;
    struct doc *d = &g_doc[i];
    d->path[0] = 0;
    d->text[0] = 0;
    d->cursor = 0;
    d->open = 1;
    d->lang = SYN_PLAIN;
    d->truncated = 0;
    d->saved_hash = doc_hash(d->text);
    g_cur = i;
    return i;
}

int doc_open(const char *path, long (*read)(const char *, char *, size_t)) {
    if (!path || !path[0]) return -1;
    /* Already open? Hand back the same tab. Two buffers over one file diverge,
     * and whichever was saved last would silently win. */
    for (int i = 0; i < DOC_MAX; i++)
        if (g_doc[i].open && !strcmp(g_doc[i].path, path)) { g_cur = i; return i; }

    int i = free_slot();
    if (i < 0) return -1;
    struct doc *d = &g_doc[i];
    snprintf(d->path, sizeof d->path, "%s", path);
    d->text[0] = 0;
    d->truncated = 0;
    long n = read ? read(path, d->text, DOC_CAP) : -1;
    if (n < 0) {
        /* Not there: an editor opens a NEW file at that path rather than
         * refusing, which is what every editor does and what makes "open a
         * name that does not exist yet" the ordinary way to create one. */
        d->text[0] = 0;
        n = 0;
    } else if (n >= (long)DOC_CAP - 1) {
        /* Say so. A silently truncated file that is then SAVED destroys the
         * part that did not fit, which is the worst thing an editor can do. */
        d->truncated = 1;
    }
    d->text[n < 0 ? 0 : (size_t)n] = 0;
    d->cursor = 0;
    d->open = 1;
    d->lang = syn_lang_of(path);
    d->saved_hash = doc_hash(d->text);
    g_cur = i;
    return i;
}

int doc_close(int i) {
    if (i < 0 || i >= DOC_MAX || !g_doc[i].open) return g_cur;
    if (doc_count() <= 1) {
        /* Never zero documents: an editor with nothing open has nothing to
         * show. Closing the last one empties it instead, same as every editor. */
        struct doc *d = &g_doc[i];
        d->path[0] = 0; d->text[0] = 0; d->cursor = 0;
        d->lang = SYN_PLAIN; d->truncated = 0;
        d->saved_hash = doc_hash(d->text);
        return g_cur = i;
    }
    g_doc[i].open = 0;
    if (g_cur == i) {
        for (int k = i - 1; k >= 0; k--) if (g_doc[k].open) return g_cur = k;
        for (int k = i + 1; k < DOC_MAX; k++) if (g_doc[k].open) return g_cur = k;
    }
    return g_cur;
}

int doc_select(int i) {
    if (i < 0 || i >= DOC_MAX || !g_doc[i].open || i == g_cur) return -1;
    g_cur = i;
    return 0;
}

int doc_dirty(int i) {
    struct doc *d = doc_at(i);
    return d && doc_hash(d->text) != d->saved_hash;
}

void doc_mark_saved(int i) {
    struct doc *d = doc_at(i);
    if (d) { d->saved_hash = doc_hash(d->text); d->truncated = 0; }
}

const char *doc_label(int i) {
    static char lbl[DOC_MAX][48];
    struct doc *d = doc_at(i);
    if (!d) return "";
    const char *b = d->path, *s;
    for (s = d->path; *s; s++) if (*s == '/') b = s + 1;
    snprintf(lbl[i], sizeof lbl[0], "%s%s",
             *b ? b : "Untitled", doc_dirty(i) ? " \xe2\x80\xa2" : "");
    return lbl[i];
}

void doc_line_col(const struct doc *d, int *line, int *col) {
    int ln = 1, co = 1;
    if (d) {
        int cur = d->cursor;
        for (int i = 0; i < cur && d->text[i]; i++) {
            if (d->text[i] == '\n') { ln++; co = 1; }
            else co++;
        }
    }
    if (line) *line = ln;
    if (col)  *col  = co;
}

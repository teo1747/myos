/* user/note/edit.c -- see edit.h. */

#include "edit.h"
#include <string.h>

/* ---- small helpers ------------------------------------------------------ */

static int lower(int c) { return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c; }
static int is_word(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

void ed_init(struct editor *e, char *buf, size_t cap) {
    memset(e, 0, sizeof *e);
    e->buf = buf; e->cap = cap;
    e->len = (int)strlen(buf);
    e->tab_width = 4;
    e->goal_col = -1;
}

void ed_set_text(struct editor *e, const char *s) {
    int n = (int)strlen(s);
    if (n > (int)e->cap - 1) n = (int)e->cap - 1;
    memcpy(e->buf, s, (size_t)n);
    e->buf[n] = 0;
    e->len = n;
    e->cursor = e->anchor = 0;
    e->undo_n = e->undo_at = e->undo_text_n = 0;
    e->goal_col = -1;
}

/* ---- selection ---------------------------------------------------------- */

int ed_has_sel(const struct editor *e) { return e->anchor != e->cursor; }

void ed_sel_range(const struct editor *e, int *lo, int *hi) {
    int a = e->anchor, b = e->cursor;
    if (a > b) { int t = a; a = b; b = t; }
    if (lo) *lo = a;
    if (hi) *hi = b;
}

void ed_select_all(struct editor *e) { e->anchor = 0; e->cursor = e->len; }
void ed_clear_sel(struct editor *e)  { e->anchor = e->cursor; }

int ed_copy(const struct editor *e, char *out, size_t cap) {
    if (!ed_has_sel(e) || !out || cap < 2) return 0;
    int lo, hi; ed_sel_range(e, &lo, &hi);
    int n = hi - lo;
    if (n > (int)cap - 1) n = (int)cap - 1;
    memcpy(out, e->buf + lo, (size_t)n);
    out[n] = 0;
    return n;
}

/* ---- lines -------------------------------------------------------------- */

int ed_line_start(const struct editor *e, int off) {
    off = clampi(off, 0, e->len);
    while (off > 0 && e->buf[off - 1] != '\n') off--;
    return off;
}
int ed_line_end(const struct editor *e, int off) {
    off = clampi(off, 0, e->len);
    while (off < e->len && e->buf[off] != '\n') off++;
    return off;
}
int ed_line_of(const struct editor *e, int off) {
    off = clampi(off, 0, e->len);
    int n = 0;
    for (int i = 0; i < off; i++) if (e->buf[i] == '\n') n++;
    return n;
}
int ed_col_of(const struct editor *e, int off) { return off - ed_line_start(e, off); }
int ed_line_count(const struct editor *e) { return ed_line_of(e, e->len) + 1; }

int ed_offset_of_line(const struct editor *e, int line) {
    if (line <= 0) return 0;
    int n = 0;
    for (int i = 0; i < e->len; i++)
        if (e->buf[i] == '\n' && ++n == line) return i + 1;
    return e->len;
}

/* ---- undo --------------------------------------------------------------- */

/* Record a change about to happen at [at, at+removed) that will become
 * `added` bytes. The removed bytes are copied into the pool so undo can put
 * them back; the added bytes are not, because redo can recompute them from the
 * buffer as it will be. */
static void undo_push(struct editor *e, int at, int removed, int added, int coalesce) {
    /* A new edit truncates the redo tail -- the future you did not take. */
    e->undo_n = e->undo_at;

    /* Coalescing: consecutive single characters typed at the caret become one
     * undo step, because undoing a sentence one letter at a time is not what
     * anybody means by undo. Only for inserts that continue the last one. */
    if (coalesce && e->undo_n > 0) {
        struct ed_undo *p = &e->undo[e->undo_n - 1];
        if (p->coalesce && p->op == ED_OP_INSERT && removed == 0 &&
            p->at + p->added_len == at) {
            p->added_len += added;
            return;
        }
    }

    if (e->undo_n >= ED_UNDO_MAX) {
        /* Drop the oldest, and its text with it. */
        int drop = e->undo[0].removed_len;
        memmove(e->undo, e->undo + 1, sizeof e->undo[0] * (ED_UNDO_MAX - 1));
        if (drop > 0) {
            memmove(e->undo_text, e->undo_text + drop, (size_t)(e->undo_text_n - drop));
            e->undo_text_n -= drop;
            for (int i = 0; i < ED_UNDO_MAX - 1; i++) e->undo[i].text_off -= drop;
        }
        e->undo_n = ED_UNDO_MAX - 1;
    }
    if (e->undo_text_n + removed > ED_UNDO_TEXT) {
        /* Too big to remember. Forget the whole history rather than keep a
         * broken one: an undo stack that cannot restore what it claims to is
         * worse than no undo at all. */
        e->undo_n = e->undo_at = e->undo_text_n = 0;
        return;
    }

    struct ed_undo *u = &e->undo[e->undo_n];
    u->at = at;
    u->removed_len = removed;
    u->added_len = added;
    u->cursor_before = e->cursor;
    u->sel_before = e->anchor;
    u->text_off = e->undo_text_n;
    u->op = (unsigned char)(removed && added ? ED_OP_REPLACE : removed ? ED_OP_DELETE : ED_OP_INSERT);
    u->coalesce = (unsigned char)coalesce;
    if (removed > 0) {
        memcpy(e->undo_text + e->undo_text_n, e->buf + at, (size_t)removed);
        e->undo_text_n += removed;
    }
    e->undo_n++;
    e->undo_at = e->undo_n;
}

/* The one place text actually changes. Everything above is bookkeeping. */
static void splice(struct editor *e, int at, int removed, const char *add, int added,
                   int coalesce) {
    at = clampi(at, 0, e->len);
    if (removed > e->len - at) removed = e->len - at;
    if (removed <= 0 && added <= 0) return;
    if (e->len - removed + added > (int)e->cap - 1) return;   /* refuse; never truncate */

    undo_push(e, at, removed, added, coalesce);

    memmove(e->buf + at + added, e->buf + at + removed,
            (size_t)(e->len - at - removed + 1));           /* +1 keeps the NUL */
    if (added > 0 && add) memcpy(e->buf + at, add, (size_t)added);
    e->len += added - removed;
    e->buf[e->len] = 0;
    e->cursor = e->anchor = at + added;
    e->goal_col = -1;
}

int ed_can_undo(const struct editor *e) { return e->undo_at > 0; }
int ed_can_redo(const struct editor *e) { return e->undo_at < e->undo_n; }

int ed_undo(struct editor *e) {
    if (!ed_can_undo(e)) return 0;
    struct ed_undo *u = &e->undo[--e->undo_at];
    /* put the removed bytes back where the added ones are */
    memmove(e->buf + u->at + u->removed_len, e->buf + u->at + u->added_len,
            (size_t)(e->len - u->at - u->added_len + 1));
    if (u->removed_len > 0)
        memcpy(e->buf + u->at, e->undo_text + u->text_off, (size_t)u->removed_len);
    e->len += u->removed_len - u->added_len;
    e->buf[e->len] = 0;
    e->cursor = u->cursor_before;
    e->anchor = u->sel_before;
    e->goal_col = -1;
    return 1;
}

int ed_redo(struct editor *e) {
    if (!ed_can_redo(e)) return 0;
    struct ed_undo *u = &e->undo[e->undo_at];
    /* We kept the removed text, not the added -- so redo needs the added bytes
     * from somewhere. They are still where undo left them only for a pure
     * delete; for an insert we cannot reconstruct them, so an insert's redo
     * re-reads them out of the undo pool the other way round. To keep that
     * honest the pool stores REMOVED text and redo of an insert is therefore
     * only correct when added_len == 0. Everything else is a delete-redo. */
    if (u->added_len == 0) {
        /* redo of a delete: remove the bytes again */
        memmove(e->buf + u->at, e->buf + u->at + u->removed_len,
                (size_t)(e->len - u->at - u->removed_len + 1));
        e->len -= u->removed_len;
        e->buf[e->len] = 0;
        e->cursor = e->anchor = u->at;
        e->undo_at++;
        return 1;
    }
    return 0;      /* redo of an insert is not supported; see above */
}

/* ---- editing ------------------------------------------------------------ */

void ed_delete_sel(struct editor *e) {
    if (!ed_has_sel(e)) return;
    int lo, hi; ed_sel_range(e, &lo, &hi);
    splice(e, lo, hi - lo, 0, 0, 0);
}

void ed_insert_text(struct editor *e, const char *s, int n) {
    if (n <= 0) return;
    if (ed_has_sel(e)) {
        int lo, hi; ed_sel_range(e, &lo, &hi);
        splice(e, lo, hi - lo, s, n, 0);
    } else {
        splice(e, e->cursor, 0, s, n, 0);
    }
}

void ed_insert_char(struct editor *e, char c) {
    if (ed_has_sel(e)) { ed_delete_sel(e); }
    splice(e, e->cursor, 0, &c, 1, 1);      /* coalescing: typing is one step */
}

void ed_newline(struct editor *e) {
    if (ed_has_sel(e)) ed_delete_sel(e);
    /* AUTO-INDENT: the new line starts with the same leading whitespace as the
     * one it came from. Everything indented is written this way, so an editor
     * that does not do it makes you retype the indentation of every line. */
    int ls = ed_line_start(e, e->cursor);
    char ind[128];
    int n = 0;
    while (ls + n < e->cursor && n < (int)sizeof ind - 2 &&
           (e->buf[ls + n] == ' ' || e->buf[ls + n] == '\t')) {
        ind[n + 1] = e->buf[ls + n];
        n++;
    }
    ind[0] = '\n';
    /* ...and one level deeper after an opening brace, which is the other half
     * of not retyping indentation. */
    int prev = e->cursor - 1;
    while (prev >= ls && (e->buf[prev] == ' ' || e->buf[prev] == '\t')) prev--;
    int deeper = (prev >= ls && e->buf[prev] == '{');
    int total = n + 1;
    if (deeper) {
        int add = e->use_tabs ? 1 : e->tab_width;
        for (int i = 0; i < add && total < (int)sizeof ind; i++)
            ind[total++] = e->use_tabs ? '\t' : ' ';
    }
    splice(e, e->cursor, 0, ind, total, 0);
}

void ed_backspace(struct editor *e) {
    if (ed_has_sel(e)) { ed_delete_sel(e); return; }
    if (e->cursor <= 0) return;
    /* Inside leading whitespace, delete a whole indent level -- otherwise
     * four spaces take four presses to undo one Tab. */
    int ls = ed_line_start(e, e->cursor);
    int only_ws = 1;
    for (int i = ls; i < e->cursor; i++)
        if (e->buf[i] != ' ' && e->buf[i] != '\t') { only_ws = 0; break; }
    int back = 1;
    if (only_ws && e->cursor > ls && e->buf[e->cursor - 1] == ' ') {
        int col = e->cursor - ls;
        int step = col % e->tab_width;
        if (step == 0) step = e->tab_width;
        if (step > col) step = col;
        back = step;
    }
    splice(e, e->cursor - back, back, 0, 0, 0);
}

void ed_delete(struct editor *e) {
    if (ed_has_sel(e)) { ed_delete_sel(e); return; }
    if (e->cursor >= e->len) return;
    splice(e, e->cursor, 1, 0, 0, 0);
}

/* Indent every line the selection touches; with no selection, insert one
 * level at the caret. */
void ed_indent(struct editor *e) {
    char pad[16];
    int padn = e->use_tabs ? 1 : e->tab_width;
    if (padn > (int)sizeof pad) padn = (int)sizeof pad;
    for (int i = 0; i < padn; i++) pad[i] = e->use_tabs ? '\t' : ' ';

    if (!ed_has_sel(e)) { splice(e, e->cursor, 0, pad, padn, 0); return; }

    int lo, hi; ed_sel_range(e, &lo, &hi);
    int first = ed_line_start(e, lo), last = ed_line_start(e, hi);
    /* bottom-up, so earlier offsets stay valid as the text grows */
    int line = last, added = 0;
    for (;;) {
        splice(e, line, 0, pad, padn, 0);
        added += padn;
        if (line <= first) break;
        line = ed_line_start(e, line - 1);
    }
    e->anchor = first;
    e->cursor = hi + added;
}

void ed_outdent(struct editor *e) {
    int lo, hi;
    if (ed_has_sel(e)) ed_sel_range(e, &lo, &hi);
    else lo = hi = e->cursor;
    int first = ed_line_start(e, lo), last = ed_line_start(e, hi);
    int line = last, removed = 0;
    for (;;) {
        int n = 0;
        if (e->buf[line] == '\t') n = 1;
        else while (n < e->tab_width && line + n < e->len && e->buf[line + n] == ' ') n++;
        if (n > 0) { splice(e, line, n, 0, 0, 0); removed += n; }
        if (line <= first) break;
        line = ed_line_start(e, line - 1);
    }
    if (removed) {
        e->anchor = first;
        e->cursor = clampi(hi - removed, first, e->len);
    }
}

void ed_duplicate_line(struct editor *e) {
    int ls = ed_line_start(e, e->cursor), le = ed_line_end(e, e->cursor);
    int n = le - ls;
    char tmp[1024];
    if (n > (int)sizeof tmp - 2) return;
    tmp[0] = '\n';
    memcpy(tmp + 1, e->buf + ls, (size_t)n);
    int col = e->cursor - ls;
    splice(e, le, 0, tmp, n + 1, 0);
    e->cursor = e->anchor = le + 1 + col;
}

void ed_delete_line(struct editor *e) {
    int ls = ed_line_start(e, e->cursor), le = ed_line_end(e, e->cursor);
    if (le < e->len) le++;                 /* take the newline with it */
    else if (ls > 0) ls--;                 /* last line: take the one before */
    int col = e->cursor - ed_line_start(e, e->cursor);
    splice(e, ls, le - ls, 0, 0, 0);
    e->cursor = e->anchor = clampi(ls + col, ls, ed_line_end(e, ls));
}

void ed_move_line(struct editor *e, int dir) {
    int ls = ed_line_start(e, e->cursor), le = ed_line_end(e, e->cursor);
    int col = e->cursor - ls;
    char line[1024];
    int n = le - ls;
    if (n > (int)sizeof line - 1) return;
    memcpy(line, e->buf + ls, (size_t)n); line[n] = 0;

    if (dir < 0) {
        if (ls == 0) return;
        int pls = ed_line_start(e, ls - 1);
        /* cut this line (with its newline) and put it back above the previous */
        int cut = (le < e->len) ? n + 1 : n;
        int at  = (le < e->len) ? ls : ls - 1;
        splice(e, at, cut, 0, 0, 0);
        char ins[1025];
        memcpy(ins, line, (size_t)n); ins[n] = '\n';
        splice(e, pls, 0, ins, n + 1, 0);
        e->cursor = e->anchor = pls + col;
    } else {
        if (le >= e->len) return;
        int nls = le + 1, nle = ed_line_end(e, nls);
        int nn = nle - nls;
        splice(e, ls, n + 1, 0, 0, 0);          /* remove this line */
        int at = ls + nn;
        char ins[1025];
        ins[0] = '\n';
        memcpy(ins + 1, line, (size_t)n);
        if (at > e->len) at = e->len;
        splice(e, at, 0, ins, n + 1, 0);
        e->cursor = e->anchor = at + 1 + col;
    }
    e->goal_col = -1;
}

/* ---- movement ----------------------------------------------------------- */

static void after_move(struct editor *e, int extend) {
    if (!extend) e->anchor = e->cursor;
}

void ed_move(struct editor *e, int delta, int extend) {
    /* An unshifted arrow with a selection COLLAPSES it to the near edge rather
     * than moving one character from the caret -- what every editor does, and
     * what makes "select, then press right to deselect" land where you look. */
    if (!extend && ed_has_sel(e)) {
        int lo, hi; ed_sel_range(e, &lo, &hi);
        e->cursor = e->anchor = (delta < 0) ? lo : hi;
        e->goal_col = -1;
        return;
    }
    e->cursor = clampi(e->cursor + delta, 0, e->len);
    e->goal_col = -1;
    after_move(e, extend);
}

void ed_move_word(struct editor *e, int dir, int extend) {
    int i = e->cursor;
    if (dir > 0) {
        while (i < e->len && !is_word(e->buf[i])) i++;
        while (i < e->len && is_word(e->buf[i])) i++;
    } else {
        while (i > 0 && !is_word(e->buf[i - 1])) i--;
        while (i > 0 && is_word(e->buf[i - 1])) i--;
    }
    e->cursor = i;
    e->goal_col = -1;
    after_move(e, extend);
}

void ed_move_line_v(struct editor *e, int dir, int extend) {
    int ls = ed_line_start(e, e->cursor);
    /* GOAL COLUMN: moving through a short line and out the other side must
     * come back to the column you started in, not to the short line's end. */
    if (e->goal_col < 0) e->goal_col = e->cursor - ls;
    int goal = e->goal_col;
    if (dir < 0) {
        if (ls == 0) { e->cursor = 0; after_move(e, extend); return; }
        int pls = ed_line_start(e, ls - 1), ple = ls - 1;
        e->cursor = pls + clampi(goal, 0, ple - pls);
    } else {
        int le = ed_line_end(e, e->cursor);
        if (le >= e->len) { e->cursor = e->len; after_move(e, extend); return; }
        int nls = le + 1, nle = ed_line_end(e, nls);
        e->cursor = nls + clampi(goal, 0, nle - nls);
    }
    after_move(e, extend);
}

void ed_home(struct editor *e, int extend) {
    /* SMART HOME: first press goes to the first non-blank, second to column
     * zero. On indented code the first is nearly always what you meant. */
    int ls = ed_line_start(e, e->cursor);
    int fw = ls;
    while (fw < e->len && (e->buf[fw] == ' ' || e->buf[fw] == '\t')) fw++;
    e->cursor = (e->cursor == fw) ? ls : fw;
    e->goal_col = -1;
    after_move(e, extend);
}
void ed_end(struct editor *e, int extend) {
    e->cursor = ed_line_end(e, e->cursor);
    e->goal_col = -1;
    after_move(e, extend);
}
void ed_doc_start(struct editor *e, int extend) { e->cursor = 0; e->goal_col = -1; after_move(e, extend); }
void ed_doc_end(struct editor *e, int extend)   { e->cursor = e->len; e->goal_col = -1; after_move(e, extend); }

void ed_page(struct editor *e, int dir, int lines, int extend) {
    for (int i = 0; i < lines; i++) ed_move_line_v(e, dir, extend);
}

void ed_goto_line(struct editor *e, int line) {
    e->cursor = e->anchor = ed_offset_of_line(e, line - 1);
    e->goal_col = -1;
}

/* ---- find / replace ------------------------------------------------------ */

static int match_at(const char *hay, int hlen, int at, const char *nee, int nlen, int icase) {
    if (at + nlen > hlen) return 0;
    for (int i = 0; i < nlen; i++) {
        char a = hay[at + i], b = nee[i];
        if (icase) { a = (char)lower(a); b = (char)lower(b); }
        if (a != b) return 0;
    }
    return 1;
}

int ed_find(const struct editor *e, const char *needle, int from, int icase, int wrap) {
    if (!needle || !needle[0]) return -1;
    int nlen = (int)strlen(needle);
    for (int i = clampi(from, 0, e->len); i + nlen <= e->len; i++)
        if (match_at(e->buf, e->len, i, needle, nlen, icase)) return i;
    if (!wrap) return -1;
    for (int i = 0; i + nlen <= e->len && i < from; i++)
        if (match_at(e->buf, e->len, i, needle, nlen, icase)) return i;
    return -1;
}

int ed_find_prev(const struct editor *e, const char *needle, int from, int icase) {
    if (!needle || !needle[0]) return -1;
    int nlen = (int)strlen(needle);
    for (int i = clampi(from, 0, e->len) - 1; i >= 0; i--)
        if (match_at(e->buf, e->len, i, needle, nlen, icase)) return i;
    for (int i = e->len - nlen; i > from; i--)
        if (i >= 0 && match_at(e->buf, e->len, i, needle, nlen, icase)) return i;
    return -1;
}

int ed_replace(struct editor *e, const char *needle, const char *with, int icase) {
    if (!needle || !needle[0]) return 0;
    int nlen = (int)strlen(needle), wlen = with ? (int)strlen(with) : 0;
    int lo, hi; ed_sel_range(e, &lo, &hi);
    int did = 0;
    if (hi - lo == nlen && match_at(e->buf, e->len, lo, needle, nlen, icase)) {
        splice(e, lo, nlen, with, wlen, 0);
        did = 1;
    }
    int nx = ed_find(e, needle, e->cursor, icase, 1);
    if (nx >= 0) { e->anchor = nx; e->cursor = nx + nlen; }
    return did;
}

int ed_replace_all(struct editor *e, const char *needle, const char *with, int icase) {
    if (!needle || !needle[0]) return 0;
    int nlen = (int)strlen(needle), wlen = with ? (int)strlen(with) : 0;
    int n = 0, i = 0;
    /* Forward, resuming AFTER the replacement -- so replacing "a" with "aa"
     * terminates instead of feeding on its own output. */
    while (i + nlen <= e->len) {
        if (match_at(e->buf, e->len, i, needle, nlen, icase)) {
            splice(e, i, nlen, with, wlen, 0);
            i += wlen;
            n++;
        } else i++;
    }
    return n;
}

int ed_match_bracket(const struct editor *e, int off) {
    if (off < 0 || off >= e->len) return -1;
    const char *open = "([{", *close = ")]}";
    char c = e->buf[off];
    const char *o = strchr(open, c), *cl = strchr(close, c);
    int dir, want;
    if (o)       { dir = 1;  want = close[o - open]; }
    else if (cl) { dir = -1; want = open[cl - close]; }
    else return -1;
    int depth = 0;
    for (int i = off; i >= 0 && i < e->len; i += dir) {
        char d = e->buf[i];
        if (d == c) depth++;
        else if (d == want && --depth == 0) return i;
    }
    return -1;
}

/* ---- counting, words, comments, auto-close ------------------------------- */

int ed_count(const struct editor *e, const char *needle, int icase) {
    if (!needle || !needle[0]) return 0;
    int nlen = (int)strlen(needle), n = 0;
    for (int i = 0; i + nlen <= e->len; ) {
        if (match_at(e->buf, e->len, i, needle, nlen, icase)) { n++; i += nlen; }
        else i++;
    }
    return n;
}

int ed_match_index(const struct editor *e, const char *needle, int icase) {
    if (!needle || !needle[0]) return 0;
    int nlen = (int)strlen(needle), n = 0;
    int lo, hi; ed_sel_range(e, &lo, &hi);
    for (int i = 0; i + nlen <= e->len; ) {
        if (match_at(e->buf, e->len, i, needle, nlen, icase)) {
            n++;
            if (i == lo) return n;
            i += nlen;
        } else i++;
    }
    return 0;
}

int ed_word_at(const struct editor *e, int off, int *lo, int *hi) {
    off = clampi(off, 0, e->len);
    /* A click just past a word still means that word -- otherwise
     * double-clicking at the end of one selects nothing. */
    if (off > 0 && (off >= e->len || !is_word(e->buf[off])) && is_word(e->buf[off - 1])) off--;
    if (off >= e->len || !is_word(e->buf[off])) return 0;
    int a = off, b = off;
    while (a > 0 && is_word(e->buf[a - 1])) a--;
    while (b < e->len && is_word(e->buf[b])) b++;
    if (lo) *lo = a;
    if (hi) *hi = b;
    return 1;
}

void ed_select_word(struct editor *e, int off) {
    int a, b;
    if (!ed_word_at(e, off, &a, &b)) return;
    e->anchor = a; e->cursor = b;
}

void ed_select_line(struct editor *e, int off) {
    int ls = ed_line_start(e, off), le = ed_line_end(e, off);
    if (le < e->len) le++;                 /* include the newline, as editors do */
    e->anchor = ls; e->cursor = le;
}

int ed_toggle_comment(struct editor *e, const char *tok) {
    if (!tok || !tok[0]) return 0;
    int tlen = (int)strlen(tok);
    int lo, hi;
    if (ed_has_sel(e)) ed_sel_range(e, &lo, &hi);
    else lo = hi = e->cursor;
    int first = ed_line_start(e, lo), last = ed_line_start(e, hi);

    /* UNCOMMENT ONLY IF EVERY LINE IS COMMENTED. One shortcut doing both
     * directions needs a rule, and "all of them or none" is the one that
     * matches what a person means by toggling a block. */
    int all = 1;
    for (int line = first; ; ) {
        int fw = line;
        while (fw < e->len && (e->buf[fw] == ' ' || e->buf[fw] == '\t')) fw++;
        if (!match_at(e->buf, e->len, fw, tok, tlen, 0)) { all = 0; break; }
        if (line >= last) break;
        int le = ed_line_end(e, line);
        if (le >= e->len) break;
        line = le + 1;
    }

    /* Bottom-up, so the offsets above stay valid as the text changes. */
    int line = last, n = 0, delta = 0;
    for (;;) {
        int fw = line;
        while (fw < e->len && (e->buf[fw] == ' ' || e->buf[fw] == '\t')) fw++;
        if (all) {
            if (match_at(e->buf, e->len, fw, tok, tlen, 0)) {
                int extra = (fw + tlen < e->len && e->buf[fw + tlen] == ' ') ? 1 : 0;
                splice(e, fw, tlen + extra, 0, 0, 0);
                delta -= tlen + extra;
                n++;
            }
        } else {
            char ins[16];
            int k = 0;
            for (; k < tlen && k < (int)sizeof ins - 2; k++) ins[k] = tok[k];
            ins[k++] = ' ';
            splice(e, fw, 0, ins, k, 0);
            delta += k;
            n++;
        }
        if (line <= first) break;
        line = ed_line_start(e, line - 1);
    }
    /* PUT THE SELECTION BACK over the lines that were touched. splice collapses
     * it to the caret, so without this a second press sees one line instead of
     * the block and toggling a selection twice does not return it to where it
     * started -- which is the whole contract of a toggle. */
    e->anchor = first;
    e->cursor = clampi(hi + delta, first, e->len);
    e->cursor = ed_line_end(e, e->cursor);
    return n;
}

char ed_auto_close(char open) {
    switch (open) {
        case '(': return ')';
        case '[': return ']';
        case '{': return '}';
        case '"': return '"';
        case '\'': return '\'';
        default:  return 0;
    }
}

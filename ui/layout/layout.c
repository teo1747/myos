/* ui/layout/layout.c -- EmbLink UI Piece 5 (see layout.h).
 *
 * Phase 1 measures intrinsic sizes bottom-up; Phase 2 arranges top-down with
 * CSS-accurate flex grow/shrink, justify/align, and text wrapping, writing the
 * resolved parent-relative geometry straight into Piece 3's scene nodes. */

#include "layout.h"
#include <stdlib.h>

/* ------------------------------------------------------------------------- */
/* arena (same paged + generation ABA-guard discipline as Piece 3)           */
/* ------------------------------------------------------------------------- */

static void lz(void *p, size_t n) { unsigned char *b = p; for (size_t i = 0; i < n; i++) b[i] = 0; }

static struct layout_node *lslot(struct layout_arena *a, uint32_t idx) {
    uint32_t pg = idx / LAYOUT_PAGE_SIZE, off = idx % LAYOUT_PAGE_SIZE;
    if (pg >= LAYOUT_MAX_PAGES || !a->pages[pg]) return 0;
    return &a->pages[pg][off];
}
static bool lensure(struct layout_arena *a, uint32_t idx) {
    uint32_t pg = idx / LAYOUT_PAGE_SIZE;
    if (pg >= LAYOUT_MAX_PAGES) return false;
    if (a->pages[pg]) return true;
    struct layout_node *p = malloc(sizeof(struct layout_node) * LAYOUT_PAGE_SIZE);
    if (!p) return false;
    lz(p, sizeof(struct layout_node) * LAYOUT_PAGE_SIZE);
    a->pages[pg] = p; a->n_pages_allocated++;
    return true;
}
void layout_arena_init(struct layout_arena *a) { lz(a, sizeof(*a)); a->next_never_used = 1; }
void layout_arena_destroy(struct layout_arena *a) {
    for (uint32_t i = 0; i < LAYOUT_MAX_PAGES; i++) { free(a->pages[i]); a->pages[i] = 0; }
    a->n_pages_allocated = 0; a->free_list_head = 0; a->next_never_used = 1;
}

struct layout_node *layout_resolve(struct layout_arena *a, struct layout_handle h) {
    if (h.index == 0) return 0;
    struct layout_node *n = lslot(a, h.index);
    if (!n || n->self.index != h.index || n->self.generation != h.generation) return 0;
    return n;
}

static void lappend_child(struct layout_arena *a, struct layout_handle parent, struct layout_handle child);

static void lunlink_from_parent(struct layout_arena *a, struct layout_handle h) {
    struct layout_node *n = layout_resolve(a, h);
    if (!n) return;
    struct layout_node *p = layout_resolve(a, n->parent);
    if (!p) { n->next_sibling = LAYOUT_HANDLE_NULL; return; }
    if (p->first_child.index == h.index) { p->first_child = n->next_sibling; }
    else {
        struct layout_handle it = p->first_child;
        while (!layout_handle_is_null(it)) {
            struct layout_node *in = layout_resolve(a, it);
            if (!in) break;
            if (in->next_sibling.index == h.index) { in->next_sibling = n->next_sibling; break; }
            it = in->next_sibling;
        }
    }
    n->next_sibling = LAYOUT_HANDLE_NULL;
}

void layout_reparent(struct layout_arena *a, struct layout_handle h,
                     struct layout_handle new_parent, struct layout_handle after) {
    struct layout_node *n = layout_resolve(a, h);
    struct layout_node *np = layout_resolve(a, new_parent);
    if (!n || !np) return;
    lunlink_from_parent(a, h);
    n->parent = new_parent;
    if (layout_handle_is_null(after)) {
        n->next_sibling = np->first_child;
        np->first_child = h;
    } else {
        struct layout_node *as = layout_resolve(a, after);
        if (!as || after.index == h.index) {   /* bad/self anchor -> append last */
            n->next_sibling = LAYOUT_HANDLE_NULL; lappend_child(a, new_parent, h); return;
        }
        n->next_sibling = as->next_sibling;
        as->next_sibling = h;
        if (n->next_sibling.index == h.index)  /* never point at self */
            n->next_sibling = LAYOUT_HANDLE_NULL;
    }
}

static uint32_t lalloc_slot(struct layout_arena *a) {
    if (a->free_list_head) {
        uint32_t i = a->free_list_head;
        struct layout_node *n = lslot(a, i);
        a->free_list_head = n ? n->next_sibling.index : 0;
        return i;
    }
    uint32_t i = a->next_never_used;
    if (i / LAYOUT_PAGE_SIZE >= LAYOUT_MAX_PAGES || !lensure(a, i)) return 0;
    a->next_never_used++;
    return i;
}

static void lappend_child(struct layout_arena *a, struct layout_handle parent, struct layout_handle child) {
    struct layout_node *p = layout_resolve(a, parent);
    if (!p) return;
    if (layout_handle_is_null(p->first_child)) { p->first_child = child; return; }
    struct layout_handle c = p->first_child;
    for (;;) {
        struct layout_node *cn = layout_resolve(a, c);
        if (!cn || layout_handle_is_null(cn->next_sibling)) { if (cn) cn->next_sibling = child; return; }
        c = cn->next_sibling;
    }
}

struct layout_handle layout_create_node(struct layout_arena *a, struct layout_handle parent) {
    uint32_t idx = lalloc_slot(a);
    if (!idx) return LAYOUT_HANDLE_NULL;
    struct layout_node *n = lslot(a, idx);
    uint32_t gen = n->self.generation; if (gen == 0) gen = 1;
    lz(n, sizeof(*n));
    n->self.index = idx; n->self.generation = gen;
    n->width.mode = SIZE_INTRINSIC; n->height.mode = SIZE_INTRINSIC;
    n->grid_row = -1; n->grid_col = -1; n->grid_rowspan = 1;   /* auto-flow */
    struct layout_handle h = n->self;
    if (layout_resolve(a, parent)) { n->parent = parent; lappend_child(a, parent, h); }
    return h;
}

void layout_destroy_node(struct layout_arena *a, struct layout_handle h) {
    struct layout_node *n = layout_resolve(a, h);
    if (!n) return;
    /* UNLINK IT FROM ITS PARENT FIRST. Without this the parent keeps pointing
     * at a slot that is on the free list, and the moment that slot is handed
     * out again -- with a bumped generation -- the parent's link resolves to
     * nothing and the child walk STOPS THERE. Every sibling after the
     * destroyed node silently leaves the tree: still built, still linked in the
     * instance tree, never measured and never arranged, so it collapses to
     * nothing at the origin.
     *
     * The declarative layer has always done this (destroy_instance calls
     * inst_unlink_child); the layout arena simply forgot the same step, and it
     * only shows on a subtree that is removed and later rebuilt -- a dialog, a
     * menu, the desktop's application launcher opening a second time. */
    lunlink_from_parent(a, h);
    struct layout_handle c = n->first_child;
    while (!layout_handle_is_null(c)) {
        struct layout_node *cn = layout_resolve(a, c);
        struct layout_handle next = cn ? cn->next_sibling : LAYOUT_HANDLE_NULL;
        layout_destroy_node(a, c);
        c = next;
    }
    n->self.index = 0; n->self.generation++;
    n->next_sibling.index = a->free_list_head; a->free_list_head = h.index;
}

/* ------------------------------------------------------------------------- */
/* font metric helpers                                                       */
/* ------------------------------------------------------------------------- */

static float text_char_advance(struct font *f, uint32_t cp, float size_px) {
    struct glyph_cache_entry *e = glyph_cache_lookup_or_rasterize(font_global_atlas(), f, cp, size_px);
    return e ? e->advance_px : 0.0f;
}
/* One-line, unwrapped pixel width (Phase-1 text intrinsic).
 *
 * This MUST step the string the way the renderer does. It used to walk BYTES:
 * every non-ASCII character -- every icon glyph, every chevron, arrow, accent --
 * was measured as its 2-4 raw UTF-8 bytes looked up as separate codepoints, so
 * a single icon measured roughly THREE garbage advances wide while the renderer
 * drew one glyph. Centring then centred that inflated box, which is why every
 * icon sat far left inside an oversized button: the button was not too big and
 * the glyph was not off-centre, the string was measured as the wrong string. */
static float text_line_width(struct font *f, const char *s, float size_px) {
    float w = 0;
    for (const char *p = s; *p; ) {
        uint32_t cp;
        p += font_utf8_decode(p, &cp);
        w += text_char_advance(f, cp, size_px);
    }
    return w;
}
static float font_line_height(struct font *f, float size_px) {
    float scale = size_px / (float)f->units_per_em;
    return (float)(f->ascent + f->descent + f->line_gap) * scale;
}

/* resolve a layout node's paired scene node (for TEXT/IMAGE reads + write-back) */
static struct scene_node *paired(struct scene_arena *sa, struct layout_node *n) {
    return scene_resolve(sa, n->scene_node);
}
static int is_text(struct scene_arena *sa, struct layout_node *n) {
    struct scene_node *s = paired(sa, n);
    return s && s->kind == SCENE_NODE_TEXT;
}

/* ------------------------------------------------------------------------- */
/* text wrapping (Section 4)                                                 */
/* ------------------------------------------------------------------------- */

int layout_debug_wrap_lines(struct layout_arena *la, struct scene_arena *sa,
                            struct layout_handle text_node, float width) {
    struct layout_node *n = layout_resolve(la, text_node);
    if (!n) return 0;
    struct scene_node *s = paired(sa, n);
    if (!s || s->kind != SCENE_NODE_TEXT || !s->data.text.utf8) return 0;
    struct font *f = font_for_handle(s->data.text.font_handle);
    if (!f) return 0;
    float size = s->data.text.size_px;
    const char *str = s->data.text.utf8;
    float space_w = text_char_advance(f, ' ', size);

    int lines = 1; float cur = 0;
    const unsigned char *p = (const unsigned char *)str;
    while (*p) {
        if (*p == ' ') { p++; continue; }               /* word boundary */
        const unsigned char *w0 = p;
        while (*p && *p != ' ') p++;                     /* word = [w0, p) */
        float word_w = 0;
        for (const char *q = (const char *)w0; q < (const char *)p; ) {
            uint32_t cp; q += font_utf8_decode(q, &cp);
            word_w += text_char_advance(f, cp, size);
        }

        if (word_w <= width) {                           /* whole word fits on a line */
            if (cur == 0) cur = word_w;
            else if (cur + space_w + word_w <= width) cur += space_w + word_w;
            else { lines++; cur = word_w; }
        } else {                                         /* character-wrap fallback */
            if (cur > 0) { lines++; cur = 0; }
            for (const char *q = (const char *)w0; q < (const char *)p; ) {
                uint32_t cp; q += font_utf8_decode(q, &cp);
                float cw = text_char_advance(f, cp, size);
                if (cur == 0) cur = cw;
                else if (cur + cw <= width) cur += cw;
                else { lines++; cur = cw; }
            }
        }
    }
    return lines;
}

float layout_measure_height_at_width(struct layout_arena *la, struct scene_arena *sa,
                                     struct layout_handle text_node, float width) {
    struct layout_node *n = layout_resolve(la, text_node);
    if (!n) return 0;
    struct scene_node *s = paired(sa, n);
    if (!s || s->kind != SCENE_NODE_TEXT) return 0;
    struct font *f = font_for_handle(s->data.text.font_handle);
    if (!f) return 0;
    /* memoized: wrapping is a pure function of (content, font, size, width),
     * and this is called from SEVEN sites per arrange -- for a document,
     * hundreds of times per frame at an unchanged width. The line-width memo
     * above owns the (hash,font,size) part of the key; this adds width. */
    if (s->data.text.hash &&
        n->meas_hash == s->data.text.hash &&
        n->meas_font == s->data.text.font_handle &&
        n->meas_size == s->data.text.size_px &&
        n->meas_wrap_w == width)
        return n->meas_wrap_h;
    int lines = layout_debug_wrap_lines(la, sa, text_node, width);
    float h = lines * font_line_height(f, s->data.text.size_px);
    if (s->data.text.hash &&
        n->meas_hash == s->data.text.hash &&
        n->meas_font == s->data.text.font_handle &&
        n->meas_size == s->data.text.size_px) {
        n->meas_wrap_w = width;
        n->meas_wrap_h = h;
    }
    return h;
}

/* ------------------------------------------------------------------------- */
/* Phase 1: intrinsic sizes (bottom-up)                                      */
/* ------------------------------------------------------------------------- */

static void measure_intrinsic(struct layout_arena *la, struct scene_arena *sa, struct layout_handle h) {
    struct layout_node *n = layout_resolve(la, h);
    if (!n) return;
    for (struct layout_handle c = n->first_child; !layout_handle_is_null(c); ) {
        struct layout_node *cn = layout_resolve(la, c);
        struct layout_handle next = cn ? cn->next_sibling : LAYOUT_HANDLE_NULL;
        measure_intrinsic(la, sa, c);
        c = next;
    }
    n = layout_resolve(la, h);   /* re-resolve (children calls may have grown pages) */

    /* --- intrinsic width --- */
    if (n->width.mode == SIZE_FIXED) {
        n->intrinsic_w = n->width.fixed_value;
    } else if (!n->is_container) {
        struct scene_node *s = paired(sa, n);
        if (s && s->kind == SCENE_NODE_TEXT && s->data.text.utf8) {
            struct font *f = font_for_handle(s->data.text.font_handle);
            /* memoized: same content+font+size -> same width, no glyph walk */
            if (f && s->data.text.hash &&
                n->meas_hash == s->data.text.hash &&
                n->meas_font == s->data.text.font_handle &&
                n->meas_size == s->data.text.size_px) {
                n->intrinsic_w = n->meas_line_w;
            } else {
                n->intrinsic_w = f ? text_line_width(f, s->data.text.utf8, s->data.text.size_px) : 0;
                if (f && s->data.text.hash) {
                    n->meas_hash   = s->data.text.hash;
                    n->meas_font   = s->data.text.font_handle;
                    n->meas_size   = s->data.text.size_px;
                    n->meas_line_w = n->intrinsic_w;
                    n->meas_wrap_w = -1.0f;        /* new content: old wrap memo dies */
                }
            }
        } else if (s && s->kind == SCENE_NODE_IMAGE) {
            n->intrinsic_w = (float)s->data.image.w;
        } else {
            n->intrinsic_w = 0;
        }
    } else {
        float sum = 0, mx = 0; int cnt = 0;
        for (struct layout_handle c = n->first_child; !layout_handle_is_null(c); ) {
            struct layout_node *cn = layout_resolve(la, c);
            if (!cn) break;
            if (cn->is_overlay) { c = cn->next_sibling; continue; }   /* out of flow */
            sum += cn->intrinsic_w; if (cn->intrinsic_w > mx) mx = cn->intrinsic_w; cnt++;
            c = cn->next_sibling;
        }
        if (n->axis == AXIS_ROW) n->intrinsic_w = sum + (cnt > 1 ? n->spacing * (cnt - 1) : 0);
        else                     n->intrinsic_w = mx;
        n->intrinsic_w += n->padding_left + n->padding_right;
    }

    /* --- intrinsic height (cross for ROW / main for COLUMN). TEXT height is
     * deferred to arrange (Section 2/3); use one line as a placeholder. --- */
    if (n->height.mode == SIZE_FIXED) {
        n->intrinsic_h = n->height.fixed_value;
    } else if (!n->is_container) {
        struct scene_node *s = paired(sa, n);
        if (s && s->kind == SCENE_NODE_TEXT) {
            struct font *f = font_for_handle(s->data.text.font_handle);
            n->intrinsic_h = f ? font_line_height(f, s->data.text.size_px) : 0;
        } else if (s && s->kind == SCENE_NODE_IMAGE) {
            n->intrinsic_h = (float)s->data.image.h;
        } else {
            n->intrinsic_h = 0;
        }
    } else {
        float sum = 0, mx = 0; int cnt = 0;
        for (struct layout_handle c = n->first_child; !layout_handle_is_null(c); ) {
            struct layout_node *cn = layout_resolve(la, c);
            if (!cn) break;
            if (cn->is_overlay) { c = cn->next_sibling; continue; }   /* out of flow */
            sum += cn->intrinsic_h; if (cn->intrinsic_h > mx) mx = cn->intrinsic_h; cnt++;
            c = cn->next_sibling;
        }
        if (n->axis == AXIS_COLUMN) n->intrinsic_h = sum + (cnt > 1 ? n->spacing * (cnt - 1) : 0);
        else                        n->intrinsic_h = mx;
        n->intrinsic_h += n->padding_top + n->padding_bottom;
    }
}

/* ------------------------------------------------------------------------- */
/* Phase 2: arrange (top-down)                                               */
/* ------------------------------------------------------------------------- */

static void write_scene(struct scene_arena *sa, struct layout_node *n) {
    scene_set_size(sa, n->scene_node, n->resolved_w, n->resolved_h);
    /* resolved position + the authored post-layout offset (transitions/slides) */
    scene_set_transform(sa, n->scene_node,
                        n->resolved_x + n->offset_x, n->resolved_y + n->offset_y, 0,
                        0, 0, 0, 1, 1, 1, 1);
}

static void arrange(struct layout_arena *la, struct scene_arena *sa,
                    struct layout_handle h, float W, float H);

/* Height a WRAP row container needs when its children flow into lines at total
 * width `avail_w`. Mirrors the text height-depends-on-width path so a column
 * parent can size a wrapping child before placing its siblings. Row-wrap only. */
static float measure_wrap_height(struct layout_arena *la, struct scene_arena *sa,
                                 struct layout_handle h, float avail_w);
static float measure_grid_height(struct layout_arena *la, struct scene_arena *sa,
                                 struct layout_handle h, float avail_w);

/* Height of a subtree at a KNOWN width.
 *
 * The missing case. arrange already measures a child properly when it is text
 * (wrapped at its width), a grid, or a wrap row -- but a plain CONTAINER child
 * fell through to its `intrinsic_h`, which measure_intrinsic computes bottom-up
 * before any width is known and which therefore reports a wrapping row as ONE
 * LINE.
 *
 * That is invisible until a wrap row is nested. A document is exactly that:
 * column -> block -> Flow. When the column measured its blocks it used each
 * block's intrinsic height, so every paragraph was budgeted one line however
 * many it actually wrapped to, and the next block was placed on top of it.
 * measure_wrap_height was computing the right answer (54 boxes, 3 lines,
 * 48.9px) for a caller that no longer existed at that depth.
 *
 * So: recurse, applying the same rules arrange does, at the width the child
 * will actually get. */
/* Does anything in here wrap? Intrinsic heights are computed before any width
 * is known, so they are right for everything EXCEPT a subtree that wraps --
 * which is why the expensive width-dependent measurement is gated on this
 * rather than run everywhere. */
static int subtree_wraps(struct layout_arena *la, struct layout_handle h) {
    struct layout_node *n = layout_resolve(la, h);
    if (!n || !n->is_container) return 0;
    if (n->wrap && n->axis == AXIS_ROW) return 1;
    for (struct layout_handle c = n->first_child; !layout_handle_is_null(c); ) {
        struct layout_node *cn = layout_resolve(la, c);
        if (!cn) break;
        if (subtree_wraps(la, c)) return 1;
        c = cn->next_sibling;
    }
    return 0;
}

static float measure_subtree_height(struct layout_arena *la, struct scene_arena *sa,
                                    struct layout_handle h, float avail_w) {
    struct layout_node *n = layout_resolve(la, h);
    if (!n) return 0;
    if (n->height.mode == SIZE_FIXED) return n->height.fixed_value;
    if (!n->is_container)
        return is_text(sa, n) ? layout_measure_height_at_width(la, sa, h, avail_w)
                              : n->intrinsic_h;

    float cw = avail_w - n->padding_left - n->padding_right;
    if (cw < 0) cw = 0;

    /* A GRID measures by ROWS, not by children. Without this the column branch
     * below sums every CELL -- a 3x5 table reported the height of fifteen rows
     * instead of five, and everything after it on the page was pushed off the
     * bottom while the table itself drew correctly. Exactly the hole
     * measure_wrap_height fills for wrapping rows, one shape over. */
    if (n->grid_cols > 0) return measure_grid_height(la, sa, h, avail_w);

    if (n->axis == AXIS_ROW) {
        if (n->wrap) return measure_wrap_height(la, sa, h, avail_w);

        /* A row is its tallest child -- but the height of a child that wraps
         * depends on the width it will GET, so the widths have to be shared out
         * first. A flexible child measured at its own intrinsic width is
         * measured unwrapped, and reports one line. That is a list item: the
         * bullet takes its intrinsic width and the text column takes the rest,
         * and if you hand that column its intrinsic width instead you get a
         * one-line row with three lines of text hanging out the bottom of it,
         * under whatever comes next. */
        float fixed_sum = 0; int cnt = 0, flex = 0;
        for (struct layout_handle c = n->first_child; !layout_handle_is_null(c); ) {
            struct layout_node *cn = layout_resolve(la, c);
            if (!cn) break;
            if (!cn->is_overlay) {
                cnt++;
                if (cn->width.mode == SIZE_FLEX || cn->width.flex_grow > 0) flex++;
                else fixed_sum += (cn->width.mode == SIZE_FIXED) ? cn->width.fixed_value
                                                                 : cn->intrinsic_w;
            }
            c = cn->next_sibling;
        }
        if (cnt > 1) fixed_sum += n->spacing * (float)(cnt - 1);
        float share = flex > 0 ? (cw - fixed_sum) / (float)flex : 0;
        if (share < 0) share = 0;

        float mx = 0;
        for (struct layout_handle c = n->first_child; !layout_handle_is_null(c); ) {
            struct layout_node *cn = layout_resolve(la, c);
            if (!cn) break;
            if (!cn->is_overlay) {
                float w;
                if (cn->width.mode == SIZE_FIXED)                        w = cn->width.fixed_value;
                else if (cn->width.mode == SIZE_FLEX || cn->width.flex_grow > 0) w = share;
                else w = cn->intrinsic_w > 0 ? cn->intrinsic_w : cw;
                float ch = measure_subtree_height(la, sa, c, w);
                if (ch > mx) mx = ch;
            }
            c = cn->next_sibling;
        }
        return mx + n->padding_top + n->padding_bottom;
    }

    float total = 0; int cnt = 0;                      /* a column is their sum */
    for (struct layout_handle c = n->first_child; !layout_handle_is_null(c); ) {
        struct layout_node *cn = layout_resolve(la, c);
        if (!cn) break;
        if (!cn->is_overlay) { total += measure_subtree_height(la, sa, c, cw); cnt++; }
        c = cn->next_sibling;
    }
    if (cnt > 1) total += n->spacing * (float)(cnt - 1);
    return total + n->padding_top + n->padding_bottom;
}

static float measure_wrap_height(struct layout_arena *la, struct scene_arena *sa,
                                 struct layout_handle h, float avail_w) {
    struct layout_node *n = layout_resolve(la, h);
    if (!n || !n->is_container) return 0;
    float content_w = avail_w - n->padding_left - n->padding_right;
    if (content_w < 0) content_w = 0;
    /* 64 was sized for chips and tags. A DOCUMENT's inline run is one box per
     * WORD -- hundreds of them -- and stopping at 64 measured the height of the
     * first sixty-four words only, so a wrapped paragraph reported a fraction
     * of its real height and the next block was laid straight over it. The cap
     * still exists (this runs during measurement and must not recurse forever),
     * but anything past it is now ACCOUNTED FOR rather than dropped. */
    #define WRAP_MAX 512
    float bm[WRAP_MAX], bc[WRAP_MAX]; int nk = 0, dropped = 0;
    for (struct layout_handle c = n->first_child; !layout_handle_is_null(c); ) {
        struct layout_node *cn = layout_resolve(la, c);
        if (!cn) break;
        if (!cn->is_overlay) {
            if (nk < WRAP_MAX) {
                float cw = (cn->width.mode == SIZE_FIXED) ? cn->width.fixed_value : cn->intrinsic_w;
                float ch = is_text(sa, cn) ? layout_measure_height_at_width(la, sa, c, cw)
                         : (cn->height.mode == SIZE_FIXED ? cn->height.fixed_value : cn->intrinsic_h);
                bm[nk] = cw; bc[nk] = ch; nk++;
            } else dropped++;
        }
        c = cn->next_sibling;
    }
    float total = 0; int i = 0, nlines = 0;
    while (i < nk) {
        int j = i; float lm = 0, lc = 0;
        while (j < nk) {
            float add = bm[j] + (j > i ? n->spacing : 0);
            if (j > i && lm + add > content_w + 0.5f) break;
            lm += add; if (bc[j] > lc) lc = bc[j]; j++;
        }
        if (j == i) j = i + 1;
        total += lc + (nlines > 0 ? n->spacing : 0);
        nlines++; i = j;
    }
    /* the overflow past the cap: charge it the average line, so a very long
     * run is over-measured slightly rather than under-measured badly */
    if (dropped && nlines > 0) total += (total / (float)nlines) * ((float)dropped / (float)(nk / (nlines ? nlines : 1) + 1));
    return total + n->padding_top + n->padding_bottom;
}

/* Height of a GRID container at total width `avail_w`: N equal columns, children
 * auto-flow with colspan, each row as tall as its tallest cell. Same width->
 * height deferral so a column parent can size a grid child before its siblings. */
/* --- automatic table layout, in miniature -------------------------------
 *
 * A column is as wide as its widest cell, and whatever is left over (or
 * missing) is shared out in proportion. Equal columns are the right answer for
 * a grid of TILES and the wrong one for a TABLE: they made the rank column of
 * a news page exactly as wide as its headline column, which is how a real site
 * ends up looking like a spreadsheet.
 *
 * Deliberately simpler than CSS's real algorithm, which distributes surplus
 * between min-content and max-content per column. This uses one measurement
 * (each cell's intrinsic width) and scales, in both directions -- which gets
 * the shape of a layout table right and degrades sanely on a dense one. */
#define GRID_MAX_COLS 64
#define GRID_MAX_ROWS 256
/* The measure pass gathers children onto the C STACK, so this is a bound on
 * how many a grid can be measured with -- generous for a page layout and well
 * under a table, which is why a table's rows are measured the same way but
 * counted, not indexed. Beyond it the tail is measured as if it were on the
 * last row, which is wrong by a row rather than catastrophically. */
#define GRID_MEASURE_KIDS 1024

static void grid_col_widths(struct layout_arena *la, struct layout_node *n,
                            float content_w, float *out) {
    int cols = n->grid_cols;
    if (cols > GRID_MAX_COLS) cols = GRID_MAX_COLS;
    float cgap = n->grid_col_gap;
    float want[GRID_MAX_COLS];
    for (int i = 0; i < cols; i++) want[i] = 0;

    /* Pass 1: cells that occupy ONE column set that column's appetite. A
     * spanning cell says nothing about any single column it covers. */
    int col = 0;
    for (struct layout_handle c = n->first_child; !layout_handle_is_null(c); ) {
        struct layout_node *cn = layout_resolve(la, c);
        if (!cn) break;
        if (!cn->is_overlay) {
            int span = cn->grid_span > 0 ? cn->grid_span : 1;
            if (span > cols) span = cols;
            if (col > 0 && col + span > cols) col = 0;
            if (span == 1 && col < cols) {
                float w = (cn->width.mode == SIZE_FIXED) ? cn->width.fixed_value
                                                         : cn->intrinsic_w;
                if (w > want[col]) want[col] = w;
            }
            col += span;
        }
        c = cn->next_sibling;
    }

    /* Pass 2: a spanning cell must still FIT across the columns it covers; any
     * shortfall goes to the last of them, which is where a browser puts it. */
    col = 0;
    for (struct layout_handle c = n->first_child; !layout_handle_is_null(c); ) {
        struct layout_node *cn = layout_resolve(la, c);
        if (!cn) break;
        if (!cn->is_overlay) {
            int span = cn->grid_span > 0 ? cn->grid_span : 1;
            if (span > cols) span = cols;
            if (col > 0 && col + span > cols) col = 0;
            if (span > 1 && col + span <= cols) {
                float w = (cn->width.mode == SIZE_FIXED) ? cn->width.fixed_value
                                                         : cn->intrinsic_w;
                float have = cgap * (float)(span - 1);
                for (int k = 0; k < span; k++) have += want[col + k];
                if (w > have) want[col + span - 1] += w - have;
            }
            col += span;
        }
        c = cn->next_sibling;
    }

    float avail = content_w - cgap * (float)(cols - 1);
    if (avail < 0) avail = 0;

    /* EXPLICIT TRACKS, when the author stated them. Fixed tracks take their
     * width first; whatever is left is shared among the fr tracks by weight;
     * an `auto` track keeps the content appetite computed above.
     *
     * This is what separates a page layout from a table. Sizing every track by
     * its content is right for a <table> -- the columns should fit what is in
     * them -- and wrong for `grid-template-columns: 12.25rem minmax(0,1fr)`,
     * where the first number is a decision the author already made. Wikipedia
     * builds its whole chrome that way, and content-sizing it put the sidebar
     * and the article in two columns divided by how much text each held. */
    if (n->grid_ntrack > 0) {
        int nt = n->grid_ntrack < cols ? n->grid_ntrack : cols;
        float fixed = 0, frsum = 0;
        for (int i = 0; i < cols; i++) {
            unsigned char m = i < nt ? n->grid_track_mode[i] : LT_AUTO;
            if (m == LT_PX)      { out[i] = n->grid_track_val[i]; fixed += out[i]; }
            else if (m == LT_FR) { out[i] = 0; frsum += n->grid_track_val[i]; }
            else                 { out[i] = want[i]; fixed += out[i]; }
        }
        float left = avail - fixed;
        if (left < 0) left = 0;
        if (frsum > 0.0001f)
            for (int i = 0; i < cols; i++)
                if (i < nt && n->grid_track_mode[i] == LT_FR)
                    out[i] = left * (n->grid_track_val[i] / frsum);
        return;
    }

    float sum = 0;
    for (int i = 0; i < cols; i++) sum += want[i];
    if (sum <= 0.01f) {                    /* nothing to go on: share equally */
        float e = cols > 0 ? avail / (float)cols : 0;
        for (int i = 0; i < cols; i++) out[i] = e;
        return;
    }
    float k = avail / sum;                 /* grows AND shrinks: same formula */
    for (int i = 0; i < cols; i++) out[i] = want[i] * k;
}

/* WHERE EVERY CHILD SITS IN THE GRID -- resolved once, by one function.
 *
 * Both the measure pass and the arrange pass need this, and the code below
 * already carries a scar from the last time they were written twice and
 * disagreed: the row takes its height from its tallest cell, so a placement
 * the two passes compute differently makes rows overlap. Now they cannot
 * disagree, because there is only one answer.
 *
 * A child with `grid-area` states its row and column; everything else
 * auto-flows into the next free run of columns, wrapping when it will not fit
 * -- which is exactly what the grid did before placement existed, so a grid
 * with no explicit placement lays out identically.
 *
 * Returns the number of ROWS the grid needs; fills r/c/rs/cs per child in
 * document order. Auto-flow does NOT hunt for gaps left by explicitly placed
 * cells: CSS calls that "sparse" packing and it is a refinement, not the
 * behaviour anything here depends on. */
static int grid_places(struct layout_arena *la, struct layout_node *n, int cols,
                       struct layout_handle *kid, int nk,
                       unsigned short *r, unsigned short *c,
                       unsigned short *rs, unsigned short *cs) {
    (void)n;
    int flow_r = 0, flow_c = 0, rows = 0;
    for (int i = 0; i < nk; i++) {
        struct layout_node *k = kid ? layout_resolve(la, kid[i]) : 0;
        if (!k || k->is_overlay) { r[i] = c[i] = 0; rs[i] = cs[i] = 0; continue; }
        int span = k->grid_span > 0 ? k->grid_span : 1;
        if (span > cols) span = cols;
        int rspan = k->grid_rowspan > 0 ? k->grid_rowspan : 1;
        if (k->grid_row >= 0 && k->grid_col >= 0) {
            int gr = k->grid_row < GRID_MAX_ROWS ? k->grid_row : GRID_MAX_ROWS - 1;
            r[i] = (unsigned short)gr;
            c[i] = (unsigned short)(k->grid_col < cols ? k->grid_col : cols - 1);
        } else {
            if (flow_c > 0 && flow_c + span > cols) { flow_r++; flow_c = 0; }
            /* CLAMPED, and the type is wide enough to hold it. A one-column
             * grid of a thousand items reaches row 1000, and an unsigned char
             * turns that into row 232 -- the item is then placed in a row that
             * already has one, its height lands on the wrong row, and the
             * measure and arrange passes stop agreeing about how tall the grid
             * is. That is not an off-by-one: it put boxes 300000 pixels down a
             * page whose own height was 14781. */
            if (flow_r > GRID_MAX_ROWS - 1) flow_r = GRID_MAX_ROWS - 1;
            r[i] = (unsigned short)flow_r; c[i] = (unsigned short)flow_c;
            flow_c += span;
        }
        rs[i] = (unsigned short)rspan; cs[i] = (unsigned short)span;
        if (r[i] + rspan > rows) rows = r[i] + rspan;
    }
    return rows;
}

/* x of a column's left edge, given the resolved widths */
static float grid_col_x(const float *w, float cgap, int col) {
    float x = 0;
    for (int i = 0; i < col; i++) x += w[i] + cgap;
    return x;
}

static float measure_grid_height(struct layout_arena *la, struct scene_arena *sa,
                                 struct layout_handle h, float avail_w) {
    struct layout_node *n = layout_resolve(la, h);
    if (!n || n->grid_cols <= 0) return 0;
    /* clamped: colw[] is indexed by it */
    int cols = n->grid_cols > GRID_MAX_COLS ? GRID_MAX_COLS : n->grid_cols;
    float cgap = n->grid_col_gap, rgap = n->grid_row_gap;
    float content_w = avail_w - n->padding_left - n->padding_right;
    if (content_w < 0) content_w = 0;
    float colw[GRID_MAX_COLS];
    grid_col_widths(la, n, content_w, colw);

    /* Gather the children, place them, then sum the ROW heights -- rows now
     * being real indices rather than "however many times the flow wrapped",
     * because a grid-area can put two children on the same row out of
     * document order. */
    struct layout_handle kid[GRID_MEASURE_KIDS];
    int nk = 0;
    for (struct layout_handle c = n->first_child;
         !layout_handle_is_null(c) && nk < GRID_MEASURE_KIDS; ) {
        struct layout_node *cn = layout_resolve(la, c);
        if (!cn) break;
        kid[nk++] = c;
        c = cn->next_sibling;
    }
    unsigned short pr[GRID_MEASURE_KIDS], pc[GRID_MEASURE_KIDS],
                   prs[GRID_MEASURE_KIDS], pcs[GRID_MEASURE_KIDS];
    int nrows = grid_places(la, n, cols, kid, nk, pr, pc, prs, pcs);
    if (nrows > GRID_MAX_ROWS) nrows = GRID_MAX_ROWS;
    float rowh[GRID_MAX_ROWS];
    for (int i = 0; i < nrows; i++) rowh[i] = 0;

    for (int i = 0; i < nk; i++) {
        struct layout_node *cn = layout_resolve(la, kid[i]);
        if (!cn || cn->is_overlay || !pcs[i]) continue;
        float cw = cgap * (float)(pcs[i] - 1);
        for (int k2 = 0; k2 < pcs[i] && pc[i] + k2 < cols; k2++) cw += colw[pc[i] + k2];
        /* A CELL is usually a container, and a container's intrinsic_h was
         * computed before any width was known -- so a cell whose text
         * wraps measured as ONE LINE. In a grid that is not a cosmetic
         * error: the row takes its height from the tallest cell, so every
         * row after it sat too high and the table's rows overlapped.
         * Measure the subtree at the width the cell will actually get. */
        float ch = (cn->height.mode == SIZE_FIXED) ? cn->height.fixed_value
                 : is_text(sa, cn) ? layout_measure_height_at_width(la, sa, kid[i], cw)
                 : cn->is_container ? measure_subtree_height(la, sa, kid[i], cw)
                 : cn->intrinsic_h;
        /* A cell spanning rows contributes to the LAST of them, the same rule
         * grid_col_widths uses for a cell spanning columns. */
        int rr = pr[i] + prs[i] - 1;
        if (rr >= nrows) rr = nrows - 1;
        if (rr >= 0 && ch > rowh[rr]) rowh[rr] = ch;
    }
    float total = 0;
    for (int i = 0; i < nrows; i++) total += rowh[i];
    total += rgap * (nrows > 1 ? nrows - 1 : 0);
    return total + n->padding_top + n->padding_bottom;
}

/* Children are gathered into a SHARED, depth-stacked scratch pool rather than
 * onto the C stack.
 *
 * They used to live in `kids[64]` and friends, four fixed arrays per arrange()
 * frame. Sixty-four is plenty for an application's dialog and nowhere near
 * enough for a document: a <ul> with a hundred items, or a table with thirty
 * rows, walked straight past it -- and the overflow was SILENT. The extra
 * children were never arranged, so they kept their default 0x0 and every one
 * of them painted at the parent's origin, stacked on top of the first row.
 * That is what the "overlapping first list item" on danluu.com was.
 *
 * They cannot simply be made bigger: arrange() recurses once per nesting
 * level, so a 1024-entry set of stack arrays would be megabytes deep on a real
 * document. arrange() is strictly depth-first, so a bump allocator restored on
 * exit is exactly the right shape -- and the wrapper below is what guarantees
 * the restore happens on every one of the several return paths. */
#define LAYOUT_KID_POOL 8192
static struct layout_handle g_kid_pool[LAYOUT_KID_POOL];
static float g_base_pool[LAYOUT_KID_POOL], g_final_pool[LAYOUT_KID_POOL],
             g_cross_pool[LAYOUT_KID_POOL];
static int   g_kid_top;
static int   g_kid_dropped;   /* containers whose children did not fit */

int layout_children_dropped(void) { return g_kid_dropped; }
void layout_reset_children_dropped(void) { g_kid_dropped = 0; }

static void arrange_inner(struct layout_arena *la, struct scene_arena *sa,
                          struct layout_handle h, float W, float H);

static float g_cb_x, g_cb_y, g_cb_w, g_cb_h;

static void arrange(struct layout_arena *la, struct scene_arena *sa,
                    struct layout_handle h, float W, float H) {
    int save = g_kid_top;
    /* Carry the containing block INTO this node's coordinate space, and take
     * it over if this node is itself positioned. Saved and restored around the
     * subtree, the same way the child pool is -- arrange is depth-first, so
     * that is all the bookkeeping either of them needs. */
    float sx = g_cb_x, sy = g_cb_y, sw = g_cb_w, sh = g_cb_h;
    struct layout_node *n = layout_resolve(la, h);
    if (n) {
        if (n->pos_container || sw <= 0) {
            g_cb_x = n->padding_left;
            g_cb_y = n->padding_top;
            g_cb_w = W - n->padding_left - n->padding_right;
            g_cb_h = H - n->padding_top - n->padding_bottom;
        } else {
            /* the ancestor's box, seen from here */
            g_cb_x -= n->resolved_x;
            g_cb_y -= n->resolved_y;
        }
    }
    arrange_inner(la, sa, h, W, H);
    g_cb_x = sx; g_cb_y = sy; g_cb_w = sw; g_cb_h = sh;
    g_kid_top = save;                 /* pop, whichever way the callee returned */
}

/* A STATED size in pixels, if the node states one: SIZE_FIXED already is
 * pixels, SIZE_PERCENT is a fraction of the containing block's content size on
 * that axis. Returns 0 and clears *has when the size is auto. */
static float stated_px(const struct layout_size *s, float avail, int *has) {
    if (s->mode == SIZE_FIXED)   { *has = 1; return s->fixed_value; }
    if (s->mode == SIZE_PERCENT) {
        *has = 1;
        float v = s->fixed_value * avail + s->pct_px;
        return v < 0 ? 0 : v;      /* calc() can go negative; a box cannot */
    }
    *has = 0;
    return 0;
}

/* Place an OUT-OF-FLOW box inside its containing block: CSS absolute
 * positioning, in the shape this engine can express. A stated edge pins that
 * side; both edges on an axis give the size; neither leaves the box at the
 * content origin, sized as the caller asked. */
/* THE CONTAINING BLOCK for absolutely positioned boxes: the content box of the
 * nearest ancestor that is itself positioned, expressed in the coordinate
 * space of the node being arranged right now.
 *
 * It used to be the immediate parent, always -- which is not what CSS says and
 * is not a near miss. `position: relative` on a wrapper exists precisely so a
 * descendant several levels down can be placed against IT, and every dropdown
 * menu, tooltip and badge on the web is built that way. python.org's menus
 * landed on top of its article for exactly this reason.
 *
 * Tracked as a saved/restored global rather than threaded through arrange():
 * arrange is strictly depth-first, so a save on entry and a restore on exit is
 * the same discipline the child pool already uses. */
static void place_positioned(struct layout_arena *la, struct scene_arena *sa,
                             struct layout_handle kh, struct layout_node *k,
                             struct layout_node *n, float W, float H) {
    (void)n; (void)W; (void)H;
    float cx = g_cb_x, cy = g_cb_y;
    float cw = g_cb_w, ch = g_cb_h;
    int L = (k->ins_set & 8) != 0, R = (k->ins_set & 2) != 0;
    int T = (k->ins_set & 1) != 0, B = (k->ins_set & 4) != 0;

    int has_w = 0, has_h = 0;
    float w = stated_px(&k->width,  cw, &has_w);
    float h = stated_px(&k->height, ch, &has_h);
    if (!has_w) w = (L && R) ? cw - k->ins_left - k->ins_right
                             : (k->intrinsic_w > 0 ? k->intrinsic_w : cw);
    if (!has_h) h = (T && B) ? ch - k->ins_top - k->ins_bottom
                             : (k->intrinsic_h > 0 ? k->intrinsic_h : ch);
    if (w < 0) w = 0;
    if (h < 0) h = 0;

    float x = cx, y = cy;
    if (L)      x = cx + k->ins_left;
    else if (R) x = cx + cw - k->ins_right - w;
    if (T)      y = cy + k->ins_top;
    else if (B) y = cy + ch - k->ins_bottom - h;

    k->resolved_x = x; k->resolved_y = y;
    k->resolved_w = w; k->resolved_h = h;
    if (k->is_container) arrange(la, sa, kh, w, h);
    else                 write_scene(sa, k);
}

static void arrange_inner(struct layout_arena *la, struct scene_arena *sa,
                          struct layout_handle h, float W, float H) {
    struct layout_node *n = layout_resolve(la, h);
    if (!n) return;
    n->resolved_w = W; n->resolved_h = H;
    write_scene(sa, n);
    if (!n->is_container) return;

    int is_row = (n->axis == AXIS_ROW);
    float content_main  = is_row ? (W - n->padding_left - n->padding_right)
                                 : (H - n->padding_top - n->padding_bottom);
    float content_cross = is_row ? (H - n->padding_top - n->padding_bottom)
                                 : (W - n->padding_left - n->padding_right);
    float main_pad0  = is_row ? n->padding_left : n->padding_top;
    float cross_pad0 = is_row ? n->padding_top : n->padding_left;

    /* gather children into the shared scratch pool (popped by arrange()) */
    int room = LAYOUT_KID_POOL - g_kid_top;
    struct layout_handle *kids = g_kid_pool + g_kid_top;
    int nk = 0;
    for (struct layout_handle c = n->first_child; !layout_handle_is_null(c); ) {
        struct layout_node *cn = layout_resolve(la, c);
        if (!cn) break;
        if (nk >= room) { g_kid_dropped++; break; }   /* bounded, and COUNTED */
        kids[nk++] = c;
        c = cn->next_sibling;
    }
    float *base = g_base_pool + g_kid_top, *finalm = g_final_pool + g_kid_top,
          *crossv = g_cross_pool + g_kid_top;
    g_kid_top += nk;
    if (nk == 0) return;

    /* --- 2D grid: N equal columns, children auto-flow left-to-right / top-to-
     * bottom with per-child colspan; each row is as tall as its tallest cell.
     * Bypasses the flex machinery entirely. --- */
    if (n->grid_cols > 0) {
        int cols = n->grid_cols > GRID_MAX_COLS ? GRID_MAX_COLS : n->grid_cols;
        float cgap = n->grid_col_gap, rgap = n->grid_row_gap;
        float grid_w = W - n->padding_left - n->padding_right;
        float colw[GRID_MAX_COLS];
        grid_col_widths(la, n, grid_w, colw);
        /* The SAME placement the measure pass used -- see grid_places. */
        unsigned short pr[GRID_MEASURE_KIDS], pc[GRID_MEASURE_KIDS],
                       prs[GRID_MEASURE_KIDS], pcs[GRID_MEASURE_KIDS];
        int nplace = nk < GRID_MEASURE_KIDS ? nk : GRID_MEASURE_KIDS;
        int nrows = grid_places(la, n, cols, kids, nplace, pr, pc, prs, pcs);
        if (nrows > GRID_MAX_ROWS) nrows = GRID_MAX_ROWS;
        float rowh[GRID_MAX_ROWS], rowy[GRID_MAX_ROWS];
        for (int i = 0; i < nrows; i++) rowh[i] = 0;
        /* Row heights first, so a cell can be placed on any row without
         * needing the ones after it to have been walked already. */
        for (int i = 0; i < nplace; i++) {
            struct layout_node *k = layout_resolve(la, kids[i]);
            if (!k || k->is_overlay || !pcs[i]) continue;
            float cw = cgap * (float)(pcs[i] - 1);
            for (int k2 = 0; k2 < pcs[i] && pc[i] + k2 < cols; k2++) cw += colw[pc[i] + k2];
            float ch = (k->height.mode == SIZE_FIXED) ? k->height.fixed_value
                     : is_text(sa, k) ? layout_measure_height_at_width(la, sa, kids[i], cw)
                     : k->is_container ? measure_subtree_height(la, sa, kids[i], cw)
                     : k->intrinsic_h;
            int rr = pr[i] + prs[i] - 1;
            if (rr >= nrows) rr = nrows - 1;
            if (rr >= 0 && ch > rowh[rr]) rowh[rr] = ch;
        }
        { float y = n->padding_top - n->scroll_offset;
          for (int i = 0; i < nrows; i++) { rowy[i] = y; y += rowh[i] + rgap; } }

        for (int i = 0; i < nk; i++) {
            struct layout_node *k = layout_resolve(la, kids[i]);
            if (k->is_overlay) {
                k->resolved_x = n->padding_left; k->resolved_y = n->padding_top;
                /* parent-fill by default; an EXPLICIT fixed size wins, so a
                 * dismiss scrim can out-size the strip that declared it */
                k->resolved_w = k->width.mode  == SIZE_FIXED ? k->width.fixed_value
                              : W - n->padding_left - n->padding_right;
                k->resolved_h = k->height.mode == SIZE_FIXED ? k->height.fixed_value
                              : H - n->padding_top - n->padding_bottom;
                if (k->is_container) arrange(la, sa, kids[i], k->resolved_w, k->resolved_h);
                else                 write_scene(sa, k);
                continue;
            }
            if (i >= nplace || !pcs[i]) continue;
            float cw = cgap * (float)(pcs[i] - 1);
            for (int k2 = 0; k2 < pcs[i] && pc[i] + k2 < cols; k2++) cw += colw[pc[i] + k2];
            float cx = n->padding_left + grid_col_x(colw, cgap, pc[i]);
            /* The cell's box is as tall as the ROWS it occupies, so a cell in
             * a short row is not stretched by a tall one elsewhere and a cell
             * spanning rows really covers them. */
            float ch = 0;
            for (int rr = pr[i]; rr < pr[i] + prs[i] && rr < nrows; rr++)
                ch += rowh[rr] + (rr > pr[i] ? rgap : 0);
            float cy = (pr[i] < nrows) ? rowy[pr[i]] : (n->padding_top - n->scroll_offset);
            k->resolved_x = cx; k->resolved_y = cy; k->resolved_w = cw; k->resolved_h = ch;
            if (k->is_container) arrange(la, sa, kids[i], cw, ch);
            else                 write_scene(sa, k);
        }
        return;
    }

    float sum_base = 0, sum_grow = 0;

    for (int i = 0; i < nk; i++) {
        struct layout_node *k = layout_resolve(la, kids[i]);
        base[i] = 0;
        if (k->is_overlay) continue;         /* out of flow: sized to the parent below */
        struct layout_size *ms = is_row ? &k->width  : &k->height;
        enum layout_align   ca = k->align;   /* child's own cross alignment override? no -- parent's align applies */
        (void)ca;
        int ktext = is_text(sa, k);

        int has_main = 0;
        /* A PERCENTAGE resolves only against a DEFINITE containing block.
         *
         * `height: 100%` inside a parent whose own height is auto does not
         * mean "as tall as the parent" -- the parent does not have a height
         * yet, it is about to get one from its children. CSS says such a
         * percentage computes to auto, and that rule is not a nicety: without
         * it every child is handed the height the parent measured, they stack,
         * and the parent's height no longer matches the sum of what is inside
         * it. Three children in a 119px box each came out 119 tall and reached
         * 357. MDN's page did that at several levels and ended up placing text
         * 307394 pixels down a document whose own box was 14781.
         *
         * The main axis is the one that can feed back -- a column's height
         * comes from its children -- so it is the one checked. */
        int definite_main = is_row ||
                            n->height.mode == SIZE_FIXED ||
                            n->height.mode == SIZE_FLEX;
        struct layout_size auto_main;
        if (!definite_main && ms->mode == SIZE_PERCENT) {
            auto_main = *ms;
            auto_main.mode = SIZE_INTRINSIC;
            ms = &auto_main;
        }
        float stated_main = stated_px(ms, content_main, &has_main);
        if (has_main) {
            base[i] = stated_main;
        } else if (k->grid_cols > 0 && !is_row) {
            /* COLUMN: a grid child's HEIGHT is its auto-flowed grid height at the
             * width it will get. */
            float cw = (n->align == ALIGN_STRETCH) ? content_cross : k->intrinsic_w;
            base[i] = measure_grid_height(la, sa, kids[i], cw);
        } else if (k->wrap && k->is_container && !is_row) {
            /* COLUMN: a wrap-row child's HEIGHT is its wrapped-line height at the
             * width it will get (stretch -> our content width, else intrinsic). */
            float cw = (n->align == ALIGN_STRETCH) ? content_cross : k->intrinsic_w;
            base[i] = measure_wrap_height(la, sa, kids[i], cw);
        } else if (k->is_container && !is_row) {
            /* COLUMN: a container child's HEIGHT measured at the width it will
             * get, so a wrap row NESTED inside it is counted (see
             * measure_subtree_height). Without this a paragraph gets one line's
             * budget and the next block lands on top of it. */
            float cw = (n->align == ALIGN_STRETCH) ? content_cross : k->intrinsic_w;
            base[i] = measure_subtree_height(la, sa, kids[i], cw);
        } else if (ktext && !is_row) {
            /* COLUMN: main size is HEIGHT -> wrap at the child's cross WIDTH */
            float cw = (n->align == ALIGN_STRETCH) ? content_cross : k->intrinsic_w;
            base[i] = layout_measure_height_at_width(la, sa, kids[i], cw);
        } else {
            base[i] = is_row ? k->intrinsic_w : k->intrinsic_h;   /* incl. text one-line width (ROW flex-basis) */
        }
        if (ms->max_size > 0 && base[i] > ms->max_size) base[i] = ms->max_size;   /* cap the basis */
        sum_base += base[i];
        sum_grow += ms->flex_grow;
    }

    float total_base = sum_base + (nk > 1 ? n->spacing * (nk - 1) : 0);
    float remaining = content_main - total_base;

    for (int i = 0; i < nk; i++) finalm[i] = base[i];
    if (remaining > 0 && sum_grow > 0) {
        for (int i = 0; i < nk; i++) {
            struct layout_node *k = layout_resolve(la, kids[i]);
            if (k->is_overlay) continue;
            struct layout_size *ms = is_row ? &k->width : &k->height;
            finalm[i] = base[i] + remaining * (ms->flex_grow / sum_grow);
        }
    } else if (remaining < 0) {
        /* OVERFLOW. This used to do nothing at all: flex_shrink is never
         * assigned by anyone, so every weight was 0, sum_w was 0, and the
         * whole branch fell through leaving children at their full base size.
         * The row simply overflowed its parent and the far end was clipped --
         * which is exactly what "shrinking the window cuts off the interface"
         * looks like from the outside.
         *
         * The weights come from the size MODE rather than from a field nobody
         * sets, and the deficit is taken in TWO PASSES, which is the part CSS
         * does not do and a user interface wants:
         *
         *   pass 1  boxes that asked to FLEX give up space first. They
         *           volunteered to be flexible -- the path field, the title
         *           zone, the content pane. Taking their slack is invisible.
         *   pass 2  only if that was not enough do intrinsically-sized boxes
         *           start to give. Squashing a button is a real cost, so it
         *           is paid last.
         *
         * Only the DEFAULT depends on the mode. flex_shrink stays orthogonal
         * to it, as the header promises and T3 checks: a caller that sets an
         * explicit weight gets exactly that, on a fixed box too. It is the
         * absence of a weight that is interpreted -- and there, a box that
         * asked for an EXACT size never gives, because a 24px traffic light or
         * a 4px dot is a decision rather than a preference.
         *
         * min_size is honoured throughout, and whatever one pass cannot absorb
         * carries into the next. */
        /* Two containers must NOT shrink their children, because for them
         * overflow is the whole point rather than a failure:
         *
         *   - a container with NO EXTENT is not laying out a row, it is an
         *     anchor whose children are placed by their own offsets (the
         *     desktop's zero-sized icon overlay). Everything there looks like
         *     overflow; shrinking squashed the desktop icons into their
         *     captions.
         *   - a container that CLIPS its children has declared itself a window
         *     onto something larger. A ScrollView is exactly that: its content
         *     is meant to be taller than the viewport, and that is what there
         *     is to scroll. Shrinking it to fit collapsed the content instead
         *     -- Settings' whole right-hand pane rendered empty, and the
         *     Terminal showed only the last couple of lines.
         *
         * In both cases the honest answer is that there is nothing to
         * distribute. */
        struct scene_node *sn = paired(sa, n);
        int clips = sn && sn->clip_children;

        /* ...and neither may the CONTENT of one. A scroll view clips, so it is
         * already exempt -- but the column inside it is not, and that column is
         * exactly where a document lives. Its children overflow it by design:
         * being taller than the viewport is what there is to scroll. Shrinking
         * them to fit squashed every wrapped paragraph into the heading below
         * it, which measured correctly (nk=54, nlines=3, total=48.9) and was
         * then thrown away here. If the parent clips, this container's children
         * are scrollable content and must keep their height. */
        int parent_clips = 0;
        if (!layout_handle_is_null(n->parent)) {
            struct layout_node *pn = layout_resolve(la, n->parent);
            struct scene_node *psn = pn ? paired(sa, pn) : 0;
            parent_clips = psn && psn->clip_children;
        }

        /* ...and a WRAPPING row must not shrink AT ALL. Overflow is not a
         * failure there, it is the trigger: a wrap row resolves too much
         * content by starting a new LINE, which is why the wrap arm below
         * places children at base[] and never looks at finalm[].
         *
         * Shrinking one anyway is invisible in the arrangement and lethal in
         * the measurement. The cross size of a text child is its height AT ITS
         * FINAL WIDTH -- so a paragraph of 54 word boxes, overflowing its 896px
         * line by design, had every word squashed to a fraction of its width
         * and then measured there. Each word CHARACTER-wrapped: "a " reported
         * one line, "This " three, "page " four. The row's own height stayed
         * right (it comes from the measure pass, which doesn't shrink), so the
         * line stride became the tallest of those lies -- 65px for a 16px line
         * -- and the paragraph's three lines spread out far enough for the next
         * heading to be drawn between them.
         *
         * Which is exactly the report: the top of the document is wrong and the
         * bottom is fine. The bottom is fine because a paragraph short enough
         * not to overflow never enters this branch at all. */
        int wrapping = n->wrap && is_row;

        float deficit = (content_main > 0.01f && !clips && !parent_clips && !wrapping)
                        ? -remaining : 0.0f;
        for (int pass = 0; pass < 2 && deficit > 0.01f; pass++) {
            float sum_w = 0;
            for (int i = 0; i < nk; i++) {
                struct layout_node *k = layout_resolve(la, kids[i]);
                if (k->is_overlay) continue;
                struct layout_size *ms = is_row ? &k->width : &k->height;
                int expl = ms->flex_shrink > 0;
                int eligible = (pass == 0) ? (expl || ms->mode == SIZE_FLEX)
                                           : (!expl && ms->mode == SIZE_INTRINSIC);
                if (!eligible) continue;
                sum_w += (expl ? ms->flex_shrink : 1.0f) * finalm[i];
            }
            if (sum_w <= 0) continue;
            float absorbed = 0;
            for (int i = 0; i < nk; i++) {
                struct layout_node *k = layout_resolve(la, kids[i]);
                if (k->is_overlay) continue;
                struct layout_size *ms = is_row ? &k->width : &k->height;
                int expl = ms->flex_shrink > 0;
                int eligible = (pass == 0) ? (expl || ms->mode == SIZE_FLEX)
                                           : (!expl && ms->mode == SIZE_INTRINSIC);
                if (!eligible) continue;
                float w = expl ? ms->flex_shrink : 1.0f;
                float want = deficit * ((w * finalm[i]) / sum_w);
                float floor_ = ms->min_size > 0 ? ms->min_size : 0.0f;
                float give = finalm[i] - floor_;
                if (give < 0) give = 0;
                if (want > give) want = give;
                finalm[i] -= want;
                absorbed += want;
            }
            deficit -= absorbed;
            if (absorbed <= 0.01f) continue;   /* this pass had nothing to give */
        }
    }

    /* clamp the resolved MAIN size to each child's [min, max]. min already
     * enforced during shrink; max caps growth (a "clamped-responsive" box that
     * fills available space up to a ceiling). */
    for (int i = 0; i < nk; i++) {
        struct layout_node *k = layout_resolve(la, kids[i]);
        if (k->is_overlay) continue;
        struct layout_size *ms = is_row ? &k->width : &k->height;
        if (ms->min_size > 0 && finalm[i] < ms->min_size) finalm[i] = ms->min_size;
        if (ms->max_size > 0 && finalm[i] > ms->max_size) finalm[i] = ms->max_size;
    }

    /* cross size per child */
    for (int i = 0; i < nk; i++) {
        struct layout_node *k = layout_resolve(la, kids[i]);
        crossv[i] = 0;
        if (k->is_overlay) continue;
        struct layout_size *cs = is_row ? &k->height : &k->width;
        int ktext = is_text(sa, k);
        int has_cross = 0;
        float stated_cross = stated_px(cs, content_cross, &has_cross);
        if (has_cross) {
            /* a definite cross size always wins -- CSS stretch only applies to
             * auto-sized items (a 50px-wide child of a stretch column stays
             * 50px; stretching it broke fixed-size boxes once the ROOT became
             * a stretch column). A percentage is definite too, once the
             * containing block is known, which is here. */
            crossv[i] = stated_cross;
        } else if (n->align == ALIGN_STRETCH) {
            crossv[i] = content_cross;
        } else if (ktext && is_row) {
            crossv[i] = layout_measure_height_at_width(la, sa, kids[i], finalm[i]); /* height at final width */
        } else if (is_row && k->is_container && subtree_wraps(la, kids[i])) {
            /* ROW: a container child that WRAPS somewhere inside it cannot be
             * measured by intrinsic_h -- intrinsics are computed before any
             * width is known, so a wrapping row inside counts as one line. Ask
             * for its height at the width it just got instead. This is the
             * list item: [bullet][text column], where the column wraps to three
             * lines and the row claimed to be one line tall, so the next item
             * was placed on top of it.
             *
             * Gated on actually containing a wrap, so every ordinary row --
             * toolbars, buttons, list rows -- keeps its intrinsic height and
             * this cannot regress them. */
            crossv[i] = measure_subtree_height(la, sa, kids[i], finalm[i]);
        } else {
            crossv[i] = is_row ? k->intrinsic_h : k->intrinsic_w;
        }
    }

    /* clamp the CROSS size to each child's [min, max] on that axis */
    for (int i = 0; i < nk; i++) {
        struct layout_node *k = layout_resolve(la, kids[i]);
        if (k->is_overlay) continue;
        struct layout_size *cs = is_row ? &k->height : &k->width;
        if (cs->min_size > 0 && crossv[i] < cs->min_size) crossv[i] = cs->min_size;
        if (cs->max_size > 0 && crossv[i] > cs->max_size) crossv[i] = cs->max_size;
    }

    /* --- flex-wrap: pack children into lines along the main axis, stack the
     * lines on the cross axis. Children keep their flex-basis width (no grow
     * across lines); each line's cross size is its tallest child; align applies
     * within a line. Row-wrap (the common case). --- */
    if (n->wrap && is_row) {
        float line_gap = n->spacing;
        float cross_cursor = cross_pad0 - n->scroll_offset;   /* vertical scroll shifts lines up */
        int i = 0;
        while (i < nk) {
            int j = i; float lm = 0, lc = 0;
            while (j < nk) {
                struct layout_node *kj = layout_resolve(la, kids[j]);
                if (kj->is_overlay) { j++; continue; }
                float add = base[j] + (j > i ? n->spacing : 0);
                if (j > i && lm + add > content_main + 0.5f) break;   /* wrap */
                lm += add; if (crossv[j] > lc) lc = crossv[j]; j++;
            }
            if (j == i) j = i + 1;                                     /* >=1 per line */
            /* JUSTIFY, PER LINE. The non-wrap arm below has always honoured
             * justify; this one started every line at the padding edge, so a
             * wrapping row ignored it entirely -- and since a paragraph of text
             * IS a wrapping row, `text-align: center` in a stylesheet did
             * nothing at all. Each line box justifies on its own, which is what
             * text-align means: centre every line, not the paragraph as a
             * block. `lm` is already this line's used width including gaps. */
            float cursor = main_pad0;
            if (n->justify != JUSTIFY_START) {
                float slack = content_main - lm;
                if (slack > 0) switch (n->justify) {
                    case JUSTIFY_CENTER: cursor += slack * 0.5f; break;
                    case JUSTIFY_END:    cursor += slack;        break;
                    default:             break;   /* SPACE_BETWEEN: see below */
                }
            }
            for (int q = i; q < j; q++) {
                struct layout_node *k = layout_resolve(la, kids[q]);
                if (k->is_overlay) continue;
                float cross_pos = cross_cursor;
                switch (n->align) {
                    case ALIGN_CENTER: cross_pos = cross_cursor + (lc - crossv[q]) * 0.5f; break;
                    case ALIGN_END:    cross_pos = cross_cursor + (lc - crossv[q]);        break;
                    default:           break;   /* START / STRETCH -> line top */
                }
                k->resolved_x = cursor;   k->resolved_y = cross_pos;
                k->resolved_w = base[q];  k->resolved_h = crossv[q];
                if (k->is_container) arrange(la, sa, kids[q], k->resolved_w, k->resolved_h);
                else                 write_scene(sa, k);
                cursor += base[q] + n->spacing;
            }
            cross_cursor += lc + line_gap;
            i = j;
        }
        return;
    }

    /* main-axis positions per justify */
    float used = 0; for (int i = 0; i < nk; i++) used += finalm[i];
    float leftover = content_main - used - (nk > 1 ? n->spacing * (nk - 1) : 0);
    if (leftover < 0) leftover = 0;
    float cursor = main_pad0, gap = n->spacing;
    switch (n->justify) {
        case JUSTIFY_START:         break;
        case JUSTIFY_CENTER:        cursor += leftover * 0.5f; break;
        case JUSTIFY_END:           cursor += leftover; break;
        case JUSTIFY_SPACE_BETWEEN: if (nk > 1) gap = n->spacing + leftover / (nk - 1); break;
    }
    if (!is_row) cursor -= n->scroll_offset;   /* vertical scroll shifts children up */

    for (int i = 0; i < nk; i++) {
        struct layout_node *k = layout_resolve(la, kids[i]);
        if (k->is_overlay) {
            /* OUT OF FLOW. Without offsets this fills the parent's content box,
             * which is what a modal scrim wants. With them it is CSS absolute
             * positioning: an edge that was stated pins that side, both edges
             * stated give the width, and neither leaves it at the content
             * origin sized to its content. */
            place_positioned(la, sa, kids[i], k, n, W, H);
            continue;   /* no cursor advance */
        }
        /* position: relative -- offset AFTER the flow has placed it, so the
         * siblings never notice. That is the whole difference between relative
         * and absolute, and the reason relative is safe to apply late. */
        float rel_x = 0, rel_y = 0;
        if (k->relative) {
            if (k->ins_set & 8)      rel_x =  k->ins_left;
            else if (k->ins_set & 2) rel_x = -k->ins_right;
            if (k->ins_set & 1)      rel_y =  k->ins_top;
            else if (k->ins_set & 4) rel_y = -k->ins_bottom;
        }
        float main_pos = cursor;
        float cross_pos = cross_pad0;
        switch (n->align) {
            case ALIGN_START:   cross_pos = cross_pad0; break;
            case ALIGN_CENTER:  cross_pos = cross_pad0 + (content_cross - crossv[i]) * 0.5f; break;
            case ALIGN_END:     cross_pos = cross_pad0 + (content_cross - crossv[i]); break;
            case ALIGN_STRETCH: cross_pos = cross_pad0; break;
        }
        if (is_row) {
            k->resolved_x = main_pos; k->resolved_y = cross_pos;
            k->resolved_w = finalm[i]; k->resolved_h = crossv[i];
        } else {
            k->resolved_y = main_pos; k->resolved_x = cross_pos;
            k->resolved_h = finalm[i]; k->resolved_w = crossv[i];
        }
        k->resolved_x += rel_x; k->resolved_y += rel_y;   /* position: relative */
        if (k->is_container) arrange(la, sa, kids[i], k->resolved_w, k->resolved_h);
        else                 write_scene(sa, k);
        cursor += finalm[i] + gap;
    }
}

void layout_run(struct layout_arena *la, struct scene_arena *sa,
                struct layout_handle root, float W, float H) {
    struct layout_node *r = layout_resolve(la, root);
    if (!r) return;
    measure_intrinsic(la, sa, root);
    r = layout_resolve(la, root);
    r->resolved_x = 0; r->resolved_y = 0;
    /* No containing block yet: the root establishes one, which is the initial
     * containing block CSS calls the viewport. */
    g_cb_x = g_cb_y = 0; g_cb_w = 0; g_cb_h = 0;
    arrange(la, sa, root, W, H);
}

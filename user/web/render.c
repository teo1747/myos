/* user/web/render.c -- document tree + computed styles -> EmUI nodes.
 *
 * This is where the CSS box model meets a layout engine that was built for
 * application UI, and the mapping is the whole trick:
 *
 *   a BLOCK      becomes a vertical stack. Blocks stack down the page; that is
 *                what a column does. Margins become padding.
 *   an INLINE    run becomes a Flow -- EmUI's real flex-wrap row -- holding one
 *                text node PER WORD. Wrapping between words is then the layout
 *                engine's job, not ours, and it already does it well.
 *   a LIST ITEM  is a row of [marker][block], so the text hangs correctly under
 *                itself rather than wrapping back under the bullet.
 *   <pre>        is a block that neither collapses whitespace nor wraps: one
 *                text node per source line.
 *
 * Splitting inline text into per-word nodes is the single decision that makes
 * this work. It costs nodes, and it buys correct wrapping, correct mixed
 * styling within a paragraph, and per-word hit testing for links -- which is
 * how a link that wraps across two lines stays clickable on both.
 *
 * This file reads `struct vstyle` and NEVER a tag name. See docs/BROWSER.md §4.
 */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "ui.h"
#include "em.h"
#include "theme.h"
#include "html.h"
#include "style.h"
#include "url.h"
#include "css.h"
#include "imgcache.h"
#include "svg.h"
#include "form.h"
#include "kit.h"
#include "render.h"

static void (*g_on_link)(const char *href);
static const char *g_hover_href;      /* link under the pointer, for the status line */

/* Link targets must outlive the frame that emits them: EmUI keeps the pointer.
 * The document arena owns the href strings, so this only needs to survive
 * until the click is acted on -- the app copies before navigating. */
static const char *g_pending;
/* The document's author stylesheet for this render pass. Held for the pass
 * rather than threaded through every function, because EVERY style question
 * needs it and passing it down eight call sites would be noise. */
static const struct css_sheet *g_sheet;
static int  (*g_has_listener)(int node);
static void (*g_on_click)(int node);
static void (*g_on_submit)(int node);
/* the field that had keyboard focus on the last frame, or -1 */
static int g_focused_field = -1;
int vellum_focused_field(void) { return g_focused_field; }

const char *vellum_hovered_link(void) { return g_hover_href; }

static EmFont font_for(const struct vstyle *s) {
    /* Size and WEIGHT are independent in CSS, and the toolkit's roles must not
     * conflate them: `font-size: 19px` with no `font-weight` is a large
     * paragraph, not a heading. Subtitle is the large regular face. */
    if (s->size == 3) return s->bold ? Heading : Subtitle;
    if (s->size == 2) return s->bold ? Title   : Subtitle;
    if (s->size == 1) return Caption;
    return s->bold ? BodyBold : Body;
}

static Color argb(unsigned v);

static Color color_for(const struct vstyle *s) {
    const struct ui_theme *t = ui_theme();
    /* An AUTHOR colour wins -- that is what the cascade decided. Absent one,
     * the THEME decides, so an unstyled page follows the desktop into dark
     * mode instead of being black-on-white in the middle of it. */
    /* A colour set ON this element wins. An INHERITED one does not outrank the
     * user-agent's link colour -- see style.h. */
    if (s->color && (s->color_own || !s->link)) {
        Color c;
        c.r = (float)((s->color >> 16) & 0xFF) / 255.0f;
        c.g = (float)((s->color >>  8) & 0xFF) / 255.0f;
        c.b = (float)( s->color        & 0xFF) / 255.0f;
        c.a = (float)((s->color >> 24) & 0xFF) / 255.0f;
        if (c.a <= 0.0f) c.a = 1.0f;
        return c;
    }
    (void)t;
    /* The PAGE's defaults, not the theme's. A document is written against a
     * white canvas, so its unstated colours have to come from that same world
     * or the two halves disagree -- see style.h. */
    if (s->link) return argb(PAGE_LINK);
    if (s->mono) return argb(PAGE_QUIET);    /* code reads as a quieter voice */
    return argb(PAGE_INK);
}

/* One word of an inline run. A link's words are BUTTONS so each is clickable
 * on its own -- which is what keeps a link that wraps across a line break
 * clickable on both halves. */
static void emit_word(const char *w, const struct vstyle *s, const char *href) {
    if (href && g_on_link) {
        /* NO horizontal padding. The word already carries the space that
         * followed it in the source, so 2px a side adds 4px between every pair
         * of linked words -- text inside a link was set looser than the text
         * beside it, and a headline made of links came out visibly gappy. The
         * word's own box is hit area enough. */
        if (Button(w).ghost().font(font_for(s)).py(EmZero).px(EmZero)
                .color(color_for(s)).id(w).clicked()) {
            g_pending = href;
        }
        return;
    }
    Text(w).font(font_for(s)).color(color_for(s));
}

/* Emit a text run word by word into the surrounding Flow. */
/* A whitespace-only text node was seen and is owed to the next word. Reset per
 * inline run, so a run never opens with one. See the blank-text branch of
 * emit_inline. */
static int g_pending_space;

static void emit_text(const char *txt, const struct vstyle *s, const char *href) {
    static char word[256];
    size_t n = 0;
    /* Did the SOURCE end this run with a space? "and " before an <i> did, and
     * dropping it welds the words either side of the tag together
     * ("italicand"). The space belongs to the text, not to the loop. */
    size_t tl = strlen(txt);
    int trail = tl && txt[tl - 1] == ' ';
    /* ...and a LEADING one for the same reason from the other side. " and "
     * after a </i> carries its space in front; the word loop drops leading
     * whitespace, so the space that separated the tag from the next word
     * disappeared and you read "italicand". The first word carries it. */
    int lead = tl && txt[0] == ' ';
    int first = 1;
    for (const char *p = txt; ; p++) {
        if (*p && *p != ' ') {
            if (n + 1 < sizeof word) word[n++] = *p;
            continue;
        }
        if (n) {
            word[n] = 0;
            /* the pool: EmUI keeps the pointer for the frame, so a stack
             * buffer reused per word would render the LAST word everywhere */
            /* WIDE ENOUGH FOR A URL. At 68 bytes a word longer than about
             * sixty-six characters was silently cut -- and the words that long
             * in running text are almost always URLs, which is exactly where
             * losing the tail changes what the text SAYS. suckless.org prints a
             * commit link in a paragraph and it came out ending mid-hash. The
             * scan buffer above is 256, so this matches it rather than
             * inventing a second, smaller limit. */
            static char pool[512][260];
            static int  pn;
            if (pn >= 512) pn = 0;
            /* trailing space unless the run ends here: the space is part of
             * the word box, so a following comma sits flush against it */
            snprintf(pool[pn], sizeof pool[0], "%s%s%s",
                     (first && (lead || g_pending_space)) ? " " : "", word,
                     (*p || trail) ? " " : "");
            if (first) g_pending_space = 0;
            first = 0;
            emit_word(pool[pn], s, href);
            pn++;
            n = 0;
        }
        if (!*p) break;
    }
}

/* An <img>. The base URL is needed to resolve src, the cache is what turns a
 * URL into pixels, and the content width is what an oversized picture gets
 * clamped to -- none of which belong in the DOM, so all three are held for the
 * render pass like the stylesheet is. */
static const char *g_base;
static float g_content_w;

/* Emit one picture, or a stand-in that occupies THE SAME SPACE.
 *
 * Reserving the box before the bytes arrive is the whole point. A picture that
 * appears at its natural size after the page has been laid out shoves the
 * paragraph the reader is in the middle of -- the single most irritating thing
 * a browser does, and it is entirely avoidable whenever the markup or the
 * stylesheet said how big the picture is.
 *
 * Size is decided in cascade order (CSS beats the markup's attributes beats the
 * picture's own natural size), then clamped to the content width with the
 * ASPECT PRESERVED, because a wide image that overflows its column is worse
 * than a smaller one. */
static void emit_image(struct html_doc *d, int n, const struct vstyle *st) {
    const char *src = d->nodes[n].href;      /* the parser stores src here */
    if (!src || !src[0]) return;

    char url[512];
    if (url_resolve(g_base ? g_base : "", src, url, sizeof url) != 0)
        snprintf(url, sizeof url, "%s", src);

    struct img_slot *s = imgcache_want(url);
    if (s && (s->state == IMG_WANTED || s->state == IMG_LOADING)) em_request_frame();

    int ready = s && s->state == IMG_READY && s->px;

    /* --- how big? CSS, then the attributes, then what arrived --- */
    float w = 0, h = 0;
    if (st->width)  w = (float)st->width;
    if (st->height) h = (float)st->height;
    if (!w && d->nodes[n].img_w) w = (float)d->nodes[n].img_w;
    if (!h && d->nodes[n].img_h) h = (float)d->nodes[n].img_h;

    /* One dimension stated, the other implied by the picture's own shape --
     * which is only knowable once it has arrived. */
    if (ready) {
        float nw = (float)s->w, nh = (float)s->h;
        if (w && !h) h = nh * (w / nw);
        else if (h && !w) w = nw * (h / nh);
        else if (!w && !h) { w = nw; h = nh; }
    }
    if (w <= 0 || h <= 0) {
        /* Nothing to reserve honestly: no stated size and no picture yet.
         * Alt text is the right stand-in -- inventing a box height would move
         * the page exactly as much as not reserving one. */
        const char *alt = d->nodes[n].alt;
        if (alt && alt[0]) { Text(alt).font(font_for(st)).color(ui_theme()->text_secondary); return; }
        Text((s && s->state == IMG_FAILED) ? "\xE2\x9C\x95" : "\xE2\x97\xAF").caption().tertiary();
        return;
    }

    /* --- clamp to the column, preserving the aspect --- */
    if (g_content_w > 16.0f && w > g_content_w) {
        h *= g_content_w / w;
        w  = g_content_w;
    }

    if (ready) {
        em_flush();
        ui_image_sized((uint64_t)(uintptr_t)s->px, s->px, s->w, s->h, w, h);
        return;
    }

    /* The reserved box: the picture's space, held open, faintly outlined so it
     * reads as "something is coming" rather than as a rendering hole. */
    em_flush();
    ui_begin_vstack(0x1A6E0000ULL ^ (uint64_t)(uintptr_t)s);
    ui_set_size((struct layout_size){ .mode = SIZE_FIXED, .fixed_value = w },
                (struct layout_size){ .mode = SIZE_FIXED, .fixed_value = h });
    ui_set_corner_radius(4);
    { struct paint pl = { 0 };
      pl.kind = PAINT_SOLID;
      pl.solid = ui_theme()->surface_alt;
      pl.solid.a *= (s && s->state == IMG_FAILED) ? 0.35f : 0.6f;
      ui_set_paint(pl); }
    ui_end_stack();
}

/* Is this subtree inline-only? An inline run ends where a block begins. */
/* Text that is only whitespace. A document is written with newlines between
 * its tags, so these nodes are everywhere -- harmless in a paragraph, and NOT
 * harmless in a flex or grid container, where CSS declines to make an
 * anonymous item out of one. Left in, the newline between two <div>s becomes a
 * third grid cell and every card after it lands one column late. */
static int is_blank_text(struct html_doc *d, int n) {
    if (d->nodes[n].kind != HTML_TEXT) return 0;
    const char *t = d->nodes[n].text;
    if (!t) return 1;
    for (; *t; t++)
        if (*t != ' ' && *t != '\t' && *t != '\n' && *t != '\r') return 0;
    return 1;
}

static int is_inline(struct html_doc *d, int n, const struct vstyle *parent) {
    if (d->nodes[n].kind == HTML_TEXT) return 1;
    struct vstyle s;
    vstyle_for_node(d, n, parent, g_sheet, &s);
    /* An ABSOLUTELY positioned box leaves the flow, and CSS blockifies it --
     * `position: absolute` on a <span> makes it a block. Without this the span
     * stays part of an inline run, which has no box to position. (Relative
     * positioning does NOT blockify, so an inline that is merely nudged is
     * still not offset here -- see docs/TODO.md.) */
    if (s.position == VP_ABSOLUTE || s.position == VP_FIXED) return 0;
    /* BLOCKIFIED: every element child of a flex or grid container is a flex
     * ITEM, whatever its own display says. CSS says so, and without it a nav
     * bar written as `<nav style="display:flex"><a>One</a><a>Two</a></nav>`
     * has its links merged into ONE inline run and therefore one item -- so
     * the gap and the justification apply to the whole strip instead of
     * between the links, which is not a subtle difference. */
    if (parent && (parent->display == VD_FLEX || parent->display == VD_GRID))
        return 0;
    /* controls flow INLINE with their labels, which is what a form looks like */
    return s.display == VD_INLINE || s.display == VD_INLINE_BLOCK ||
           s.display == VD_IMAGE ||
           s.display == VD_FIELD  || s.display == VD_BUTTON ||
           s.display == VD_CHECK  || s.display == VD_RADIO || s.display == VD_SELECT;
}

static void render_block(struct html_doc *d, int node, const struct vstyle *s,
                         const char *href, int list_index);
static void emit_field(struct html_doc *d, int n, const struct vstyle *st);
static void emit_button(struct html_doc *d, int n, const struct vstyle *st);
static void emit_check(struct html_doc *d, int n, const struct vstyle *st, int radio);
static void emit_select(struct html_doc *d, int n, const struct vstyle *st);

/* One inline child, at any depth. RECURSIVE on purpose: this was a
 * hand-unrolled three-level walk that only knew about text, so an <img> (or
 * any element) nested inside an inline wrapper was silently dropped -- a
 * <figure><img></figure> rendered as nothing at all, with no error anywhere.
 * Depth is bounded so hostile markup cannot recurse us to death. */
/* Is this box one that BREAKS an inline formatting context? A block, a table,
 * a row, a cell or a list item inside a run of inline content is not inline
 * content -- CSS says the inline context ends and a block box begins, and a
 * renderer that walks it as inline anyway flattens the whole subtree. */
static float g_zoom = 1.0f;

/* A CSS pixel in device pixels: every length the cascade produces passes
 * through here, so page zoom is one multiply rather than a change at each use.
 * Declared this early because emit_inline needs it. */
static float zpx(short v) { return (float)v * g_zoom; }

/* Is the box being emitted sized by its CONTENT rather than by its container?
 * True for a flex/grid item, a float, and an inline-block -- all three shrink
 * to fit. Declared here because emit_inline needs it and render_children sets
 * it; the long note on what it means for flex is at render_children. */
static int g_flex_item;

static int breaks_inline(unsigned char display) {
    return display == VD_BLOCK || display == VD_LIST_ITEM || display == VD_TABLE ||
           display == VD_ROW   || display == VD_CELL      || display == VD_CAPTION ||
           display == VD_FLEX  || display == VD_GRID;
}


/* ---- inline <svg> -------------------------------------------------------
 *
 * The drawing is rasterised ONCE per (node, size) and kept, because a frame is
 * 16ms and following a path grammar is not free -- re-rendering every icon on
 * every frame would turn a scroll into a slideshow. Bounded like every other
 * arena here: a fixed number of slots and a fixed pixel budget, and a page
 * with more icons than that draws the first ones and nothing for the rest.
 */
#define SVG_SLOTS   24
#define SVG_MAX_SIDE 64
static struct svg_slot {
    int      node, w, h, used;
    uint32_t px[SVG_MAX_SIDE * SVG_MAX_SIDE];
} g_svg[SVG_SLOTS];
static int g_svg_n;

void render_svg_cache_reset(void) { g_svg_n = 0; memset(g_svg, 0, sizeof g_svg); }

static struct svg_slot *svg_slot_for(struct html_doc *d, int n, int w, int h) {
    for (int i = 0; i < g_svg_n; i++)
        if (g_svg[i].used && g_svg[i].node == n && g_svg[i].w == w && g_svg[i].h == h)
            return &g_svg[i];
    if (g_svg_n >= SVG_SLOTS) return 0;
    struct svg_slot *s = &g_svg[g_svg_n];
    static uint8_t scratch[512 * 1024];
    if (svg_render((const uint8_t *)d->nodes[n].svg, strlen(d->nodes[n].svg),
                   s->px, sizeof s->px, scratch, sizeof scratch,
                   (uint32_t)w, (uint32_t)h, 0xFF000000u) != 0)
        return 0;
    s->node = n; s->w = w; s->h = h; s->used = 1;
    g_svg_n++;
    return s;
}

static void emit_svg(struct html_doc *d, int n, const struct vstyle *st) {
    if (!d->nodes[n].svg) return;
    /* SIZE: what CSS said, else what the document says, else the text size --
     * an icon with no size at all is meant to be as tall as the line it sits
     * on, which is what `1em` means and what every icon font did before SVG. */
    uint32_t dw = 0, dh = 0;
    svg_probe((const uint8_t *)d->nodes[n].svg, strlen(d->nodes[n].svg), &dw, &dh);
    float w = st->width  ? (float)st->width  : 0;
    float h = st->height ? (float)st->height : 0;
    if (!w && !h) {
        if (dw && dh) { w = (float)dw; h = (float)dh; }
        else          { w = h = 16.0f; }
    } else if (w && !h) h = dh && dw ? w * (float)dh / (float)dw : w;
    else if (h && !w)   w = dh && dw ? h * (float)dw / (float)dh : h;
    w *= g_zoom; h *= g_zoom;
    if (w < 1) w = 1;
    if (h < 1) h = 1;

    int rw = (int)(w + 0.5f), rh = (int)(h + 0.5f);
    if (rw > SVG_MAX_SIDE) rw = SVG_MAX_SIDE;
    if (rh > SVG_MAX_SIDE) rh = SVG_MAX_SIDE;
    struct svg_slot *s = svg_slot_for(d, n, rw, rh);
    em_flush();
    if (!s) {
        /* No slot or an undrawable document: hold the space rather than
         * collapsing the row around a missing icon. */
        ui_begin_vstack(0x5A6C0000ULL ^ (uint64_t)(unsigned)n);
        ui_set_size((struct layout_size){ .mode = SIZE_FIXED, .fixed_value = w },
                    (struct layout_size){ .mode = SIZE_FIXED, .fixed_value = h });
        ui_end_stack();
        return;
    }
    ui_image_sized(0x5A6C0000ULL ^ (uint64_t)(unsigned)n, s->px,
                   (uint32_t)rw, (uint32_t)rh, w, h);
}

static void emit_inline(struct html_doc *d, int c, const struct vstyle *st,
                        const char *href, int depth) {
    /* Deep, because real markup is deep: Hacker News reaches nine levels of
     * inline nesting before its first link, and the old bound of 8 silently
     * dropped every one of their titles while the plain " | " between them --
     * one level shallower -- came through. A bound this size is a runaway
     * guard, not a policy about how documents may be written. */
    if (depth > 24) return;
    if (d->nodes[c].kind == HTML_TEXT) {
        if (!d->nodes[c].text) return;
        /* A text node that is ONLY whitespace still separates the elements
         * either side of it. emit_text builds words out of non-space runs, so
         * it emits nothing at all for one -- and
         *
         *     <a>tosh</a> <span>7 hours ago</span>
         *
         * came out as "tosh7 hours ago". It had been masked by 2px of padding
         * on every link word, so removing that padding is what made it
         * visible: one bug hiding another, and the page looked slightly
         * wrong-but-plausible the whole time.
         *
         * Only BETWEEN things, and only in an inline run. A blank text node
         * with nothing beside it is layout whitespace in the source, and
         * emitting a space for it would open a line box where the document
         * has none. */
        if (is_blank_text(d, c)) {
            /* Remember it; do not emit it. The space belongs to the NEXT word,
             * which is what makes it disappear when there is no next word --
             * whitespace at the start or end of a line box is collapsed away
             * in CSS, and emitting it as its own leaf put a stray space at the
             * top of nearly every real page. It also costs no extra box. */
            g_pending_space = 1;
            return;
        }
        emit_text(d->nodes[c].text, st, href);
        return;
    }
    struct vstyle s;
    vstyle_for_node(d, c, st, g_sheet, &s);
    if (s.display == VD_NONE) return;
    /* AFTER computing its own style, not before: an image is styled by the
     * rules that match IT (`.half { width: 150px }`), and handing it the
     * parent's vstyle silently ignored every one of them. */
    if (d->nodes[c].svg)        { emit_svg(d, c, &s); return; }
    if (s.display == VD_IMAGE)  { emit_image(d, c, &s); return; }
    if (s.display == VD_FIELD)  { emit_field(d, c, &s); return; }
    if (s.display == VD_BUTTON) { emit_button(d, c, &s); return; }
    if (s.display == VD_CHECK)  { emit_check(d, c, &s, 0); return; }
    if (s.display == VD_RADIO)  { emit_check(d, c, &s, 1); return; }
    if (s.display == VD_SELECT) { emit_select(d, c, &s); return; }
    /* A block-level box inside an inline run ends the run and becomes a block.
     * Without this, a <table> that happens to sit inside an inline element gets
     * walked as if it were text, and its rows and cells lose all their
     * structure -- which is exactly what a page wrapped in <center> used to do
     * to itself. */
    /* INLINE-BLOCK: a box, laid out where the words are. It goes through the
     * same block emitter -- so it keeps its padding, background and border --
     * but inside the Flow this run has already opened, and sized to its
     * content rather than to the line. That last part is what `g_flex_item`
     * means here, and it is the same trick a float uses two functions down. */
    if (s.display == VD_INLINE_BLOCK) {
        em_flush();
        int was = g_flex_item;
        g_flex_item = 1;                       /* shrink to fit, like an item */
        /* MARGIN OUTSIDE THE PAINT, which for this one box has to be arranged
         * here. The block emitter folds margin into padding -- invisible on a
         * full-width block, and unmissable on a box with a background and a
         * rounded corner: google.com's "Connexion" pill came out half again
         * too tall and too wide, painting its own 12px margin blue. Wrapping
         * it in a transparent box that carries the margin puts the paint back
         * on the border box where CSS says it lives. (The block path has the
         * same flaw and is left alone deliberately -- see docs/TODO.md.) */
        struct vstyle inner = s;
        short mt = inner.margin_top, mb = inner.margin_bottom, ml = inner.indent;
        inner.margin_top = inner.margin_bottom = inner.indent = 0;
        if (mt || mb || ml) {
            HStack(.spacing = 0, .align = Leading,
                   .pt = zpx(mt), .pb = zpx(mb), .pl = zpx(ml)) {
                render_block(d, c, &inner, href, 0);
            }
        } else {
            render_block(d, c, &inner, href, 0);
        }
        g_flex_item = was;
        return;
    }
    if (breaks_inline(s.display)) { em_flush(); render_block(d, c, &s, href, 0); return; }
    const char *h = d->nodes[c].href ? d->nodes[c].href : href;
    for (int k = d->nodes[c].first_child; k >= 0; k = d->nodes[k].next_sibling)
        emit_inline(d, k, &s, h, depth + 1);
}

/* Walk a run of inline siblings [from, to) into one wrapping row. */
/* A packed 0xAARRGGBB from the cascade, as the toolkit's float Color. Alpha 0
 * is "not set" everywhere in vstyle, so callers test before calling. */
static Color argb(unsigned v) {
    Color c;
    c.r = (float)((v >> 16) & 0xFF) / 255.0f;
    c.g = (float)((v >>  8) & 0xFF) / 255.0f;
    c.b = (float)( v        & 0xFF) / 255.0f;
    c.a = (float)((v >> 24) & 0xFF) / 255.0f;
    if (c.a <= 0.0f) c.a = 1.0f;
    return c;
}

/* Told about every block's box as it is emitted -- see vellum_set_box_hook. */
static void (*g_box_hook)(int node, unsigned idx, unsigned gen);
void vellum_set_box_hook(void (*fn)(int, unsigned, unsigned)) { g_box_hook = fn; }

/* The grid whose named areas the children being emitted right now belong to,
 * or NULL. A child states `grid-area: columnStart`; only its PARENT knows
 * where columnStart is, so the parent leaves itself here while its children
 * are emitted. Grids nest, so every setter saves and restores. */
static const struct vstyle *g_area_owner;

/* The page's zoom. Lengths the AUTHOR stated -- a 240px sidebar, 16px of
 * padding -- scale with the text, or a zoomed page is large type inside boxes
 * that did not grow. */
/* g_zoom is defined above zpx, which is needed early by emit_inline */
void vellum_set_zoom(float z) { g_zoom = (z > 0.2f && z < 6.0f) ? z : 1.0f; }
float vellum_zoom(void) { return g_zoom; }
/* zpx is declared above emit_inline, which needs it for inline-block margins */

/* justify-content / align-items in the toolkit's spelling. */
static EmAlign em_of(unsigned char vj, EmAlign dflt) {
    switch (vj) {
        case VJ_CENTER:  return Center;
        case VJ_END:     return Trailing;
        case VJ_BETWEEN: return SpaceBetween;
        case VJ_AROUND:  return SpaceAround;
        case VJ_EVENLY:  return SpaceEvenly;
        case VJ_STRETCH: return Fill;
        case VJ_START:   return Leading;
    }
    return dflt;
}

/* Open the container this element's `display` asks for.
 *
 * The layout engine has done flex and grid since it was written -- the whole
 * toolkit is built on them -- so a CSS flex container is not new layout, it is
 * the CSS spelling of machinery that already exists. That is the whole reason
 * this is short. */
/* Is the box being emitted a FLEX ITEM? Set by render_children while it walks
 * a flex or grid container's children, because a box cannot see its own
 * parent's display and the two want opposite widths: a block-level box fills
 * its container, a flex item is sized by its content unless it grows. */
/* declared above emit_inline, which needs it for inline-block */
/* ...and whether that container runs down the page WITH A DEFINITE HEIGHT.
 *
 * flex-grow and flex-basis are MAIN-AXIS properties, so in a `flex-direction:
 * column` container `flex: 1` grows a child's HEIGHT. But free space only
 * exists on an axis whose container has a size to be free of -- CSS says the
 * same -- and routing grow onto the height of an AUTO-height column is worse
 * than not routing it at all: the column's height comes from its children, so
 * there is never anything to grow into, and the flex weight instead makes the
 * child the first thing squashed when the column overflows (layout.c gives
 * boxes that asked to flex up their space before intrinsic ones). On
 * rust-lang.org that collapsed <main> to nothing and drew the footer under the
 * navbar. So: a STATED height routes grow to the height; anything else keeps
 * the width behaviour this always had. */
static int g_flex_col;      /* the container is a COLUMN: basis/grow are height */
static int g_flex_col_def;  /* ...and it has a definite height, so it can hand out space */

static void open_box(const struct vstyle *s, EmProps bp) {
    /* TWO GAPS ON TWO AXES. `gap: 30px 10px` is row-gap then column-gap, and
     * which one is the MAIN gap depends on the direction: down a column the
     * row gap separates the items, across a row the column gap does and the
     * row gap separates the wrapped lines. `spacing` is the engine's main-axis
     * gap, so the mapping happens here rather than in the engine. */
    short main_gap = s->gap, cross_gap = -1;
    if (s->col_gap >= 0) {
        if (s->flex_col) { main_gap = s->gap; cross_gap = s->col_gap; }
        else             { main_gap = s->col_gap; cross_gap = s->gap; }
    }
    if (main_gap > 0) bp.spacing = (float)main_gap * g_zoom;
    /* max-width, on every display type -- the engine clamps both the main and
     * the cross axis by it, so a centred content column stops growing at its
     * cap whether it is a block, a flex container or a grid. */
    if (s->max_width > 0) bp.maxw = (float)s->max_width * g_zoom;
    if (s->display == VD_FLEX) {
        bp.justify = em_of(s->justify, Leading);
        /* UNSET align-items keeps the start-alignment this has always had,
         * even though CSS's initial value is STRETCH. The correct default is
         * written and was measured: it fixes a real bug (a `flex-direction:
         * column` container shrinks its rows to their own text -- see
         * tests/web/flex2.html, DOWNB) and it BREAKS rust-lang.org, where it
         * collapses <main> to nothing and draws the footer under the navbar.
         * Something else about that page is wrong and stretch only exposes it.
         * Shipping the fix would trade a bug nobody has hit for one that is
         * visible on a front page, so it waits for the diagnosis -- which is
         * logged in docs/TODO.md rather than left as a surprise here. A STATED
         * align-items is honoured in full; this is only the default. */
        bp.align = em_of(s->align_items, Fill);
        if (s->flex_col)       { em_vstack_(bp); return; }
        if (s->flex_wrap)      { em_flow_(bp);
                                 /* only a WRAPPING row has a second axis to
                                  * space, which is why this is set here */
                                 if (cross_gap >= 0)
                                     ui_set_cross_spacing((float)cross_gap * g_zoom);
                                 return; }
        em_hstack_(bp);
        return;
    }
    if (s->display == VD_GRID) {
        em_grid_(s->grid_cols > 0 ? s->grid_cols : 1, bp);
        /* ...and the sizes the author stated for those tracks, if any. Without
         * them every track is content-sized, which is a table's rule and not a
         * page layout's -- see layout.h. */
        if (s->grid_ntrack > 0) {
            unsigned char mode[VSTYLE_TRACKS];
            float val[VSTYLE_TRACKS];
            for (int i = 0; i < s->grid_ntrack && i < VSTYLE_TRACKS; i++) {
                mode[i] = s->grid_track_mode[i] == VT_PX ? LT_PX
                        : s->grid_track_mode[i] == VT_FR ? LT_FR : LT_AUTO;
                val[i]  = s->grid_track_mode[i] == VT_FR
                        ? (float)s->grid_track_val[i] / 16.0f
                        : (float)s->grid_track_val[i];
            }
            ui_set_grid_tracks(mode, val, s->grid_ntrack);
        }
        return;
    }
    if (s->justify) bp.justify = em_of(s->justify, Leading);
    em_vstack_(bp);
}

/* State the box's size EXPLICITLY, every time.
 *
 * em_apply_box only calls ui_set_size when a prop asks for one, so a widget
 * instance that is REUSED next frame keeps whatever size it was last given.
 * That is invisible in a harness which renders one tree a few times, and very
 * visible in the app, which builds an empty view first and the document
 * second: the instances are matched by position across two DIFFERENT trees, so
 * a box inherited `grow` from whatever occupied its slot before, ate the row's
 * leftover space and shoved its siblings to the right edge. Saying the size
 * out loud every frame is what makes a retained tree behave like an immediate
 * one. */
/* The layout engine subtracts padding from a node's size to get its content
 * box, so a layout node IS a border box. That makes `box-sizing: border-box`
 * -- what nearly every modern stylesheet sets globally -- free, and makes the
 * CSS DEFAULT the case needing work: under content-box a stated width is the
 * CONTENT, and the border box is that much wider. */
static float box_inset(const struct vstyle *s) {
    return (float)(s->pad_left + s->pad_right) + 2.0f * (float)s->border_width;
}

static void size_box(const struct vstyle *s, int flex_item) {
    /* Placement first: a box that claims a named area is put where the area
     * is, rather than wherever the auto-flow happened to reach. */
    if (g_area_owner && s->grid_area) {
        for (int a = 0; a < g_area_owner->n_areas; a++)
            if (g_area_owner->area_name[a] == s->grid_area) {
                ui_set_grid_place(g_area_owner->area_r[a], g_area_owner->area_c[a],
                                  g_area_owner->area_rs[a], g_area_owner->area_cs[a]);
                break;
            }
    }
    struct layout_size w, h;
    /* flex-shrink, ONLY when the author asked for more than the default.
     *
     * CSS's initial shrink is 1, and the engine's IMPLICIT default already
     * behaves like 1 -- but with a priority CSS lacks: boxes that asked to
     * flex give up space before intrinsically-sized ones, so a toolbar squashes
     * its content pane rather than its buttons (layout.c, "pass 1 / pass 2").
     * An EXPLICIT weight deliberately bypasses that priority, so stating the
     * default on every item flattened it -- and a row that could no longer
     * shrink its flexible child in isolation grew two lines taller instead.
     * Only a stated weight above the default is worth losing the priority for.
     *
     * `flex-shrink: 0` is expressed by the SIZE, not here: a stated basis is a
     * SIZE_FIXED box, and the engine never shrinks one of those. Without a
     * basis (`flex: none`) an intrinsic box can still give in pass 2, which is
     * a bounded and visible imperfection rather than a wrong number. */
    float shr = (s->shrink > 1) ? (float)s->shrink : 0.0f;
    float grow_w = s->border_box ? 0.0f : box_inset(s);
    float grow_h = s->border_box ? 0.0f
                 : (float)(s->pad_top + s->pad_bottom) + 2.0f * (float)s->border_width;
    if (s->width_pct)      w = (struct layout_size){ .mode = SIZE_PERCENT,
                                                     .fixed_value = (float)s->width_pct / 100.0f,
                                                     .pct_px = (float)s->width };
    else if (s->width > 0) w = (struct layout_size){ .mode = SIZE_FIXED,
                                                     .fixed_value = (float)s->width + grow_w };
    /* THE MAIN AXIS OF THE CONTAINER. flex-grow and flex-basis apply there and
     * nowhere else; a column's items grow in height. */
    else if (s->grow && !g_flex_col)
                           w = (struct layout_size){ .mode = s->basis >= 0 ? SIZE_FIXED : SIZE_FLEX,
                                                     .fixed_value = s->basis >= 0
                                                                  ? (float)s->basis * g_zoom + grow_w : 0,
                                                     .flex_grow = (float)s->grow,
                                                     .flex_shrink = shr };
    else if (s->basis >= 0 && !g_flex_col)
                           w = (struct layout_size){ .mode = SIZE_FIXED,
                                                     .fixed_value = (float)s->basis * g_zoom + grow_w,
                                                     .flex_shrink = shr };
    /* A ROW's item is sized by its content on the main axis. A COLUMN's item
     * is not: width is its CROSS axis, and CSS stretches it to the column.
     * We do not apply that default yet (see open_box), so the fill has to be
     * asked for here -- which is exactly what this did before flex-grow was
     * routed to the height axis. Dropping it made every `flex: 1` row inside a
     * `flex-direction: column` shrink to its own text. */
    else if (flex_item && !g_flex_col)
                           w = (struct layout_size){ .mode = SIZE_INTRINSIC,
                                                     .flex_shrink = shr };
    else                   w = (struct layout_size){ .mode = SIZE_FLEX,  .flex_grow = 1 };
    if (s->height_pct)     h = (struct layout_size){ .mode = SIZE_PERCENT,
                                                     .fixed_value = (float)s->height_pct / 100.0f,
                                                     .pct_px = (float)s->height };
    else if (s->height > 0) h = (struct layout_size){ .mode = SIZE_FIXED,
                                                      .fixed_value = (float)s->height + grow_h };
    else if (s->grow && g_flex_col_def)
                            h = (struct layout_size){ .mode = s->basis >= 0 ? SIZE_FIXED : SIZE_FLEX,
                                                      .fixed_value = s->basis >= 0
                                                                   ? (float)s->basis * g_zoom + grow_h : 0,
                                                      .flex_grow = (float)s->grow,
                                                      .flex_shrink = shr };
    else if (s->basis >= 0 && g_flex_col_def)
                            h = (struct layout_size){ .mode = SIZE_FIXED,
                                                      .fixed_value = (float)s->basis * g_zoom + grow_h,
                                                      .flex_shrink = shr };
    else                   h = (struct layout_size){ .mode = SIZE_INTRINSIC };
    ui_set_size(w, h);
    /* align-self, last: it is a property of the CHILD about its own cross
     * alignment, and only the child knows it. VJ_AUTO leaves the container's
     * align-items in charge, which is the initial value. */
    if (s->align_self != VJ_AUTO) {
        int a = s->align_self == VJ_CENTER  ? ALIGN_CENTER
              : s->align_self == VJ_END     ? ALIGN_END
              : s->align_self == VJ_STRETCH ? ALIGN_STRETCH : ALIGN_START;
        ui_set_align_self(a);
    }
}

static void render_inline_run(struct html_doc *d, int from, int to,
                              const struct vstyle *parent, const char *href) {
    /* spacing 0: the inter-word space is baked into each word instead (see
     * emit_text). A uniform gap between boxes puts one in front of a comma
     * that arrived as its own text node -- "bold , italic". */
    /* text-align lives on the ROW, not on the block: the block still fills its
     * container (or a centred paragraph would shrink to its longest line and
     * take its background with it) -- it is the words inside each line box
     * that move. */
    EmAlign j = parent->align == VA_CENTER ? Center
              : parent->align == VA_RIGHT  ? Trailing : Leading;
    g_pending_space = 0;          /* a line never opens with a space */
    Flow(.spacing = 0, .justify = j) {
        for (int c = from; c >= 0 && c != to; c = d->nodes[c].next_sibling)
            emit_inline(d, c, parent, href, 0);
    }
}

/* A text field. The toolkit already owns editing, focus, the caret and the
 * keyboard -- so a form control is not new machinery here, it is the browser
 * handing EmUI a buffer and getting typing back. The buffer belongs to form.c
 * (the user's typing is not part of the document; see form.h), which is what
 * lets a re-render leave a half-filled form alone.
 *
 * Enter submits, because a search box you cannot submit from the keyboard is
 * a search box that feels broken. */
/* A boolean control's state, kept per node and stable across frames because
 * the toolkit binds a pointer to it. The FORM's copy stays the source of
 * truth for submission -- this is the toolkit's working copy, synced in before
 * the control draws and out again straight after, so a click is visible to
 * form_submit in the same frame it happened. */
#define CHECK_MAX 64
static struct { int node, used; bool on; } g_check[CHECK_MAX];

static bool *check_slot(int node) {
    for (int i = 0; i < CHECK_MAX; i++)
        if (g_check[i].used && g_check[i].node == node) return &g_check[i].on;
    for (int i = 0; i < CHECK_MAX; i++)
        if (!g_check[i].used) { g_check[i].used = 1; g_check[i].node = node;
                                g_check[i].on = false; return &g_check[i].on; }
    return 0;
}

/* --- <details> ------------------------------------------------------------ *
 *
 * A disclosure: the <summary> is always shown, everything else only when the
 * element is open. It is real HTML, not a widget a page builds -- and leaving
 * it unimplemented does not degrade gracefully, it renders every collapsed
 * section expanded. MDN's CSS reference has 112 of them and came out 306780
 * pixels tall, with the article somewhere in the middle of it.
 *
 * Whether a particular one is open is UI STATE, like a checkbox's: the document
 * says where it starts (`open`), and the reader changes it from there. So it
 * lives beside the checkbox table and for the same reason -- the DOM is what
 * the page said, not what the reader has since done to it. */
#define DETAILS_MAX 256
static struct { int used; int node; bool open; } g_details[DETAILS_MAX];

static bool *details_slot(struct html_doc *d, int node) {
    for (int i = 0; i < DETAILS_MAX; i++)
        if (g_details[i].used && g_details[i].node == node) return &g_details[i].open;
    for (int i = 0; i < DETAILS_MAX; i++)
        if (!g_details[i].used) {
            g_details[i].used = 1; g_details[i].node = node;
            g_details[i].open = d->nodes[node].open ? true : false;
            return &g_details[i].open;
        }
    /* Out of slots: show it OPEN. A disclosure nobody can open is content the
     * reader cannot reach at all, which is worse than one that will not shut. */
    return 0;
}

/* One page's disclosures are not the next page's. Called where the other
 * per-page UI state is dropped -- see render.h. */
void vellum_reset_details(void) { memset(g_details, 0, sizeof g_details); }

/* Turning one radio on turns the rest of its group off. The group is every
 * radio sharing a `name`, which is what makes a radio a radio -- without this
 * they are just checkboxes that look round. */
static void radio_clear_group(struct html_doc *d, int node) {
    const char *nm = d->nodes[node].name;
    if (!nm) return;
    for (int i = 0; i < d->n; i++) {
        if (i == node || d->nodes[i].kind != HTML_ELEM) continue;
        if (!d->nodes[i].name || strcmp(d->nodes[i].name, nm)) continue;
        const char *ty = d->nodes[i].type;
        if (!ty || strcmp(ty, "radio")) continue;
        bool *o = check_slot(i);
        if (o) *o = false;
        form_set(d, i, "");
    }
}

static void emit_check(struct html_doc *d, int n, const struct vstyle *st, int radio) {
    bool *on = check_slot(n);
    if (!on) { Text("[too many controls]").caption().tertiary(); return; }
    /* sync IN: the document's `checked`, or whatever a script last set */
    const char *cur = form_peek(n);
    *on = (cur && cur[0]);
    bool before = *on;

    const char *label = d->nodes[n].alt ? d->nodes[n].alt : "";
    Checkbox(label, on).color(argb(PAGE_INK));
    em_flush();                       /* the click lands here, not later */

    if (*on != before) {
        if (radio && *on) radio_clear_group(d, n);
        /* A checkbox submits its `value`, or "on" when it has none -- which is
         * what a form on the other end expects to receive. */
        const char *v = d->nodes[n].value ? d->nodes[n].value : "on";
        form_set(d, n, *on ? v : "");
    }
    (void)st;
}

/* A <select> is its <option> children. The labels are their text and the
 * submitted value is the option's `value`, or its text when it has none --
 * exactly what a browser sends. */
static void emit_select(struct html_doc *d, int n, const struct vstyle *st) {
    static const char *labels[16];
    static int opts[16];
    int nopt = 0;
    for (int c = d->nodes[n].first_child; c >= 0 && nopt < 16; c = d->nodes[c].next_sibling) {
        if (d->nodes[c].kind != HTML_ELEM || strcmp(d->nodes[c].tag, "option")) continue;
        int t = d->nodes[c].first_child;
        labels[nopt] = (t >= 0 && d->nodes[t].text) ? d->nodes[t].text : "";
        opts[nopt] = c;
        nopt++;
    }
    if (!nopt) { Text("[empty select]").caption().tertiary(); return; }

    /* A select needs a stable INDEX across frames, the way a checkbox needs a
     * stable bool. check_slot allocates the row; the index lives beside it. */
    if (!check_slot(n)) { Text("[too many controls]").caption().tertiary(); return; }
    static int sel_store[CHECK_MAX];
    int idx = 0;
    for (int i = 0; i < CHECK_MAX; i++)
        if (g_check[i].used && g_check[i].node == n) { idx = i; break; }

    /* sync IN from the form, so a script's setValue moves the control */
    const char *cur = form_peek(n);
    if (cur && cur[0]) {
        for (int i = 0; i < nopt; i++) {
            const char *ov = d->nodes[opts[i]].value ? d->nodes[opts[i]].value : labels[i];
            if (!strcmp(ov, cur)) { sel_store[idx] = i; break; }
        }
    }
    int before = sel_store[idx];
    float w = st->width ? (float)st->width : 200.0f;
    HStack(.width = w, .py = 2) { Dropdown(labels, nopt, &sel_store[idx]); }
    em_flush();
    if (sel_store[idx] != before || !(cur && cur[0])) {
        int k = sel_store[idx];
        if (k < 0 || k >= nopt) k = 0;
        const char *ov = d->nodes[opts[k]].value ? d->nodes[opts[k]].value : labels[k];
        form_set(d, n, ov);
    }
}

static void emit_field(struct html_doc *d, int n, const struct vstyle *st) {
    char *buf = form_value(d, n);
    if (!buf) {                       /* table full: say so rather than lie */
        Text("[too many fields]").caption().tertiary();
        return;
    }
    const char *ph = d->nodes[n].alt ? d->nodes[n].alt : "";
    float w = st->width ? (float)st->width : 260.0f;
    HStack(.width = w, .py = 2) {
        /* Remember WHICH field has focus, so the app can submit on Enter. The
         * toolkit's field reports focus but has no notion of submission -- and
         * it should not: Enter meaning "submit" is a form's idea, not a text
         * box's. */
        if (TextField(buf, FORM_VALUE_MAX, ph).focused()) g_focused_field = n;
    }
}

/* A button. `value` is its label for <input type=submit>, its text content for
 * <button> -- and "Submit" when the page said neither, because an unlabelled
 * button is a control nobody can use. */
static void render_children(struct html_doc *d, int node, const struct vstyle *s,
                            const char *href);

/* Does this button have an ELEMENT child -- an icon, a wrapper, anything that
 * is not just text? Then it has content to render rather than a caption. */
static int first_elem_child(struct html_doc *d, int n) {
    for (int c = d->nodes[n].first_child; c >= 0; c = d->nodes[c].next_sibling)
        if (d->nodes[c].kind == HTML_ELEM) return c;
    return -1;
}

static void emit_button(struct html_doc *d, int n, const struct vstyle *st) {
    static char label[128];
    const char *v = d->nodes[n].value;
    if (v && v[0]) snprintf(label, sizeof label, "%s", v);
    else {
        size_t len = 0; label[0] = 0;
        for (int c = d->nodes[n].first_child; c >= 0; c = d->nodes[c].next_sibling)
            if (d->nodes[c].kind == HTML_TEXT && d->nodes[c].text) {
                int w = snprintf(label + len, sizeof label - len, "%s", d->nodes[c].text);
                if (w > 0) len += (size_t)w;
            }
        /* NO FALLBACK LABEL. A button with neither a value nor text is an
         * ICON button, and writing "Submit" on it states something the page
         * never said -- Brave's header has six and read as six Submit buttons.
         * The icon itself is drawn below. */
    }
    /* WHITESPACE IS NOT A LABEL. A button written across several lines has a
     * text child of " ", and treating that as its label skipped the icon path
     * entirely -- the button drew as empty chrome with the icon it actually
     * contains left undrawn. */
    { size_t a = 0, b2 = strlen(label);
      while (a < b2 && (label[a]==' '||label[a]=='\t'||label[a]=='\n'||label[a]=='\r')) a++;
      while (b2 > a && (label[b2-1]==' '||label[b2-1]=='\t'||label[b2-1]=='\n'||label[b2-1]=='\r')) b2--;
      memmove(label, label + a, b2 - a); label[b2 - a] = 0; }

    /* A BUTTON WITH CONTENT, rather than a caption.
     *
     * Button() takes a string, so this used to render a button's LABEL and
     * nothing else -- one string, or (once icons could be drawn) its first
     * icon. A real page's button is a box with children: an icon and a word, or
     * two icons, or a wrapper around either. Everything past the first was
     * simply not walked, which is why 33 of Brave's 34 inline <svg> elements
     * were never even REACHED by the renderer, let alone drawn.
     *
     * So: if the button has element children, it is built here from the
     * primitives Button is itself built from -- a styled box that reports its
     * own clicks -- and its children are rendered inside it. Text-only buttons
     * keep the toolkit's Button, which knows how a caption should look. */
    if (!label[0] || first_elem_child(d, n) >= 0) {
        int icon = first_elem_child(d, n);
        if (icon >= 0) {
            em_flush();
            HStack(.px = 6, .py = 4, .align = Center, .spacing = 4,
                   .background = argb(0xFFEFEFEFU), .border = 1,
                   .border_color = argb(0xFF9A9A9AU), .corner = 6) {
                struct instance_handle self = ui_open();
                render_children(d, n, st, 0);
                if (ui_consume_click(self)) {
                    if (g_has_listener && g_has_listener(n)) { if (g_on_click) g_on_click(n); }
                    else if (g_on_submit) g_on_submit(n);
                }
            }
            return;
        }
    }
    (void)st;
    HStack(.py = 2) {
        /* A page's submit button, not a desktop primary action: the web's
         * button is a light face with dark text and a thin border. */
        /* KEYED BY THE NODE, not by the label. Identity in a retained tree is
         * the key, and every unlabelled button carried the same one -- six
         * buttons claiming to be the same instance, which is a reconciliation
         * bug waiting for the frame that notices. */
        char bid[24];
        snprintf(bid, sizeof bid, "btn%d", n);
        if (Button(label).id(bid)
                .bg(argb(0xFFEFEFEFU)).color(argb(PAGE_INK))
                .border(1).clicked()) {
            /* A listener wins over submission: a script that took the click
             * asked to handle it, and firing both would submit a form the page
             * meant to intercept. */
            if (g_has_listener && g_has_listener(n)) { if (g_on_click) g_on_click(n); }
            else if (g_on_submit) g_on_submit(n);
        }
    }
}

/* --- tables ---------------------------------------------------------------
 *
 * A table is the one document structure whose columns must line up ACROSS
 * independent rows -- which is precisely what a row of HStacks cannot do and
 * what the layout engine's grid already does. So a <table> becomes one grid of
 * N columns and every cell is a child of it, in reading order.
 *
 * The honest limit, stated because it is visible: the grid's tracks are EQUAL
 * width. A real table sizes each column to its content, which needs a
 * measurement pass the renderer cannot do (it emits; layout measures later).
 * Equal columns keep every row aligned -- the property that makes a table a
 * table -- and cost some space in a table of one short column and one long
 * one. Content-proportional tracks are a layout-engine feature, logged.
 */

/* Cells of one row, in order; descends through thead/tbody/tfoot, which carry
 * no box of their own. Returns the count. */
static int row_cells(struct html_doc *d, int row, int *out, int max,
                     const struct vstyle *pst) {
    int n = 0;
    for (int c = d->nodes[row].first_child; c >= 0 && n < max; c = d->nodes[c].next_sibling) {
        if (d->nodes[c].kind != HTML_ELEM) continue;
        struct vstyle cs;
        vstyle_for_node(d, c, pst, g_sheet, &cs);
        if (cs.display == VD_CELL) out[n++] = c;
    }
    return n;
}

/* Every row of a table, descending through row groups. */
static int table_rows(struct html_doc *d, int tbl, int *out, int max,
                      const struct vstyle *tst) {
    int n = 0;
    for (int c = d->nodes[tbl].first_child; c >= 0 && n < max; c = d->nodes[c].next_sibling) {
        if (d->nodes[c].kind != HTML_ELEM) continue;
        struct vstyle cs;
        vstyle_for_node(d, c, tst, g_sheet, &cs);
        if (cs.display == VD_ROW) { out[n++] = c; continue; }
        if (cs.display == VD_BLOCK) {            /* thead/tbody/tfoot: descend */
            for (int r = d->nodes[c].first_child; r >= 0 && n < max; r = d->nodes[r].next_sibling) {
                if (d->nodes[r].kind != HTML_ELEM) continue;
                struct vstyle rs;
                vstyle_for_node(d, r, &cs, g_sheet, &rs);
                if (rs.display == VD_ROW) out[n++] = r;
            }
        }
    }
    return n;
}

#define TBL_MAX_ROWS 128
#define TBL_MAX_COLS  12

static void render_children(struct html_doc *d, int node, const struct vstyle *s,
                            const char *href);

/* Returns 0 when `node` has no table structure to draw, so the caller can fall
 * back to laying it out as an ordinary block.
 *
 * It used to return silently and the subtree DISAPPEARED, which matters far
 * more than it sounds: `display: table` is a layout idiom on the real web, not
 * only a thing <table> does -- centring, equal columns, and the clearfix that
 * every CSS framework ships. A div carrying it has no rows, so the whole
 * branch was dropped. Block is the honest degradation: a real engine would
 * wrap the children in anonymous table boxes and the result would look much
 * the same at this level of fidelity. */
static int render_table(struct html_doc *d, int node, const struct vstyle *st,
                        const char *href) {
    /* ON THE STACK, and it matters: this function RECURSES. A table inside a
     * table's cell re-enters it, and a `static` row list meant the inner table
     * overwrote the outer's -- so after the nested table returned, the outer
     * carried on reading the INNER table's rows. Hacker News (a table whose
     * third row holds a 92-row table, and whose fourth is the footer) rendered
     * its footer as a repeat of an item row, and the real footer -- guidelines,
     * FAQ, lists, API, security, legal -- was never drawn at all. The nesting
     * is shallow and 128 ints is half a kilobyte; the static saved nothing and
     * cost the bottom of the page. */
    int rows[TBL_MAX_ROWS];
    int nrow = table_rows(d, node, rows, TBL_MAX_ROWS, st);
    if (!nrow) return 0;

    /* The column count is the WIDEST row: a row with fewer cells leaves the
     * tail empty rather than shifting the ones after it into the wrong
     * column, which is what makes a ragged table still readable. */
    int ncol = 1;
    for (int i = 0; i < nrow; i++) {
        int cells[TBL_MAX_COLS];
        int nc = row_cells(d, rows[i], cells, TBL_MAX_COLS, st);
        int span_total = 0;
        for (int c = 0; c < nc; c++) {
            int sp = d->nodes[cells[c]].img_w;   /* colspan, parsed into img_w */
            span_total += sp > 0 ? sp : 1;
        }
        if (span_total > ncol) ncol = span_total;
    }
    if (ncol > TBL_MAX_COLS) ncol = TBL_MAX_COLS;

    const struct ui_theme *t = ui_theme();
    /* Does this table want rules drawn? <table border=N> is the only thing on
     * the old web that says so, and its absence means no. */
    int rules = d->nodes[node].tborder > 0;
    VStack(.spacing = 0, .align = Fill,
           .pt = (float)st->margin_top, .pb = (float)st->margin_bottom) {
        /* the caption, if the author wrote one: a table's title belongs above
         * it and outside the grid */
        for (int c = d->nodes[node].first_child; c >= 0; c = d->nodes[c].next_sibling) {
            if (d->nodes[c].kind != HTML_ELEM) continue;
            struct vstyle cs;
            vstyle_for_node(d, c, st, g_sheet, &cs);
            if (cs.display != VD_CAPTION) continue;
            VStack(.spacing = 0, .align = Fill, .pb = (float)cs.margin_bottom) {
                render_children(d, c, &cs, href);
            }
        }

        em_flush();
        ui_begin_vstack(0x7AB10000ULL ^ (uint64_t)(uintptr_t)&d->nodes[node]);
        ui_set_grid(ncol, 0.0f, 0.0f);
        ui_set_size((struct layout_size){ .mode = SIZE_FLEX, .flex_grow = 1 },
                    (struct layout_size){ .mode = SIZE_INTRINSIC });

        for (int i = 0; i < nrow; i++) {
            int cells[TBL_MAX_COLS];
            int nc = row_cells(d, rows[i], cells, TBL_MAX_COLS, st);
            int placed = 0;
            for (int c = 0; c < nc && placed < ncol; c++) {
                int cell = cells[c];
                struct vstyle rs, cs;
                vstyle_for_node(d, rows[i], st, g_sheet, &rs);
                vstyle_for_node(d, cell, &rs, g_sheet, &cs);
                int sp = d->nodes[cell].img_w;
                if (sp < 1) sp = 1;
                if (placed + sp > ncol) sp = ncol - placed;

                em_flush();
                ui_begin_vstack(0x7AB20000ULL ^ (uint64_t)(uintptr_t)&d->nodes[cell]);
                ui_set_grid_span(sp);
                ui_set_padding(7, 9, 7, 9);
                ui_set_align(ALIGN_STRETCH);
                /* A header row needs to READ as one, and a row separator is
                 * what stops a dense table becoming a wall. Both come from the
                 * theme so the table follows the desktop into dark mode. */
                /* The cell's OWN background first -- an author who painted
                 * this cell outranks the theme's idea of a header row. It is
                 * also how the old web colours anything at all: bgcolor= maps
                 * to background-color (see html.c), and without this Hacker
                 * News drew its orange header bar as nothing, leaving grey
                 * text on the page background. */
                if (cs.bg) {
                    struct paint cp = { 0 };
                    cp.kind = PAINT_SOLID; cp.solid = argb(cs.bg);
                    ui_set_paint(cp);
                } else if (cs.bold) {
                    struct paint hp = { 0 };
                    hp.kind = PAINT_SOLID; hp.solid = t->surface_alt;
                    ui_set_paint(hp);
                } else if (rules) {
                    /* Only when the TABLE asked for rules. A bare <table> has
                     * no borders on the web -- the initial border-style is
                     * none -- and half the old web uses tables purely for
                     * LAYOUT, writing border="0" to say so. Outlining every
                     * cell of those turns a news site into a spreadsheet. */
                    struct color line = t->text_secondary; line.a *= 0.16f;
                    ui_set_border(1.0f, line);
                }
                render_children(d, cell, &cs, href);
                em_flush();
                ui_end_stack();
                placed += sp;
            }
            /* pad a short row so the next one starts in column 0 */
            for (; placed < ncol; placed++) {
                em_flush();
                ui_begin_vstack(0x7AB30000ULL ^ ((uint64_t)i << 8) ^ (uint64_t)placed);
                ui_set_padding(7, 9, 7, 9);
                ui_end_stack();
            }
        }
        em_flush();
        ui_end_stack();
    }
    return 1;
}

/* <pre>: one text node per source line, whitespace intact, no wrapping. */
/* Gather a <pre>'s text, DESCENDANTS INCLUDED, with <br> as a newline.
 *
 * Only direct text children were read, and everything else was skipped -- so a
 * <pre> whose lines are wrapped in <span>s rendered as nothing at all. That is
 * not an edge case: it is every syntax-highlighted code block on the web,
 * because highlighting IS spans. MDN's formal-syntax block, which is entirely
 * spans and <br>s, came out blank on every reference page. */
static void pre_gather(struct html_doc *d, int n, char *out, size_t cap, size_t *len,
                       int depth) {
    if (n < 0 || depth > 16 || *len + 1 >= cap) return;
    for (int c = d->nodes[n].first_child; c >= 0; c = d->nodes[c].next_sibling) {
        if (d->nodes[c].kind == HTML_TEXT) {
            const char *t = d->nodes[c].text;
            for (; t && *t && *len + 1 < cap; t++) out[(*len)++] = *t;
        } else if (!strcmp(d->nodes[c].tag, "br")) {
            if (*len + 1 < cap) out[(*len)++] = '\n';
        } else {
            pre_gather(d, c, out, cap, len, depth + 1);
        }
        if (*len + 1 >= cap) break;
    }
    out[*len] = 0;
}

static void render_pre(struct html_doc *d, int node, const struct vstyle *s) {
    {
        static char text[8192];
        size_t tl = 0;
        pre_gather(d, node, text, sizeof text, &tl, 0);
        static char pool[128][200];
        static int pn;
        const char *p = text;
        while (*p) {
            size_t n = 0;
            char line[200];
            while (*p && *p != '\n' && n + 1 < sizeof line) line[n++] = *p++;
            line[n] = 0;
            if (*p == '\n') p++;
            if (pn >= 128) pn = 0;
            snprintf(pool[pn], sizeof pool[0], "%s", line[0] ? line : " ");
            Text(pool[pn]).font(Caption).color(ui_theme()->text_secondary);
            em_flush();     /* the DSL stages a leaf: `line` is reused below */
            pn++;
        }
    }
    (void)s;
}

/* The style of an element child, or 0 if it is not an element. */
static int child_style(struct html_doc *d, int c, const struct vstyle *s,
                       struct vstyle *out) {
    if (c < 0 || d->nodes[c].kind != HTML_ELEM) return 0;
    vstyle_for_node(d, c, s, g_sheet, out);
    return 1;
}

static void render_range(struct html_doc *d, int from, int to,
                         const struct vstyle *s, const char *href, int *li);

/* FLOATS, as a row.
 *
 * CSS floats take a box out of flow, pin it to one edge, and shorten the LINE
 * BOXES of everything that follows until something clears -- so text wraps
 * around a floated image and then reclaims the full width below it. This
 * renderer has no exclusion regions: an inline run is a wrapping row that
 * knows nothing about boxes beside it.
 *
 * So a float and the content that flows beside it become an actual ROW:
 * [float][the rest] (or [the rest][float] for float: right). That is exactly
 * right for the two shapes floats are really used in -- an image with text
 * beside it, and float-based columns -- and it is WRONG in one visible way:
 * the text never reclaims the full width below the float, it stays in its
 * column. Named here and in docs/TODO.md rather than left to be discovered.
 *
 * Returns the sibling to continue from. */
static int render_float_group(struct html_doc *d, int c, int to,
                              const struct vstyle *s, const char *href, int *li) {
    /* One group's worth of floats. The old cap was 8 and a float past it was
     * SKIPPED -- kernel.org floats nine footer links and the ninth simply did
     * not exist. Now a full group STOPS collecting, so the overflow stays in
     * the sibling walk and opens the next group: bounded work, nothing lost.
     * (Progress is guaranteed: a full group consumed FLOATS_MAX siblings.) */
#define FLOATS_MAX 32
    int lf[FLOATS_MAX], rf[FLOATS_MAX], nl = 0, nr = 0;
    /* the run of consecutive floated siblings that opens the group */
    while (c >= 0 && c != to) {
        struct vstyle cs;
        if (!child_style(d, c, s, &cs)) {
            if (is_blank_text(d, c)) { c = d->nodes[c].next_sibling; continue; }
            break;
        }
        if (!cs.floatp) break;
        if (cs.display != VD_NONE) {
            if (cs.floatp == VF_RIGHT) { if (nr == FLOATS_MAX) break; rf[nr++] = c; }
            else                       { if (nl == FLOATS_MAX) break; lf[nl++] = c; }
        }
        c = d->nodes[c].next_sibling;
    }

    /* everything that flows BESIDE them: up to the next clearing sibling */
    int rest_from = c, rest_to = c;
    while (rest_to >= 0 && rest_to != to) {
        struct vstyle cs;
        if (child_style(d, rest_to, s, &cs) && (cs.clearp || cs.floatp)) break;
        rest_to = d->nodes[rest_to].next_sibling;
    }

    /* A float row that is ONLY floats is the other shape floats are used in: a
     * grid of columns (`.blogroll li { float: left; width: 33% }`). Those wrap
     * -- three across, then the next line -- and a row that cannot wrap does
     * not merely misplace the overflow, it CLIPS it: kernel.org's fifth footer
     * link was gone off the end of the line. So floats-only becomes a Flow.
     * With in-flow content beside them the non-wrapping row is still right:
     * that is the image-with-text-beside-it shape, which must not break. */
    int grid = (rest_from == rest_to) && (nl + nr > 1);
    EmProps rowp = { .spacing = 10, .align = Leading, .grow = 1 };
    if (grid) em_flow_(rowp); else em_hstack_(rowp);
    {
        int outer = g_flex_item;
        g_flex_item = 1;                 /* a float shrinks to fit, like an item */
        for (int i = 0; i < nl; i++) {
            struct vstyle cs; child_style(d, lf[i], s, &cs);
            render_block(d, lf[i], &cs, d->nodes[lf[i]].href ? d->nodes[lf[i]].href : href, 0);
        }
        if (rest_from != rest_to) {
            g_flex_item = 0;             /* ...and the text column takes the rest */
            VStack(.spacing = 2, .align = Fill, .grow = 1) {
                render_range(d, rest_from, rest_to, s, href, li);
            }
        }
        g_flex_item = 1;
        for (int i = 0; i < nr; i++) {
            struct vstyle cs; child_style(d, rf[i], s, &cs);
            render_block(d, rf[i], &cs, d->nodes[rf[i]].href ? d->nodes[rf[i]].href : href, 0);
        }
        g_flex_item = outer;
    }
    em_end_();
    return rest_to;
}

/* How many items a container may REORDER. Past this the children are emitted
 * in document order, which is what they did before `order` existed -- a
 * bounded, visible degradation rather than a heap the page controls. */
#define FLEX_ORDER_MAX 128

static void render_range(struct html_doc *d, int from, int to,
                         const struct vstyle *s, const char *href, int *li) {
    int c = from;
    int flexish = (s->display == VD_FLEX || s->display == VD_GRID);

    /* `order`: flex items are laid out by it, and by document position among
     * equals. Only taken when SOMEONE STATED ONE and every item is an element
     * -- a bare text node inside a flex container is an anonymous item this
     * cannot name, and reordering around it would drop it. Everything else
     * falls through to the ordinary walk below, unchanged. */
    if (flexish) {
        int idx[FLEX_ORDER_MAX]; short ord[FLEX_ORDER_MAX];
        int n = 0, any = 0, usable = 1;
        for (int e = from; e >= 0 && e != to; e = d->nodes[e].next_sibling) {
            if (is_blank_text(d, e)) continue;
            if (n >= FLEX_ORDER_MAX) { usable = 0; break; }
            struct vstyle cs2;
            if (!child_style(d, e, s, &cs2)) { usable = 0; break; }   /* anonymous item */
            if (cs2.display == VD_NONE) continue;
            idx[n] = e; ord[n] = cs2.order;
            if (cs2.order) any = 1;
            n++;
        }
        if (usable && any) {
            /* Insertion sort, STABLE -- equal orders must keep document order,
             * which is both what CSS says and the only thing that makes this
             * safe to apply to a container the author only partly annotated. */
            for (int i = 1; i < n; i++) {
                int ki = idx[i]; short ko = ord[i]; int j = i - 1;
                while (j >= 0 && ord[j] > ko) { idx[j+1] = idx[j]; ord[j+1] = ord[j]; j--; }
                idx[j+1] = ki; ord[j+1] = ko;
            }
            for (int i = 0; i < n; i++) {
                struct vstyle cs2;
                if (!child_style(d, idx[i], s, &cs2) || cs2.display == VD_NONE) continue;
                if (cs2.display == VD_LIST_ITEM) (*li)++;
                render_block(d, idx[i], &cs2,
                             d->nodes[idx[i]].href ? d->nodes[idx[i]].href : href, *li);
            }
            return;
        }
    }

    while (c >= 0 && c != to) {
        if (flexish && is_blank_text(d, c)) { c = d->nodes[c].next_sibling; continue; }
        /* `display: none` is removed from the box tree ENTIRELY -- so it is
         * skipped HERE, before anything decides whether a run starts or ends.
         * Left in, it counted as a block and split the inline run around it:
         * `<span>a</span><script></script><span>b</span>` became two line
         * boxes, which is how giving <script> a node of its own (so that
         * document.currentScript could exist) made three corpus pages taller. */
        if (d->nodes[c].kind == HTML_ELEM) {
            struct vstyle hid;
            if (child_style(d, c, s, &hid) && hid.display == VD_NONE) {
                c = d->nodes[c].next_sibling;
                continue;
            }
        }
        struct vstyle cs;
        /* A FLOAT opens a row that the following content shares. Not inside a
         * flex or grid container, where CSS says float does not apply. */
        if (!flexish && child_style(d, c, s, &cs) && cs.floatp && cs.display != VD_NONE) {
            c = render_float_group(d, c, to, s, href, li);
            continue;
        }
        if (is_inline(d, c, s)) {
            int start = c;
            while (c >= 0 && c != to && is_inline(d, c, s)) c = d->nodes[c].next_sibling;
            render_inline_run(d, start, c, s, href);   /* c is the run's end */
        } else {
            if (child_style(d, c, s, &cs) && cs.display != VD_NONE) {
                if (cs.display == VD_LIST_ITEM) (*li)++;
                render_block(d, c, &cs, d->nodes[c].href ? d->nodes[c].href : href, *li);
            }
            c = d->nodes[c].next_sibling;
        }
    }
}

static void render_children(struct html_doc *d, int node, const struct vstyle *s,
                            const char *href) {
    int li = 0;
    int flexish = (s->display == VD_FLEX || s->display == VD_GRID);
    int outer_item = g_flex_item, outer_col = g_flex_col, outer_def = g_flex_col_def;
    g_flex_item = flexish;
    /* TWO DIFFERENT FACTS, and conflating them cost rust-lang.org its layout.
     *
     * WHICH AXIS a child's flex-basis and flex-grow land on is decided by the
     * container's direction alone: in a column they are HEIGHT, always. That
     * the column has no definite height does not turn them into a width.
     *
     * WHETHER the column can hand out space is the other question, and that
     * one does need a definite height -- with `height: auto` there is no
     * leftover to distribute, so an item is sized by its content.
     *
     * They were one flag. `body { display:flex; flex-direction:column;
     * min-height:100vh }` states no height, so the flag was false, so
     * `body > main { flex: 1 }` was read as a WIDTH -- and `flex: 1` means
     * basis 0, which made <main> a box zero pixels wide. Everything inside it
     * then sized itself to its own content, so the front page laid out 15000px
     * across and the third of three columns began off the right edge. */
    g_flex_col  = flexish && s->display == VD_FLEX && s->flex_col;
    g_flex_col_def = g_flex_col && (s->height > 0 || s->height_pct);
    /* A child's `grid-area` names a rectangle only its PARENT knows, so the
     * parent leaves itself here for exactly as long as its children are being
     * emitted. Saved and restored because grids nest. */
    const struct vstyle *outer_owner = g_area_owner;
    g_area_owner = (s->display == VD_GRID && s->n_areas > 0) ? s : 0;
    render_range(d, d->nodes[node].first_child, -1, s, href, &li);
    g_area_owner = outer_owner;
    g_flex_item = outer_item;
    g_flex_col  = outer_col;
    g_flex_col_def = outer_def;
}

/* Emit whatever this element's display asks for, with no regard to events. */
static void render_block_inner(struct html_doc *d, int node, const struct vstyle *s,
                               const char *href, int list_index);

/* An element a script is LISTENING to becomes clickable -- and so does one
 * whose ANCESTOR is listening, because that is what bubbling means.
 *
 * The hit box wraps EVERY display path, which it did not before: the
 * list-item, image, table and control branches all returned before reaching
 * it, so a click on an <li> inside a listening <ul> was consumed by the ul and
 * arrived with `event.target` set to the ul. Delegation exists precisely so a
 * handler can ask which item was clicked, so that made the feature useless
 * while appearing to work.
 *
 * Only listening elements get a box: a page where every <div> is a hit target
 * is a page whose links and text selection stop working. */
static void render_block(struct html_doc *d, int node, const struct vstyle *s,
                         const char *href, int list_index) {
    if (!(g_has_listener && g_has_listener(node))) {
        render_block_inner(d, node, s, href, list_index);
        return;
    }
    em_flush();
    ui_box_begin(0xC11C0000ULL ^ (uint64_t)(uintptr_t)&d->nodes[node]);
    struct instance_handle self = ui_open();
    ui_set_size((struct layout_size){ .mode = SIZE_FLEX, .flex_grow = 1 },
                (struct layout_size){ .mode = SIZE_INTRINSIC });
    render_block_inner(d, node, s, href, list_index);
    em_flush();
    ui_box_end();
    /* The INNERMOST listening box consumes -- ui_consume_click clears the
     * click, and a child's box is closed before its parent's, so the deepest
     * one asks first. That is what makes event.target the element the person
     * actually clicked. */
    if (ui_consume_click(self) && g_on_click) g_on_click(node);
}

static void render_block_inner(struct html_doc *d, int node, const struct vstyle *s,
                               const char *href, int list_index) {
    /* <svg> IS A REPLACED ELEMENT. Its children are a different rendering
     * model -- paths, groups, defs -- and none of them is document content.
     * Walking into them as if they were blocks is not a missing feature, it is
     * a wrong answer with a size: MDN's 83x24 logo came out 3340 pixels tall,
     * because every <path> in it became a block in the flow, and the page was
     * 300000 pixels long with the article somewhere inside.
     *
     * Drawing the vector itself is a renderer this browser does not have. A
     * box of the size the author stated is the honest stand-in: the page's
     * layout is then right, and the space is reserved rather than filled. */
    if (!strcmp(d->nodes[node].tag, "svg")) {
        float w = d->nodes[node].img_w > 0 ? zpx(d->nodes[node].img_w) : 16.0f;
        float h = d->nodes[node].img_h > 0 ? zpx(d->nodes[node].img_h) : 16.0f;
        if (s->width  > 0) w = zpx(s->width);
        if (s->height > 0) h = zpx(s->height);
        HStack(.width = w, .height = h) { }
        return;
    }
    /* A DISCLOSURE. The summary is a row you can click; the rest is there only
     * when it is open. See the note above details_slot. */
    if (!strcmp(d->nodes[node].tag, "details")) {
        bool *open = details_slot(d, node);
        bool is_open = open ? *open : true;
        VStack(.spacing = 2, .align = Fill,
               .pt = (float)s->margin_top, .pb = (float)s->margin_bottom) {
            for (int c = d->nodes[node].first_child; c >= 0; c = d->nodes[c].next_sibling) {
                int summary = (d->nodes[c].kind == HTML_ELEM &&
                               !strcmp(d->nodes[c].tag, "summary"));
                if (!summary && !is_open) continue;
                struct vstyle cs;
                if (d->nodes[c].kind == HTML_ELEM) {
                    vstyle_for_node(d, c, s, g_sheet, &cs);
                    if (cs.display == VD_NONE) continue;
                    if (summary) {
                        /* the marker a browser draws, and the hit target */
                        em_flush();
                        ui_box_begin(0xD57A0000ULL ^ (uint64_t)(uintptr_t)&d->nodes[c]);
                        struct instance_handle self = ui_open();
                        HStack(.spacing = 6, .align = Leading) {
                            Text(is_open ? "\xE2\x96\xBE" : "\xE2\x96\xB8")
                                .color(argb(PAGE_QUIET));
                            VStack(.spacing = 0, .align = Fill, .grow = 1) {
                                render_block(d, c, &cs, href, 0);
                            }
                        }
                        em_flush();
                        ui_box_end();
                        if (ui_consume_click(self) && open) {
                            *open = !*open;
                            em_structure_changed();   /* rows below it move */
                        }
                        continue;
                    }
                    render_block(d, c, &cs, href, 0);
                } else if (!is_blank_text(d, c)) {
                    render_range(d, c, d->nodes[c].next_sibling, s, href, &list_index);
                }
            }
        }
        return;
    }
    /* THE LIST MARKER, decided here and drawn below inside the item's own box.
     *
     * WHICH marker -- or none at all -- is the LIST's decision, inherited
     * (style.c, <ul>): `list-style: none` is on essentially every navigation
     * menu on the web, and an <ol> is numbered, so one hardcoded bullet was
     * two visible wrongs at once. And the item is drawn through the ordinary
     * box path rather than a private one, because a list item is an ordinary
     * block that happens to have a marker -- the private path ignored width,
     * background, padding and border, so `.blogroll li { width: 33% }` (a
     * float grid, the second commonest thing a <ul> is) sized to its text. */
    static char li_num[12];
    const char *li_mark = 0;
    if (s->display == VD_LIST_ITEM) {
        if (s->marker == VM_DECIMAL) {
            snprintf(li_num, sizeof li_num, "%d.", list_index > 0 ? list_index : 1);
            li_mark = li_num;
        } else if (s->marker != VM_NONE) {
            li_mark = "\xE2\x80\xA2";
        }
    }
    /* .align = Fill, NOT Leading. A leading-aligned block sizes to its
     * content, so the Flow inside it is handed an unbounded width and never
     * wraps -- the first render ran every paragraph off the right edge. A
     * block in a document is as wide as its parent; that is what makes the
     * line breaks happen. */
    if (s->display == VD_IMAGE) { emit_image(d, node, s); return; }
    if (s->display == VD_TABLE && render_table(d, node, s, href)) return;
    if (s->display == VD_FIELD)  { emit_field(d, node, s); return; }
    if (s->display == VD_BUTTON) { emit_button(d, node, s); return; }
    if (d->nodes[node].svg)      { emit_svg(d, node, s); return; }
    if (s->display == VD_CHECK)  { emit_check(d, node, s, 0); return; }
    if (s->display == VD_RADIO)  { emit_check(d, node, s, 1); return; }
    if (s->display == VD_SELECT) { emit_select(d, node, s); return; }

    /* The BOX the cascade asked for. Padding is inside the painted area and
     * margin outside it, which is why they stopped sharing a field once a
     * background could be seen. */
    EmProps bp = { .spacing = 2, .align = Fill,
                   .pt = zpx((short)(s->margin_top + s->pad_top)),
                   .pb = zpx((short)(s->margin_bottom + s->pad_bottom)),
                   .pl = zpx((short)(s->indent + s->pad_left)),
                   .pr = zpx(s->pad_right) };
    if (s->bg)           bp.background = argb(s->bg);
    if (s->border_width) { bp.border = (float)s->border_width;
                           bp.border_color = argb(s->border_color ? s->border_color
                                                                  : 0xFF808080u); }
    if (s->radius)       bp.corner = (float)s->radius;
    /* flex-grow is a property of the CHILD, and this is where the child is
     * emitted -- so it is read here rather than by whatever contains us. */
    if (s->grow) bp.grow = 1;
    if (s->width  > 0) bp.width  = zpx(s->width);
    if (s->height > 0) bp.height = zpx(s->height);

    int was_item = g_flex_item;
    /* An absolutely positioned box leaves the flow entirely: it stops being a
     * flex item, and its siblings must lay out as if it were not there. */
    int out_of_flow = (s->position == VP_ABSOLUTE || s->position == VP_FIXED);
    open_box(s, bp);
    /* Hand the box back to whoever is asking, so a laid-out rectangle can be
     * traced to the element that made it. ui_open() only reads the cursor, so
     * this costs nothing and changes no geometry -- which matters, because the
     * alternative (keying every block the way a listening one is keyed) would
     * make every div a hit target and break links and selection. */
    if (g_box_hook) {
        struct instance_handle bh = ui_open();
        g_box_hook(node, bh.index, bh.generation);
    }
    if (out_of_flow) ui_set_overlay(true);
    /* Any non-static box is a containing block for its positioned descendants
     * -- which is the entire reason a page writes `position: relative` on a
     * wrapper that has no offsets of its own. */
    if (s->position != VP_STATIC) ui_set_pos_container(1);
    if (s->clip) ui_set_clip_children(true);
    if (s->position != VP_STATIC && (s->ins_set || out_of_flow))
        ui_set_insets((float)s->ins_top, (float)s->ins_right,
                      (float)s->ins_bottom, (float)s->ins_left,
                      s->ins_set, s->position == VP_RELATIVE);
    size_box(s, was_item && !out_of_flow);
    {
        if (li_mark) {
            /* [marker][content] as a ROW, so wrapped text hangs under itself
             * instead of sliding back under the bullet. */
            HStack(.spacing = 8, .align = Leading, .grow = 1) {
                Text(li_mark).caption().tertiary();
                VStack(.spacing = 2, .align = Fill, .grow = 1) {
                    render_children(d, node, s, href);
                }
            }
        }
        else if (s->pre) render_pre(d, node, s);
        else             render_children(d, node, s, href);
    }
    em_end_();
}

const char *vellum_render(struct html_doc *d, int root) {
    return vellum_render_styled(d, root, 0);
}

const char *vellum_render_styled(struct html_doc *d, int root,
                                 const struct css_sheet *sheet) {
    return vellum_render_page(d, root, sheet, 0);
}

const char *vellum_render_page(struct html_doc *d, int root,
                               const struct css_sheet *sheet, const char *base) {
    return vellum_render_sized(d, root, sheet, base, 0.0f);
}

const char *vellum_render_sized(struct html_doc *d, int root,
                                const struct css_sheet *sheet, const char *base,
                                float content_w) {
    g_base = base;
    g_content_w = content_w;
    g_focused_field = -1;
    g_pending = 0;
    g_hover_href = 0;
    g_sheet = sheet;
    /* NOT cleared here. The memo survives across frames on purpose -- see
     * style.h -- and is dropped by whoever changes the document or the sheet. */
    struct vstyle rs;
    vstyle_root(&rs);
    /* Open the brackets around the DOCUMENT only -- the app emits its chrome
     * outside this call and keeps the desktop's size and colours.
     *
     * The controls need one for the same reason the text does: a page is drawn
     * on its own white canvas, so a text field taking the theme's dark surface
     * and the desktop's accent was the browser's chrome leaking into the page.
     * A form on a document should look like part of the document. */
    { struct ui_ctl_palette page = {
          .surface     = argb(PAGE_CANVAS),
          .border      = argb(0xFF767676U),   /* the web's own control grey */
          .focus       = argb(PAGE_LINK),
          .text        = argb(PAGE_INK),
          .placeholder = argb(PAGE_QUIET) };
      ui_set_control_palette(&page); }
    em_set_text_scale(g_zoom);
    if (root >= 0) render_block(d, root, &rs, 0, 0);
    em_set_text_scale(1.0f);
    ui_set_control_palette(0);
    g_sheet = 0;
    return g_pending;
}

void vellum_set_link_handler(void (*fn)(const char *href)) { g_on_link = fn; }

void vellum_set_submit_handler(void (*fn)(int node)) { g_on_submit = fn; }

void vellum_set_event_hooks(int (*has)(int node), void (*click)(int node)) {
    g_has_listener = has;
    g_on_click = click;
}

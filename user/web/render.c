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
#include <stdio.h>

#include "ui.h"
#include "em.h"
#include "theme.h"
#include "html.h"
#include "style.h"
#include "url.h"
#include "css.h"
#include "imgcache.h"
#include "form.h"
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
    if (s->link) return t->accent;
    if (s->mono) return t->text_secondary;   /* code reads as a quieter voice */
    return t->text;
}

/* One word of an inline run. A link's words are BUTTONS so each is clickable
 * on its own -- which is what keeps a link that wraps across a line break
 * clickable on both halves. */
static void emit_word(const char *w, const struct vstyle *s, const char *href) {
    if (href && g_on_link) {
        if (Button(w).ghost().font(font_for(s)).py(0).px(2)
                .color(color_for(s)).id(w).clicked()) {
            g_pending = href;
        }
        return;
    }
    Text(w).font(font_for(s)).color(color_for(s));
}

/* Emit a text run word by word into the surrounding Flow. */
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
            static char pool[512][68];
            static int  pn;
            if (pn >= 512) pn = 0;
            /* trailing space unless the run ends here: the space is part of
             * the word box, so a following comma sits flush against it */
            snprintf(pool[pn], sizeof pool[0], "%s%s%s",
                     (first && lead) ? " " : "", word, (*p || trail) ? " " : "");
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
    return s.display == VD_INLINE || s.display == VD_IMAGE ||
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
static int breaks_inline(unsigned char display) {
    return display == VD_BLOCK || display == VD_LIST_ITEM || display == VD_TABLE ||
           display == VD_ROW   || display == VD_CELL      || display == VD_CAPTION ||
           display == VD_FLEX  || display == VD_GRID;
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
        if (d->nodes[c].text) emit_text(d->nodes[c].text, st, href);
        return;
    }
    struct vstyle s;
    vstyle_for_node(d, c, st, g_sheet, &s);
    if (s.display == VD_NONE) return;
    /* AFTER computing its own style, not before: an image is styled by the
     * rules that match IT (`.half { width: 150px }`), and handing it the
     * parent's vstyle silently ignored every one of them. */
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

/* The page's zoom. Lengths the AUTHOR stated -- a 240px sidebar, 16px of
 * padding -- scale with the text, or a zoomed page is large type inside boxes
 * that did not grow. */
static float g_zoom = 1.0f;
void vellum_set_zoom(float z) { g_zoom = (z > 0.2f && z < 6.0f) ? z : 1.0f; }
float vellum_zoom(void) { return g_zoom; }
static float zpx(short v) { return (float)v * g_zoom; }

/* justify-content / align-items in the toolkit's spelling. */
static EmAlign em_of(unsigned char vj, EmAlign dflt) {
    switch (vj) {
        case VJ_CENTER:  return Center;
        case VJ_END:     return Trailing;
        case VJ_BETWEEN: return SpaceBetween;
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
static int g_flex_item;

static void open_box(const struct vstyle *s, EmProps bp) {
    if (s->gap > 0) bp.spacing = (float)s->gap * g_zoom;
    if (s->display == VD_FLEX) {
        bp.justify = em_of(s->justify, Leading);
        /* align-items DEFAULTS to stretch in CSS, which is Fill here -- and is
         * also what a block container wants, so the default below is shared. */
        bp.align = em_of(s->align_items, Fill);
        if (s->flex_col)       { em_vstack_(bp); return; }
        if (s->flex_wrap)      { em_flow_(bp);   return; }
        em_hstack_(bp);
        return;
    }
    if (s->display == VD_GRID) {
        em_grid_(s->grid_cols > 0 ? s->grid_cols : 1, bp);
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
    struct layout_size w, h;
    float grow_w = s->border_box ? 0.0f : box_inset(s);
    float grow_h = s->border_box ? 0.0f
                 : (float)(s->pad_top + s->pad_bottom) + 2.0f * (float)s->border_width;
    if (s->width_pct)      w = (struct layout_size){ .mode = SIZE_PERCENT,
                                                     .fixed_value = (float)s->width_pct / 100.0f,
                                                     .pct_px = (float)s->width };
    else if (s->width > 0) w = (struct layout_size){ .mode = SIZE_FIXED,
                                                     .fixed_value = (float)s->width + grow_w };
    else if (s->grow)      w = (struct layout_size){ .mode = SIZE_FLEX,  .flex_grow = 1 };
    else if (flex_item)    w = (struct layout_size){ .mode = SIZE_INTRINSIC };
    else                   w = (struct layout_size){ .mode = SIZE_FLEX,  .flex_grow = 1 };
    if (s->height_pct)     h = (struct layout_size){ .mode = SIZE_PERCENT,
                                                     .fixed_value = (float)s->height_pct / 100.0f,
                                                     .pct_px = (float)s->height };
    else if (s->height > 0) h = (struct layout_size){ .mode = SIZE_FIXED,
                                                      .fixed_value = (float)s->height + grow_h };
    else                   h = (struct layout_size){ .mode = SIZE_INTRINSIC };
    ui_set_size(w, h);
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
    Checkbox(label, on);
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
        if (!label[0]) snprintf(label, sizeof label, "Submit");
    }
    (void)st;
    HStack(.py = 2) {
        if (Button(label).primary().id(label).clicked()) {
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
    static int rows[TBL_MAX_ROWS];
    int nrow = table_rows(d, node, rows, TBL_MAX_ROWS, st);
    if (!nrow) return 0;

    /* The column count is the WIDEST row: a row with fewer cells leaves the
     * tail empty rather than shifting the ones after it into the wrong
     * column, which is what makes a ragged table still readable. */
    int ncol = 1;
    for (int i = 0; i < nrow; i++) {
        static int cells[TBL_MAX_COLS];
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
            static int cells[TBL_MAX_COLS];
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
                if (cs.bold) {
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
static void render_pre(struct html_doc *d, int node, const struct vstyle *s) {
    for (int c = d->nodes[node].first_child; c >= 0; c = d->nodes[c].next_sibling) {
        if (d->nodes[c].kind != HTML_TEXT || !d->nodes[c].text) continue;
        static char pool[128][200];
        static int pn;
        const char *p = d->nodes[c].text;
        while (*p) {
            size_t n = 0;
            char line[200];
            while (*p && *p != '\n' && n + 1 < sizeof line) line[n++] = *p++;
            line[n] = 0;
            if (*p == '\n') p++;
            if (pn >= 128) pn = 0;
            snprintf(pool[pn], sizeof pool[0], "%s", line[0] ? line : " ");
            Text(pool[pn]).font(Caption).color(ui_theme()->text_secondary);
            pn++;
        }
    }
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
    int lf[8], rf[8], nl = 0, nr = 0;
    /* the run of consecutive floated siblings that opens the group */
    while (c >= 0 && c != to) {
        struct vstyle cs;
        if (!child_style(d, c, s, &cs)) {
            if (is_blank_text(d, c)) { c = d->nodes[c].next_sibling; continue; }
            break;
        }
        if (!cs.floatp) break;
        if (cs.display != VD_NONE) {
            if (cs.floatp == VF_RIGHT) { if (nr < 8) rf[nr++] = c; }
            else                       { if (nl < 8) lf[nl++] = c; }
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

    HStack(.spacing = 10, .align = Leading, .grow = 1) {
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
    return rest_to;
}

static void render_range(struct html_doc *d, int from, int to,
                         const struct vstyle *s, const char *href, int *li) {
    int c = from;
    int flexish = (s->display == VD_FLEX || s->display == VD_GRID);
    while (c >= 0 && c != to) {
        if (flexish && is_blank_text(d, c)) { c = d->nodes[c].next_sibling; continue; }
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
    int outer_item = g_flex_item;
    g_flex_item = flexish;
    render_range(d, d->nodes[node].first_child, -1, s, href, &li);
    g_flex_item = outer_item;
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
    if (s->display == VD_LIST_ITEM) {
        /* [marker][content] as a ROW, so wrapped text hangs under itself
         * instead of sliding back under the bullet */
        HStack(.spacing = 8, .align = Leading, .grow = 1,
               .pb = (float)s->margin_bottom) {
            Text("\xE2\x80\xA2").caption().tertiary();
            VStack(.spacing = 2, .align = Fill, .grow = 1) {
                render_children(d, node, s, href);
            }
        }
        (void)list_index;
        return;
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
    if (out_of_flow) ui_set_overlay(true);
    if (s->clip) ui_set_clip_children(true);
    if (s->position != VP_STATIC && (s->ins_set || out_of_flow))
        ui_set_insets((float)s->ins_top, (float)s->ins_right,
                      (float)s->ins_bottom, (float)s->ins_left,
                      s->ins_set, s->position == VP_RELATIVE);
    size_box(s, was_item && !out_of_flow);
    {
        if (s->pre) render_pre(d, node, s);
        else        render_children(d, node, s, href);
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
    /* One pass, one set of computed styles. See vstyle_cache_reset. */
    vstyle_cache_reset();
    struct vstyle rs;
    vstyle_root(&rs);
    /* Open the zoom bracket around the DOCUMENT only. The chrome is emitted by
     * the app outside this call and keeps its own size. */
    em_set_text_scale(g_zoom);
    if (root >= 0) render_block(d, root, &rs, 0, 0);
    em_set_text_scale(1.0f);
    g_sheet = 0;
    return g_pending;
}

void vellum_set_link_handler(void (*fn)(const char *href)) { g_on_link = fn; }

void vellum_set_submit_handler(void (*fn)(int node)) { g_on_submit = fn; }

void vellum_set_event_hooks(int (*has)(int node), void (*click)(int node)) {
    g_has_listener = has;
    g_on_click = click;
}

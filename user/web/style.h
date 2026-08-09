/* user/web/style.h -- computed styles: the seam CSS will arrive through.
 *
 * Everything downstream of this file reads ONLY `struct vstyle`. The renderer
 * never asks what tag a node is, never matches a selector, never looks at an
 * attribute. That is the whole point: when CSS lands (docs/BROWSER.md §4) it
 * changes how this struct gets FILLED and nothing else -- not the parser, not
 * the renderer, not layout.
 *
 * v1 fills it from a user-agent stylesheet: a table of sensible defaults per
 * tag, which is what every browser did before CSS existed and what every
 * browser still falls back to. About a hundred lines, and it is the difference
 * between a document you can read and a wall of undifferentiated text.
 */
#ifndef _EMBLINK_WEB_STYLE_H_
#define _EMBLINK_WEB_STYLE_H_

enum {
    VD_INLINE = 0, VD_BLOCK, VD_LIST_ITEM, VD_NONE,
    /* A BOX that flows with the text. `display: inline-block` was folded into
     * VD_INLINE, which threw the box away with it: its padding, width, height
     * and background all stopped existing, and a row of nav links written as
     * inline-blocks welded into one word -- google.com's "GmailImages". It is
     * neither of the two it was being made into: a block would take the whole
     * line, an inline has no box at all. */
    VD_INLINE_BLOCK,
    /* A REPLACED element: its content is not markup but a picture. Named in
     * the display enum rather than sniffed by tag in the renderer, because
     * render.c's rule is that it reads this struct and never a tag -- and the
     * first exception (an strcmp for "img") was already one too many. */
    VD_IMAGE,
    /* Table structure. CSS models these as display values too (table,
     * table-row, table-cell), which is exactly why they belong here: the
     * renderer asks what KIND of box this is, not what it was called. */
    VD_TABLE, VD_ROW, VD_CELL, VD_CAPTION,
    /* Form controls. The first part of the web that is not read-only, and
     * therefore the first whose box the RENDERER cannot derive from the
     * document alone -- see form.h. */
    VD_FIELD, VD_BUTTON,
    /* The controls that are not a text box: a checkbox or radio (a BOOLEAN,
     * so it needs different storage and a different box) and a select (a
     * choice among its own <option> children). */
    VD_CHECK, VD_RADIO, VD_SELECT,
    /* Flex and grid containers. The layout engine has done both since it was
     * written -- the whole toolkit is built on them -- so these are the CSS
     * spelling of machinery that already exists, not new layout. */
    VD_FLEX, VD_GRID,
};
/* justify-content / align-items, in the small set that decides real layouts. */
enum { VJ_START = 0, VJ_CENTER, VJ_END, VJ_BETWEEN, VJ_STRETCH,
       /* AROUND and EVENLY used to fall back to BETWEEN, which pins the first
        * and last items to the container's edges -- the one thing neither
        * keyword means. The layout engine distinguishes them now, so this can
        * too. */
       VJ_AROUND, VJ_EVENLY,
       /* align-self only: "no override, use the container's align-items".
        * Must not be 0, because 0 is a real value (flex-start). */
       VJ_AUTO = 15 };
/* position. STATIC is the default and means "wherever the flow puts it".
 * FIXED is treated as ABSOLUTE against the nearest positioned ancestor rather
 * than the viewport -- an honest approximation, and the difference only shows
 * when the page scrolls under a fixed header. Named in TODO. */
enum { VP_STATIC = 0, VP_RELATIVE, VP_ABSOLUTE, VP_FIXED };
/* float / clear. */
enum { VF_NONE = 0, VF_LEFT, VF_RIGHT };
/* list-style-type. 0 is UNSET, not `none`: the two must be distinguishable or
 * `ul { list-style: none }` reads as "nobody said", and the item's own default
 * puts the bullet straight back. */
enum { VM_UNSET = 0, VM_BULLET, VM_DECIMAL, VM_NONE };
/* text-align. Justify is accepted and treated as left: a justified paragraph
 * needs inter-word stretching the line breaker does not do, and silently
 * left-aligning is what every browser did before it could justify. */
enum { VA_LEFT = 0, VA_CENTER, VA_RIGHT };

/* Mirrors layout.h's LT_* -- kept separate so style.h does not depend on the
 * layout engine, and converted where the two meet in render.c. */
enum { VT_AUTO = 0, VT_PX, VT_FR };
#define VSTYLE_TRACKS 8
#define VSTYLE_AREAS  8

struct vstyle {
    unsigned char display;
    /* GRID TRACKS the author stated. Sizes, not just a count: see layout.h for
     * why a page layout needs them and a table does not. grid_ntrack == 0
     * means "no template" and the tracks are sized from their content. */
    unsigned char grid_track_mode[VSTYLE_TRACKS];   /* VT_* */
    unsigned char grid_ntrack;
    /* NAMED AREAS. A container's `grid-template-areas` is reduced here to one
     * rectangle per distinct name -- which is all a placer needs, and far less
     * than the row-by-row map it was written as. A child's `grid-area` is the
     * name it claims, hashed: comparing 16 bits beats carrying a string
     * through every copy of this struct, and a collision between two of the
     * eight names a container may have would misplace a box rather than
     * corrupt anything. */
    unsigned short area_name[VSTYLE_AREAS];                  /* 0 = unused */
    unsigned char  area_r[VSTYLE_AREAS],  area_c[VSTYLE_AREAS];
    unsigned char  area_rs[VSTYLE_AREAS], area_cs[VSTYLE_AREAS];
    unsigned char  n_areas;
    unsigned short grid_area;    /* the CHILD's claim, 0 = none */
    short         grid_track_val[VSTYLE_TRACKS];    /* px, or fr weight x16 */
    unsigned char size;        /* 0 body, 1 caption, 2 title, 3 heading */
    unsigned char bold, italic, mono, underline, link, pre;
    unsigned char marker;      /* VM_* -- how a list item is bulleted   */
    short margin_top, margin_bottom, indent;
    /* An AUTHOR colour, 0xAARRGGBB, or 0 for "the theme decides". Zero is not
     * a colour here on purpose: a document that sets no colour must follow the
     * OS theme (a light page in a dark desktop is the author's choice, not an
     * accident), so the renderer only overrides its palette when this is set.
     * Inherited like every other text property. */
    unsigned int color;
    /* Was that colour set ON THIS ELEMENT, or inherited from an ancestor?
     *
     * The distinction decides whether a link is still blue. `body { color:
     * #333 }` inherits into every <a>, and if an inherited colour outranks the
     * user-agent's link colour then a page that sets a body colour -- which is
     * most pages -- loses every link it has. In CSS the UA rule for `a` beats
     * inheritance, and only a rule that names the link itself beats the UA. */
    unsigned char color_own;
    /* A stated box size in px, 0 = auto. Only images use it today, and only
     * because a picture is the one thing whose natural size arrives LATER
     * than the layout that has to hold it. */
    short width, height;   /* px -- or the +px term of a percentage, see below */
    /* ...stated as a PERCENTAGE instead, 0-100. A percentage is relative to
     * the containing block, whose size nobody knows while the stylesheet is
     * being read -- so it travels as a percentage and layout resolves it.
     *
     * When a percentage IS set, `width`/`height` above stop meaning "pixels"
     * and start meaning "pixels ADDED to the percentage" -- which is what a
     * calc() reduces to: `calc(100% - 240px)` is width_pct 100, width -240.
     * The overload is deliberate (two fields, not four) and is the reason this
     * comment exists; read one without the other and you get a sidebar layout
     * that is exactly one sidebar too wide. */
    unsigned char width_pct, height_pct;
    /* max-width, in px. ITS OWN FIELD, because it was written into `width`
     * above and that channel was already taken: `.w-100 { width: 100% }`
     * followed by `.mw9-l { max-width: 96rem }` left width_pct 100 AND width
     * 1536, which the calc() overload reads as "100% PLUS 1536px". rust-lang's
     * header came out 2592px wide inside a 1056px page, and everything under
     * it inherited the mistake. A cap and a size are different statements.
     * min-width has no field yet -- see docs/TODO.md. */
    short         max_width;
    /* box-sizing: border-box. Nearly every modern stylesheet sets this
     * globally, and without it a box with padding and a stated width is wider
     * than the author asked for -- which on a grid of cards is the difference
     * between three across and two. */
    unsigned char border_box;

    /* --- the BOX. Not inherited (CSS says so, and a page that painted every
     * descendant with its parent's background would be unreadable). 0 alpha
     * means "not set", the same convention `color` already uses. --------- */
    unsigned int bg;             /* background-color, 0xAARRGGBB          */
    unsigned int border_color;
    short        border_width;   /* px; 0 = no border                     */
    short        radius;         /* border-radius in px                   */
    short        pad_top, pad_right, pad_bottom, pad_left;

    /* --- flex / grid. Box properties, so not inherited. ------------------ */
    unsigned char justify;       /* justify-content, VJ_*                  */
    unsigned char align_items;   /* align-items, VJ_* (STRETCH = fill)     */
    unsigned char flex_col;      /* flex-direction: column                 */
    unsigned char flex_wrap;     /* flex-wrap: wrap                        */
    unsigned char grid_cols;     /* display:grid track count, 0 = not one  */
    short         gap;           /* row-gap: BETWEEN LINES of a wrapping row */
    short         col_gap;       /* column-gap: between items. -1 = same as gap */
    unsigned char align_self;    /* VJ_* on THIS element; VJ_AUTO = inherit  */
    /* --- the three flex-item properties, on THIS element. Read where the
     * child is emitted rather than by its parent, which is how the toolkit
     * spells it.
     *
     * `grow` was a FLAG, so `flex: 2` and `flex: 1` produced identical boxes
     * and every two-to-one split on the web came out fifty-fifty. It is the
     * actual weight now. `basis` is the size the item starts at before any of
     * the leftover is handed out -- `flex: 0 0 240px` is a 240px sidebar and
     * was previously a box sized by its text. */
    unsigned char grow;          /* flex-grow weight, 0 = does not grow      */
    unsigned char shrink;        /* flex-shrink weight; 1 is CSS's default   */
    short         basis;         /* flex-basis in px; -1 = auto (not stated) */
    short         order;         /* flex `order`; items sort by it, then DOM */

    /* --- position / overflow. Box properties; not inherited. ------------- */
    unsigned char position;      /* VP_*                                   */
    unsigned char floatp;        /* VF_* -- float: left / right            */
    unsigned char clearp;        /* bit 0 clears left, bit 1 clears right  */
    unsigned char clip;          /* overflow: hidden/auto/scroll           */
    /* top/right/bottom/left, in px, with a bit per edge saying it was stated
     * at all -- because 0 is a perfectly ordinary offset and "unset" has to be
     * distinguishable from it, or every absolute box pins itself to the top
     * left corner whether the author asked or not. */
    short         ins_top, ins_right, ins_bottom, ins_left;
    unsigned char ins_set;       /* bit 0 top, 1 right, 2 bottom, 3 left   */

    /* --- inherited text layout ------------------------------------------ */
    unsigned char align;         /* VA_*                                   */
    short         line_height;   /* px; 0 = the font's own leading         */
};

/* The document root's style: everything inherits from this. */
/* THE PAGE'S INITIAL VALUES -- a browser's, not the desktop's.
 *
 * A web page is written against a white canvas with dark text. That is not a
 * preference, it is the initial value every page on the web assumes, and it is
 * why a page can set a background and say nothing about colour. Rendering that
 * page on the OS's dark surface with the OS's light text produced Wikipedia as
 * pale grey headings on the white background the page DID set: our foreground
 * over their background, readable in neither direction.
 *
 * So the document keeps its own palette and the browser's chrome keeps the
 * desktop's -- which is exactly what a desktop browser does in dark mode, and
 * the reason a page looks the same there as anywhere else. A page that states
 * its own colours overrides all of this, as it always could. */
#define PAGE_INK    0xFF1B1B1BU     /* text */
#define PAGE_CANVAS 0xFFFFFFFFU     /* the paper under it */
#define PAGE_LINK   0xFF1A0DABU     /* the web's blue */
#define PAGE_QUIET  0xFF545454U     /* code, and anything meant to recede */

void vstyle_root(struct vstyle *out);

/* Compute an ELEMENT's style: the user-agent stylesheet, then the author's
 * stylesheet (cascade), then its inline style="" -- CSS's origin order, each
 * stage overriding the last. `sheet` and `doc`/`node` may be absent (NULL/-1),
 * which is exactly the pre-CSS behaviour.  */
struct css_sheet; struct html_doc;
/* Drop the per-pass style memo. The renderer calls this once at the top of
 * each pass; nothing may survive a frame, which is what lets a new stylesheet
 * take effect and what makes the memo safe. See style.c. */
void vstyle_cache_reset(void);
/* Drop it because the DOCUMENT or the SHEET changed -- a new page, a
 * stylesheet arriving, a script mutating the DOM. The renderer no longer
 * clears it per frame: a frame that only scrolls changes nobody's style, and
 * recomputing it all was the single biggest cost on a large page. */
void vstyle_cache_invalidate(void);

void vstyle_for_node(struct html_doc *doc, int node, const struct vstyle *parent,
                     const struct css_sheet *sheet, struct vstyle *out);

/* Compute `tag`'s style given its parent's. Inheritable properties (size,
 * weight, italic, mono, colour-ish flags) descend; box properties (display,
 * margins, indent, marker) do not -- that split is CSS's inheritance model and
 * getting it wrong makes a <b> inside a heading reset to body text. */
void vstyle_for(const char *tag, const struct vstyle *parent, struct vstyle *out);

#endif /* _EMBLINK_WEB_STYLE_H_ */

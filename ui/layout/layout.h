#ifndef __EMBLINK_UI_LAYOUT_H__
#define __EMBLINK_UI_LAYOUT_H__

/* ui/layout/layout.h -- EmbLink UI Piece 5: the layout engine.
 *
 * Flexbox-style top-down proposed-size / bottom-up actual-size negotiation --
 * the model SwiftUI itself is under its declarative syntax, not a constraint
 * solver. Pure userland C, no kernel. Consumes Piece 4b font metrics for text
 * measurement; writes resolved geometry into Piece 3's scene tree via its
 * existing scene_set_size / scene_set_transform mutation API.
 *
 * A SEPARATE tree from the scene tree, 1:1 paired: each layout node references
 * exactly one scene node it produces output for. Same paged-arena +
 * {index,generation} ABA-safe handle discipline as Piece 3, reused.
 *
 * This piece is a PURE, fully-re-runnable function (input tree -> output
 * geometry). Deciding WHEN to re-run (dirty-subtree tracking) is Piece 6's job
 * -- the same boundary Piece 3 drew against Piece 4a's dirty-rect logic. */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "scene.h"   /* struct node_handle, struct scene_arena */
#include "font.h"    /* text measurement */

struct layout_handle { uint32_t index, generation; };
static const struct layout_handle LAYOUT_HANDLE_NULL = { 0, 0 };
static inline bool layout_handle_is_null(struct layout_handle h) { return h.index == 0; }

enum layout_axis    { AXIS_ROW, AXIS_COLUMN };
enum layout_justify { JUSTIFY_START, JUSTIFY_CENTER, JUSTIFY_END, JUSTIFY_SPACE_BETWEEN,
                      /* SPACE_AROUND gives every item an equal share of the
                       * leftover, half of it on each side, so the end gaps are
                       * half the middle ones. SPACE_EVENLY makes all n+1 gaps
                       * equal instead. They differ only at the two ends, and
                       * that difference is the whole reason an author picks
                       * one -- collapsing both onto SPACE_BETWEEN, as this did,
                       * pins the first and last items to the edges, which is
                       * the one thing neither keyword means. */
                      JUSTIFY_SPACE_AROUND, JUSTIFY_SPACE_EVENLY };
enum layout_align   { ALIGN_START, ALIGN_CENTER, ALIGN_END, ALIGN_STRETCH };
/* SIZE_PERCENT keeps the FRACTION in fixed_value (0.5 == 50%), resolved
 * against the containing block's content size on that axis -- which is a
 * number nobody knows until arrange, and is exactly why a percentage cannot
 * be turned into pixels when the stylesheet is read. Anywhere this mode is
 * not handled it falls through to intrinsic, which is a sane degradation
 * rather than a wrong number. */
enum size_mode      { SIZE_FIXED, SIZE_INTRINSIC, SIZE_FLEX, SIZE_PERCENT };

struct layout_size {
    enum size_mode mode;
    float fixed_value;    /* SIZE_FIXED: px. SIZE_PERCENT: the fraction. */
    float flex_grow;      /* weight for POSITIVE remaining space */
    float flex_shrink;    /* weight for NEGATIVE remaining space -- ORTHOGONAL to mode */
    float min_size;        /* floor a shrinking node won't cross; default 0 */
    float max_size;        /* ceiling a growing node won't cross; 0 = no cap */
    /* LAST, on purpose. This struct is built with positional initialisers in
     * several places, so a field added in the middle silently shifts every one
     * of them -- flex_grow became pct_px and the whole flex algorithm went
     * quiet-wrong. New fields go here, and the initialisers below were made
     * designated so the next one cannot do this again. */
    float pct_px;         /* SIZE_PERCENT only: pixels ADDED to the fraction,
                           * which is what calc(100% - 240px) reduces to. */
};

/* A grid track's sizing. AUTO is "as much as the content wants", which is the
 * only mode there was before an author could state one. */
enum { LT_AUTO = 0, LT_PX, LT_FR };
#define LAYOUT_TRACKS 12

struct layout_node {
    struct layout_handle self;
    struct layout_handle parent;
    struct layout_handle first_child, next_sibling;   /* intrusive list (also free-list link) */

    struct node_handle scene_node;   /* the paired Piece-3 node this outputs to */

    bool is_container;                /* true: arranges children; false: leaf */
    enum layout_axis    axis;
    enum layout_justify justify;
    enum layout_align   align;

    float padding_top, padding_right, padding_bottom, padding_left;
    float spacing;                     /* main-axis gap between children */
    bool  wrap;                        /* flex-wrap: overflowing children flow onto
                                        * new lines stacked on the cross axis */
    /* THE CROSS-AXIS GAP, when it differs from the main one. CSS states two:
     * `gap: 12px 20px` is row-gap then column-gap, and for a wrapping row the
     * first separates LINES and the second separates ITEMS. One `spacing` had
     * to be both, so a card grid with a large row gap got it between its cards
     * as well. 0 means "same as spacing", which is what a single-value gap
     * means and keeps every existing caller unchanged. */
    float cross_spacing;
    /* align-self: this child's own cross alignment, overriding its container's.
     * Encoded as align+1 because ALIGN_START is 0 and a zeroed node has to mean
     * "no override" -- otherwise every child ever created would silently claim
     * it had been told to align to the start. Read it through child_align(). */
    unsigned char align_self;
    int   grid_cols;                   /* >0 => 2D grid: N columns, auto-flow */
    /* EXPLICIT TRACK SIZES, when the author stated them
     * (`grid-template-columns: 12.25rem minmax(0,1fr)`). Without these every
     * track was sized from its content, which is right for a table and wrong
     * for a page layout: a 196px sidebar next to a 1fr article came out as two
     * columns sharing the width by how much text each held. Empty (n == 0)
     * keeps the content-sized behaviour, which is what a <table> wants. */
    unsigned char grid_track_mode[LAYOUT_TRACKS];   /* LT_* */
    float         grid_track_val[LAYOUT_TRACKS];    /* px, or fr weight */
    int           grid_ntrack;
    float grid_col_gap, grid_row_gap;  /* grid track gaps */
    int   grid_span;                   /* a grid CHILD spans this many columns (default 1) */
    /* EXPLICIT PLACEMENT for a grid child, from `grid-area`. -1 = auto-flow,
     * which is what everything did before named areas existed. Row and column
     * are zero-based; grid_span is the column span, so only the row span needs
     * its own field. */
    int   grid_row, grid_col, grid_rowspan;
    float scroll_offset;               /* column scroll: children shift up by this many px */
    float offset_x, offset_y;          /* post-layout translate ADDED to the resolved
                                        * position (CSS `transform: translate`-style) --
                                        * for transitions/slides; doesn't affect flow */
    bool  is_overlay;                  /* fill the parent, excluded from flow (modals/popovers) */
    /* Offsets for a POSITIONED box. For an overlay these are CSS's
     * top/right/bottom/left against the containing block; for an in-flow box
     * they are `position: relative`, added after the flow has placed it so the
     * siblings never notice. `ins_set` has a bit per edge because 0 is an
     * ordinary offset and must be distinguishable from unset. */
    float ins_top, ins_right, ins_bottom, ins_left;
    unsigned char ins_set;             /* bit 0 top, 1 right, 2 bottom, 3 left */
    unsigned char relative;            /* offsets apply, but stay in flow      */
    /* This box ESTABLISHES a containing block for positioned descendants --
     * anything whose `position` is not static. An absolutely positioned box is
     * placed against the nearest such ancestor, not against whatever happens
     * to contain it. */
    unsigned char pos_container;

    struct layout_size width, height;  /* own sizing, per axis */

    /* --- computed scratch (not authored) --- */
    float intrinsic_w, intrinsic_h;                        /* Phase 1 */
    float resolved_x, resolved_y, resolved_w, resolved_h;  /* Phase 2, PARENT-relative px */

    /* --- text-measurement memo (computed scratch) ---
     * Measuring text is the layout engine's real per-frame cost: one-line
     * width walks every glyph through the font engine, and wrapped height
     * SIMULATES the whole wrap. Both are pure functions of (content, font,
     * size[, width]) -- and the scene node already carries a content hash
     * (added for dirty tracking), so the answers can be remembered on the
     * node. A 500-word document then re-measures NOTHING while scrolling: the
     * content, font and width are all unchanged frame to frame.
     * meas_hash 0 = no memo (also the state of a node whose text was written
     * without scene_set_text, which never memoizes and stays correct). */
    uint32_t meas_hash;      /* content hash the memo was computed against */
    uint32_t meas_font;      /* + font handle ... */
    float    meas_size;      /* + size_px: the full key for line width */
    float    meas_line_w;    /* cached one-line width */
    float    meas_wrap_w;    /* width the wrap height was computed at (<0 = none) */
    float    meas_wrap_h;    /* cached wrapped height at meas_wrap_w */

    bool dirty;   /* set by authoring mutation; consumed by Piece 6 */
};

/* ------------------------------------------------------------------------- */
/* arena / lifecycle (same shape as Piece 3's scene arena)                   */
/* ------------------------------------------------------------------------- */

#define LAYOUT_PAGE_SIZE 128
#define LAYOUT_MAX_PAGES 128

/* How many containers had children the layout scratch could not hold. Non-zero
 * means boxes were NOT positioned and are sitting at 0x0 on their parent's
 * origin -- a visibly broken page, so callers that can report it should. */
int  layout_children_dropped(void);
void layout_reset_children_dropped(void);

/* How many containers had children the layout scratch could not hold. Non-zero
 * means boxes were NOT positioned and are sitting at 0x0 on their parent's
 * origin -- a visibly broken page, so callers that can report it should. */
int  layout_children_dropped(void);
void layout_reset_children_dropped(void);

struct layout_arena {
    struct layout_node *pages[LAYOUT_MAX_PAGES];
    uint32_t            n_pages_allocated;
    uint32_t            free_list_head;
    uint32_t            next_never_used;
};

void layout_arena_init(struct layout_arena *a);
void layout_arena_destroy(struct layout_arena *a);

/* Create a leaf (is_container=false) as the last child of `parent`
 * (LAYOUT_HANDLE_NULL => root). Zero-init: SIZE_INTRINSIC on both axes,
 * grow/shrink 0. Caller fills in fields + scene_node. */
struct layout_handle layout_create_node(struct layout_arena *a, struct layout_handle parent);
void layout_destroy_node(struct layout_arena *a, struct layout_handle h);
struct layout_node *layout_resolve(struct layout_arena *a, struct layout_handle h);

/* Move `h` under `new_parent`, inserted AFTER `after` (LAYOUT_HANDLE_NULL =>
 * first child). Used by Piece 7's keyed reconciliation to keep sibling order
 * matching declared order after a reorder. */
void layout_reparent(struct layout_arena *a, struct layout_handle h,
                     struct layout_handle new_parent, struct layout_handle after);

/* ------------------------------------------------------------------------- */
/* the layout computation                                                    */
/* ------------------------------------------------------------------------- */

/* Run Phase 1 (intrinsic sizes, bottom-up) then Phase 2 (arrange, top-down)
 * for `root` given its assigned size (W,H) -- e.g. a window's surface size or
 * the compositor's screen size. Writes every node's resolved geometry into its
 * paired scene node. `sa` is the scene arena (for text metrics + write-back). */
void layout_run(struct layout_arena *la, struct scene_arena *sa,
                struct layout_handle root, float W, float H);

/* Word-wrap (with single-overlong-word character-wrap fallback) height of a
 * TEXT-paired node at a proposed width, in pixels. Exposed for tests + for
 * arrange's cross-axis text resolution. */
float layout_measure_height_at_width(struct layout_arena *la, struct scene_arena *sa,
                                     struct layout_handle text_node, float proposed_width);

/* Test/diagnostic: line count word-wrap produces at a proposed width. */
int layout_debug_wrap_lines(struct layout_arena *la, struct scene_arena *sa,
                            struct layout_handle text_node, float proposed_width);

#endif /* __EMBLINK_UI_LAYOUT_H__ */

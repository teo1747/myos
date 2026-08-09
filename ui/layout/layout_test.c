/* ui/layout/layout_test.c -- EmbLink UI Piece 5 selftests (Section 5).
 *
 * Pure userland. Builds a compact synthetic font (empty glyphs, real advances
 * via hmtx + real hhea ascent/descent/line_gap) so text measurement has known
 * metrics without any glyph outlines. Layout nodes pair with real Piece-3
 * scene nodes so the write-back (T6) is exercised for real.
 *   make layout-test  -> exits 0 iff every T1..T6 invariant holds. */

#include "layout.h"
#include "scene.h"
#include "font.h"
#include <stdio.h>
#include <stdlib.h>

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  FAIL: %s\n", (msg)); g_fail++; } \
    else         { printf("  ok:   %s\n", (msg)); } \
} while (0)
static int feq(float a, float b) { float d = a-b; if (d<0) d=-d; return d < 0.05f; }

static struct scene_arena  SA;
static struct layout_arena LA;

/* pair a new layout node with a new scene node under the given parents */
static struct layout_handle mk(struct layout_handle lp, struct node_handle sp,
                               enum scene_node_kind kind, struct node_handle *out_s) {
    struct node_handle s = scene_create_node(&SA, kind, sp);
    struct layout_handle l = layout_create_node(&LA, lp);
    struct layout_node *ln = layout_resolve(&LA, l);
    ln->scene_node = s;
    if (out_s) *out_s = s;
    return l;
}
static struct layout_node *L(struct layout_handle h) { return layout_resolve(&LA, h); }

/* ---- synthetic font: all-empty glyphs, advance 500, hhea 800/-200/0 ---- */
static uint8_t g_ttf[8192]; static uint32_t g_len;
static void w8(uint8_t v){ g_ttf[g_len++]=v; }
static void w16(uint16_t v){ w8(v>>8); w8(v); }
static void w32(uint32_t v){ w16(v>>16); w16(v); }
static void wi16(int16_t v){ w16((uint16_t)v); }
static void w16_at(uint32_t o,uint16_t v){ g_ttf[o]=v>>8; g_ttf[o+1]=v; }
static void w32_at(uint32_t o,uint32_t v){ w16_at(o,v>>16); w16_at(o+2,v); }

static uint32_t build_font(void) {
    g_len = 0;
    const int nt = 7;
    w32(0x00010000); w16(nt); w16(0); w16(0); w16(0);
    uint32_t dir = g_len;
    for (int i=0;i<nt;i++){ w32(0);w32(0);w32(0);w32(0); }
    struct { const char *tag; uint32_t off,len; } rec[7]; int ri=0;
    #define BEG(T) do{ rec[ri].tag=(T); rec[ri].off=g_len; }while(0)
    #define ENDT() do{ rec[ri].len=g_len-rec[ri].off; while(g_len&3){ w8(0); } ri++; }while(0)

    BEG("head"); { uint32_t s=g_len; for(int i=0;i<54;i++) w8(0);
        w32_at(s+0,0x00010000); w16_at(s+18,1000); w16_at(s+50,1); } ENDT();
    BEG("maxp"); w32(0x00010000); w16(128); ENDT();
    BEG("hhea"); { uint32_t s=g_len; for(int i=0;i<36;i++) w8(0);
        w16_at(s+4,800); w16_at(s+6,(uint16_t)(int16_t)-200); w16_at(s+8,0); w16_at(s+34,1); } ENDT();
    BEG("hmtx"); w16(500); w16(0); ENDT();                 /* 1 metric: advance 500 */
    BEG("cmap"); { uint32_t cs=g_len;
        w16(0); w16(1); w16(3); w16(1); w32(12);
        w16(4); uint32_t lo=g_len; w16(0); w16(0); w16(4); w16(4); w16(1); w16(0);
        w16(0x007e); w16(0xFFFF);           /* endCode */
        w16(0);                              /* pad */
        w16(0x0020); w16(0xFFFF);            /* startCode */
        wi16(0); w16(1);                     /* idDelta: glyph = codepoint */
        w16(0); w16(0);                      /* idRangeOffset */
        w16_at(lo,(uint16_t)(g_len-cs-12)); } ENDT();
    BEG("glyf"); ENDT();                                    /* empty: all glyphs blank */
    BEG("loca"); for(int i=0;i<129;i++) w32(0); ENDT();     /* every glyph empty */

    for (int i=0;i<nt;i++){ uint32_t r=dir+i*16;
        g_ttf[r]=rec[i].tag[0]; g_ttf[r+1]=rec[i].tag[1]; g_ttf[r+2]=rec[i].tag[2]; g_ttf[r+3]=rec[i].tag[3];
        w32_at(r+4,0); w32_at(r+8,rec[i].off); w32_at(r+12,rec[i].len); }
    return g_len;
}

/* ---- T1: SPACE_BETWEEN distribution ------------------------------------ */
static void t1_space_between(void) {
    printf("T1 fixed children, justify SPACE_BETWEEN:\n");
    scene_arena_init(&SA); layout_arena_init(&LA);
    struct layout_handle root = mk(LAYOUT_HANDLE_NULL, NODE_HANDLE_NULL, SCENE_NODE_GROUP, 0);
    L(root)->is_container = true; L(root)->axis = AXIS_ROW; L(root)->justify = JUSTIFY_SPACE_BETWEEN;
    struct layout_handle c1 = mk(root, NODE_HANDLE_NULL, SCENE_NODE_RECT, 0);
    struct layout_handle c2 = mk(root, NODE_HANDLE_NULL, SCENE_NODE_RECT, 0);
    struct layout_handle c3 = mk(root, NODE_HANDLE_NULL, SCENE_NODE_RECT, 0);
    L(c1)->width = (struct layout_size){ SIZE_FIXED, 10, 0,0,0 }; L(c1)->height = (struct layout_size){ SIZE_FIXED,10,0,0,0 };
    L(c2)->width = (struct layout_size){ SIZE_FIXED, 20, 0,0,0 }; L(c2)->height = (struct layout_size){ SIZE_FIXED,10,0,0,0 };
    L(c3)->width = (struct layout_size){ SIZE_FIXED, 30, 0,0,0 }; L(c3)->height = (struct layout_size){ SIZE_FIXED,10,0,0,0 };

    layout_run(&LA, &SA, root, 100, 50);
    CHECK(feq(L(c1)->resolved_x, 0),  "child 1 at x=0");
    CHECK(feq(L(c2)->resolved_x, 30), "child 2 at x=30 (10 + 20 gap)");
    CHECK(feq(L(c3)->resolved_x, 70), "child 3 at x=70");
    CHECK(feq(L(c3)->resolved_x + L(c3)->resolved_w, 100), "child 3 right edge at x=100");
    layout_arena_destroy(&LA); scene_arena_destroy(&SA);
}

/* ---- T2: flex_grow exact numeric --------------------------------------- */
static void t2_grow(void) {
    printf("T2 flex_grow distribution:\n");
    scene_arena_init(&SA); layout_arena_init(&LA);
    struct layout_handle root = mk(LAYOUT_HANDLE_NULL, NODE_HANDLE_NULL, SCENE_NODE_GROUP, 0);
    L(root)->is_container = true; L(root)->axis = AXIS_ROW;
    struct layout_handle a = mk(root, NODE_HANDLE_NULL, SCENE_NODE_RECT, 0);
    struct layout_handle b = mk(root, NODE_HANDLE_NULL, SCENE_NODE_RECT, 0);
    L(a)->width = (struct layout_size){ SIZE_FIXED, 20, 1,0,0 };
    L(b)->width = (struct layout_size){ SIZE_FIXED, 10, 2,0,0 };
    layout_run(&LA, &SA, root, 100, 50);
    /* remaining 70 split 1:2 over bases 20/10 */
    CHECK(feq(L(a)->resolved_w, 20 + 70.0f/3.0f), "A width = 20 + 70/3 (~43.33)");
    CHECK(feq(L(b)->resolved_w, 10 + 140.0f/3.0f), "B width = 10 + 70*2/3 (~56.67)");
    CHECK(feq(L(a)->resolved_w + L(b)->resolved_w, 100), "widths sum to exactly 100");
    layout_arena_destroy(&LA); scene_arena_destroy(&SA);
}

/* ---- T3: CSS-accurate size-weighted flex_shrink ------------------------ */
static void t3_shrink(void) {
    printf("T3 flex_shrink (size-weighted, not equal split):\n");
    scene_arena_init(&SA); layout_arena_init(&LA);
    struct layout_handle root = mk(LAYOUT_HANDLE_NULL, NODE_HANDLE_NULL, SCENE_NODE_GROUP, 0);
    L(root)->is_container = true; L(root)->axis = AXIS_ROW;
    struct layout_handle a = mk(root, NODE_HANDLE_NULL, SCENE_NODE_RECT, 0);
    struct layout_handle b = mk(root, NODE_HANDLE_NULL, SCENE_NODE_RECT, 0);
    L(a)->width = (struct layout_size){ SIZE_FIXED, 60, 0,1,0 };
    L(b)->width = (struct layout_size){ SIZE_FIXED, 40, 0,1,0 };
    layout_run(&LA, &SA, root, 80, 50);   /* overflow 20; weights 60 and 40 */
    CHECK(feq(L(a)->resolved_w, 48), "A shrinks by 20*60/100=12 -> 48 (NOT 50)");
    CHECK(feq(L(b)->resolved_w, 32), "B shrinks by 20*40/100=8  -> 32 (NOT 50)");
    layout_arena_destroy(&LA); scene_arena_destroy(&SA);
}

/* ---- T3b: the DEFAULT shrink -- flexible boxes give first, fixed never --- */
/* Nobody assigns flex_shrink, so before this the whole shrink branch was dead
 * and an over-wide row simply overflowed and got clipped ("resizing cuts off
 * the interface"). The default is now inferred from the size mode, and it is
 * ordered: FLEX absorbs the overflow, INTRINSIC only helps if that was not
 * enough, FIXED never gives. */
static void t3b_default_shrink(void) {
    printf("T3b default shrink (flex first, intrinsic second, fixed never):\n");
    scene_arena_init(&SA); layout_arena_init(&LA);
    struct layout_handle root = mk(LAYOUT_HANDLE_NULL, NODE_HANDLE_NULL, SCENE_NODE_GROUP, 0);
    L(root)->is_container = true; L(root)->axis = AXIS_ROW;

    /* Three 40-wide children whose MODES differ. The intrinsic and flex ones
     * get their 40 from a fixed child, the way a real button or a grow-y field
     * gets its basis from its content -- setting intrinsic_w by hand does not
     * work, the measure pass computes it. */
    struct layout_handle fx = mk(root, NODE_HANDLE_NULL, SCENE_NODE_RECT, 0);
    L(fx)->width = (struct layout_size){ SIZE_FIXED, 40, 0,0,0 };

    struct layout_handle it = mk(root, NODE_HANDLE_NULL, SCENE_NODE_GROUP, 0);
    L(it)->is_container = true; L(it)->axis = AXIS_ROW;
    L(it)->width = (struct layout_size){ SIZE_INTRINSIC, 0, 0,0,0 };
    struct layout_handle itc = mk(it, NODE_HANDLE_NULL, SCENE_NODE_RECT, 0);
    L(itc)->width = (struct layout_size){ SIZE_FIXED, 40, 0,0,0 };

    struct layout_handle fl = mk(root, NODE_HANDLE_NULL, SCENE_NODE_GROUP, 0);
    L(fl)->is_container = true; L(fl)->axis = AXIS_ROW;
    L(fl)->width = (struct layout_size){ SIZE_FLEX, 0, 1,0,0 };
    struct layout_handle flc = mk(fl, NODE_HANDLE_NULL, SCENE_NODE_RECT, 0);
    L(flc)->width = (struct layout_size){ SIZE_FIXED, 40, 0,0,0 };

    /* 120 of content into 100: a 20 deficit the FLEX child can absorb alone */
    layout_run(&LA, &SA, root, 100, 50);
    CHECK(feq(L(fx)->resolved_w, 40), "fixed keeps its 40");
    CHECK(feq(L(it)->resolved_w, 40), "intrinsic untouched -- flex covered it");
    CHECK(feq(L(fl)->resolved_w, 20), "flex absorbed the whole 20");

    /* 60: the flex child bottoms out and the intrinsic one must help */
    layout_run(&LA, &SA, root, 60, 50);
    CHECK(feq(L(fx)->resolved_w, 40), "fixed STILL keeps its 40");
    CHECK(L(fl)->resolved_w < 1.0f,   "flex gave everything it had");
    CHECK(feq(L(it)->resolved_w, 20), "intrinsic gave exactly the remainder");
    layout_arena_destroy(&LA); scene_arena_destroy(&SA);
}

/* ---- T3c: a wrap row NESTED in blocks (the browser's paragraph) --------- */
/* document -> html -> body -> p -> wrap-row-of-words. The measured height of
 * the outermost block must be ONE paragraph's wrapped height, not that height
 * multiplied by how deeply it happens to be nested. */
static void t3c_nested_wrap(uint32_t fh) {
    printf("T3c nested wrap height (browser paragraph):\n");
    scene_arena_init(&SA); layout_arena_init(&LA);

    struct layout_handle root = mk(LAYOUT_HANDLE_NULL, NODE_HANDLE_NULL, SCENE_NODE_GROUP, 0);
    L(root)->is_container = true; L(root)->axis = AXIS_COLUMN; L(root)->align = ALIGN_STRETCH;
    /* the ScrollView: a FIXED height far taller than the content, which is the
     * one thing the browser's chain has that this test did not */
    L(root)->height = (struct layout_size){ SIZE_FIXED, 400, 0,0,0 };

    /* four nested columns, as document/html/body/p are */
    struct layout_handle a = root, lev[3];
    for (int i = 0; i < 3; i++) {
        struct layout_handle b = mk(a, NODE_HANDLE_NULL, SCENE_NODE_GROUP, 0);
        L(b)->is_container = true; L(b)->axis = AXIS_COLUMN; L(b)->align = ALIGN_STRETCH;
        a = b; lev[i] = b;
    }
    /* Blocks carry CSS margins as padding, as render.c gives them. */
    L(lev[2])->padding_top = 4; L(lev[2])->padding_bottom = 12;

    /* The wrap row of WORDS: text nodes measured through the font, and one
     * link word with a button's own padding -- the three ways the fixed-box
     * version differed from what the browser actually emits. */
    struct layout_handle fl = mk(a, NODE_HANDLE_NULL, SCENE_NODE_GROUP, 0);
    L(fl)->is_container = true; L(fl)->axis = AXIS_ROW; L(fl)->wrap = true;
    L(fl)->align = ALIGN_START;
    struct color black = {0,0,0,1};
    for (int i = 0; i < 9; i++) {
        struct node_handle sw;
        struct layout_handle w = mk(fl, NODE_HANDLE_NULL, SCENE_NODE_TEXT, &sw);
        scene_set_text(&SA, sw, "aa ", fh, 20.0f, black);     /* 2 glyphs @20px */
    }
    /* the link word: a padded CONTAINER wrapping its text, as a ghost button is */
    struct layout_handle lb = mk(fl, NODE_HANDLE_NULL, SCENE_NODE_GROUP, 0);
    L(lb)->is_container = true; L(lb)->axis = AXIS_ROW;
    L(lb)->padding_left = 2; L(lb)->padding_right = 2;
    { struct node_handle sl;
      struct layout_handle lt = mk(lb, NODE_HANDLE_NULL, SCENE_NODE_TEXT, &sl);
      scene_set_text(&SA, sl, "aa ", fh, 20.0f, black); (void)lt; }

    /* a heading AFTER the paragraph -- the thing that was being overdrawn */
    struct layout_handle nxt = mk(L(lev[2])->parent.index ? lev[1] : lev[1],
                                  NODE_HANDLE_NULL, SCENE_NODE_GROUP, 0);
    L(nxt)->is_container = true; L(nxt)->axis = AXIS_COLUMN;
    { struct node_handle sh;
      struct layout_handle ht = mk(nxt, NODE_HANDLE_NULL, SCENE_NODE_TEXT, &sh);
      struct color b2 = {0,0,0,1};
      scene_set_text(&SA, sh, "heading", fh, 20.0f, b2); (void)ht; }

    layout_run(&LA, &SA, root, 100, 400);
    float wrap = L(fl)->resolved_h, blk = L(lev[2])->resolved_h;
    /* a SIBLING after the paragraph, to see where it actually lands */
    (void)0;
    int lines = layout_debug_wrap_lines(&LA, &SA, fl, 100.0f);
    printf("       wrap=%.1f  p=%.1f  body=%.1f  html=%.1f\n",
           wrap, blk, L(lev[1])->resolved_h, L(lev[0])->resolved_h);

    /* Each word is ~40px wide at 20px/glyph; 100px holds two per line, so ten
     * words make five lines of 20px = 100. The exact count matters less than
     * the two invariants below. */
    CHECK(wrap > 20.0f, "a wrapped row of TEXT is taller than one line");
    CHECK(blk >= wrap && blk <= wrap + 20.0f,
          "the block is its wrap row plus its own margins -- not a multiple");
    /* body now holds the paragraph AND the following heading, so it is taller
     * by exactly that heading -- still no accumulation of the wrap height */
    CHECK(L(lev[0])->resolved_h >= blk && L(lev[0])->resolved_h <= blk + 40.0f,
          "the outer block is the paragraph plus the heading, nothing multiplied");
    printf("       next-block y=%.1f  (paragraph block ends at %.1f)\n",
           L(nxt)->resolved_y, L(lev[2])->resolved_y + blk);
    CHECK(L(nxt)->resolved_y >= L(lev[2])->resolved_y + blk - 0.5f,
          "the block AFTER the paragraph starts below it -- no overdraw");
    (void)lines;
    layout_arena_destroy(&LA); scene_arena_destroy(&SA);
}

/* ---- T3d: an OVERFLOWING wrap row must not shrink its children ---------- */
/* The bug this locks out shipped in the browser and looked like a wrap-height
 * fault, which is why it survived several rounds of looking at wrap heights.
 *
 * A wrap row overflows BY DESIGN -- that is what makes it start a new line --
 * so the flex-shrink pass fired on every paragraph and squashed all 54 word
 * boxes. The arrangement never noticed: the wrap arm places children at their
 * base width. The MEASUREMENT did. A text child's cross size is its height at
 * its final width, so each squashed word was measured at a fraction of its
 * width and CHARACTER-wrapped: "a " reported one line, "This " three, "page "
 * four. The line stride became the tallest of those lies -- 65px for a 16px
 * line -- and a paragraph's lines spread far enough apart for the next heading
 * to be drawn between them.
 *
 * Two assertions, because either alone would have missed it: the words keep
 * their width, and they keep ONE line's height. */
static void t3d_wrap_no_shrink(uint32_t fh) {
    printf("T3d overflowing wrap row does not shrink:\n");
    scene_arena_init(&SA); layout_arena_init(&LA);

    struct layout_handle row = mk(LAYOUT_HANDLE_NULL, NODE_HANDLE_NULL, SCENE_NODE_GROUP, 0);
    L(row)->is_container = true; L(row)->axis = AXIS_ROW; L(row)->wrap = true;
    L(row)->align = ALIGN_START;

    struct color black = {0,0,0,1};
    struct layout_handle w[12];
    for (int i = 0; i < 12; i++) {
        struct node_handle sw;
        w[i] = mk(row, NODE_HANDLE_NULL, SCENE_NODE_TEXT, &sw);
        scene_set_text(&SA, sw, "wordy ", fh, 20.0f, black);
    }
    /* One word's unwrapped size. Intrinsics are computed BY layout, so the
     * baseline comes from a pass at a width nothing can overflow. */
    layout_run(&LA, &SA, row, 10000, 400);
    float one = L(w[0])->resolved_w, lineh = L(w[0])->resolved_h;

    /* now a width that fits about two words: a deep overflow, which is the
     * ordinary state of every paragraph in a document */
    layout_run(&LA, &SA, row, one * 2.4f, 400);

    printf("       word w=%.1f (unwrapped %.1f)  h=%.1f (one line %.1f)  row h=%.1f\n",
           L(w[5])->resolved_w, one, L(w[5])->resolved_h, lineh, L(row)->resolved_h);

    int squashed = 0, tall = 0;
    for (int i = 0; i < 12; i++) {
        if (L(w[i])->resolved_w < one - 0.5f)      squashed++;
        if (L(w[i])->resolved_h > lineh + 0.5f)    tall++;
    }
    CHECK(squashed == 0, "no word is shrunk below its width -- the row wraps instead");
    CHECK(tall == 0, "so no word character-wraps, and every one is a single line");

    /* And the lines are stacked one line-height apart, not one lie apart.
     * Count the distinct y values the words landed on -- that is the number of
     * lines the row actually produced. */
    float ys[12]; int nlines = 0, i;
    for (i = 0; i < 12; i++) {
        float y = L(w[i])->resolved_y;
        int seen = 0;
        for (int j = 0; j < nlines; j++) if (ys[j] > y - 0.5f && ys[j] < y + 0.5f) seen = 1;
        if (!seen) ys[nlines++] = y;
    }
    float span = 0;
    for (i = 0; i < nlines; i++) if (ys[i] > span) span = ys[i];
    printf("       %d lines, last at y=%.1f (a line height apart would be %.1f)\n",
           nlines, span, lineh * (float)(nlines - 1));
    CHECK(nlines > 1, "the row really did wrap -- otherwise this proves nothing");
    CHECK(span <= lineh * (float)(nlines - 1) + 0.5f,
          "the line stride is the line height, so lines do not spread apart");

    layout_arena_destroy(&LA); scene_arena_destroy(&SA);
}

/* ---- T4: ALIGN_STRETCH stretches AUTO cross sizes; FIXED always wins ---- */
/* (CSS semantics: stretch applies only to auto-sized items. A definite cross
 * size is never overridden -- relied on by declare's hit-test clip test once
 * the ui root became a stretch column.) */
static void t4_stretch(void) {
    printf("T4 ALIGN_STRETCH cross-size rules:\n");
    scene_arena_init(&SA); layout_arena_init(&LA);
    struct layout_handle root = mk(LAYOUT_HANDLE_NULL, NODE_HANDLE_NULL, SCENE_NODE_GROUP, 0);
    L(root)->is_container = true; L(root)->axis = AXIS_COLUMN; L(root)->align = ALIGN_STRETCH;
    struct layout_handle c = mk(root, NODE_HANDLE_NULL, SCENE_NODE_RECT, 0);
    L(c)->width  = (struct layout_size){ SIZE_INTRINSIC, 0, 0,0,0 };  /* auto cross-width */
    L(c)->height = (struct layout_size){ SIZE_FIXED, 10, 0,0,0 };
    struct layout_handle f = mk(root, NODE_HANDLE_NULL, SCENE_NODE_RECT, 0);
    L(f)->width  = (struct layout_size){ SIZE_FIXED, 10, 0,0,0 };     /* definite cross-width */
    L(f)->height = (struct layout_size){ SIZE_FIXED, 10, 0,0,0 };
    layout_run(&LA, &SA, root, 50, 40);   /* parent cross-width 50 */
    CHECK(feq(L(c)->resolved_w, 50), "auto-width child stretches to the parent's 50");
    CHECK(feq(L(f)->resolved_w, 10), "FIXED-width child keeps its 10 (stretch never overrides definite)");
    layout_arena_destroy(&LA); scene_arena_destroy(&SA);
}

/* ---- T5: text wrapping (word-wrap + character fallback) ---------------- */
static void t5_wrap(uint32_t fh) {
    printf("T5 text wrapping:\n");
    scene_arena_init(&SA); layout_arena_init(&LA);
    struct node_handle stext;
    struct layout_handle t = mk(LAYOUT_HANDLE_NULL, NODE_HANDLE_NULL, SCENE_NODE_TEXT, &stext);
    struct color black = {0,0,0,1};
    scene_set_text(&SA, stext, "aa aa aa", fh, 20.0f, black);   /* 3 words, 20px each; space 10px */

    /* at width 25: each 20px word forces its own line -> 3 lines, 3*20px=60 */
    int lines = layout_debug_wrap_lines(&LA, &SA, t, 25.0f);
    float h = layout_measure_height_at_width(&LA, &SA, t, 25.0f);
    CHECK(lines == 3, "word-wrap: 3 lines at width 25");
    CHECK(feq(h, 60.0f), "height == 3 * line_height (20px)");

    /* single unbroken over-wide word must character-wrap, never overflow */
    scene_set_text(&SA, stext, "aaaaaa", fh, 20.0f, black);      /* 60px, no spaces */
    int clines = layout_debug_wrap_lines(&LA, &SA, t, 25.0f);
    CHECK(clines > 1, "character-fallback wraps a single overlong word");
    layout_arena_destroy(&LA); scene_arena_destroy(&SA);
}

/* ---- T6: end-to-end write-back into the scene tree --------------------- */
static void t6_writeback(uint32_t fh) {
    printf("T6 end-to-end write-back into scene tree:\n");
    scene_arena_init(&SA); layout_arena_init(&LA);
    struct node_handle s_row, s_col, s_text;
    struct layout_handle row = mk(LAYOUT_HANDLE_NULL, NODE_HANDLE_NULL, SCENE_NODE_GROUP, &s_row);
    L(row)->is_container = true; L(row)->axis = AXIS_ROW; L(row)->padding_left = 5; L(row)->padding_top = 3;
    struct layout_handle col = mk(row, s_row, SCENE_NODE_GROUP, &s_col);
    L(col)->is_container = true; L(col)->axis = AXIS_COLUMN;
    struct layout_handle txt = mk(col, s_col, SCENE_NODE_TEXT, &s_text);
    struct color black = {0,0,0,1};
    scene_set_text(&SA, s_text, "hi", fh, 20.0f, black);

    layout_run(&LA, &SA, row, 200, 100);

    struct layout_node *lt = L(txt);
    struct scene_node *st = scene_resolve(&SA, s_text);
    CHECK(st != 0, "deepest text scene node resolves");
    CHECK(st && feq(st->width, lt->resolved_w) && feq(st->height, lt->resolved_h),
          "scene node size == layout resolved size");
    CHECK(st && feq(st->tx, lt->resolved_x) && feq(st->ty, lt->resolved_y),
          "scene node transform == layout resolved position");
    /* and it's genuinely nonzero geometry, not both trivially 0 */
    CHECK(lt->resolved_w > 0 && lt->resolved_h > 0, "text resolved to real (nonzero) geometry");
    layout_arena_destroy(&LA); scene_arena_destroy(&SA);
}


/* ---- T7: SPACE_AROUND and SPACE_EVENLY differ at the ENDS ---------------
 * The whole reason an author picks one over the other is what happens at the
 * two edges, so that is what is asserted -- a test that only checked the
 * middle gaps would have passed while both collapsed onto SPACE_BETWEEN. */
static void t7_space_around_evenly(void) {
    printf("T7 SPACE_AROUND / SPACE_EVENLY:\n");
    for (int evenly = 0; evenly < 2; evenly++) {
        scene_arena_init(&SA); layout_arena_init(&LA);
        struct layout_handle root = mk(LAYOUT_HANDLE_NULL, NODE_HANDLE_NULL, SCENE_NODE_GROUP, 0);
        L(root)->is_container = true; L(root)->axis = AXIS_ROW;
        L(root)->justify = evenly ? JUSTIFY_SPACE_EVENLY : JUSTIFY_SPACE_AROUND;
        struct layout_handle c[3];
        for (int i = 0; i < 3; i++) {
            c[i] = mk(root, NODE_HANDLE_NULL, SCENE_NODE_RECT, 0);
            L(c[i])->width  = (struct layout_size){ .mode = SIZE_FIXED, .fixed_value = 20 };
            L(c[i])->height = (struct layout_size){ .mode = SIZE_FIXED, .fixed_value = 10 };
        }
        layout_run(&LA, &SA, root, 120, 50);   /* 60 used, 60 leftover, 3 items */
        if (!evenly) {
            /* share 20 each, half (10) before the first */
            CHECK(feq(L(c[0])->resolved_x, 10), "AROUND: first at half a share");
            CHECK(feq(L(c[1])->resolved_x, 50), "AROUND: second a whole share on");
            CHECK(feq(L(c[2])->resolved_x + 20, 110), "AROUND: last ends half a share short");
        } else {
            /* four equal gaps of 15 */
            CHECK(feq(L(c[0])->resolved_x, 15), "EVENLY: first at one gap");
            CHECK(feq(L(c[1])->resolved_x, 50), "EVENLY: gaps are all equal");
            CHECK(feq(L(c[2])->resolved_x + 20, 105), "EVENLY: last leaves one gap");
        }
        layout_arena_destroy(&LA); scene_arena_destroy(&SA);
    }
}

/* ---- T8: align-self overrides the container, and only for that child ---- */
static void t8_align_self(void) {
    printf("T8 align-self overrides align-items:\n");
    scene_arena_init(&SA); layout_arena_init(&LA);
    struct layout_handle root = mk(LAYOUT_HANDLE_NULL, NODE_HANDLE_NULL, SCENE_NODE_GROUP, 0);
    L(root)->is_container = true; L(root)->axis = AXIS_ROW; L(root)->align = ALIGN_START;
    struct layout_handle a = mk(root, NODE_HANDLE_NULL, SCENE_NODE_RECT, 0);
    struct layout_handle b = mk(root, NODE_HANDLE_NULL, SCENE_NODE_RECT, 0);
    struct layout_handle c = mk(root, NODE_HANDLE_NULL, SCENE_NODE_RECT, 0);
    L(a)->width = (struct layout_size){ .mode = SIZE_FIXED, .fixed_value = 10 };
    L(b)->width = (struct layout_size){ .mode = SIZE_FIXED, .fixed_value = 10 };
    L(c)->width = (struct layout_size){ .mode = SIZE_FIXED, .fixed_value = 10 };
    L(a)->height = (struct layout_size){ .mode = SIZE_FIXED, .fixed_value = 20 };
    L(b)->height = (struct layout_size){ .mode = SIZE_FIXED, .fixed_value = 20 };
    L(c)->height = (struct layout_size){ .mode = SIZE_FIXED, .fixed_value = 20 };
    L(b)->align_self = ALIGN_CENTER + 1;
    L(c)->align_self = ALIGN_END + 1;

    layout_run(&LA, &SA, root, 100, 100);
    CHECK(feq(L(a)->resolved_y, 0),  "no align-self: the container's START applies");
    CHECK(feq(L(b)->resolved_y, 40), "align-self:center centres just that child");
    CHECK(feq(L(c)->resolved_y, 80), "align-self:end pushes just that child down");
    layout_arena_destroy(&LA); scene_arena_destroy(&SA);
}

/* ---- T9: the cross gap separates LINES, the main gap separates items ----
 * `gap: 30px 10px` on a wrapping row means 10 between cards and 30 between
 * rows of cards. One spacing served both, so the row gap leaked sideways. */
static void t9_cross_gap(void) {
    printf("T9 separate cross-axis gap:\n");
    scene_arena_init(&SA); layout_arena_init(&LA);
    struct layout_handle root = mk(LAYOUT_HANDLE_NULL, NODE_HANDLE_NULL, SCENE_NODE_GROUP, 0);
    L(root)->is_container = true; L(root)->axis = AXIS_ROW; L(root)->wrap = true;
    L(root)->spacing = 10; L(root)->cross_spacing = 30;
    struct layout_handle c[4];
    for (int i = 0; i < 4; i++) {
        c[i] = mk(root, NODE_HANDLE_NULL, SCENE_NODE_RECT, 0);
        L(c[i])->width  = (struct layout_size){ .mode = SIZE_FIXED, .fixed_value = 40 };
        L(c[i])->height = (struct layout_size){ .mode = SIZE_FIXED, .fixed_value = 20 };
    }
    layout_run(&LA, &SA, root, 100, 200);      /* 2 per line: 40 + 10 + 40 = 90 */
    CHECK(feq(L(c[1])->resolved_x, 50), "items are separated by the MAIN gap (10)");
    CHECK(feq(L(c[2])->resolved_y, 50), "lines are separated by the CROSS gap (30)");
    CHECK(feq(L(c[2])->resolved_x, 0),  "the second line starts at the left again");
    layout_arena_destroy(&LA); scene_arena_destroy(&SA);
}

int main(void) {
    printf("=== EmbLink UI Piece 5: layout-engine selftests ===\n");
    uint32_t fh = font_load(g_ttf, build_font());
    if (!fh) { printf("  FAIL: synthetic font_load\n"); return 1; }
    t1_space_between();
    t2_grow();
    t3_shrink();
    t3b_default_shrink();
    t3c_nested_wrap(fh);
    t3d_wrap_no_shrink(fh);
    t4_stretch();
    t5_wrap(fh);
    t6_writeback(fh);
    t7_space_around_evenly();
    t8_align_self();
    t9_cross_gap();
    printf("=== layout-test: %s (%d failures) ===\n", g_fail ? "FAIL" : "OK", g_fail);
    return g_fail ? 1 : 0;
}

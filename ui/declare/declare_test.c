/* ui/declare/declare_test.c -- EmbLink UI Piece 7 selftests (Section 8).
 * Pure userland.  make declare-test  -> exits 0 iff every T1..T7 holds. */

#include "ui.h"
#include "scene.h"
#include "layout.h"
#include "scope.h"
#include <stdio.h>

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  FAIL: %s\n", (msg)); g_fail++; } \
    else         { printf("  ok:   %s\n", (msg)); } \
} while (0)

static struct scene_arena  SA;
static struct layout_arena LA;

static int heq(struct instance_handle a, struct instance_handle b) {
    return a.index == b.index && a.generation == b.generation;
}
static void fresh(void) { scene_arena_init(&SA); layout_arena_init(&LA); ui_init(&SA, &LA); }
static void done(void)  { layout_arena_destroy(&LA); scene_arena_destroy(&SA); }

/* ---- T1: stable reuse + no-op skip ------------------------------------- */
static void app_two_texts(void) {
    ui_begin_vstack(0);
      ui_text("hello");
      ui_text("world");
    ui_end_stack();
}
static void t1_reuse(void) {
    printf("T1 stable reuse + no-op skip:\n");
    fresh();
    ui_frame_begin(); app_two_texts(); ui_frame_end();
    struct instance_handle vs = ui_first_child(ui_root());
    struct instance_handle t0 = ui_first_child(vs);
    struct instance_handle t1 = ui_next_sibling(t0);
    struct node_handle s0 = ui_scene_of(t0);
    uint32_t mut_after_first = ui_debug_mutation_count();

    ui_frame_begin(); app_two_texts(); ui_frame_end();     /* identical redeclare */
    struct instance_handle vs2 = ui_first_child(ui_root());
    struct instance_handle t0b = ui_first_child(vs2);
    struct instance_handle t1b = ui_next_sibling(t0b);

    CHECK(heq(vs, vs2) && heq(t0, t0b) && heq(t1, t1b), "same instance handles reused across identical passes");
    CHECK(ui_scene_of(t0b).index == s0.index, "same paired scene node reused");
    CHECK(ui_debug_mutation_count() == mut_after_first, "no mutations on an identical redeclare (no-op skip)");
    done();
}

/* ---- T2: sweep removes a dropped child --------------------------------- */
static int g_t2_count = 3;
static void app_n_children(void) {
    ui_begin_vstack(0);
      for (int i = 0; i < g_t2_count; i++) ui_text("item %d", i);
    ui_end_stack();
}
static void t2_sweep(void) {
    printf("T2 sweep on shrink:\n");
    fresh();
    g_t2_count = 3;
    ui_frame_begin(); app_n_children(); ui_frame_end();
    struct instance_handle vs = ui_first_child(ui_root());
    struct instance_handle c0 = ui_first_child(vs);
    struct instance_handle c1 = ui_next_sibling(c0);
    struct instance_handle c2 = ui_next_sibling(c1);
    struct node_handle   s2 = ui_scene_of(c2);
    struct layout_handle l2 = ui_layout_of(c2);
    CHECK(instance_resolve(c2) != 0, "3rd child exists after first pass");

    g_t2_count = 2;
    ui_frame_begin(); app_n_children(); ui_frame_end();     /* only 2 now */
    CHECK(instance_resolve(c2) == 0, "3rd instance destroyed by sweep");
    CHECK(scene_resolve(&SA, s2) == 0, "3rd child's scene node resolves NULL");
    CHECK(layout_resolve(&LA, l2) == 0, "3rd child's layout node resolves NULL");
    done();
}

/* ---- T3 (crux): fine-grained component re-running ---------------------- */
static struct signal_handle g_sigA, g_sigB;
static int g_runA, g_runB, g_runParent;
static void comp_A(void *p) { (void)p; int v=0; signal_get(g_sigA, &v, sizeof v); g_runA++; ui_text("A%d", v); }
static void comp_B(void *p) { (void)p; int v=0; signal_get(g_sigB, &v, sizeof v); g_runB++; ui_text("B%d", v); }
static void app_parent(void) {
    g_runParent++;
    ui_begin_vstack(0);
      int dummy = 0;
      ui_component(comp_A, &dummy, sizeof dummy, 1);
      ui_component(comp_B, &dummy, sizeof dummy, 2);
    ui_end_stack();
}
static void t3_fine_grained(void) {
    printf("T3 fine-grained component re-running:\n");
    fresh();
    int z = 0; g_sigA = signal_create(&z, sizeof z); g_sigB = signal_create(&z, sizeof z);
    g_runA = g_runB = g_runParent = 0;

    ui_frame_begin(); app_parent(); ui_frame_end();
    int a0 = g_runA, b0 = g_runB, p0 = g_runParent;
    CHECK(a0 == 1 && b0 == 1, "both components ran once on creation");

    int one = 1; signal_set(g_sigA, &one, sizeof one);   /* only A's signal changes */
    reactivity_flush();
    CHECK(g_runA == a0 + 1, "A re-ran exactly once (its signal changed)");
    CHECK(g_runB == b0,     "B did NOT re-run (its signal untouched)");
    CHECK(g_runParent == p0, "parent did NOT re-run (no ancestor cascade)");
    done();
}

/* ---- T4: props change forces a re-run ---------------------------------- */
static int g_runP;
static void comp_props(void *p) { (void)p; g_runP++; ui_text("x"); }
static int g_t4_prop = 10;
static void app_props(void) {
    ui_begin_vstack(0);
      ui_component(comp_props, &g_t4_prop, sizeof g_t4_prop, 5);
    ui_end_stack();
}
static void t4_props_change(void) {
    printf("T4 props-change re-run:\n");
    fresh();
    g_runP = 0;
    g_t4_prop = 10; ui_frame_begin(); app_props(); ui_frame_end();
    CHECK(g_runP == 1, "component ran on creation");

    /* redeclare with identical props -> no re-run */
    ui_frame_begin(); app_props(); ui_frame_end();
    CHECK(g_runP == 1, "identical props -> no re-run");

    /* redeclare with different props -> re-run (condition b) */
    g_t4_prop = 20; ui_frame_begin(); app_props(); ui_frame_end();
    CHECK(g_runP == 2, "changed props force a re-run even with no internal signal change");
    done();
}

/* ---- T5: keyed reorder preserves identity ------------------------------ */
static int g_t5_order[3] = {1,2,3};
static void app_keyed(void) {
    ui_begin_vstack(0);
      for (int i = 0; i < 3; i++) ui_text_keyed((uint64_t)g_t5_order[i], "k%d", g_t5_order[i]);
    ui_end_stack();
}
static void t5_keyed_reorder(void) {
    printf("T5 keyed reorder preserves identity:\n");
    fresh();
    g_t5_order[0]=1; g_t5_order[1]=2; g_t5_order[2]=3;
    ui_frame_begin(); app_keyed(); ui_frame_end();
    struct instance_handle vs = ui_first_child(ui_root());
    /* record handle of each key by scanning children */
    struct instance_handle by_key[4] = {0};
    for (struct instance_handle c = ui_first_child(vs); !instance_handle_is_null(c); c = ui_next_sibling(c)) {
        struct instance *ci = instance_resolve(c);
        if (ci) by_key[ci->explicit_key] = c;
    }
    struct node_handle sc1 = ui_scene_of(by_key[1]);

    g_t5_order[0]=3; g_t5_order[1]=1; g_t5_order[2]=2;      /* reorder */
    ui_frame_begin(); app_keyed(); ui_frame_end();
    struct instance_handle vs2 = ui_first_child(ui_root());
    struct instance_handle by_key2[4] = {0};
    for (struct instance_handle c = ui_first_child(vs2); !instance_handle_is_null(c); c = ui_next_sibling(c)) {
        struct instance *ci = instance_resolve(c);
        if (ci) by_key2[ci->explicit_key] = c;
    }
    CHECK(heq(by_key[1], by_key2[1]) && heq(by_key[2], by_key2[2]) && heq(by_key[3], by_key2[3]),
          "each key's instance handle identical before/after reorder");
    CHECK(ui_scene_of(by_key2[1]).index == sc1.index, "key 1's scene node preserved (state not reset)");
    /* and order actually changed: first child now key 3 */
    struct instance *first = instance_resolve(ui_first_child(vs2));
    CHECK(first && first->explicit_key == 3, "sibling order now reflects the reorder (key 3 first)");
    done();
}

/* ---- T5b: a row that APPEARS must not displace its siblings ------------- *
 *
 * The failure this pins down cost a browser two evenings. Its chrome is a
 * column of rows, one of which -- a find bar -- appears when you open it.
 * Unkeyed children are matched to last frame's by POSITION, so the moment that
 * row was inserted every row below it adopted its neighbour's retained
 * instance, and since a reused instance keeps whatever size nobody restated,
 * the whole window came back laid out as something else: the find bar drawn
 * over the address bar, the document's viewport wearing the divider's height.
 *
 * It looked like a repaint bug for as long as it was only ever LOOKED at.
 *
 * The rule the fix rests on: give a key to the rows that STAY, not only to the
 * one that comes and goes -- it is the stable rows that get displaced.
 */
static int g_t5b_extra;
static int g_t5b_keys = 1;

/* A row whose size is RETAINED rather than restated every frame.
 *
 * That is what makes the displacement visible, and it is not contrived: the
 * toolkit's box only writes a size when a prop asks for one, so a row that
 * settled at a height keeps it until something says otherwise. A test whose
 * rows restate their size every frame cannot see this bug at all -- the first
 * version of this test did exactly that and passed while the browser on the
 * other screen was visibly broken. */
static int g_t5b_state_sizes = 1;

static void row(uint64_t key, float h) {
    ui_box_begin(key);
    if (g_t5b_state_sizes)
        ui_set_size((struct layout_size){ .mode = SIZE_FIXED, .fixed_value = 200 },
                    (struct layout_size){ .mode = SIZE_FIXED, .fixed_value = h });
    ui_box_end();
}

static void app_optional_row(void) {
    ui_begin_vstack(0);
    row(g_t5b_keys ? 0xA1 : 0, 48);              /* header */
    if (g_t5b_extra) row(g_t5b_keys ? 0xEE : 0, 20);   /* the row that comes and goes */
    row(g_t5b_keys ? 0xB2 : 0, 30);              /* body   */
    row(g_t5b_keys ? 0xC3 : 0, 12);              /* footer */
    ui_end_stack();
}

static struct instance_handle child_by_key(struct instance_handle parent, uint64_t key) {
    for (struct instance_handle c = ui_first_child(parent); !instance_handle_is_null(c); c = ui_next_sibling(c)) {
        struct instance *ci = instance_resolve(c);
        if (ci && ci->explicit_key == key) return c;
    }
    return INSTANCE_HANDLE_NULL;
}

static void t5b_inserted_row(void) {
    printf("T5b an inserted row does not displace keyed siblings:\n");
    fresh();
    g_t5b_extra = 0;
    ui_frame_begin(); app_optional_row(); ui_frame_end();
    ui_run_layout(400, 300);
    struct instance_handle col = ui_first_child(ui_root());
    struct instance_handle h0 = child_by_key(col, 0xA1);
    struct instance_handle b0 = child_by_key(col, 0xB2);
    struct instance_handle f0 = child_by_key(col, 0xC3);
    struct scene_node *bn = scene_resolve(&SA, ui_scene_of(b0));
    float body_h = bn ? bn->height : -1;
    CHECK(body_h == 30, "the body starts at the height it asked for");

    /* ...now the row appears, which is the whole test */
    g_t5b_extra = 1; g_t5b_state_sizes = 0;
    ui_frame_begin(); app_optional_row(); ui_frame_end();
    ui_run_layout(400, 300);
    col = ui_first_child(ui_root());
    CHECK(heq(child_by_key(col, 0xA1), h0) &&
          heq(child_by_key(col, 0xB2), b0) &&
          heq(child_by_key(col, 0xC3), f0),
          "every stable row keeps its own instance when a row is inserted above it");

    struct scene_node *hn = scene_resolve(&SA, ui_scene_of(child_by_key(col, 0xA1)));
    struct scene_node *b2 = scene_resolve(&SA, ui_scene_of(child_by_key(col, 0xB2)));
    struct scene_node *f2 = scene_resolve(&SA, ui_scene_of(child_by_key(col, 0xC3)));
    CHECK(hn && hn->height == 48, "the header is still 48 tall, not the inserted row's 20");
    CHECK(b2 && b2->height == 30, "the body is still 30 -- not its neighbour's height");
    CHECK(f2 && f2->height == 12, "and the footer is still 12");

    /* the inserted row is really there, or the checks above prove nothing */
    CHECK(!instance_handle_is_null(child_by_key(col, 0xEE)), "the new row was actually inserted");
    done();

    /* THE CONTROL. The same column with no keys at all, to show that the keys
     * above are what is doing the work rather than the layout happening to be
     * right anyway. Unkeyed rows match by position, so the footer inherits the
     * body's instance and comes back 30 tall instead of 12 -- and a test that
     * did not demonstrate this would be asserting nothing. */
    fresh();
    g_t5b_extra = 0; g_t5b_keys = 0; g_t5b_state_sizes = 1;
    ui_frame_begin(); app_optional_row(); ui_frame_end();
    ui_run_layout(400, 300);
    g_t5b_extra = 1; g_t5b_state_sizes = 0;
    ui_frame_begin(); app_optional_row(); ui_frame_end();
    ui_run_layout(400, 300);
    col = ui_first_child(ui_root());
    float hs[8]; int n = 0;
    for (struct instance_handle c = ui_first_child(col);
         !instance_handle_is_null(c) && n < 8; c = ui_next_sibling(c)) {
        struct scene_node *sn = scene_resolve(&SA, ui_scene_of(c));
        hs[n++] = sn ? sn->height : -1;
    }
    CHECK(n == 4, "unkeyed: four rows, as declared");
    printf("      (unkeyed heights: %.0f %.0f %.0f %.0f -- declared 48 20 30 12)\n",
           hs[0], hs[1], hs[2], hs[3]);
    CHECK(!(n == 4 && hs[0] == 48 && hs[1] == 20 && hs[2] == 30 && hs[3] == 12),
          "unkeyed: the rows are DISPLACED -- this is the bug the keys exist for");
    g_t5b_keys = 1; g_t5b_state_sizes = 1;
    done();
}

/* ---- T6: hit-test clip-awareness --------------------------------------- */
static void t6_hit_clip(void) {
    printf("T6 hit-test clip-awareness:\n");
    fresh();
    /* clipping container 50x50 at origin; child 40x40 placed at x=30 (half
     * outside), clipped away past x=50. */
    ui_frame_begin();
    ui_box_begin(0);
      ui_set_size((struct layout_size){ .mode = SIZE_FIXED, .fixed_value = 50 }, (struct layout_size){ .mode = SIZE_FIXED, .fixed_value = 50 });
      ui_set_clip_children(true);
      ui_box_begin(0);
        ui_set_size((struct layout_size){ .mode = SIZE_FIXED, .fixed_value = 40 }, (struct layout_size){ .mode = SIZE_FIXED, .fixed_value = 40 });
      ui_box_end();
    ui_box_end();
    ui_frame_end();

    /* place the child manually at x=30 (layout would stack it at 0; we want it
     * straddling the clip edge for the test) */
    struct instance_handle outer = ui_first_child(ui_root());
    struct instance_handle inner = ui_first_child(outer);
    ui_run_layout(200, 200);
    /* override inner's scene transform to x=30 so its right half is clipped */
    scene_set_transform(&SA, ui_scene_of(inner), 30, 5, 0, 0,0,0,1, 1,1,1);

    /* a click at x=40 (inside inner's rect 30..70, but inside clip 0..50) hits */
    ui_dispatch_click(40, 20);
    CHECK(ui_consume_click(inner), "click inside the visible (unclipped) part hits");

    /* a click at x=60 (inside inner's rect 30..70 but OUTSIDE clip 0..50) misses */
    ui_dispatch_click(60, 20);
    CHECK(!ui_consume_click(inner), "click in the clipped-away part does NOT hit");
    done();
}

/* ---- T6b: z-layers -- an early elevated box beats a later flow box ------ */
static void t6b_hit_layers(void) {
    printf("T6b hit-test z-layers:\n");
    fresh();
    ui_frame_begin();
    ui_box_begin(0);                       /* an early scrim raised to layer 1 */
      ui_set_size((struct layout_size){ .mode = SIZE_FIXED, .fixed_value = 100 }, (struct layout_size){ .mode = SIZE_FIXED, .fixed_value = 100 });
      ui_set_layer(1);
    ui_box_end();
    ui_box_begin(0);                       /* a LATER flow box over the same pixels */
      ui_set_size((struct layout_size){ .mode = SIZE_FIXED, .fixed_value = 100 }, (struct layout_size){ .mode = SIZE_FIXED, .fixed_value = 100 });
    ui_box_end();
    ui_frame_end();

    struct instance_handle scrim = ui_first_child(ui_root());
    struct instance_handle later = ui_next_sibling(scrim);
    ui_run_layout(200, 200);
    /* stack both at the origin so they overlap (layout would stagger them) */
    scene_set_transform(&SA, ui_scene_of(later), 0, 0, 0, 0,0,0,1, 1,1,1);

    ui_dispatch_click(50, 50);
    CHECK(ui_consume_click(scrim), "the elevated (layer 1) box wins the point");
    ui_dispatch_click(50, 50);
    CHECK(!ui_consume_click(later), "the later flow box does NOT win it");
    done();
}

/* ---- T7: button click is a one-frame pulse ----------------------------- */
static bool g_last_click;
static void app_button(void) {
    ui_begin_vstack(0);
      g_last_click = ui_button("go");
    ui_end_stack();
}
static void t7_button_pulse(void) {
    printf("T7 button click one-frame pulse:\n");
    fresh();
    ui_frame_begin(); app_button(); ui_frame_end();
    ui_run_layout(200, 200);

    /* find the button box (first child of the vstack) and click its center */
    struct instance_handle vs = ui_first_child(ui_root());
    struct instance_handle btn = ui_first_child(vs);
    struct scene_node *bs = scene_resolve(&SA, ui_scene_of(btn));
    /* place + size the button so we can click it deterministically */
    scene_set_transform(&SA, ui_scene_of(btn), 10, 10, 0, 0,0,0,1, 1,1,1);
    if (bs) { bs->width = 40; bs->height = 20; }

    ui_dispatch_click(20, 15);
    ui_frame_begin(); app_button(); ui_frame_end();       /* pass that reads the flag */
    CHECK(g_last_click, "button returns true on the declare pass after the click");

    ui_frame_begin(); app_button(); ui_frame_end();       /* next pass */
    CHECK(!g_last_click, "button returns false on the following pass (pulse, not sticky)");
    done();
}

/* ---- T8: a press must latch on a MAGNIFYING icon --------------------------
 * The dock repro, boiled down. A dock icon is [box [imgbutton [image]]] where
 * the IMAGE LEAF is keyed by its pixel pointer -- and a magnifying icon swaps
 * mip levels as it swells, so that key CHANGES while the pointer approaches.
 * The press edge captures whatever last frame's tree had under the pointer;
 * this asserts the box still recognises that capture as its own. */
static uint32_t px_a[64], px_b[64];    /* two "mip levels" of one icon */

static int t8_active;
static void t8_app(int which_level, float size) {
    ui_begin_vstack(0xB0B0);                       /* the dock pill */
      ui_begin_vstack(0xD0C1);                     /* the slot */
        ui_begin_vstack(0xFACE);                   /* drag_icon's box */
          ui_begin_vstack(0x1C0);                  /* em_image_button's box */
            void *px = which_level ? (void *)px_b : (void *)px_a;
            /* keyed like the FIXED em_image_button: stable identity, varying
             * pixels. Flip this back to (uintptr_t)px to watch T8 fail. */
            ui_image_sized(0x1CA7, px, 8, 8, size - 8, size - 8);
          ui_end_stack();
          t8_active = ui_is_active();              /* what drag_icon reads */
        ui_end_stack();
      ui_end_stack();
    ui_end_stack();
}

static void t8_frame(int level, float size, float px_, float py_, int down) {
    ui_pointer(px_, py_, down != 0);
    ui_frame_begin(); t8_app(level, size); ui_frame_end();
    ui_run_layout(200, 200);
}

static void t8_magnifier_press(void) {
    printf("T8 press latch across a mip-level swap (the dock magnifier):\n");
    fresh();

    /* frame 1: pointer far away, small icon, level A */
    t8_frame(0, 40, 190, 190, 0);
    /* frame 2: pointer arrives over the icon; still level A geometry retained */
    t8_frame(0, 40, 16, 16, 0);
    /* frame 3: the magnifier crosses a level boundary: level B, bigger */
    t8_frame(1, 56, 16, 16, 0);
    /* frame 4: THE PRESS. The edge is hit-tested against frame 3's tree. */
    t8_frame(1, 56, 16, 16, 1);
    CHECK(t8_active, "press latches when the level flipped BEFORE the press");

    /* release cleanly */
    t8_frame(1, 56, 16, 16, 0);

    /* now the nastier order: the level flips ON the press frame itself */
    t8_frame(0, 40, 16, 16, 0);      /* retained tree back to level A */
    t8_frame(1, 56, 16, 16, 1);      /* press + flip in the same frame */
    CHECK(t8_active, "press latches when the level flips ON the press frame");

    done();
}

int main(void) {
    t8_magnifier_press();
    printf("=== EmbLink UI Piece 7: declarative-API selftests ===\n");
    t1_reuse();
    t2_sweep();
    t3_fine_grained();
    t4_props_change();
    t5_keyed_reorder();
    t5b_inserted_row();
    t6_hit_clip();
    t6b_hit_layers();
    t7_button_pulse();
    printf("=== declare-test: %s (%d failures) ===\n", g_fail ? "FAIL" : "OK", g_fail);
    return g_fail ? 1 : 0;
}

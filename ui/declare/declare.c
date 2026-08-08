/* ui/declare/declare.c -- EmbLink UI Piece 7 implementation (see ui.h).
 *
 * The retained instance tree + cursor-based reconciliation (§2), the component
 * re-run rules (§3, the load-bearing part), property no-op-skip diffing (§5),
 * and clip-aware hit-testing (§7). Built entirely on Pieces 3/5/6 primitives;
 * modifies none of them. */

#include "ui.h"
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>

/* ------------------------------------------------------------------------- */
/* globals + instance pool                                                   */
/* ------------------------------------------------------------------------- */

/* One instance per emitted view -- and in a DOCUMENT that means one per WORD.
 * 4096 was sized for an application's worth of widgets; a real page needs
 * thousands (danluu.com's index: 2214 measured, Hacker News: 1255).
 *
 * SIZED, not maximised. An instance carries a shadow copy of its text, so this
 * array IS libembk.so's .bss -- and the shared library is mapped by every
 * process on the desktop. 65536 took it to 48MB. Overflow past this is now
 * COUNTED (ui_instance_overflow) instead of silently returning null and
 * letting the caller draw a collapsed box; paging this arena the way the scene
 * and layout arenas already are is the proper fix, logged in docs/TODO.md. */
/* Sized from what a real page builds, not from a round number: Wikipedia's
 * article needs about 10400 views and dropped 2178 of them at 8192 -- and a
 * dropped view is the bottom of the page simply not being there. Raised with
 * the arena sizes it has to keep up with (html.h's NODE_MAX), and no further:
 * this pool is per-process BSS, and an earlier guess of 65536 took the shared
 * library to 48MB. */
#define INST_MAX          24576
/* Real HTML nests deeper than a hand-written UI does -- an English Wikipedia
 * article got past 64 without trying. The stack is two handles per level, so
 * this is a few kilobytes; the depth CLAMP below, not this number, is what
 * makes an arbitrarily deep document safe. */
#define CURSOR_STACK_MAX  256

static struct instance g_inst[INST_MAX];
static uint32_t g_inst_hw = 1;
static uint32_t g_inst_free[INST_MAX]; static int g_inst_free_top = -1;

static struct scene_arena  *g_sa;
static struct layout_arena *g_la;
static struct instance_handle g_root;

struct reconcile_cursor { struct instance_handle parent, insert_after; };
static struct reconcile_cursor g_cursor[CURSOR_STACK_MAX];
static int g_cursor_top = -1;
/* Containers entered PAST the stack's depth. They are still entered as far as
 * the caller is concerned -- they just do not get a cursor level -- so their
 * exit has to consume one of these instead of popping a level that was never
 * pushed.
 *
 * Without this counter, overflow silently unbalanced the stack: every refused
 * push was still matched by a real pop, g_cursor_top walked off the bottom,
 * and cur_box() started returning NULL into an unchecked dereference. A deeply
 * nested page did not degrade, it SEGFAULTED -- which on the metal is a ring-3
 * fault that kills the browser. Found by rendering Wikipedia. */
static int g_cursor_over;
static int g_depth_overflowed;
/* How many children were actually re-linked -- the work the no-op guard above
 * exists to avoid. Counted rather than timed so a test can assert on it. */
static unsigned long g_relink_walks;

static uint32_t g_mutation_count;
/* pointer / hover / press state (§7, extended for a live event loop) */
static struct instance_handle g_hovered;   /* deepest instance under the pointer */
static struct instance_handle g_clicked;   /* deepest instance a press landed on */
static struct instance_handle g_active;    /* instance that owns the current drag (pointer capture) */
static struct instance_handle g_focused;   /* instance with keyboard focus (text field) */
static float g_ptr_x, g_ptr_y;
static bool  g_ptr_down;
static bool  g_press_edge;                  /* a press landed this frame (for defocus-on-outside-click) */
static bool  g_focus_claimed;               /* a widget claimed focus this frame */
/* per-frame typed-character queue: the loop feeds keys via ui_input_char, the
 * focused text field drains it via ui_input_take; cleared each ui_frame_end. */
static char  g_input_buf[32];
static int   g_input_n;
static float g_wheel;                       /* per-frame scroll-wheel delta, consumed by the hovered scroll view */
static uint32_t g_font;
static float    g_text_size = 16.0f;
static struct color g_text_color = { 0, 0, 0, 1 };
static struct color g_text_bg;      /* a==0: none. One-shot, like the gradient. */

uint32_t ui_debug_mutation_count(void) { return g_mutation_count; }

struct instance *instance_resolve(struct instance_handle h) {
    if (h.index == 0 || h.index >= INST_MAX) return 0;
    struct instance *s = &g_inst[h.index];
    if (!s->used || s->self.index != h.index || s->self.generation != h.generation) return 0;
    return s;
}

/* How many allocations the pool refused this frame. Zero on every UI this OS
 * ships; non-zero means a view was DROPPED, and a caller that cares (the
 * browser's harness) can say so instead of rendering the wreckage. */
static uint32_t g_inst_overflow;
uint32_t ui_instance_overflow(void) { return g_inst_overflow; }
uint32_t ui_instance_used(void) { return g_inst_hw; }

static struct instance_handle inst_alloc(void) {
    uint32_t idx;
    if (g_inst_free_top >= 0) idx = g_inst_free[g_inst_free_top--];
    else if (g_inst_hw < INST_MAX) idx = g_inst_hw++;
    else { g_inst_overflow++; return INSTANCE_HANDLE_NULL; }
    struct instance *s = &g_inst[idx];
    uint32_t gen = s->self.generation; if (gen == 0) gen = 1;
    memset(s, 0, sizeof(*s));
    s->self.index = idx; s->self.generation = gen; s->used = true;
    return s->self;
}

/* ------------------------------------------------------------------------- */
/* instance <-> scene user_data packing (§7 back-reference)                  */
/* ------------------------------------------------------------------------- */

static uint64_t pack_handle(struct instance_handle h) {
    return ((uint64_t)h.index << 32) | (uint64_t)h.generation;
}
static struct instance_handle unpack_handle(uint64_t v) {
    struct instance_handle h = { (uint32_t)(v >> 32), (uint32_t)(v & 0xffffffffu) };
    return h;
}

static enum scene_node_kind scene_kind_for(enum instance_kind k) {
    switch (k) {
        case INSTANCE_TEXT:  return SCENE_NODE_TEXT;
        case INSTANCE_IMAGE: return SCENE_NODE_IMAGE;
        case INSTANCE_BOX:   return SCENE_NODE_RECT;   /* can carry a fill + children + clip */
        case INSTANCE_COMPONENT:
        default:             return SCENE_NODE_GROUP;  /* pure container */
    }
}

/* append child to instance parent's sibling list */
static void inst_append_child(struct instance *p, struct instance_handle c) {
    if (instance_handle_is_null(p->first_child)) { p->first_child = c; return; }
    struct instance_handle it = p->first_child;
    for (;;) {
        struct instance *in = instance_resolve(it);
        if (!in || instance_handle_is_null(in->next_sibling)) { if (in) in->next_sibling = c; return; }
        it = in->next_sibling;
    }
}
static void inst_unlink_child(struct instance *p, struct instance_handle c) {
    if (!p) return;
    if (p->first_child.index == c.index) { struct instance *cn = instance_resolve(c);
        p->first_child = cn ? cn->next_sibling : INSTANCE_HANDLE_NULL; return; }
    struct instance_handle it = p->first_child;
    while (!instance_handle_is_null(it)) {
        struct instance *in = instance_resolve(it);
        if (!in) break;
        if (in->next_sibling.index == c.index) {
            struct instance *cn = instance_resolve(c);
            in->next_sibling = cn ? cn->next_sibling : INSTANCE_HANDLE_NULL;
            break;
        }
        it = in->next_sibling;
    }
}

static struct instance_handle create_instance(struct instance_handle parent,
                                              enum instance_kind kind,
                                              uint64_t key, bool has_key) {
    struct instance *pi = instance_resolve(parent);
    if (!pi) return INSTANCE_HANDLE_NULL;
    struct instance_handle h = inst_alloc();
    struct instance *n = instance_resolve(h);
    if (!n) return INSTANCE_HANDLE_NULL;

    n->kind = kind; n->parent = parent;
    n->has_explicit_key = has_key; n->explicit_key = key;

    n->scene_node  = scene_create_node(g_sa, scene_kind_for(kind), pi->scene_node);
    n->layout_node = layout_create_node(g_la, pi->layout_node);
    struct layout_node *ln = layout_resolve(g_la, n->layout_node);
    if (ln) { ln->scene_node = n->scene_node;
              ln->is_container = (kind == INSTANCE_BOX || kind == INSTANCE_COMPONENT); }
    struct scene_node *sn = scene_resolve(g_sa, n->scene_node);
    if (sn) sn->user_data = pack_handle(h);

    inst_append_child(pi, h);
    return h;
}

static void destroy_instance(struct instance_handle h) {
    struct instance *n = instance_resolve(h);
    if (!n) return;
    /* recurse children first */
    struct instance_handle c = n->first_child;
    while (!instance_handle_is_null(c)) {
        struct instance *cn = instance_resolve(c);
        struct instance_handle next = cn ? cn->next_sibling : INSTANCE_HANDLE_NULL;
        destroy_instance(c);
        c = next;
    }
    /* unlink from parent */
    struct instance *pi = instance_resolve(n->parent);
    if (pi) inst_unlink_child(pi, h);
    /* tear down owned resources (component scope + props, then paired nodes) */
    if (n->scope.index) scope_destroy(n->scope);
    if (n->props_copy) free(n->props_copy);
    scene_destroy_node(g_sa, n->scene_node);
    layout_destroy_node(g_la, n->layout_node);

    n->used = false; n->self.index = 0; n->self.generation++;
    g_inst_free[++g_inst_free_top] = h.index;
}

/* ------------------------------------------------------------------------- */
/* reconciliation (§2)                                                       */
/* ------------------------------------------------------------------------- */

/* relink instance + its paired scene/layout nodes to sit right after `after`
 * (INSTANCE_HANDLE_NULL => first) under `parent`. Keeps declared order ==
 * sibling order, including after a keyed reorder (§2 step 3). */
static void relink_after(struct instance_handle parent, struct instance_handle h,
                         struct instance_handle after) {
    struct instance *pi = instance_resolve(parent);
    struct instance *n  = instance_resolve(h);
    if (!pi || !n) return;

    /* ALREADY IN PLACE -- the overwhelmingly common case, since an
     * immediate-mode reconciler re-declares the same children in the same
     * order every frame.
     *
     * Without this check the unlink below runs anyway, and it finds a child's
     * predecessor by walking the parent's list from the front: O(k) for the
     * k'th child, so O(N^2) for a parent with N of them. A page that is one
     * long list -- which is most pages worth reading -- paid that every frame.
     * It was 164ms of Wikipedia's 174ms, and it is why a synthetic page of
     * sibling divs got 4x slower for every doubling.
     *
     * scene_reparent and layout_reparent already guard the same no-op for
     * exactly the same reason; the instance tree was the one that did not. */
    if (instance_handle_is_null(after)) {
        if (pi->first_child.index == h.index) return;
    } else {
        struct instance *as0 = instance_resolve(after);
        if (as0 && as0->next_sibling.index == h.index) return;
    }

    g_relink_walks++;      /* what T5c counts -- see ui_relink_walks */
    inst_unlink_child(pi, h);
    if (instance_handle_is_null(after)) {
        n->next_sibling = pi->first_child; pi->first_child = h;
    } else {
        struct instance *as = instance_resolve(after);
        if (!as) { n->next_sibling = INSTANCE_HANDLE_NULL; inst_append_child(pi, h); }
        else { n->next_sibling = as->next_sibling; as->next_sibling = h; }
    }
    /* mirror the order into the scene + layout trees */
    struct instance *aft = instance_resolve(after);
    struct node_handle   after_scene  = aft ? aft->scene_node  : (struct node_handle){0,0};
    struct layout_handle after_layout = aft ? aft->layout_node : (struct layout_handle){0,0};
    scene_reparent(g_sa, n->scene_node, pi->scene_node, after_scene);
    layout_reparent(g_la, n->layout_node, pi->layout_node, after_layout);
}

/* match-or-create at the current cursor position. Sets *created. */
static struct instance_handle match_or_create(enum instance_kind kind, uint64_t key,
                                              bool has_key, bool *created) {
    struct reconcile_cursor *cur = &g_cursor[g_cursor_top];
    struct instance *pi = instance_resolve(cur->parent);
    *created = false;
    if (!pi) return INSTANCE_HANDLE_NULL;

    struct instance_handle matched = INSTANCE_HANDLE_NULL;
    /* Where this child was LAST time, if the order has not changed. Both paths
     * below start here, and for the keyed path that is the whole difference
     * between linear and quadratic: a keyed lookup used to scan the parent's
     * child list, so a parent with N keyed children cost N^2 per frame.
     *
     * The browser gives every block a key derived from its DOM node, so a page
     * that is one long list -- which is most pages worth reading -- paid that
     * in full. Wikipedia spent 164ms of a 174ms frame here, and a synthetic
     * page of 2000 sibling divs went 4x slower for every doubling.
     *
     * Children come back in the same order almost always, so checking that one
     * position first answers it in O(1) and the scan stays for the case it was
     * written for: a keyed child that genuinely moved. */
    struct instance_handle expected;
    if (instance_handle_is_null(cur->insert_after)) expected = pi->first_child;
    else { struct instance *ia = instance_resolve(cur->insert_after);
           expected = ia ? ia->next_sibling : INSTANCE_HANDLE_NULL; }

    if (has_key) {
        struct instance *xi = instance_resolve(expected);
        if (xi && xi->has_explicit_key && xi->explicit_key == key && xi->kind == kind)
            matched = expected;
        for (struct instance_handle c = matched.index ? INSTANCE_HANDLE_NULL : pi->first_child;
             !instance_handle_is_null(c); ) {
            struct instance *cn = instance_resolve(c);
            if (!cn) break;
            struct instance_handle next = cn->next_sibling;
            if (cn->has_explicit_key && cn->explicit_key == key) {
                if (cn->kind == kind) matched = c;
                else { destroy_instance(c); }   /* key kept, type changed -> replace */
                break;
            }
            c = next;
        }
    } else {
        struct instance *ci = instance_resolve(expected);
        if (ci && ci->kind == kind && !ci->has_explicit_key && !ci->visited_this_run)
            matched = expected;
    }

    struct instance_handle inst;
    if (!instance_handle_is_null(matched)) { inst = matched; }
    else { inst = create_instance(cur->parent, kind, key, has_key); *created = true; }

    relink_after(cur->parent, inst, cur->insert_after);
    struct instance *n = instance_resolve(inst);
    if (n) { n->visited_this_run = true; n->has_explicit_key = has_key; n->explicit_key = key; }
    cur->insert_after = inst;
    return inst;
}

static void reset_children_unvisited(struct instance_handle parent) {
    struct instance *p = instance_resolve(parent);
    if (!p) return;
    for (struct instance_handle c = p->first_child; !instance_handle_is_null(c); ) {
        struct instance *cn = instance_resolve(c);
        if (!cn) break;
        cn->visited_this_run = false;
        c = cn->next_sibling;
    }
}

static void sweep_unvisited_children(struct instance_handle parent) {
    struct instance *p = instance_resolve(parent);
    if (!p) return;
    struct instance_handle c = p->first_child;
    while (!instance_handle_is_null(c)) {
        struct instance *cn = instance_resolve(c);
        if (!cn) break;
        struct instance_handle next = cn->next_sibling;
        if (!cn->visited_this_run) destroy_instance(c);
        c = next;
    }
}

/* enter a container: match/create it, then push a cursor for its children */
static struct instance_handle enter_container(enum instance_kind kind, uint64_t key, bool has_key) {
    bool created;
    struct instance_handle inst = match_or_create(kind, key, has_key, &created);
    reset_children_unvisited(inst);
    if (g_cursor_top + 1 < CURSOR_STACK_MAX)
        g_cursor[++g_cursor_top] = (struct reconcile_cursor){ inst, INSTANCE_HANDLE_NULL };
    else { g_cursor_over++; g_depth_overflowed = 1; }   /* clamp, and stay balanced */
    return inst;
}
static void exit_container(void) {
    if (g_cursor_over > 0) { g_cursor_over--; return; }
    if (g_cursor_top < 0) return;
    struct instance_handle parent = g_cursor[g_cursor_top].parent;
    sweep_unvisited_children(parent);
    g_cursor_top--;
}

static struct instance *cur_box(void) {
    if (g_cursor_top < 0) return 0;
    return instance_resolve(g_cursor[g_cursor_top].parent);
}

/* ------------------------------------------------------------------------- */
/* component re-running (§3)                                                  */
/* ------------------------------------------------------------------------- */

static void component_trampoline(void *ctx) {
    struct instance *inst = ctx;
    if (!inst || !inst->used) return;
    if (g_cursor_top + 1 >= CURSOR_STACK_MAX) return;
    g_cursor[++g_cursor_top] = (struct reconcile_cursor){ inst->self, INSTANCE_HANDLE_NULL };
    reset_children_unvisited(inst->self);
    if (inst->component_fn) inst->component_fn(inst->props_copy);
    sweep_unvisited_children(inst->self);
    g_cursor_top--;
}

void ui_component(void (*fn)(void *props), const void *props, size_t props_size, uint64_t key) {
    bool created;
    struct instance_handle h = match_or_create(INSTANCE_COMPONENT, key, key != 0, &created);
    struct instance *inst = instance_resolve(h);
    if (!inst) return;

    if (created) {
        inst->component_fn = fn;
        inst->props_copy = malloc(props_size ? props_size : 1);
        if (props && props_size) memcpy(inst->props_copy, props, props_size);
        inst->props_size = props_size;
        inst->scope = scope_create(component_trampoline, inst);
        scope_rerun(inst->scope);                                  /* (a) just created */
    } else {
        bool changed = (props_size != inst->props_size) ||
                       (props_size && memcmp(inst->props_copy, props, props_size) != 0);
        if (changed) {
            if (props_size != inst->props_size) { free(inst->props_copy); inst->props_copy = malloc(props_size ? props_size : 1); }
            if (props && props_size) memcpy(inst->props_copy, props, props_size);
            inst->props_size = props_size;
            scope_rerun(inst->scope);                              /* (b) props changed */
        } else {
            struct scope *sc = scope_resolve(inst->scope);
            if (sc && sc->dirty) scope_rerun(inst->scope);         /* (c) independently dirty */
        }
    }
}

/* ------------------------------------------------------------------------- */
/* containers + property setters                                             */
/* ------------------------------------------------------------------------- */

void ui_box_begin(uint64_t key) { enter_container(INSTANCE_BOX, key, key != 0); }
void ui_box_end(void) { exit_container(); }
/* cur_box() can be NULL -- at the depth clamp, and after an unbalanced end
 * that no longer corrupts anything but still has nowhere to write. Checked
 * here rather than trusted: this exact dereference is what turned a deep page
 * into a crash. */
void ui_begin_vstack(uint64_t key) {
    enter_container(INSTANCE_BOX, key, key != 0);
    struct instance *b = cur_box();
    if (!b) return;
    struct layout_node *ln = layout_resolve(g_la, b->layout_node);
    if (ln) ln->axis = AXIS_COLUMN;
}
void ui_begin_hstack(uint64_t key) {
    enter_container(INSTANCE_BOX, key, key != 0);
    struct instance *b = cur_box();
    if (!b) return;
    struct layout_node *ln = layout_resolve(g_la, b->layout_node);
    if (ln) ln->axis = AXIS_ROW;
}
void ui_end_stack(void) { exit_container(); }

int ui_depth_overflowed(void) { return g_depth_overflowed; }

unsigned long ui_relink_walks(void)      { return g_relink_walks; }
void          ui_relink_walks_reset(void) { g_relink_walks = 0; }

void ui_set_paint(struct paint p) {
    struct instance *b = cur_box(); if (!b) return;
    if (b->shadow.has_paint && memcmp(&b->shadow.paint, &p, sizeof p) == 0) return;
    b->shadow.paint = p; b->shadow.has_paint = true;
    scene_set_paint(g_sa, b->scene_node, &p);
    g_mutation_count++;
}
void ui_set_corner_radius(float r) {
    struct instance *b = cur_box(); if (!b) return;
    if (b->shadow.has_corner && b->shadow.corner_radius == r) return;
    b->shadow.corner_radius = r; b->shadow.has_corner = true;
    struct scene_node *sn = scene_resolve(g_sa, b->scene_node);
    if (sn) { sn->corner_radius = r; sn->dirty = true; }
    g_mutation_count++;
}
void ui_set_padding(float top, float right, float bottom, float left) {
    struct instance *b = cur_box(); if (!b) return;
    if (b->shadow.has_pad && b->shadow.pt==top && b->shadow.pr==right && b->shadow.pb==bottom && b->shadow.pl==left) return;
    b->shadow.pt=top; b->shadow.pr=right; b->shadow.pb=bottom; b->shadow.pl=left; b->shadow.has_pad=true;
    struct layout_node *ln = layout_resolve(g_la, b->layout_node);
    if (ln) { ln->padding_top=top; ln->padding_right=right; ln->padding_bottom=bottom; ln->padding_left=left; ln->dirty=true; }
    g_mutation_count++;
}
void ui_set_spacing(float s) {
    struct instance *b = cur_box(); if (!b) return;
    if (b->shadow.has_spacing && b->shadow.spacing == s) return;
    b->shadow.spacing = s; b->shadow.has_spacing = true;
    struct layout_node *ln = layout_resolve(g_la, b->layout_node);
    if (ln) { ln->spacing = s; ln->dirty = true; }
    g_mutation_count++;
}
void ui_set_size(struct layout_size w, struct layout_size h) {
    struct instance *b = cur_box(); if (!b) return;
    if (b->shadow.has_size && memcmp(&b->shadow.sw,&w,sizeof w)==0 && memcmp(&b->shadow.shh,&h,sizeof h)==0) return;
    b->shadow.sw = w; b->shadow.shh = h; b->shadow.has_size = true;
    struct layout_node *ln = layout_resolve(g_la, b->layout_node);
    if (ln) { ln->width = w; ln->height = h; ln->dirty = true; }
    g_mutation_count++;
}
void ui_set_size_bounds(float min_w, float max_w, float min_h, float max_h) {
    struct instance *b = cur_box(); if (!b) return;
    struct layout_node *ln = layout_resolve(g_la, b->layout_node);
    if (!ln) return;
    ln->width.min_size  = min_w; ln->width.max_size  = max_w;
    ln->height.min_size = min_h; ln->height.max_size = max_h;
    ln->dirty = true;
}
void ui_set_shadow(bool enabled, float dx, float dy, float blur, struct color color) {
    struct instance *b = cur_box(); if (!b) return;
    /* route through the guarded scene setter so an unchanged shadow (re-applied
     * every frame by an immediate-mode kit) does NOT re-dirty the node -- this is
     * the card's big blurred shadow; re-dirtying it repaints the whole card. */
    scene_set_shadow(g_sa, b->scene_node, enabled, dx, dy, blur, color);
}
void ui_set_backdrop_blur(bool enabled, float radius) {
    struct instance *b = cur_box(); if (!b) return;
    scene_set_backdrop_blur(g_sa, b->scene_node, enabled, radius);
}
void ui_set_opacity(float opacity) {
    struct instance *b = cur_box(); if (!b) return;
    /* guarded scene setter: unchanged opacity does not re-dirty, so a group at
     * full opacity (the steady state after a transition) costs nothing. */
    scene_set_opacity(g_sa, b->scene_node, opacity);
}
void ui_set_offset(float x, float y) {
    struct instance *b = cur_box(); if (!b) return;
    /* post-layout translate: layout adds this to the resolved position when it
     * writes the scene transform, so (0,0) is a no-op and layout's guarded
     * scene_set_transform means an unchanged offset doesn't re-dirty. */
    struct layout_node *ln = layout_resolve(g_la, b->layout_node);
    if (ln) { ln->offset_x = x; ln->offset_y = y; }
}
void ui_set_insets(float top, float right, float bottom, float left,
                   unsigned set, bool relative) {
    struct instance *b = cur_box(); if (!b) return;
    struct layout_node *ln = layout_resolve(g_la, b->layout_node);
    if (!ln) return;
    ln->ins_top = top; ln->ins_right = right;
    ln->ins_bottom = bottom; ln->ins_left = left;
    ln->ins_set = (unsigned char)set;
    ln->relative = relative ? 1 : 0;
}

void ui_set_clip_children(bool clip) {
    struct instance *b = cur_box(); if (!b) return;
    if (b->shadow.has_clip && b->shadow.clip == clip) return;
    b->shadow.clip = clip; b->shadow.has_clip = true;
    scene_set_clip_children(g_sa, b->scene_node, clip);
    g_mutation_count++;
}
/* Mark the open box as an overlay: it fills its parent and is excluded from the
 * parent's flow (modals/popovers paint on top without disturbing layout). */
/* Raise the OPEN box (and its whole subtree) to z-layer `l`: 0 = flow,
 * 1 = popovers/dialog scrims, 2 = drag ghosts. Drives paint AND hit together
 * (scene.h `layer`). */
void ui_set_layer(int l) {
    struct instance *b = cur_box(); if (!b) return;
    scene_set_layer(g_sa, b->scene_node, (uint8_t)(l < 0 ? 0 : l > 3 ? 3 : l));
}

void ui_set_overlay(bool on) {
    struct instance *b = cur_box(); if (!b) return;
    struct layout_node *ln = layout_resolve(g_la, b->layout_node);
    if (ln && ln->is_overlay != on) { ln->is_overlay = on; ln->dirty = true; }
}
void ui_set_border(float width, struct color color) {
    struct instance *b = cur_box(); if (!b) return;
    scene_set_border(g_sa, b->scene_node, width, color);
}
void ui_set_border_gradient(float width, const struct paint *paint) {
    struct instance *b = cur_box(); if (!b) return;
    scene_set_border_gradient(g_sa, b->scene_node, width, paint);
}
void ui_set_axis(enum layout_axis a) {
    struct instance *b = cur_box(); if (!b) return;
    struct layout_node *ln = layout_resolve(g_la, b->layout_node);
    if (ln) { ln->axis = a; ln->dirty = true; }
}
void ui_set_wrap(bool wrap) {
    struct instance *b = cur_box(); if (!b) return;
    struct layout_node *ln = layout_resolve(g_la, b->layout_node);
    if (ln) { ln->wrap = wrap; ln->dirty = true; }
}
void ui_set_grid(int cols, float col_gap, float row_gap) {
    struct instance *b = cur_box(); if (!b) return;
    struct layout_node *ln = layout_resolve(g_la, b->layout_node);
    if (ln) { ln->grid_cols = cols; ln->grid_col_gap = col_gap; ln->grid_row_gap = row_gap; ln->dirty = true; }
}
void ui_set_grid_tracks(const unsigned char *mode, const float *val, int n) {
    struct instance *b = cur_box(); if (!b) return;
    struct layout_node *ln = layout_resolve(g_la, b->layout_node);
    if (!ln) return;
    if (n > LAYOUT_TRACKS) n = LAYOUT_TRACKS;
    if (n < 0) n = 0;
    for (int i = 0; i < n; i++) { ln->grid_track_mode[i] = mode[i]; ln->grid_track_val[i] = val[i]; }
    ln->grid_ntrack = n;
    ln->dirty = true;
}

void ui_set_pos_container(int on) {
    struct instance *b = cur_box(); if (!b) return;
    struct layout_node *ln = layout_resolve(g_la, b->layout_node);
    if (ln) { ln->pos_container = on ? 1 : 0; ln->dirty = true; }
}

void ui_set_grid_place(int row, int col, int rowspan, int colspan) {
    struct instance *b = cur_box(); if (!b) return;
    struct layout_node *ln = layout_resolve(g_la, b->layout_node);
    if (!ln) return;
    ln->grid_row = row; ln->grid_col = col;
    ln->grid_rowspan = rowspan > 0 ? rowspan : 1;
    ln->grid_span = colspan > 0 ? colspan : 1;
    ln->dirty = true;
}

void ui_set_grid_span(int span) {   /* on the CHILD box */
    struct instance *b = cur_box(); if (!b) return;
    struct layout_node *ln = layout_resolve(g_la, b->layout_node);
    if (ln) { ln->grid_span = span; ln->dirty = true; }
}
void ui_set_justify(enum layout_justify j) {
    struct instance *b = cur_box(); if (!b) return;
    struct layout_node *ln = layout_resolve(g_la, b->layout_node);
    if (ln) { ln->justify = j; ln->dirty = true; }
}
void ui_set_align(enum layout_align a) {
    struct instance *b = cur_box(); if (!b) return;
    struct layout_node *ln = layout_resolve(g_la, b->layout_node);
    if (ln) { ln->align = a; ln->dirty = true; }
}
/* Column scroll: shift the open box's children up by `dy` px (pairs with
 * ui_set_clip_children to make a scroll viewport). Re-layouts only on change. */
void ui_set_scroll_offset(float dy) {
    struct instance *b = cur_box(); if (!b) return;
    struct layout_node *ln = layout_resolve(g_la, b->layout_node);
    if (ln && ln->scroll_offset != dy) { ln->scroll_offset = dy; ln->dirty = true; }
}
/* Open box's content height (sum of children, unclamped) and viewport height
 * (resolved) from the last layout -- a scroll view uses these to clamp its
 * offset. We sum children rather than read intrinsic_h because a scroll viewport
 * has a FIXED height, so its own intrinsic_h is that fixed size, not the content. */
bool ui_open_content_extent(float *content_h, float *viewport_h) {
    struct instance *b = cur_box(); if (!b) return false;
    struct layout_node *ln = layout_resolve(g_la, b->layout_node);
    if (!ln) return false;
    float sum = 0; int cnt = 0;
    for (struct layout_handle c = ln->first_child; !layout_handle_is_null(c); ) {
        struct layout_node *cn = layout_resolve(g_la, c);
        if (!cn) break;
        sum += cn->intrinsic_h; cnt++;
        c = cn->next_sibling;
    }
    if (cnt > 1) sum += ln->spacing * (cnt - 1);
    sum += ln->padding_top + ln->padding_bottom;
    if (content_h)  *content_h  = sum;
    if (viewport_h) *viewport_h = ln->resolved_h;
    return true;
}
void ui_set_text_color(struct color c) { g_text_color = c; }
void ui_set_text_bg(struct color c) { g_text_bg = c; }

/* One-shot gradient for the NEXT text: set just before ui_text(), consumed and
 * cleared by it (so it never leaks to later text). PAINT_NONE = flat color. */
static struct paint g_text_paint;   /* zero-init -> PAINT_NONE */
void ui_set_text_gradient(const struct paint *p) {
    g_text_paint = p ? *p : (struct paint){0};
}

/* ------------------------------------------------------------------------- */
/* text + button + spacer                                                    */
/* ------------------------------------------------------------------------- */

static void set_text_on(struct instance_handle h, const char *str) {
    struct instance *n = instance_resolve(h);
    if (!n) return;
    /* BEFORE the no-op guard below: a run whose only change is being selected
     * has identical text, font and colour, so the guard would return before
     * ever applying it -- and the highlight would appear on the next unrelated
     * edit instead of on the click.
     *
     * Only ever SETS, never clears. Clearing here would make every build wipe
     * the highlight and every post-layout pass write it back, so each selected
     * run would be marked content-dirty TWICE a frame -- turning a live
     * selection into a full repaint per frame, which is the exact cost the
     * dirty-rect path exists to avoid. Whoever set a background owns removing
     * it. */
    if (g_text_bg.a > 0) {
        scene_set_text_bg(g_sa, n->scene_node, g_text_bg);
        g_text_bg.a = 0;
    }
    bool grad = (g_text_paint.kind != PAINT_NONE);
    if (!grad && n->shadow.has_text && strncmp(n->shadow.text, str, sizeof n->shadow.text) == 0 &&
        n->shadow.font_handle == g_font && n->shadow.text_size == g_text_size &&
        memcmp(&n->shadow.text_color, &g_text_color, sizeof(struct color)) == 0)
        return;   /* no-op skip (never while a gradient is pending) */
    strncpy(n->shadow.text, str, sizeof n->shadow.text - 1);
    n->shadow.text[sizeof n->shadow.text - 1] = 0;
    n->shadow.font_handle = g_font; n->shadow.text_size = g_text_size; n->shadow.text_color = g_text_color;
    n->shadow.has_text = true;
    /* the scene node owns the string; point it at our durable shadow copy.
     * NOTE: that copy was just overwritten IN PLACE, so scene_set_text's own
     * no-op guard sees identical pointers+bytes -- force the dirty explicitly
     * (our strncmp above already proved the content actually changed). */
    scene_set_text(g_sa, n->scene_node, n->shadow.text, g_font, g_text_size, g_text_color);
    if (grad) scene_set_text_gradient(g_sa, n->scene_node, &g_text_paint);
    scene_mark_dirty(g_sa, n->scene_node);
    g_text_paint.kind = PAINT_NONE;   /* one-shot: consumed */
    g_mutation_count++;
}

void ui_text(const char *fmt, ...) {
    char buf[256]; va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    bool created; struct instance_handle h = match_or_create(INSTANCE_TEXT, 0, false, &created);
    set_text_on(h, buf);
}
void ui_text_keyed(uint64_t key, const char *fmt, ...) {
    char buf[256]; va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    bool created; struct instance_handle h = match_or_create(INSTANCE_TEXT, key, key != 0, &created);
    set_text_on(h, buf);
}

void ui_spacer(void) {
    enter_container(INSTANCE_BOX, 0, false);
    struct instance *b = cur_box();
    struct layout_node *ln = b ? layout_resolve(g_la, b->layout_node) : 0;
    if (ln) { ln->width.mode = SIZE_FLEX; ln->width.flex_grow = 1;
              ln->height.mode = SIZE_FLEX; ln->height.flex_grow = 1; }
    exit_container();
}

/* An image leaf: a bitmap (premultiplied BGRA) scaled to fill a grow-width row
 * of the given height. The caller owns `pixels` and must keep it alive as long
 * as the node references it. scene_set_image guards on the pointer, so passing
 * a stable buffer re-renders only when the pointer/dims change (fine for a chart
 * with fixed data drawn into a persistent buffer). */
void ui_image(uint64_t key, const void *pixels, uint32_t iw, uint32_t ih, float height_px) {
    bool created; struct instance_handle h = match_or_create(INSTANCE_IMAGE, key, key != 0, &created);
    struct instance *n = instance_resolve(h);
    if (!n) return;
    struct layout_node *ln = layout_resolve(g_la, n->layout_node);
    if (ln) {
        ln->width.mode  = SIZE_FLEX;  ln->width.flex_grow = 1;
        ln->height.mode = SIZE_FIXED; ln->height.fixed_value = height_px;
    }
    scene_set_image(g_sa, n->scene_node, pixels, iw, ih, EMBK_PIXFMT_BGRA8888_PRE);
}

void ui_image_sized(uint64_t key, const void *pixels, uint32_t iw, uint32_t ih,
                    float width_px, float height_px) {
    bool created; struct instance_handle h = match_or_create(INSTANCE_IMAGE, key, key != 0, &created);
    struct instance *n = instance_resolve(h);
    if (!n) return;
    struct layout_node *ln = layout_resolve(g_la, n->layout_node);
    if (ln) {
        ln->width.mode = SIZE_FIXED;   ln->width.fixed_value = width_px;
        ln->height.mode = SIZE_FIXED;  ln->height.fixed_value = height_px;
    }
    scene_set_image(g_sa, n->scene_node, pixels, iw, ih, EMBK_PIXFMT_BGRA8888_PRE);
}

/* As ui_image_sized, but draws the image as a stencil filled with `tint` (the
 * image's alpha is the coverage) -- what lets a single-colour icon follow the
 * theme the way a text glyph does. The tint is passed here rather than set on
 * "the open box" because an image is a LEAF: it is created without being
 * opened, so a set-on-open-box call would land on its parent container. */
void ui_image_sized_tinted(uint64_t key, const void *pixels, uint32_t iw, uint32_t ih,
                           float width_px, float height_px, struct color tint) {
    bool created; struct instance_handle h = match_or_create(INSTANCE_IMAGE, key, key != 0, &created);
    struct instance *n = instance_resolve(h);
    if (!n) return;
    struct layout_node *ln = layout_resolve(g_la, n->layout_node);
    if (ln) {
        ln->width.mode = SIZE_FIXED;   ln->width.fixed_value = width_px;
        ln->height.mode = SIZE_FIXED;  ln->height.fixed_value = height_px;
    }
    scene_set_image(g_sa, n->scene_node, pixels, iw, ih, EMBK_PIXFMT_BGRA8888_PRE);
    scene_set_image_tint(g_sa, n->scene_node, true, tint);
}

void ui_image_fill(uint64_t key, const void *pixels, uint32_t iw, uint32_t ih) {
    bool created; struct instance_handle h = match_or_create(INSTANCE_IMAGE, key, key != 0, &created);
    struct instance *n = instance_resolve(h);
    if (!n) return;
    struct layout_node *ln = layout_resolve(g_la, n->layout_node);
    if (ln) {
        ln->width.mode = SIZE_FLEX; ln->width.flex_grow = 1;
        ln->height.mode = SIZE_FLEX; ln->height.flex_grow = 1;
        ln->is_overlay = true;
    }
    scene_set_image(g_sa, n->scene_node, pixels, iw, ih, EMBK_PIXFMT_BGRA8888_PRE);
}

static bool button_common(uint64_t key, bool has_key, const char *label) {
    struct instance_handle b = enter_container(INSTANCE_BOX, key, has_key);
    ui_text("%s", label);
    exit_container();
    return ui_consume_click(b);   /* click on b or any descendant (the label) */
}
bool ui_button(const char *label) { return button_common(0, false, label); }
bool ui_button_keyed(uint64_t key, const char *label) { return button_common(key, key != 0, label); }

/* ------------------------------------------------------------------------- */
/* driver                                                                    */
/* ------------------------------------------------------------------------- */

void ui_set_font(uint32_t fh) { g_font = fh; }
void ui_set_text_size(float px) { g_text_size = px; }

void ui_init(struct scene_arena *sa, struct layout_arena *la) {
    g_sa = sa; g_la = la; g_cursor_top = -1; g_mutation_count = 0;
    g_hovered = INSTANCE_HANDLE_NULL; g_clicked = INSTANCE_HANDLE_NULL;
    g_active = INSTANCE_HANDLE_NULL; g_focused = INSTANCE_HANDLE_NULL; g_ptr_down = false;
    g_press_edge = false; g_focus_claimed = false; g_input_n = 0; g_wheel = 0.0f;
    struct instance_handle h = inst_alloc();
    struct instance *n = instance_resolve(h);
    n->kind = INSTANCE_BOX;
    n->scene_node  = scene_create_node(sa, SCENE_NODE_GROUP, NODE_HANDLE_NULL);
    n->layout_node = layout_create_node(la, LAYOUT_HANDLE_NULL);
    struct layout_node *ln = layout_resolve(la, n->layout_node);
    if (ln) {
        ln->scene_node = n->scene_node; ln->is_container = true;
        /* The root is a stretch-aligned COLUMN: a full-size top-level view
         * (Screen/Window with grow sizing) then actually fills the canvas --
         * default (START, no stretch) arranged it at its INTRINSIC height, so
         * "fill the window" roots silently top-packed once their content was
         * shorter than the canvas (exposed by V4's bottom-anchored TabView). */
        ln->axis = AXIS_COLUMN;
        ln->align = ALIGN_STRETCH;
    }
    struct scene_node *sn = scene_resolve(sa, n->scene_node);
    if (sn) sn->user_data = pack_handle(h);
    g_root = h;
}
struct instance_handle ui_root(void) { return g_root; }

struct instance_handle ui_open(void) {
    if (g_cursor_top < 0) return INSTANCE_HANDLE_NULL;
    return g_cursor[g_cursor_top].parent;
}

/* true iff `anc` is `node` or one of its ancestors -- so a hover/click on a
 * button's inner text also counts as a hover/click on the button box. */
static bool is_ancestor_or_self(struct instance_handle anc, struct instance_handle node) {
    if (instance_handle_is_null(anc)) return false;
    struct instance_handle h = node;
    while (!instance_handle_is_null(h)) {
        if (h.index == anc.index && h.generation == anc.generation) return true;
        struct instance *n = instance_resolve(h);
        if (!n) break;
        h = n->parent;
    }
    return false;
}

bool ui_consume_click(struct instance_handle h) {
    if (is_ancestor_or_self(h, g_clicked)) { g_clicked = INSTANCE_HANDLE_NULL; return true; }
    return false;
}
bool ui_is_hovered(void) { return is_ancestor_or_self(ui_open(), g_hovered); }
bool ui_is_pressed(void) { return g_ptr_down && is_ancestor_or_self(ui_open(), g_hovered); }
/* The button state on its own. ui_is_pressed answers "is THIS widget being
 * pressed", which a drag spanning many widgets cannot ask. */
bool ui_pointer_down(void) { return g_ptr_down; }

void ui_frame_begin(void) {
    g_depth_overflowed = 0; g_cursor_over = 0;
    g_cursor_top = -1;
    reset_children_unvisited(g_root);
    g_cursor[++g_cursor_top] = (struct reconcile_cursor){ g_root, INSTANCE_HANDLE_NULL };
}
void ui_frame_end(void) {
    sweep_unvisited_children(g_root);
    g_cursor_top = -1;
    /* a press that landed on no focusable widget clears keyboard focus */
    if (g_press_edge && !g_focus_claimed) g_focused = INSTANCE_HANDLE_NULL;
    g_press_edge = false;
    g_focus_claimed = false;
    g_input_n = 0;                 /* typed queue is consumed within the frame */
    g_wheel = 0.0f;                /* unconsumed wheel delta does not carry over */
}
void ui_run_layout(float W, float H) {
    struct instance *r = instance_resolve(g_root);
    if (r) layout_run(g_la, g_sa, r->layout_node, W, H);
}

/* --- navigation accessors (tests) --- */
struct instance_handle ui_first_child(struct instance_handle h) {
    struct instance *n = instance_resolve(h); return n ? n->first_child : INSTANCE_HANDLE_NULL;
}
struct instance_handle ui_next_sibling(struct instance_handle h) {
    struct instance *n = instance_resolve(h); return n ? n->next_sibling : INSTANCE_HANDLE_NULL;
}
/* The scene the declarative layer is building into. Exposed so a caller that
 * must reason about RESOLVED geometry -- where layout actually put things --
 * can walk it after the layout pass. */
struct scene_arena *ui_scene_arena(void) { return g_sa; }

struct node_handle ui_scene_of(struct instance_handle h) {
    struct instance *n = instance_resolve(h); struct node_handle z = {0,0}; return n ? n->scene_node : z;
}
struct layout_handle ui_layout_of(struct instance_handle h) {
    struct instance *n = instance_resolve(h); struct layout_handle z = {0,0}; return n ? n->layout_node : z;
}

/* ------------------------------------------------------------------------- */
/* hit-testing (§7, clip-aware)                                              */
/* ------------------------------------------------------------------------- */

struct hit_ctx {
    float px, py;
    struct node_handle best[4];   /* topmost hit per z-layer (HIT_LAYERS) */
    bool  found[4];
};

static void node_world_rect(const struct scene_node *n, const float world[16],
                            float *x0, float *y0, float *x1, float *y1) {
    float cx[4] = { 0, n->width, 0, n->width };
    float cy[4] = { 0, 0, n->height, n->height };
    float minx=1e30f,miny=1e30f,maxx=-1e30f,maxy=-1e30f;
    for (int i = 0; i < 4; i++) {
        float X = world[0]*cx[i] + world[4]*cy[i] + world[12];
        float Y = world[1]*cx[i] + world[5]*cy[i] + world[13];
        if (X<minx) minx=X;
        if (X>maxx) maxx=X;
        if (Y<miny) miny=Y;
        if (Y>maxy) maxy=Y;
    }
    *x0=minx; *y0=miny; *x1=maxx; *y1=maxy;
}
static int pt_in(float px, float py, float x0, float y0, float x1, float y1) {
    return px >= x0 && px < x1 && py >= y0 && py < y1;
}

#define HIT_LAYERS 4
static void hit_rec(struct hit_ctx *c, struct node_handle h, const float wparent[16],
                    float cx0, float cy0, float cx1, float cy1, int layer) {
    struct scene_node *n = scene_resolve(g_sa, h);
    if (!n) return;
    float local[16], world[16];
    scene_trs_to_matrix(n, local);
    scene_mat4_mul(wparent, local, world);

    /* the subtree's effective z-layer: a node can raise, never lower (same
     * semantics the renderer paints by -- see scene.h `layer`) */
    if (n->layer > layer) layer = n->layer;
    int L = layer >= HIT_LAYERS ? HIT_LAYERS - 1 : layer;

    /* an elevated subtree escapes ancestor clips exactly as it escapes them
     * when painting -- so a dropdown hanging below its clipped strip is
     * clickable everywhere it is VISIBLE */
    float ecx0 = cx0, ecy0 = cy0, ecx1 = cx1, ecy1 = cy1;
    if (n->layer > 0) { ecx0 = -1e30f; ecy0 = -1e30f; ecx1 = 1e30f; ecy1 = 1e30f; }

    float x0,y0,x1,y1; node_world_rect(n, world, &x0,&y0,&x1,&y1);
    int in_bounds = pt_in(c->px, c->py, x0, y0, x1, y1);
    int in_clip   = pt_in(c->px, c->py, ecx0, ecy0, ecx1, ecy1);
    if (in_bounds && in_clip) {                   /* topmost-in-layer wins */
        c->best[L] = h; c->found[L] = true;
    }

    /* descendant clip = current clip intersected with THIS node's rect if it clips */
    float nx0=ecx0, ny0=ecy0, nx1=ecx1, ny1=ecy1;
    if (n->clip_children) {
        if (x0 > nx0) nx0 = x0;
        if (y0 > ny0) ny0 = y0;
        if (x1 < nx1) nx1 = x1;
        if (y1 < ny1) ny1 = y1;
    }
    for (struct node_handle ch = n->first_child; ; ) {
        struct scene_node *cn = scene_resolve(g_sa, ch);
        if (!cn) break;
        struct node_handle next = cn->next_sibling;
        hit_rec(c, ch, world, nx0, ny0, nx1, ny1, layer);
        ch = next;
    }
}

/* Hit-test the retained tree (last frame's geometry) for the deepest instance
 * under (px,py). Returns INSTANCE_HANDLE_NULL if nothing is hit. */
static struct instance_handle instance_at(float px, float py) {
    struct instance *r = instance_resolve(g_root);
    if (!r) return INSTANCE_HANDLE_NULL;
    struct hit_ctx c; c.px = px; c.py = py;
    for (int i = 0; i < 4; i++) { c.best[i].index = 0; c.best[i].generation = 0; c.found[i] = false; }
    float ident[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    float huge = 1e30f;
    hit_rec(&c, r->scene_node, ident, -huge, -huge, huge, huge, 0);
    /* the highest layer that claims the point wins -- what you SEE on top is
     * what your click lands on, because paint uses the same field */
    int L = -1;
    for (int i = 3; i >= 0; i--) if (c.found[i]) { L = i; break; }
    if (L < 0) return INSTANCE_HANDLE_NULL;
    struct scene_node *sn = scene_resolve(g_sa, c.best[L]);
    if (!sn) return INSTANCE_HANDLE_NULL;
    struct instance *inst = instance_resolve(unpack_handle(sn->user_data));
    return inst ? inst->self : INSTANCE_HANDLE_NULL;
}

void ui_dispatch_click(float px, float py) { g_clicked = instance_at(px, py); }

/* Feed one frame of pointer state (screen->surface-local coords already
 * applied by the caller). Updates hover, and fires a click on the press
 * (button 0->1) edge over whatever the pointer is on. */
void ui_pointer(float x, float y, bool down) {
    g_ptr_x = x; g_ptr_y = y;
    g_hovered = instance_at(x, y);
    if (down && !g_ptr_down) {           /* press edge */
        g_clicked = g_hovered;
        g_active  = g_hovered;           /* capture: this widget owns the drag until release */
        g_press_edge = true;             /* for defocus when the press misses every field */
    }
    if (!down) g_active = INSTANCE_HANDLE_NULL;   /* release edge frees the capture */
    g_ptr_down = down;
}

/* --- keyboard focus + typed-input queue (text fields) ------------------- */

/* Enqueue a typed character for this frame's focused field. Called by the event
 * loop for each byte from embk_key_poll(). */
void ui_input_char(int c) {
    if (c && g_input_n < (int)sizeof(g_input_buf)) g_input_buf[g_input_n++] = (char)c;
}
/* Drain the queued characters into dst (up to max); returns the count. Does not
 * clear the queue (ui_frame_end does) so only the focused field applies them. */
int ui_input_take(char *dst, int max) {
    int n = g_input_n < max ? g_input_n : max;
    for (int i = 0; i < n; i++) dst[i] = g_input_buf[i];
    return n;
}
void ui_request_focus(struct instance_handle h) { g_focused = h; g_focus_claimed = true; }
bool ui_any_focus(void) { return !instance_handle_is_null(g_focused); }
bool ui_has_focus(struct instance_handle h) {
    return h.index == g_focused.index && h.generation == g_focused.generation && h.index != 0;
}

/* Scroll wheel: the loop feeds this frame's delta; the open box consumes it only
 * when the pointer is over it (so scrolling targets the view under the cursor). */
void ui_wheel(float dy) { g_wheel += dy; }
float ui_take_wheel(void) {
    if (g_wheel != 0.0f && is_ancestor_or_self(ui_open(), g_hovered)) {
        float w = g_wheel; g_wheel = 0.0f; return w;
    }
    return 0.0f;
}

/* Read the live pointer position (surface-local). Widgets that map the pointer
 * into their own box -- sliders, scroll thumbs -- need the raw coordinate. */
void ui_pointer_pos(float *x, float *y) {
    if (x) *x = g_ptr_x;
    if (y) *y = g_ptr_y;
}

/* Is the currently-open box the drag owner (pressed on it, button still held)?
 * Unlike ui_is_pressed this stays true even if the pointer slides off the box
 * mid-drag -- pointer capture, exactly what a slider/scrollbar needs. */
bool ui_is_active(void) { return g_ptr_down && is_ancestor_or_self(ui_open(), g_active); }

/* World rect (last frame's arranged geometry) of an instance's scene node.
 * Pure-translation scene, so we compose the ancestor transforms top-down. */
static bool instance_world_rect(struct instance_handle h,
                                float *ox, float *oy, float *ow, float *oh) {
    struct instance *inst = instance_resolve(h);
    if (!inst) return false;
    struct node_handle chain[64];
    int n = 0;
    for (struct node_handle nh = inst->scene_node; !node_handle_is_null(nh) && n < 64; ) {
        chain[n++] = nh;
        struct scene_node *sn = scene_resolve(g_sa, nh);
        if (!sn) break;
        nh = sn->parent;
    }
    float world[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    for (int i = n - 1; i >= 0; i--) {
        struct scene_node *sn = scene_resolve(g_sa, chain[i]);
        if (!sn) return false;
        float local[16], nw[16];
        scene_trs_to_matrix(sn, local);
        scene_mat4_mul(world, local, nw);
        for (int k = 0; k < 16; k++) world[k] = nw[k];
    }
    struct scene_node *sn = scene_resolve(g_sa, inst->scene_node);
    if (!sn) return false;
    float x0, y0, x1, y1;
    node_world_rect(sn, world, &x0, &y0, &x1, &y1);
    if (ox) *ox = x0;
    if (oy) *oy = y0;
    if (ow) *ow = x1 - x0;
    if (oh) *oh = y1 - y0;
    return true;
}

/* World rect of a specific instance / of the currently-open box. */
bool ui_rect_of(struct instance_handle h, float *x, float *y, float *w, float *ht) {
    return instance_world_rect(h, x, y, w, ht);
}
bool ui_open_rect(float *x, float *y, float *w, float *ht) {
    return instance_world_rect(ui_open(), x, y, w, ht);
}

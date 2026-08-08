/* ui/backend/scene_render.c -- EmbLink UI Piece 4a: the traversal driver
 * (see scene_render.h). Implements Section 5 (grouped opacity) and Section 6
 * (the dirty-rect algorithm, specified as explicit steps precisely because
 * it's easy to get subtly wrong). */

#include "scene_render.h"
#if defined(SCENE_TRACE) || defined(SCROLL_DEBUG)
#include <stdio.h>
#endif
#include <stdlib.h>
#include <math.h>
#include <string.h>


/* ------------------------------------------------------------------------- */
/* geometry helpers                                                          */
/* ------------------------------------------------------------------------- */

struct frect { float x, y, w, h; };   /* w<=0 || h<=0 == empty */

static struct frect frect_empty(void) { struct frect f = {0,0,0,0}; return f; }
static int frect_is_empty(struct frect f) { return f.w <= 0 || f.h <= 0; }

static struct frect frect_union(struct frect a, struct frect b) {
    if (frect_is_empty(a)) return b;
    if (frect_is_empty(b)) return a;
    float x0 = a.x < b.x ? a.x : b.x;
    float y0 = a.y < b.y ? a.y : b.y;
    float x1 = (a.x + a.w) > (b.x + b.w) ? (a.x + a.w) : (b.x + b.w);
    float y1 = (a.y + a.h) > (b.y + b.h) ? (a.y + a.h) : (b.y + b.h);
    struct frect f = { x0, y0, x1 - x0, y1 - y0 };
    return f;
}
static int frect_overlap(struct frect a, struct frect b) {
    if (frect_is_empty(a) || frect_is_empty(b)) return 0;
    return !(a.x + a.w <= b.x || b.x + b.w <= a.x ||
             a.y + a.h <= b.y || b.y + b.h <= a.y);
}

/* World-space axis-aligned bounding box of a node's local (0,0,w,h) rect. For
 * pure translate+scale this is exact; for rotation it's the rotated rect's
 * AABB (fine for the CPU backend, which draws axis-aligned rects). */
static struct frect node_world_aabb(const struct scene_node *n, const float world[16]) {
    float cx[4] = { 0, n->width, 0, n->width };
    float cy[4] = { 0, 0, n->height, n->height };
    float minx = 1e30f, miny = 1e30f, maxx = -1e30f, maxy = -1e30f;
    for (int i = 0; i < 4; i++) {
        float X = world[0] * cx[i] + world[4] * cy[i] + world[12];
        float Y = world[1] * cx[i] + world[5] * cy[i] + world[13];
        if (X < minx) minx = X;
        if (X > maxx) maxx = X;
        if (Y < miny) miny = Y;
        if (Y > maxy) maxy = Y;
    }
    struct frect f = { minx, miny, maxx - minx, maxy - miny };
    return f;
}

/* Union of a whole subtree's world AABBs (Section 5's group bounding rect). */
static int g_sb_depth;
static struct frect subtree_bounds(struct scene_arena *a, struct node_handle h,
                                   const float wparent[16]) {
    struct scene_node *n = scene_resolve(a, h);
    if (!n || g_sb_depth > 1024) return frect_empty();
    g_sb_depth++;
    float local[16], world[16];
    scene_trs_to_matrix(n, local);
    scene_mat4_mul(wparent, local, world);
    struct frect b = node_world_aabb(n, world);
    struct node_handle c = n->first_child;
    uint32_t guard = 512;   /* cap sibling walks (cycle-safe) */
    while (!node_handle_is_null(c) && guard-- != 0) {
        struct scene_node *cn = scene_resolve(a, c);
        struct node_handle next = cn ? cn->next_sibling : NODE_HANDLE_NULL;
        b = frect_union(b, subtree_bounds(a, c, world));
        c = next;
    }
    g_sb_depth--;
    return b;
}

/* ------------------------------------------------------------------------- */
/* dirty-rect accumulation (Section 6)                                       */
/* ------------------------------------------------------------------------- */

static void dirty_add(struct scene_renderer *r, struct frect f) {
    if (r->full || frect_is_empty(f)) return;
    if (r->n_dirty >= 16) { r->full = 1; return; }   /* Section 0 cap -> full-screen */
    struct clip_rect c = { f.x, f.y, f.w, f.h, 0.0f };
    r->dirty[r->n_dirty++] = c;
}
static int dirty_hits(struct scene_renderer *r, struct frect f) {
    if (r->full) return 1;
    for (int i = 0; i < r->n_dirty; i++) {
        struct frect d = { r->dirty[i].x, r->dirty[i].y, r->dirty[i].w, r->dirty[i].h };
        if (frect_overlap(f, d)) return 1;
    }
    return 0;
}

/* one gathered node (document order == paint order) */
struct noderec {
    struct node_handle h;
    struct frect       aabb;
    struct frect       foot;    /* aabb grown by the shadow's reach -- what the
                                 * node actually STAINS on the target, and
                                 * therefore what its ghost occupies when it
                                 * moves or dies */
    int                is_blur;
    int                has_group;
    struct frect       group_bounds;
    int                dirty;
    /* scroll-detection scratch (Step 1): the cached old footprint, and the
     * classification of WHY this node is dirty -- a pure move can be blitted,
     * a content change cannot. */
    struct frect       old;
    int                has_old;
    int                own_dirty;
    int                moved;
    int                clips;
    /* What this node can actually STAIN: `foot` intersected with every
     * clipping ancestor. A node scrolled past the bottom of its container has
     * a footprint below the clip and NOTHING visible there -- the scroll
     * classifier has to reason about the latter, or a document taller than its
     * viewport permanently vetoes its own fast path. Empty when fully clipped,
     * and frect_overlap treats empty as touching nothing. */
    struct frect       vis;
    struct frect       clipbox;      /* the ancestor clip itself, for the OLD foot */
    int                has_clipbox;
};

/* a ∩ b, empty if they miss */
static struct frect frect_isect(struct frect a, struct frect b) {
    float x0 = a.x > b.x ? a.x : b.x, y0 = a.y > b.y ? a.y : b.y;
    float x1 = a.x + a.w < b.x + b.w ? a.x + a.w : b.x + b.w;
    float y1 = a.y + a.h < b.y + b.h ? a.y + a.h : b.y + b.h;
    if (x1 <= x0 || y1 <= y0) return frect_empty();
    struct frect r = { x0, y0, x1 - x0, y1 - y0 };
    return r;
}

/* the painted footprint: the AABB grown by shadow reach (mirrors paint_visual) */
static struct frect node_footprint(const struct scene_node *n, struct frect ab) {
    if (!n->shadow_enabled) return ab;
    float m  = n->shadow_blur_radius + 2.0f;
    float x0 = ab.x, y0 = ab.y, x1 = ab.x + ab.w, y1 = ab.y + ab.h;
    float sx0 = ab.x + n->shadow_dx - m,        sy0 = ab.y + n->shadow_dy - m;
    float sx1 = ab.x + ab.w + n->shadow_dx + m, sy1 = ab.y + ab.h + n->shadow_dy + m;
    if (sx0 < x0) x0 = sx0;
    if (sy0 < y0) y0 = sy0;
    if (sx1 > x1) x1 = sx1;
    if (sy1 > y1) y1 = sy1;
    struct frect f = { x0, y0, x1 - x0, y1 - y0 };
    return f;
}

static void gather(struct scene_arena *a, struct node_handle h, const float wparent[16],
                   int has_group, struct frect gbounds,
                   int has_clip, struct frect clipr,
                   struct noderec *list, int *n, int cap) {
    struct scene_node *node = scene_resolve(a, h);
    if (!node || *n >= cap) return;
    float local[16], world[16];
    scene_trs_to_matrix(node, local);
    scene_mat4_mul(wparent, local, world);

    struct noderec *rec = &list[(*n)++];
    rec->h = h;
    rec->aabb = node_world_aabb(node, world);
    rec->foot = node_footprint(node, rec->aabb);
    rec->is_blur = node->backdrop_blur_enabled;
    rec->has_group = has_group;
    rec->group_bounds = gbounds;
    rec->dirty = 0;
    rec->has_old = 0; rec->own_dirty = 0; rec->moved = 0;
    rec->clips = node->clip_children ? 1 : 0;
    rec->vis = has_clip ? frect_isect(rec->foot, clipr) : rec->foot;
    rec->clipbox = clipr; rec->has_clipbox = has_clip;

    int child_has_clip = has_clip;
    struct frect child_clip = clipr;
    if (node->clip_children) {
        child_clip = has_clip ? frect_isect(clipr, rec->aabb) : rec->aabb;
        child_has_clip = 1;
    }
    int child_group = has_group;
    struct frect child_gb = gbounds;
    if (node->opacity < 1.0f) {      /* this node starts (or nests) a group */
        child_group = 1;
        child_gb = subtree_bounds(a, h, wparent);
    }
    struct node_handle c = node->first_child;
    uint32_t guard = 512;   /* cap sibling walks (cycle-safe) */
    while (!node_handle_is_null(c) && guard-- != 0) {
        struct scene_node *cn = scene_resolve(a, c);
        struct node_handle next = cn ? cn->next_sibling : NODE_HANDLE_NULL;
        gather(a, c, world, child_group, child_gb, child_has_clip, child_clip, list, n, cap);
        c = next;
    }
}

/* ------------------------------------------------------------------------- */
/* painting                                                                  */
/* ------------------------------------------------------------------------- */

static void paint_visual(struct scene_renderer *r, struct scene_node *n,
                         const float world[16], struct render_target *target,
                         float ox, float oy) {
    struct render_backend *be = r->be;
    struct frect ab = node_world_aabb(n, world);

    /* Node-level cull: if this node's painted footprint doesn't touch the dirty
     * region, its pixels are already correct on-target -- skip ALL per-pixel work
     * (text/rect/shadow). This is what makes an incremental frame cost scale with
     * the changed area, not the node count. dirty_hits() returns true under a
     * full repaint, so nothing is wrongly skipped then. The footprint is grown by
     * the shadow's reach so a shadow straddling the dirty edge still repaints. */
    float px0 = ab.x, py0 = ab.y, px1 = ab.x + ab.w, py1 = ab.y + ab.h;
    if (n->shadow_enabled) {
        float m = n->shadow_blur_radius + 2.0f;
        float sx0 = ab.x + n->shadow_dx - m,          sy0 = ab.y + n->shadow_dy - m;
        float sx1 = ab.x + ab.w + n->shadow_dx + m,   sy1 = ab.y + ab.h + n->shadow_dy + m;
        if (sx0 < px0) px0 = sx0;
        if (sy0 < py0) py0 = sy0;
        if (sx1 > px1) px1 = sx1;
        if (sy1 > py1) py1 = sy1;
    }
    struct frect foot = { px0 - ox, py0 - oy, px1 - px0, py1 - py0 };
#ifdef SCENE_TRACE
    fprintf(stderr, "paint kind=%d layer=%d rect=(%.0f,%.0f %.0fx%.0f) hit=%d\n",
            n->kind, n->layer, ab.x, ab.y, ab.w, ab.h, dirty_hits(r, foot));
#endif
    if (!dirty_hits(r, foot)) return;

    float dx = ab.x - ox, dy = ab.y - oy, w = ab.w, h = ab.h;

    switch (n->kind) {
        case SCENE_NODE_RECT:
            if (n->shadow_enabled)
                be->draw_shadow(target, dx, dy, w, h, n->corner_radius,
                                n->shadow_dx, n->shadow_dy, n->shadow_blur_radius, n->shadow_color);
            if (n->backdrop_blur_enabled)
                be->draw_backdrop_blur(target, dx, dy, w, h, n->corner_radius, n->backdrop_blur_radius);
            be->draw_rect(target, dx, dy, w, h, n->corner_radius, &n->data.rect.fill, 1.0f);
            if (n->border_width > 0 && be->draw_border)
                be->draw_border(target, dx, dy, w, h, n->corner_radius, n->border_width,
                                n->border_color, &n->border_paint);
            break;
        case SCENE_NODE_IMAGE:
            be->draw_image(target, dx, dy, w, h, n->data.image.pixels,
                           n->data.image.w, n->data.image.h, n->data.image.w * 4,
                           n->data.image.fmt, 1.0f,
                           n->data.image.tinted ? &n->data.image.tint : NULL);
            break;
        case SCENE_NODE_TEXT:
            /* the run's own background (selection), behind its glyphs */
            if (n->data.text.bg.a > 0.001f) {
                struct paint bp; bp.kind = PAINT_SOLID; bp.solid = n->data.text.bg;
                be->draw_rect(target, dx, dy, w, h, 0.0f, &bp, 1.0f);
            }
            if (be->draw_text)
                be->draw_text(target, dx, dy, n->data.text.utf8, n->data.text.font_handle,
                              n->data.text.size_px, n->data.text.color, 1.0f,
                              &n->data.text.paint, w, h);
            break;
        case SCENE_NODE_GROUP:
        default: break;
    }
}

/* Elevated subtrees found during the flow pass, painted afterwards in
 * (layer, encounter) order. wparent is stashed so the deferred pass rebuilds
 * the identical world transform; the clip stack is naturally empty by then,
 * which is exactly popover semantics (a dropdown escapes its strip's clip). */
#define ELEV_MAX 32
struct elev_list {
    struct { struct node_handle h; float wparent[16]; int layer; } e[ELEV_MAX];
    int n;
};

static void render_node(struct scene_renderer *r, struct scene_arena *a, struct node_handle h,
                        const float wparent[16], struct render_target *target, float ox, float oy,
                        int in_elev, struct elev_list *el);
static void render_node_inner(struct scene_renderer *r, struct scene_arena *a, struct node_handle h,
                              const float wparent[16], struct render_target *target, float ox, float oy,
                              int in_elev, struct elev_list *el);

/* Depth-bounded wrapper: a corrupt scene tree with a parent<->child cycle would
 * otherwise recurse forever here (no fault, just a hang). Cap the depth well
 * above any real UI nesting so a cycle degrades instead of hanging. */
static int g_render_depth;
static void render_node(struct scene_renderer *r, struct scene_arena *a, struct node_handle h,
                        const float wparent[16], struct render_target *target, float ox, float oy,
                        int in_elev, struct elev_list *el) {
    if (g_render_depth > 1024) return;
    g_render_depth++;
    render_node_inner(r, a, h, wparent, target, ox, oy, in_elev, el);
    g_render_depth--;
}

/* Paint a node's own visual (at opacity 1) + recurse children -- used for the
 * ROOT of a group's offscreen pass, where the group's own opacity is applied
 * once at the final blit, not here (Section 5 step 2). */
static void render_subtree_opaque(struct scene_renderer *r, struct scene_arena *a,
                                  struct node_handle h, const float wparent[16],
                                  struct render_target *target, float ox, float oy) {
    struct scene_node *n = scene_resolve(a, h);
    if (!n) return;
    float local[16], world[16];
    scene_trs_to_matrix(n, local);
    scene_mat4_mul(wparent, local, world);

    paint_visual(r, n, world, target, ox, oy);

    int pushed = 0;
    if (n->clip_children) {
        struct frect ab = node_world_aabb(n, world);
        struct clip_rect cr = { ab.x - ox, ab.y - oy, ab.w, ab.h, n->corner_radius };
        r->be->push_clip(target, cr); pushed = 1;
    }
    struct node_handle c = n->first_child;
    uint32_t guard = 512;   /* cap sibling walks (cycle-safe) */
    while (!node_handle_is_null(c) && guard-- != 0) {
        struct scene_node *cn = scene_resolve(a, c);
        struct node_handle next = cn ? cn->next_sibling : NODE_HANDLE_NULL;
        render_node(r, a, c, world, target, ox, oy, 1, 0);   /* groups flatten: inline */
        c = next;
    }
    if (pushed) r->be->pop_clip(target);
}

static void render_node_inner(struct scene_renderer *r, struct scene_arena *a, struct node_handle h,
                              const float wparent[16], struct render_target *target, float ox, float oy,
                              int in_elev, struct elev_list *el) {
    struct scene_node *n = scene_resolve(a, h);
    if (!n) return;

    /* an elevated subtree met during the FLOW pass: stash it for the deferred
     * pass instead of painting it here in document position */
    if (!in_elev && el && n->layer > 0 && el->n < ELEV_MAX) {
        el->e[el->n].h = h;
        for (int i = 0; i < 16; i++) el->e[el->n].wparent[i] = wparent[i];
        el->e[el->n].layer = n->layer;
        el->n++;
        return;
    }

    float local[16], world[16];
    scene_trs_to_matrix(n, local);
    scene_mat4_mul(wparent, local, world);

    if (n->opacity < 1.0f) {
        /* Section 5: grouped offscreen compositing -- render the whole subtree
         * at full opacity into a scratch target, then blit once with the
         * group's opacity, so overlapping children blend against each other at
         * full strength (no seam) and only the flattened result is dimmed. */
        struct frect b = subtree_bounds(a, h, wparent);
        int bx = (int)floorf(b.x), by = (int)floorf(b.y);
        int bw = (int)ceilf(b.x + b.w) - bx, bh = (int)ceilf(b.y + b.h) - by;
        if (bw <= 0 || bh <= 0) return;
        if (!dirty_hits(r, b)) return;   /* whole group outside dirty -> skip */

        struct render_target *scratch = cpu_scratch_acquire((uint32_t)bw, (uint32_t)bh);
        if (scratch) {
            struct clip_saved sv;
            cpu_clip_save_and_clear(&sv);   /* isolate scratch coords + lift dirty */
            render_subtree_opaque(r, a, h, wparent, scratch, (float)bx, (float)by);
            cpu_clip_restore(&sv);
            /* blit flattened result; coverage_at clips this to the dirty union */
            /* flattened subtree: already-rendered pixels, never a stencil */
            r->be->draw_image(target, (float)bx - ox, (float)by - oy, (float)bw, (float)bh,
                              scratch->pixels, (uint32_t)bw, (uint32_t)bh, scratch->stride,
                              scratch->format, n->opacity, NULL);
            cpu_scratch_release(scratch);
        }
        return;
    }

    paint_visual(r, n, world, target, ox, oy);

    int pushed = 0;
    if (n->clip_children) {
        struct frect ab = node_world_aabb(n, world);
        struct clip_rect cr = { ab.x - ox, ab.y - oy, ab.w, ab.h, n->corner_radius };
        r->be->push_clip(target, cr); pushed = 1;
    }
    struct node_handle c = n->first_child;
    uint32_t guard = 512;   /* cap sibling walks (cycle-safe) */
    while (!node_handle_is_null(c) && guard-- != 0) {
        struct scene_node *cn = scene_resolve(a, c);
        struct node_handle next = cn ? cn->next_sibling : NODE_HANDLE_NULL;
        render_node(r, a, c, world, target, ox, oy, in_elev, el);
        c = next;
    }
    if (pushed) r->be->pop_clip(target);
}

/* ------------------------------------------------------------------------- */
/* frame                                                                     */
/* ------------------------------------------------------------------------- */

void scene_render_init(struct scene_renderer *r, struct render_backend *be) {
    r->be = be; r->cache = 0; r->cache_cap = 0; r->n_dirty = 0; r->full = 0; r->has_scroll_present = 0;
}
void scene_render_destroy(struct scene_renderer *r) {
    free(r->cache); r->cache = 0; r->cache_cap = 0;
}

void scene_render_invalidate(struct scene_renderer *r) {
    if (!r || !r->cache) return;
    for (uint32_t i = 0; i < r->cache_cap; i++) r->cache[i].valid = 0;
}

static void ensure_cache(struct scene_renderer *r, uint32_t need) {
    if (r->cache_cap >= need) return;
    uint32_t cap = r->cache_cap ? r->cache_cap : 64;
    while (cap < need) cap *= 2;
    struct rect_cache *nc = (struct rect_cache *)realloc(r->cache, sizeof(*nc) * cap);
    if (!nc) return;
    for (uint32_t i = r->cache_cap; i < cap; i++) { nc[i].valid = 0; nc[i].generation = 0; }
    r->cache = nc; r->cache_cap = cap;
}

void scene_render_frame(struct scene_renderer *r, struct scene_arena *a,
                        struct node_handle root, struct render_target *target) {
    r->n_dirty = 0; r->full = 0; r->has_scroll_present = 0;
    ensure_cache(r, a->next_never_used);
    if (!r->cache) return;

    /* gather every node once, in paint order, with world AABB + group context */
    uint32_t cap = a->next_never_used;
    struct noderec *recs = (struct noderec *)malloc(sizeof(struct noderec) * (cap ? cap : 1));
    if (!recs) return;
    int nrec = 0;
    float ident[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    gather(a, root, ident, 0, frect_empty(), 0, frect_empty(), recs, &nrec, (int)cap);

    /* --- Step 1: dirty accumulation (own dirty flag OR world rect moved OR
     * newly created); union BOTH the new footprint and the cached old one so a
     * moved element doesn't leave a ghost at its previous location. The cache
     * stores the shadow-grown FOOTPRINT, not the bare AABB -- a ghost occupies
     * everything the node stained, and a drop shadow stains past the box. --- */
    uint8_t *seen = (uint8_t *)calloc(r->cache_cap ? r->cache_cap : 1, 1);
    /* --- Step 1a: CLASSIFY, adding nothing yet. Whether this frame is "a
     * scroll" is only knowable from the whole population of changes, so the
     * verdicts are gathered before any dirty rect is committed. --- */
    int n_moved = 0, n_content = 0, uniform = 1;
    float mdx = 0, mdy = 0;
    for (int i = 0; i < nrec; i++) {
        struct noderec *rec = &recs[i];
        uint32_t idx = rec->h.index;
        struct scene_node *n = scene_resolve(a, rec->h);
        struct rect_cache *cc = (idx < r->cache_cap) ? &r->cache[idx] : 0;
        int cache_live = cc && cc->valid && cc->generation == rec->h.generation;
        if (seen && idx < r->cache_cap && cache_live) seen[idx] = 1;
        int moved = 1;
        if (cache_live) {
            rec->old.x = cc->x; rec->old.y = cc->y; rec->old.w = cc->w; rec->old.h = cc->h;
            rec->has_old = 1;
            moved = !(fabsf(cc->x - rec->foot.x) < 0.01f && fabsf(cc->y - rec->foot.y) < 0.01f &&
                      fabsf(cc->w - rec->foot.w) < 0.01f && fabsf(cc->h - rec->foot.h) < 0.01f);
        }
        /* own_dirty means CONTENT changed -- a pure transform is a move, and
         * moves are what the scroll blit exists for (scene.h dirty_content) */
        rec->own_dirty = n && n->dirty_content;
        int any_dirty = n && n->dirty;
        (void)any_dirty;
        rec->moved = moved;
        rec->dirty = rec->own_dirty || moved;
        if (rec->own_dirty) {
            n_content++;
#ifdef SCROLL_DEBUG
            fprintf(stderr, "content: OWN-DIRTY kind=%d at %.0f,%.0f %.0fx%.0f\n",
                    n ? (int)n->kind : -1, rec->foot.x, rec->foot.y, rec->foot.w, rec->foot.h);
#endif
        }
        if (moved && !cache_live) {
            n_content++;            /* a NEW node is content */
#ifdef SCROLL_DEBUG
            fprintf(stderr, "content: NEW kind=%d at %.0f,%.0f %.0fx%.0f\n",
                    n ? (int)n->kind : -1, rec->foot.x, rec->foot.y, rec->foot.w, rec->foot.h);
#endif
        }
        if (moved && cache_live) {
            /* a translation keeps its size; a resize is content */
            if (fabsf(rec->old.w - rec->foot.w) > 0.01f ||
                fabsf(rec->old.h - rec->foot.h) > 0.01f) {
                n_content++;
#ifdef SCROLL_DEBUG
                fprintf(stderr, "content: RESIZE kind=%d %.0fx%.0f -> %.0fx%.0f\n",
                        n ? (int)n->kind : -1, rec->old.w, rec->old.h, rec->foot.w, rec->foot.h);
#endif
            }
            else {
                float dx = rec->foot.x - rec->old.x, dy = rec->foot.y - rec->old.y;
                if (!n_moved) { mdx = dx; mdy = dy; }
                else if (fabsf(dx - mdx) > 0.5f || fabsf(dy - mdy) > 0.5f) uniform = 0;
                n_moved++;
            }
        }
    }
    int n_vacated = 0;
    for (uint32_t i = 0; i < r->cache_cap; i++)
        if (r->cache[i].valid && seen && !seen[i]) n_vacated++;

    /* --- Step 1s: THE SCROLL BLIT. When many nodes translated by one shared
     * vertical delta and NOTHING else changed, this frame is a scroll -- and a
     * scroll does not need its pixels recomputed, it needs them MOVED. The
     * old frame already contains almost every pixel of the new one, |dy|
     * higher or lower. So: find the clip container being scrolled (the
     * smallest clipping node intersecting the motion), memmove its rows, and
     * repaint only the strip the motion exposed.
     *
     * Without this, a wheel tick moved everything in the viewport, blew the
     * 16-rect dirty cap, and degraded to a FULL repaint -- hundreds of glyph
     * blits per notch, which under TCG is a browser that scrolls in lurches.
     * With it, a notch costs one memmove and a strip of glyphs.
     *
     * The guards are the point. Any content change, any resize, any vacated
     * node, any moved-but-visible node OUTSIDE the clip aborts to the normal
     * path -- correctness first, the blit only when the frame provably is a
     * pure scroll. */
    int scrolled = 0;
#ifdef SCROLL_DEBUG
    fprintf(stderr, "scroll? full=%d uniform=%d moved=%d content=%d vacated=%d d=(%.1f,%.1f)\n",
            r->full, uniform, n_moved, n_content, n_vacated, mdx, mdy);
#endif
    /* Content changes do not veto the scroll GLOBALLY -- only content INSIDE
     * the scrolled region does. A status line ticking at the bottom of the
     * window, or a hover highlight in the toolbar, has nothing to do with the
     * document scrolling above it; those repaint as ordinary dirty rects
     * alongside the strip. The intersection check happens after R is known. */
    (void)n_content;
    if (!r->full && uniform && n_moved >= 8 &&
        fabsf(mdx) < 0.5f && fabsf(mdy) >= 1.0f && target && target->pixels) {
        /* the union of the moving content, old and new */
        struct frect mb; int have_mb = 0;
        for (int i = 0; i < nrec; i++) {
            if (!(recs[i].moved && recs[i].has_old)) continue;
            struct frect u = recs[i].foot;
            if (!have_mb) { mb = u; have_mb = 1; }
            float x1 = mb.x + mb.w, y1 = mb.y + mb.h;
            if (u.x < mb.x) mb.x = u.x;
            if (u.y < mb.y) mb.y = u.y;
            if (u.x + u.w > x1) x1 = u.x + u.w;
            if (u.y + u.h > y1) y1 = u.y + u.h;
            mb.w = x1 - mb.x; mb.h = y1 - mb.y;
        }
        /* the scrolled clip: smallest UNMOVED clipping node intersecting it */
        int ci = -1; float carea = 0;
        for (int i = 0; i < nrec; i++) {
            if (!recs[i].clips || recs[i].moved || recs[i].own_dirty) continue;
            if (!have_mb || !frect_overlap(recs[i].aabb, mb)) continue;
            float ar = recs[i].aabb.w * recs[i].aabb.h;
            if (ci < 0 || ar < carea) { ci = i; carea = ar; }
        }
#ifdef SCROLL_DEBUG
        fprintf(stderr, "scroll? clip ci=%d\n", ci);
#endif
        if (ci >= 0) {
            struct frect R = recs[ci].aabb;
            /* clamp to the target */
            if (R.x < 0) { R.w += R.x; R.x = 0; }
            if (R.y < 0) { R.h += R.y; R.y = 0; }
            if (R.x + R.w > (float)target->width)  R.w = (float)target->width  - R.x;
            if (R.y + R.h > (float)target->height) R.h = (float)target->height - R.y;
            /* every UNIFORMLY-MOVED node must be inside R or off-target
             * (moved content visible outside the clip means this was not that
             * clip's scroll); and every CONTENT change and VACATED slot must be
             * OUTSIDE R (stale pixels under the blit would be smeared). */
            int ok = R.w > 0 && R.h > 0;
            for (int i = 0; ok && i < nrec; i++) {
                struct frect t = { 0, 0, (float)target->width, (float)target->height };
                int resized = recs[i].moved && recs[i].has_old &&
                              (fabsf(recs[i].old.w - recs[i].foot.w) > 0.01f ||
                               fabsf(recs[i].old.h - recs[i].foot.h) > 0.01f);
                int is_content = recs[i].own_dirty || resized ||
                                 (recs[i].moved && !recs[i].has_old);
                /* VISIBLE extents, not raw footprints: a node scrolled past the
                 * bottom of its container stains nothing, so it can neither
                 * smear the blit nor prove the frame was not a scroll. Using
                 * the footprint meant any document taller than its viewport
                 * vetoed its own fast path -- which is every real page. */
                struct frect vnew = recs[i].vis;
                struct frect vold = recs[i].has_old
                    ? (recs[i].has_clipbox ? frect_isect(recs[i].old, recs[i].clipbox)
                                           : recs[i].old)
                    : frect_empty();
                if (is_content) {
                    if (frect_overlap(vnew, recs[ci].aabb) ||
                        frect_overlap(vold, recs[ci].aabb)) {
                        ok = 0;
#ifdef SCROLL_DEBUG
                        struct scene_node *vn = scene_resolve(a, recs[i].h);
                        fprintf(stderr, "veto: CONTENT-INSIDE kind=%d own=%d new=%d resized=%d %.0f,%.0f %.0fx%.0f\n",
                                vn ? (int)vn->kind : -1, recs[i].own_dirty,
                                recs[i].moved && !recs[i].has_old, resized,
                                recs[i].foot.x, recs[i].foot.y, recs[i].foot.w, recs[i].foot.h);
#endif
                    }
                    continue;
                }
                if (!recs[i].moved) continue;
                /* A moved node that CLIPS invalidates the reasoning above: its
                 * descendants' visible extents were computed against a clip
                 * that is itself in motion. Rare (a scrollable inside a
                 * scrollable) and cheap to refuse. */
                if (recs[i].clips) { ok = 0;
#ifdef SCROLL_DEBUG
                    fprintf(stderr, "veto: MOVED-CLIP\n");
#endif
                    continue; }
                if (!frect_overlap(vnew, t) && !frect_overlap(vold, t)) continue;
                struct frect both = vnew;
                if (!frect_is_empty(vold)) {
                    if (frect_is_empty(both)) both = vold;
                    else {
                        float x1 = both.x + both.w, y1 = both.y + both.h;
                        if (vold.x < both.x) both.x = vold.x;
                        if (vold.y < both.y) both.y = vold.y;
                        if (vold.x + vold.w > x1) x1 = vold.x + vold.w;
                        if (vold.y + vold.h > y1) y1 = vold.y + vold.h;
                        both.w = x1 - both.x; both.h = y1 - both.y;
                    }
                }
                if (!frect_overlap(both, recs[ci].aabb)) {
                    ok = 0;
#ifdef SCROLL_DEBUG
                    fprintf(stderr, "veto: MOVED-OUTSIDE %.0f,%.0f %.0fx%.0f\n",
                            both.x, both.y, both.w, both.h);
#endif
                }
            }
            for (uint32_t i = 0; ok && i < r->cache_cap; i++) {
                if (!r->cache[i].valid || (seen && seen[i])) continue;
                struct frect v = { r->cache[i].x, r->cache[i].y, r->cache[i].w, r->cache[i].h };
                if (frect_overlap(v, recs[ci].aabb)) ok = 0;   /* a ghost under the blit */
            }
#ifdef SCROLL_DEBUG
            fprintf(stderr, "scroll? ok=%d R=%.0f,%.0f %.0fx%.0f\n", ok, R.x, R.y, R.w, R.h);
#endif
            if (ok) {
                /* Blit INTERIOR rows only. The clip's edges are fractional
                 * (95.6, say): the edge row blends clipped content with
                 * whatever sits outside, and copying an interior row over it
                 * stomps that blend -- one wrong antialiased row at the top of
                 * every scroll. ceil/floor keeps the blit inside, and the edge
                 * bands added below repaint the blends properly. */
                int rx0 = (int)ceilf(R.x),        ry0 = (int)ceilf(R.y);
                int rx1 = (int)floorf(R.x + R.w), ry1 = (int)floorf(R.y + R.h);
                int dy = (int)(mdy < 0 ? mdy - 0.5f : mdy + 0.5f);
                uint8_t *px = (uint8_t *)target->pixels;
                uint32_t stride = target->stride;
                size_t rowbytes = (size_t)(rx1 - rx0) * 4;
                if (dy < 0) {          /* content moved UP: copy top-down */
                    for (int y = ry0; y < ry1; y++) {
                        int sy = y - dy;
                        if (sy >= ry1) break;             /* rest is the exposed strip */
                        memmove(px + (size_t)y * stride + (size_t)rx0 * 4,
                                px + (size_t)sy * stride + (size_t)rx0 * 4, rowbytes);
                    }
                } else {               /* content moved DOWN: copy bottom-up */
                    for (int y = ry1 - 1; y >= ry0; y--) {
                        int sy = y - dy;
                        if (sy < ry0) break;
                        memmove(px + (size_t)y * stride + (size_t)rx0 * 4,
                                px + (size_t)sy * stride + (size_t)rx0 * 4, rowbytes);
                    }
                }
                /* the exposed strip, with 2px of slack for antialiased edges */
                struct frect strip;
                strip.x = R.x; strip.w = R.w;
                if (dy < 0) { strip.y = (float)(ry1 + dy - 2); strip.h = (float)(-dy + 4); }
                else        { strip.y = (float)(ry0 - 2);      strip.h = (float)(dy + 4); }
                dirty_add(r, strip);
                /* the fractional edge rows of R: repaint their blends */
                { struct frect e1 = { R.x, R.y - 1.0f, R.w, 3.0f };
                  struct frect e2 = { R.x, R.y + R.h - 2.0f, R.w, 3.0f };
                  dirty_add(r, e1); dirty_add(r, e2); }
                /* content changes OUTSIDE R repaint as ordinary dirty rects */
                for (int i = 0; i < nrec; i++) {
                    int resized = recs[i].moved && recs[i].has_old &&
                                  (fabsf(recs[i].old.w - recs[i].foot.w) > 0.01f ||
                                   fabsf(recs[i].old.h - recs[i].foot.h) > 0.01f);
                    int is_content = recs[i].own_dirty || resized ||
                                     (recs[i].moved && !recs[i].has_old);
                    if (!is_content) continue;
                    dirty_add(r, recs[i].foot);
                    if (recs[i].has_old) dirty_add(r, recs[i].old);
                }
                scrolled = 1;
                r->sp_x = R.x; r->sp_y = R.y; r->sp_w = R.w; r->sp_h = R.h;
                r->has_scroll_present = 1;  /* the consumer must present ALL of R */
            }
        }
    }

    /* --- Step 1c: the normal dirty accumulation, skipped when the frame was
     * resolved as a scroll (the strip is the only repaint). --- */
    if (!scrolled) {
        for (int i = 0; i < nrec; i++) {
            struct noderec *rec = &recs[i];
            if (rec->dirty) {
                dirty_add(r, rec->foot);                              /* new footprint */
                if (rec->has_old) dirty_add(r, rec->old);             /* old ghost */
            }
        }
    }

    /* --- Step 1b: the VACATED. A node that existed last frame and is gone this
     * frame -- destroyed, or its arena slot handed to a successor -- appears in
     * no rec, so nothing above ever mentions its pixels; they would sit on the
     * target forever, which is exactly the "closed launcher stays painted",
     * "removed icon lingers" family of bugs every app worked around with forced
     * full repaints. Its last painted footprint is right here in the cache:
     * dirty it, then drop the slot. Runs BEFORE the blur/group expansions so
     * they see these rects too. --- */
    for (uint32_t i = 0; i < r->cache_cap; i++) {
        struct rect_cache *cc = &r->cache[i];
        if (!cc->valid) continue;
        if (seen && i < r->cache_cap && seen[i]) continue;
        struct frect old = { cc->x, cc->y, cc->w, cc->h };
        dirty_add(r, old);
        cc->valid = 0;
    }
    free(seen);

    /* --- Step 2: backdrop-blur footprint expansion. Any dirty rect touching a
     * blur node forces a full back-to-front repaint of that whole region, so
     * the blur samples FRESH behind-content, not last frame's pixels. --- */
    for (int i = 0; i < nrec; i++) {
        if (recs[i].is_blur && dirty_hits(r, recs[i].aabb))
            dirty_add(r, recs[i].aabb);
    }

    /* --- Step 3: opacity-group atomicity. A dirty rect intersecting any node
     * inside an opacity<1 group promotes to that group's whole bounds (groups
     * re-render as a unit; no partial-scratch update). --- */
    for (int i = 0; i < nrec; i++) {
        if (recs[i].has_group && dirty_hits(r, recs[i].aabb))
            dirty_add(r, recs[i].group_bounds);
    }

    /* --- Step 4: cap already collapses to full-screen in dirty_add. Paint the
     * whole tree back-to-front; coverage_at() (backend) restricts actual pixel
     * writes to the dirty union, and render_node skips groups outside it. --- */
    int have_work = r->full || r->n_dirty > 0;
    if (have_work) {
        if (r->full) {
            struct clip_rect fs = { 0, 0, (float)target->width, (float)target->height, 0 };
            r->be->begin_frame(target, &fs, 1);
        } else {
            r->be->begin_frame(target, r->dirty, (uint32_t)r->n_dirty);
        }
        struct elev_list el; el.n = 0;
        render_node(r, a, root, ident, target, 0.0f, 0.0f, 0, &el);
        /* deferred: ascending layer, stable within a layer (encounter order ==
         * document order). Insertion sort -- the list is tiny. */
        for (int i = 1; i < el.n; i++) {
            int j = i;
            while (j > 0 && el.e[j - 1].layer > el.e[j].layer) {
                struct elev_list tmp_one; tmp_one.e[0] = el.e[j - 1];
                el.e[j - 1] = el.e[j]; el.e[j] = tmp_one.e[0];
                j--;
            }
        }
        for (int i = 0; i < el.n; i++)
            render_node(r, a, el.e[i].h, el.e[i].wparent, target, 0.0f, 0.0f, 1, 0);
        r->be->end_frame(target);
    }

    /* update cache to this frame's rects + clear consumed dirty flags */
    for (int i = 0; i < nrec; i++) {
        uint32_t idx = recs[i].h.index;
        if (idx < r->cache_cap) {
            r->cache[idx].x = recs[i].foot.x; r->cache[idx].y = recs[i].foot.y;
            r->cache[idx].w = recs[i].foot.w; r->cache[idx].h = recs[i].foot.h;
            r->cache[idx].generation = recs[i].h.generation;
            r->cache[idx].valid = 1;
        }
        struct scene_node *n = scene_resolve(a, recs[i].h);
        if (n) { n->dirty = 0; n->dirty_content = 0; }
    }
    free(recs);
}

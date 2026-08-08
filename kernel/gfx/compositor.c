/* kernel/gfx/compositor.c -- the window compositor (see compositor.h). */

#include "gfx/compositor.h"
#include "drivers/video/framebuffer.h"
#include "drivers/input/mouse.h"
#include "arch/x86_64/cpu/spinlock.h"
#include "process/process.h"   /* struct process (shared_next_va, pml4_phys) for zero-copy */
#include "mm/pmm.h"            /* pmm_alloc_page / pmm_free_page / PAGE_SIZE */
#include "mm/vmm.h"            /* vmm_kmap_pages / vmm_map_in for shared windows */
#include "include/kmalloc.h"
#include "include/kprintf.h"
#include "include/errno.h"
#include "drivers/timer/timer.h"   /* timer_uptime_ms: the window-motion clock */

/* ---- window registry ---------------------------------------------------- */

struct comp_window {
    int       used;
    uint32_t  id;
    int       pid;
    int32_t   x, y;         /* window-frame top-left on screen (chrome incl.) */
    uint32_t  cw, ch;       /* content dimensions in px                       */
    int       z;            /* z-order: larger is nearer the front            */
    int       z_saved;      /* z before the desktop layer was lifted in front  */
    int       visible;
    int       desktop;      /* 1 = chromeless full-screen back layer (the home) */
    int       chromeless;   /* 1 = floating window with NO kernel chrome: the app
                             * draws its own bar/close (EmUI V4 Window/WindowBar)
                             * and moves itself via win_move. Unlike `desktop` it
                             * keeps normal z-order (raisable, focusable). */
    int       widget;       /* 1 = DESKTOP WIDGET (EmUI V5): chromeless AND kept
                             * in a z-band above the desktop but BELOW every app
                             * window; clicks raise it only within that band. */
    int       glass;        /* 1 = GLASS (EmUI V8): the window renders translucent
                             * pixels; the compositor BLURS the backdrop behind it
                             * and composites the window over it (frosted acrylic).
                             * Implies chromeless (the app owns its own chrome). */
    int       translucent;  /* 1 = per-pixel translucent, NO blur (like the desktop
                             * but a raisable floating window): starts transparent
                             * and composites over the SHARP backdrop. Lets a thin
                             * bar carry a tall transparent canvas whose only opaque
                             * pixels are the bar + its open dropdowns. */
    /* Blur-behind SUB-RECT (window-local). A translucent window is mostly
     * empty canvas -- a menu bar is a 32px strip in a window tall enough to
     * hold its dropdowns -- so full-window glass would frost a slab of desktop
     * nobody asked for. Declaring the opaque part lets the strip be real
     * frosted glass while its canvas stays honestly transparent. 0 = none. */
    int       blur_set, blur_x, blur_y, blur_w, blur_h;
    int       presented;    /* 1 once the app has pushed real pixels. A window is
                             * born BLACK and stays that way for seconds under
                             * TCG, so the open animation waits for this rather
                             * than flying a black rectangle across the screen. */
    int       minimized;    /* 1 = hidden by its MINIMIZE button, not by exit or
                             * by a close. The distinction matters: only a window
                             * the user parked can be brought back, and the dock
                             * needs to keep showing the app as running while it
                             * sits there with no pixels on screen. */
    int       pending_action; /* one-shot request delivered to the app runtime */
    /* Latched click replay: a press EDGE on this window's content is remembered
     * here so an app that was busy (e.g. mid first-frame render, seconds long
     * under TCG) still receives the click on its next win_input polls instead
     * of the click being silently eaten (win_input is a state poll, not an
     * event queue). 0 = none, 1 = replay press, 2 = replay release. */
    int       pend_click;
    int       pend_lx, pend_ly;   /* content-local coords of the latched press */
    char      title[COMP_TITLE_MAX + 1];
    uint32_t *content;      /* cw*ch, 0xAARRGGBB premultiplied. For a copy      */
                            /* window this is a kmalloc'd kernel buffer the      */
                            /* client presents INTO; for a shared window it's a  */
                            /* flat kernel view of pages the client renders into */
                            /* directly (zero-copy).                             */
    /* ---- zero-copy (shared) windows only ---- */
    int       shared;       /* 1 = client & compositor share the pixel pages     */
    uint64_t *phys;         /* the npages physical pages (compositor-owned)      */
    uint32_t  npages;
    uint64_t  kview;        /* kernel VA base of the flat view (== content)      */
    uint64_t  client_va;    /* base VA the pages are mapped at in the client     */
    uint64_t  client_pml4;  /* client address space, to unmap on teardown        */
};

static struct comp_window g_wins[COMP_MAX_WINDOWS];
static spinlock_t g_comp_lock = SPINLOCK_INIT;
static uint32_t   g_next_id = 1;
/* z bands: desktop = 0; widgets in [1, WIDGET_Z_TOP); app windows above it. */
#define WIDGET_Z_TOP 1000000
/* ...and one band above every app, for the SHELL's own full-screen surface.
 *
 * The desktop is the ground and belongs at z=0, which is right until the shell
 * needs the whole screen for itself. The Applications launcher is drawn by the
 * desktop process, so with any app window open it opened UNDERNEATH that window
 * -- the user pressed the button, the grid appeared behind the app they were
 * looking at, and nothing seemed to happen. Launchpad is in front, so the layer
 * has to be able to come to the front and go back down again. */
#define DESKTOP_FRONT_Z (WIDGET_Z_TOP * 2)
static int        g_next_z  = WIDGET_Z_TOP + 1;
static int        g_widget_z = 1;
static int        g_active  = 0;   /* desktop has been painted at least once  */
static uint32_t   g_top_id  = 0;   /* id of the currently-focused (front) window */

/* ---- pointer state (cursor + click-to-focus + title-bar drag) ----------- */
#define CURSOR_LAYER_W 20
#define CURSOR_LAYER_H 28
#define CURSOR_W (CURSOR_LAYER_W + 2)  /* complete footprint, including shadow */
#define CURSOR_H (CURSOR_LAYER_H + 2)
#define CURSOR_DAMAGE_PAD 3
#define GLASS_BLUR 7        /* backdrop blur radius for glass windows */
#define GLASS_ALPHA 216     /* window opacity over the frosted backdrop (~85%) */
/* DROP SHADOW (not a halo): black, offset downward, softer when unfocused.
 * This is what lifts a window off the wallpaper instead of leaving it a flat
 * rectangle -- and it carries focus, which the old coloured glow used to do
 * before it was removed for looking like a screenshot artefact. Depth is the
 * signal: the front window casts a deeper, wider shadow than the ones behind
 * it, exactly as stacked paper does. */
#define SHADOW_W_FOCUS  20  /* band width, focused                          */
#define SHADOW_W_IDLE   11  /* ...and unfocused: shallower, still grounded  */
#define SHADOW_A_FOCUS  120 /* peak alpha at the window edge (0-255)        */
#define SHADOW_A_IDLE    62
#define SHADOW_DY        5  /* cast DOWNWARD: light comes from above        */
#define SHADOW_PAD (SHADOW_W_FOCUS + SHADOW_DY + 2)   /* repaint growth     */
#define WORK_TOP 32         /* universal system/status bar */
#define WORK_BOTTOM 64      /* desktop taskbar */
static int      g_cursor_x, g_cursor_y, g_cursor_valid;
static fb_color_t g_cursor_under[CURSOR_W * CURSOR_H];
static int      g_cursor_under_valid;
static uint32_t g_prev_buttons;
static int      g_dragging;        /* a title-bar drag is in progress          */
static uint32_t g_drag_id;         /* which window is being dragged             */
static int      g_drag_dx, g_drag_dy;  /* pointer offset within the window frame */

/* Content-local pointer routing: each tick records the topmost window under the
 * cursor and the pointer in ITS content space, so sys_win_input can deliver it
 * to that window's app (this is what makes an in-window UI clickable -- see
 * compositor_win_input). g_ptr_pid == 0 means the pointer isn't over any
 * window's content (e.g. over a title bar). */
static int      g_ptr_pid;         /* owner pid of the hovered window (0 = none) */
static uint32_t g_ptr_win;         /* its window id                              */
/* POINTER CAPTURE (V5): while the left button stays down, keep routing to the
 * window where the press LANDED even if the cursor leaves it -- this is what
 * keeps a fast WindowBar drag (or an outward resize-grip drag) alive; local
 * coords may go negative/past the content during capture, which is fine for
 * delta-based drag math. 0 = no capture. */
static int      g_cap_pid;
static uint32_t g_cap_win;
static int      g_ptr_lx, g_ptr_ly;    /* pointer in that window's content coords */
static uint32_t g_ptr_buttons;     /* button state to deliver                    */
static int32_t  g_ptr_wheel;       /* scroll delta accrued for the hovered window */

/* ---- palette ------------------------------------------------------------ */

#define DESK_TOP_R 0x1b
#define DESK_TOP_G 0x1e
#define DESK_TOP_B 0x2b
#define DESK_BOT_R 0x0e
#define DESK_BOT_G 0x10
#define DESK_BOT_B 0x17

/* Desktop background: a vertical indigo->charcoal gradient (fallback if the
 * aurora field can't be allocated). */
static fb_color_t desktop_color(int y, int screen_h) {
    if (screen_h <= 1) screen_h = 2;
    int t = (y * 255) / (screen_h - 1);
    if (t < 0) t = 0; if (t > 255) t = 255;
    int r = DESK_TOP_R + ((DESK_BOT_R - DESK_TOP_R) * t) / 255;
    int g = DESK_TOP_G + ((DESK_BOT_G - DESK_TOP_G) * t) / 255;
    int b = DESK_TOP_B + ((DESK_BOT_B - DESK_TOP_B) * t) / 255;
    return FB_RGB(r, g, b);
}

/* ---- aurora desktop field ----------------------------------------------- *
 * The signature EmbLink backdrop: soft overlapping color blobs (indigo, violet,
 * teal, a touch of magenta) on a near-black base. Built ONCE into a full-screen
 * buffer, then paint_region just blits the exposed sub-rect from it -- so
 * repaints stay a memcpy, and the acrylic windows blur a colourful field (not
 * flat dark) so the frosted glass actually glows. Falloff is integer
 * k = s*rad^2/(rad^2 + d^2) -- no sqrt, no float (TCG-friendly). */
static uint32_t *g_aurora;
static int g_aurora_w, g_aurora_h;

static void aurora_build(int W, int H) {
    if (g_aurora && g_aurora_w == W && g_aurora_h == H) return;
    if (g_aurora) { kfree(g_aurora); g_aurora = 0; g_aurora_w = g_aurora_h = 0; }
    if (W <= 0 || H <= 0) return;
    uint32_t *buf = (uint32_t *)kmalloc((size_t)W * H * 4);
    if (!buf) return;
    struct blob { int x, y, rad, r, g, b, s; } bl[] = {
        { W*20/100, H*12/100, W*52/100,  70,  86, 226, 210 },  /* indigo,  top-left  */
        { W*88/100, H*26/100, W*46/100, 128,  74, 206, 190 },  /* violet,  right     */
        { W*60/100, H*94/100, W*58/100,  36, 150, 156, 175 },  /* teal,    bottom    */
        { W* 8/100, H*96/100, W*40/100, 196,  70, 138, 120 },  /* magenta, low-left  */
    };
    int nb = (int)(sizeof bl / sizeof bl[0]);
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            int ar = 13, ag = 15, ab = 22;             /* charcoal base */
            for (int i = 0; i < nb; i++) {
                long dx = x - bl[i].x, dy = y - bl[i].y;
                long d2 = dx*dx + dy*dy;
                long rad2 = (long)bl[i].rad * bl[i].rad;
                long k = (long)bl[i].s * rad2 / (rad2 + d2);   /* 0..s soft falloff */
                ar += (int)(bl[i].r * k / 256);
                ag += (int)(bl[i].g * k / 256);
                ab += (int)(bl[i].b * k / 256);
            }
            /* film GRAIN: a hash-based per-pixel luminance dither (~±6) breaks up
             * gradient banding and gives the wallpaper a subtle tactile texture. */
            uint32_t hsh = ((uint32_t)x * 0x1f1f1f1fu) ^ ((uint32_t)y * 0x9e3779b9u);
            hsh ^= hsh >> 15; hsh *= 0x2c1b3c6du; hsh ^= hsh >> 12;
            int gn = (int)(hsh & 15) - 8 + (int)((hsh >> 8) & 3);   /* ~[-8,+10] */
            gn = gn * 6 / 10;                                       /* scale to ~±5 */
            ar += gn; ag += gn; ab += gn;
            if (ar < 0) ar = 0; if (ag < 0) ag = 0; if (ab < 0) ab = 0;
            if (ar > 255) ar = 255;
            if (ag > 255) ag = 255;
            if (ab > 255) ab = 255;
            buf[(size_t)y*W + x] = ((uint32_t)ar << 16) | ((uint32_t)ag << 8) | (uint32_t)ab;
        }
    }
    g_aurora = buf; g_aurora_w = W; g_aurora_h = H;
}

/* ---- geometry ----------------------------------------------------------- */

/* full window footprint (frame): title bar + content, plus the border ring. */
/* A desktop (home) window is chromeless -- no title bar above its content. */
static int win_titlebar_h(const struct comp_window *w) {
    return (w->desktop || w->chromeless) ? 0 : COMP_TITLEBAR_H;
}

static void win_frame_rect(const struct comp_window *w,
                           int *x0, int *y0, int *x1, int *y1) {
    *x0 = w->x;
    *y0 = w->y;
    *x1 = w->x + (int)w->cw;
    *y1 = w->y + win_titlebar_h(w) + (int)w->ch;
}

static int imax(int a, int b) { return a > b ? a : b; }
static int imin(int a, int b) { return a < b ? a : b; }

/* App windows (not the desktop, not widgets, not translucent bars) get the
 * focused-window accent glow. A translucent bar's frame is a tall invisible
 * canvas, so a halo around it would outline empty space -- skip it. */
/* Which windows cast one: real app windows. The desktop layer is the ground
 * itself, widgets are ambient, and a translucent bar's frame is a tall
 * invisible canvas -- a shadow around any of those would outline empty space. */
static int win_shadows(const struct comp_window *w) {
    return w->used && !w->desktop && !w->widget && !w->translucent;
}
/* The rect a repaint must cover for a window: its frame, GROWN by the glow band
 * for glow windows so the halo is drawn / erased with the window (else it trails
 * on move/focus-change). */
static void win_repaint_rect(const struct comp_window *w,
                             int *x0, int *y0, int *x1, int *y1) {
    win_frame_rect(w, x0, y0, x1, y1);
    /* grow by the SHADOW's reach (widest case + its downward offset) so the
     * shadow is drawn and erased with its window instead of trailing */
    if (win_shadows(w)) {
        *x0 -= SHADOW_PAD; *y0 -= SHADOW_PAD;
        *x1 += SHADOW_PAD; *y1 += SHADOW_PAD;
    }
}

/* ---- lookup ------------------------------------------------------------- */

static struct comp_window *win_find(int pid, uint32_t id) {
    for (int i = 0; i < COMP_MAX_WINDOWS; i++)
        if (g_wins[i].used && g_wins[i].id == id && g_wins[i].pid == pid)
            return &g_wins[i];
    return 0;
}

static struct comp_window *win_by_id(uint32_t id) {
    for (int i = 0; i < COMP_MAX_WINDOWS; i++)
        if (g_wins[i].used && g_wins[i].id == id) return &g_wins[i];
    return 0;
}

/* front-most window's z (for the focus highlight). 0 if none. */
static int top_z(void) {
    int z = 0;
    for (int i = 0; i < COMP_MAX_WINDOWS; i++)
        if (g_wins[i].used && g_wins[i].visible && g_wins[i].z > z) z = g_wins[i].z;
    return z;
}

static struct comp_window *front_window(void) {
    struct comp_window *top = 0;
    for (int i = 0; i < COMP_MAX_WINDOWS; i++) {
        if (!g_wins[i].used || !g_wins[i].visible) continue;
        if (!top || g_wins[i].z > top->z) top = &g_wins[i];
    }
    return top;
}

/* front-most window whose frame contains screen (x,y), or NULL. */
static struct comp_window *topmost_at(int x, int y) {
    struct comp_window *best = 0;
    for (int i = 0; i < COMP_MAX_WINDOWS; i++) {
        if (!g_wins[i].used || !g_wins[i].visible) continue;
        int fx0, fy0, fx1, fy1; win_frame_rect(&g_wins[i], &fx0, &fy0, &fx1, &fy1);
        if (x >= fx0 && x < fx1 && y >= fy0 && y < fy1)
            if (!best || g_wins[i].z > best->z) best = &g_wins[i];
    }
    return best;
}

/* Which process owns the FRONT window -- i.e. who the keyboard belongs to.
 * Keys are a single global queue, so without an owner every process that polls
 * drains it and they race; the front window is the one the user is looking at,
 * so it gets them. 0 = no window at all (then nobody is filtered). */
uint32_t compositor_focused_pid(void) {
    uint32_t pid = 0;
    spin_lock(&g_comp_lock);
    struct comp_window *best = 0, *desk = 0;
    for (int i = 0; i < COMP_MAX_WINDOWS; i++) {
        struct comp_window *w = &g_wins[i];
        if (!w->used || !w->visible) continue;
        if (w->desktop) { desk = w; continue; }     /* the fallback owner */
        /* CHROME NEVER TAKES THE KEYBOARD. A menu bar or an ambient widget has
         * nothing to type into, so letting one hold focus could only ever steal
         * input from the app you were actually typing in -- clicking a menu
         * would silently kill your terminal's keyboard. */
        if (w->widget || w->translucent) continue;
        if (!best || w->z > best->z) best = w;
    }
    /* No app window up -> the desktop owns it, so there is ALWAYS exactly one
     * owner and never a free-for-all (that race is the bug this all fixes). */
    if (!best) best = desk;
    if (best) pid = (uint32_t)best->pid;
    spin_unlock(&g_comp_lock);
    return pid;
}

/* Screen rect of a window's title-bar CLOSE button (right side). The chromeless
 * desktop window has no title bar, so no close button (returns 0). */
#define COMP_CLOSE_SZ 16
static int win_close_rect(const struct comp_window *w, int *x0, int *y0, int *x1, int *y1) {
    if (w->desktop || w->chromeless) return 0;
    *x0 = w->x + (int)w->cw - COMP_CLOSE_SZ - 6;
    *y0 = w->y + (COMP_TITLEBAR_H - COMP_CLOSE_SZ) / 2;
    *x1 = *x0 + COMP_CLOSE_SZ;
    *y1 = *y0 + COMP_CLOSE_SZ;
    return 1;
}

static int win_max_rect(const struct comp_window *w, int *x0, int *y0, int *x1, int *y1) {
    int cx0, cy0, cx1, cy1;
    if (!win_close_rect(w, &cx0, &cy0, &cx1, &cy1)) return 0;
    *x0 = cx0 - 22; *x1 = cx1 - 22; *y0 = cy0; *y1 = cy1;
    return 1;
}

/* The minimize button was painted from the start but had no hit rect at all --
 * it was decoration you could click forever with nothing happening. */
static int win_min_rect(const struct comp_window *w, int *x0, int *y0, int *x1, int *y1) {
    int cx0, cy0, cx1, cy1;
    if (!win_close_rect(w, &cx0, &cy0, &cx1, &cy1)) return 0;
    *x0 = cx0 - 44; *x1 = cx1 - 44; *y0 = cy0; *y1 = cy1;
    return 1;
}

/* ---- rounded corners ---------------------------------------------------- *
 * A window with a hard 90-degree corner reads as a rectangle PASTED onto the
 * screen; the rounded corner is most of what makes it read as an object lying
 * on top of one. Doing it properly needs three things at once, or it looks
 * worse than the square corner it replaced: the arc must be ANTIALIASED (a
 * stair-stepped curve is uglier than no curve), the shadow must follow the arc
 * (or each corner shows a bright notch where the shadow stops), and the border
 * must curve with it (or the frame ends in four disconnected straight lines).
 *
 * The trick that gets all three cheaply: paint_region draws bottom-to-top into
 * one buffer, so at the moment a window starts painting, its four corner boxes
 * still hold EXACTLY the backdrop that belongs behind it -- aurora, desktop,
 * lower windows and its own shadow. Save those ~324 pixels, let every existing
 * paint path (opaque blit, glass, per-pixel, title bar, border) run untouched,
 * then blend the saved backdrop back in by arc coverage. One carve handles all
 * five paths and none of them needed to learn about radii. */
#define WIN_RADIUS  9       /* corner radius, px                              */
#define WIN_R_MAX  16       /* static-buffer bound                            */

struct corner_box {
    int x0, y0, x1, y1;         /* the saved sub-box (clipped to the region)  */
    int cx, cy;                 /* arc centre                                 */
    uint32_t px[WIN_R_MAX * WIN_R_MAX];
};
/* one window is painted at a time under g_comp_lock, so these are scratch */
static struct corner_box g_corner[4];
static int g_corner_n;

static uint32_t isqrt32(uint32_t x) {
    uint32_t r = 0, b = 1u << 30;
    while (b > x) b >>= 2;
    while (b) {
        if (x >= r + b) { x -= r + b; r = (r >> 1) + b; }
        else r >>= 1;
        b >>= 2;
    }
    return r;
}

/* The four corner boxes of a window's frame, and their arc centres. */
static int corner_boxes(const struct comp_window *w, int bx[4], int by[4],
                        int cx[4], int cy[4]) {
    int fx0, fy0, fx1, fy1;
    win_frame_rect(w, &fx0, &fy0, &fx1, &fy1);
    const int r = WIN_RADIUS;
    if (fx1 - fx0 < 2 * r || fy1 - fy0 < 2 * r) return 0;  /* too small to round */
    bx[0] = fx0;     by[0] = fy0;     bx[1] = fx1 - r; by[1] = fy0;
    bx[2] = fx0;     by[2] = fy1 - r; bx[3] = fx1 - r; by[3] = fy1 - r;
    cx[0] = fx0 + r; cy[0] = fy0 + r; cx[1] = fx1 - r; cy[1] = fy0 + r;
    cx[2] = fx0 + r; cy[2] = fy1 - r; cx[3] = fx1 - r; cy[3] = fy1 - r;
    return 1;
}

/* A corner must never be recomposed from a PARTIAL backdrop, or the carve eats
 * its own output: it would blend against pixels that already contain last
 * frame's blend, and after a few repaints the arc creeps back to a square (and
 * the shadow tint compounds to black). So a repaint region that clips through
 * any corner box swallows that box whole -- at most 9px of growth per side,
 * and only when an edge happens to cut a corner. */
static void region_grow_corners(int *rx0, int *ry0, int *rx1, int *ry1);

/* Stash the backdrop under the four corner boxes, before the window paints. */
static void corners_save(const struct comp_window *w,
                         int rx0, int ry0, int rx1, int ry1) {
    g_corner_n = 0;
    const int r = WIN_RADIUS;
    int ox[4], oy[4], cx[4], cy[4];
    if (!corner_boxes(w, ox, oy, cx, cy)) return;

    for (int i = 0; i < 4; i++) {
        struct corner_box *c = &g_corner[g_corner_n];
        c->x0 = imax(ox[i], rx0);     c->y0 = imax(oy[i], ry0);
        c->x1 = imin(ox[i] + r, rx1); c->y1 = imin(oy[i] + r, ry1);
        if (c->x1 <= c->x0 || c->y1 <= c->y0) continue;   /* region misses it */
        c->cx = cx[i]; c->cy = cy[i];
        for (int y = c->y0; y < c->y1; y++)
            for (int x = c->x0; x < c->x1; x++)
                c->px[(y - c->y0) * r + (x - c->x0)] =
                    fb_get_pixel((uint32_t)x, (uint32_t)y);
        g_corner_n++;
    }
}

/* Blend the saved backdrop back over the painted window, by arc coverage. */
static void corners_carve(const struct comp_window *w, int focused) {
    const int r = WIN_RADIUS;
    const int rr8 = (r * 8) * (r * 8);          /* radius^2 in eighth-pixels  */
    int has_border = (!w->desktop && !w->chromeless);
    fb_color_t bcol = focused ? FB_RGB(0x7c, 0x84, 0xf0) : FB_RGB(0x3a, 0x3f, 0x55);
    int sw = focused ? SHADOW_W_FOCUS : SHADOW_W_IDLE;
    int sa = focused ? SHADOW_A_FOCUS : SHADOW_A_IDLE;

    for (int i = 0; i < g_corner_n; i++) {
        struct corner_box *c = &g_corner[i];
        for (int y = c->y0; y < c->y1; y++) {
            for (int x = c->x0; x < c->x1; x++) {
                /* 4x4 supersampled coverage of the arc: 0 = outside the window,
                 * 16 = fully inside, between = the antialiased edge itself. */
                int cov = 0;
                for (int sy = 0; sy < 4; sy++) {
                    for (int sx = 0; sx < 4; sx++) {
                        int dx = (x - c->cx) * 8 + sx * 2 + 1;
                        int dy = (y - c->cy) * 8 + sy * 2 + 1;
                        if (dx * dx + dy * dy <= rr8) cov++;
                    }
                }
                if (cov == 16) continue;              /* untouched interior */

                uint32_t bak = c->px[(y - c->y0) * r + (x - c->x0)];
                uint32_t cur = fb_get_pixel((uint32_t)x, (uint32_t)y);
                int br = (int)((bak >> 16) & 255), bg = (int)((bak >> 8) & 255),
                    bb = (int)(bak & 255);

                /* carry the drop shadow AROUND the arc: outside the curve this
                 * pixel is no longer window, so it owes the same darkening its
                 * neighbours outside the frame already got. */
                int hx = (x - c->cx) * 2 + 1, hy = (y - c->cy) * 2 + 1;
                int dout = (int)isqrt32((uint32_t)(hx * hx + hy * hy)) - r * 2;
                if (dout > 0 && win_shadows(w)) {
                    int a = sa - sa * dout / (sw * 2);
                    if (a > 0) { br = br * (255 - a) / 255;
                                 bg = bg * (255 - a) / 255;
                                 bb = bb * (255 - a) / 255; }
                }

                int cr = (int)((cur >> 16) & 255), cg = (int)((cur >> 8) & 255),
                    cb = (int)(cur & 255);
                int orr = (cr * cov + br * (16 - cov)) / 16;
                int og  = (cg * cov + bg * (16 - cov)) / 16;
                int ob  = (cb * cov + bb * (16 - cov)) / 16;

                /* the border ring, curved: partial-coverage pixels ARE the arc,
                 * so weighting by cov*(16-cov) traces a 1px antialiased curve
                 * exactly where the straight ring had to stop. */
                if (has_border && cov > 0) {
                    int bw = cov * (16 - cov) * 255 / 64;      /* 0..255       */
                    int lr = (int)((bcol >> 16) & 255),
                        lg = (int)((bcol >> 8) & 255),
                        lb = (int)(bcol & 255);
                    orr = (orr * (255 - bw) + lr * bw) / 255;
                    og  = (og  * (255 - bw) + lg * bw) / 255;
                    ob  = (ob  * (255 - bw) + lb * bw) / 255;
                }
                fb_put_pixel_c((uint32_t)x, (uint32_t)y, FB_RGB(orr, og, ob));
            }
        }
    }
}

/* ---- window motion ------------------------------------------------------ *
 * Windows used to appear and vanish between one frame and the next, which is
 * the single loudest way a desktop says "this is a framebuffer, not a surface
 * you are touching". The earlier attempt at this lived in the TOOLKIT -- wrap
 * the app's view in an opacity/scale group -- and it failed badly: at whole-
 * window size the group path cropped the window to its text extents and left a
 * ghost of the input row (docs/TODO.md).
 *
 * Doing it in the compositor sidesteps that entirely. The compositor already
 * holds the window's FINISHED pixels, so the animation is a scale+fade of a
 * buffer: no scene graph, no layout, no scratch surface, and the app never
 * learns it happened. The window is hidden for the duration and a ghost is
 * drawn in its place; paint_region draws that ghost itself, so anything else
 * repainting mid-flight (the cursor, another window) composes correctly. */
#define ANIM_MS_OPEN   170
#define ANIM_MS_PARK   200
#define ANIM_MS_CLOSE  150   /* shorter than open: leaving should feel decisive */
#define ANIM_CLOSE_TO   92   /* shrink to 92% while fading -- a collapse, not a fly-off */
#define ANIM_OPEN_FROM  88   /* open starts at 88% and grows in -- a small move;
                              * a big one reads as a zoom effect, not a window */

enum anim_kind { ANIM_NONE = 0, ANIM_OPEN, ANIM_PARK, ANIM_UNPARK, ANIM_CLOSE };

static struct {
    int      kind;
    uint32_t win_id;
    uint64_t t0, dur;
    int      from[4], to[4];      /* x, y, w, h */
    int      last[4];             /* where the ghost was drawn last frame */
    int      have_last;
    /* A CLOSING window's pixels are freed out from under us -- the process is
     * killed and reaped while the ghost is still in the air. So closing (and
     * only closing) animates a COPY. Every other motion reads the live buffer,
     * which stays valid because the app is still there. */
    uint32_t *snap;
    int      snap_w, snap_h;
} g_anim;

static int anim_lerp(int a, int b, int num, int den) {
    return a + (int)(((int64_t)(b - a) * num) / (den ? den : 1));
}

/* ease-out cubic on 0..1000: fast start, soft landing. 1000-(1-t)^3 */
static int ease_out(int t) {
    int64_t u = 1000 - t;
    return (int)(1000 - (u * u * u) / 1000000);
}

/* Where a parked window flies to. The dock is drawn by the home process and
 * the kernel has no idea where its icons are, so this aims at the bottom
 * centre of the screen -- which is where the dock actually sits. Plumbing the
 * real icon rect through would make it exact; see docs/TODO.md. */
static void anim_dock_rect(int out[4]) {
    const fb_info_t *fi = fb_get_info();
    int W = fi ? (int)fi->width : 1024, H = fi ? (int)fi->height : 768;
    out[2] = 64; out[3] = 44;
    out[0] = W / 2 - out[2] / 2;
    out[1] = H - 56;
}

static void anim_progress(int *num, int *den) {
    uint64_t now = timer_uptime_ms();
    uint64_t el = now - g_anim.t0;
    if (el >= g_anim.dur) { *num = 1000; *den = 1000; return; }
    *num = (int)((el * 1000) / g_anim.dur);
    *den = 1000;
}

/* the ghost's rect and opacity at the current instant */
static void anim_frame_rect(int out[4], int *alpha) {
    int num, den; anim_progress(&num, &den);
    int t = ease_out(num);
    for (int i = 0; i < 4; i++) out[i] = anim_lerp(g_anim.from[i], g_anim.to[i], t, den);
    /* fade with the raw (un-eased) time so opacity and motion don't fight */
    int a = (g_anim.kind == ANIM_PARK || g_anim.kind == ANIM_CLOSE)
                ? (255 - num * 255 / 1000)
                : (num * 255 / 1000);
    if (a < 0) a = 0; if (a > 255) a = 255;
    *alpha = a;
}

/* Union of the previous and current ghost rects: what a frame must repaint. */
static void anim_dirty(int *x0, int *y0, int *x1, int *y1) {
    int r[4], a; anim_frame_rect(r, &a);
    *x0 = r[0]; *y0 = r[1]; *x1 = r[0] + r[2]; *y1 = r[1] + r[3];
    if (g_anim.have_last) {
        *x0 = imin(*x0, g_anim.last[0]); *y0 = imin(*y0, g_anim.last[1]);
        *x1 = imax(*x1, g_anim.last[0] + g_anim.last[2]);
        *y1 = imax(*y1, g_anim.last[1] + g_anim.last[3]);
    }
    *x0 -= 2; *y0 -= 2; *x1 += 2; *y1 += 2;
}

/* Kick off a motion. The window is HIDDEN for the duration (the ghost stands
 * in for it) and put back -- or not -- when the motion lands. */
static void paint_region(int rx0, int ry0, int rx1, int ry1);   /* fwd */
static void enforce_focus(void);

static void anim_start(struct comp_window *w, int kind) {
    if (!w || !w->content) return;
    if (w->desktop || w->widget || w->translucent) return;  /* chrome doesn't fly */

    int full[4], dock[4];
    win_frame_rect(w, &full[0], &full[1], &full[2], &full[3]);
    full[2] -= full[0]; full[3] -= full[1];
    anim_dock_rect(dock);

    /* if a motion is already running for another window, let it land first */
    if (g_anim.kind != ANIM_NONE && g_anim.win_id != w->id) return;

    g_anim.kind = kind;
    g_anim.win_id = w->id;
    g_anim.t0 = timer_uptime_ms();
    g_anim.have_last = 0;

    if (kind == ANIM_OPEN) {
        g_anim.dur = ANIM_MS_OPEN;
        int sw_ = full[2] * ANIM_OPEN_FROM / 100, sh_ = full[3] * ANIM_OPEN_FROM / 100;
        g_anim.from[0] = full[0] + (full[2] - sw_) / 2;
        g_anim.from[1] = full[1] + (full[3] - sh_) / 2;
        g_anim.from[2] = sw_; g_anim.from[3] = sh_;
        for (int i = 0; i < 4; i++) g_anim.to[i] = full[i];
    } else if (kind == ANIM_PARK) {
        g_anim.dur = ANIM_MS_PARK;
        for (int i = 0; i < 4; i++) { g_anim.from[i] = full[i]; g_anim.to[i] = dock[i]; }
    } else if (kind == ANIM_UNPARK) {
        g_anim.dur = ANIM_MS_PARK;
        for (int i = 0; i < 4; i++) { g_anim.from[i] = dock[i]; g_anim.to[i] = full[i]; }
    } else {  /* ANIM_CLOSE */
        g_anim.dur = ANIM_MS_CLOSE;
        int sw_ = full[2] * ANIM_CLOSE_TO / 100, sh_ = full[3] * ANIM_CLOSE_TO / 100;
        for (int i = 0; i < 4; i++) g_anim.from[i] = full[i];
        g_anim.to[0] = full[0] + (full[2] - sw_) / 2;
        g_anim.to[1] = full[1] + (full[3] - sh_) / 2;
        g_anim.to[2] = sw_; g_anim.to[3] = sh_;
        /* take the copy now, while the buffer is still ours */
        size_t n = (size_t)w->cw * w->ch;
        uint32_t *cp = (uint32_t *)kmalloc(n * 4);
        if (cp) {
            for (size_t i = 0; i < n; i++) cp[i] = w->content[i];
            g_anim.snap = cp; g_anim.snap_w = (int)w->cw; g_anim.snap_h = (int)w->ch;
        } else {
            g_anim.kind = ANIM_NONE;     /* no copy, no animation -- just close */
            return;
        }
    }
    w->visible = 0;                      /* the ghost is the window for now */
}

/* Land the motion: the window becomes real again (or stays parked). */
static void anim_finish(void) {
    struct comp_window *w = win_by_id(g_anim.win_id);
    int kind = g_anim.kind;
    int last[4]; int had = g_anim.have_last;
    for (int i = 0; i < 4; i++) last[i] = g_anim.last[i];
    g_anim.kind = ANIM_NONE; g_anim.have_last = 0;
    if (g_anim.snap) { kfree(g_anim.snap); g_anim.snap = 0; g_anim.snap_w = g_anim.snap_h = 0; }
    if (w) w->visible = (kind == ANIM_OPEN || kind == ANIM_UNPARK);
    /* repaint the window's footprint AND wherever the ghost last was */
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    if (w && w->used) win_repaint_rect(w, &x0, &y0, &x1, &y1);
    else w = 0;
    if (had) {
        if (!w) { x0 = last[0]; y0 = last[1]; x1 = last[0] + last[2]; y1 = last[1] + last[3]; }
        else {
            x0 = imin(x0, last[0]); y0 = imin(y0, last[1]);
            x1 = imax(x1, last[0] + last[2]); y1 = imax(y1, last[1] + last[3]);
        }
    }
    if (x1 > x0 && y1 > y0) paint_region(x0 - 2, y0 - 2, x1 + 2, y1 + 2);
    if (w && w->visible) enforce_focus();
}

/* ---- compositing -------------------------------------------------------- *
 * Chrome that used to be drawn in full on every partial repaint (the title-bar
 * fill and the border ring) is now clipped to the region, so a repaint that
 * doesn't reach a corner box can no longer square it off behind the carve's
 * back. The title text and window buttons stay unclipped: redrawing them
 * outside the region reproduces identical pixels over identical ones, and they
 * never reach the arcs. */
static void fill_clip(int x, int y, int w, int h, fb_color_t c,
                      int rx0, int ry0, int rx1, int ry1) {
    int x0 = imax(x, rx0), y0 = imax(y, ry0);
    int x1 = imin(x + w, rx1), y1 = imin(y + h, ry1);
    if (x1 > x0 && y1 > y0) fb_fill_rect(x0, y0, x1 - x0, y1 - y0, c);
}

/* Draw one window's chrome + content clipped to region [rx0,ry0)-(rx1,ry1). */
static void paint_window(struct comp_window *w, int focused,
                         int rx0, int ry0, int rx1, int ry1) {
    int fx0, fy0, fx1, fy1;
    win_frame_rect(w, &fx0, &fy0, &fx1, &fy1);
    /* early out if the window (grown by its glow band, when it has one) doesn't
     * touch the region at all */
    int shadow = win_shadows(w);
    int px0 = fx0, py0 = fy0, px1 = fx1, py1 = fy1;
    if (shadow) {
        px0 -= SHADOW_PAD; py0 -= SHADOW_PAD; px1 += SHADOW_PAD; py1 += SHADOW_PAD;
    }
    if (px1 <= rx0 || px0 >= rx1 || py1 <= ry0 || py0 >= ry1) return;

    /* A window is born BLACK and stays that way until the app renders -- under
     * TCG that is seconds of a black slab sitting on the desktop, which is the
     * ugliest moment in launching anything. Now that the open animation waits
     * for the first real frame anyway, simply don't draw the window until it
     * has one: the desktop stays whole, and then the window grows in. */
    if (!w->presented) return;

    /* the drop shadow: drawn first (outside the frame) so the window content
     * lands on top of it, offset downward, and deeper while focused. */
    if (shadow) {
        int sw_  = focused ? SHADOW_W_FOCUS : SHADOW_W_IDLE;
        int sa_  = focused ? SHADOW_A_FOCUS : SHADOW_A_IDLE;
        fb_glow_rect(fx0, fy0 + SHADOW_DY, fx1, fy1 + SHADOW_DY, sw_,
                     0, 0, 0, (uint8_t)sa_, rx0, ry0, rx1, ry1);
    }

    /* stash the corner backdrops NOW: everything below this line overwrites
     * them, and corners_carve needs what belonged behind this window. */
    int round = win_shadows(w);      /* same set that casts a shadow          */
    if (round) corners_save(w, rx0, ry0, rx1, ry1); else g_corner_n = 0;

    int bar_h = win_titlebar_h(w);   /* 0 for the chromeless desktop window */

    /* --- title bar (repaint whole bar if the region touches it; text is
     *     cheap and this avoids clipped-glyph artefacts). Skipped entirely for
     *     the chromeless desktop layer. --- */
    if (!w->desktop && !w->chromeless) {
        int tbx0 = w->x, tby0 = w->y, tbx1 = w->x + (int)w->cw, tby1 = w->y + COMP_TITLEBAR_H;
        if (!(tbx1 <= rx0 || tbx0 >= rx1 || tby1 <= ry0 || tby0 >= ry1)) {
            fb_color_t bar = focused ? FB_RGB(0x24, 0x24, 0x24) : FB_RGB(0x20, 0x20, 0x20);
            fill_clip(tbx0, tby0, (int)w->cw, COMP_TITLEBAR_H, bar, rx0, ry0, rx1, ry1);
            int tr = focused ? 0xf2 : 0xa8, tg = focused ? 0xf2 : 0xa8, tb = focused ? 0xf2 : 0xa8;
            int br = 0x24, bg = 0x24, bb = 0x24;
            fb_draw_string(w->title, tbx0 + 12, tby0 + (COMP_TITLEBAR_H - 16) / 2,
                           tr, tg, tb, br, bg, bb);

            /* Linux-style window controls: minimize, maximize and close.
             * Close is connected to process teardown below. The first two are
             * deliberately rendered as disabled chrome until the compositor
             * exposes minimize/maximize lifecycle operations. */
            int cbx0, cby0, cbx1, cby1;
            win_close_rect(w, &cbx0, &cby0, &cbx1, &cby1);
            int cy = (cby0 + cby1) / 2;
            int cx = (cbx0 + cbx1) / 2;
            fb_color_t ctl = focused ? FB_RGB(0x43, 0x43, 0x43) : FB_RGB(0x35, 0x35, 0x35);
            fb_color_t glyph = focused ? FB_RGB(0xe5, 0xe5, 0xe5) : FB_RGB(0x8f, 0x8f, 0x8f);
            fb_fill_circle(cx - 44, cy, COMP_CLOSE_SZ / 2, ctl);
            fb_draw_line(cx - 48, cy + 2, cx - 40, cy + 2, glyph);
            fb_fill_circle(cx - 22, cy, COMP_CLOSE_SZ / 2, ctl);
            fb_draw_line(cx - 26, cy - 3, cx - 19, cy - 3, glyph);
            fb_draw_line(cx - 26, cy - 3, cx - 26, cy + 4, glyph);
            fb_draw_line(cx - 19, cy - 3, cx - 19, cy + 4, glyph);
            fb_draw_line(cx - 26, cy + 4, cx - 19, cy + 4, glyph);
            fb_fill_circle((cbx0 + cbx1) / 2, (cby0 + cby1) / 2, COMP_CLOSE_SZ / 2,
                           ctl);
            fb_color_t xc = glyph;
            fb_draw_line(cbx0 + 5, cby0 + 5, cbx1 - 5, cby1 - 5, xc);
            fb_draw_line(cbx1 - 5, cby0 + 5, cbx0 + 5, cby1 - 5, xc);
        }
    }

    /* --- content: blit only the sub-rectangle overlapping the region --- */
    int cx0 = w->x, cy0 = w->y + bar_h;
    int cx1 = cx0 + (int)w->cw, cy1 = cy0 + (int)w->ch;
    int sx0 = imax(cx0, rx0), sy0 = imax(cy0, ry0);
    int sx1 = imin(cx1, rx1), sy1 = imin(cy1, ry1);
    if (sx1 > sx0 && sy1 > sy0 && w->content) {
        int src_x = sx0 - cx0, src_y = sy0 - cy0;
        const uint32_t *src = w->content + (size_t)src_y * w->cw + src_x;
        if (w->glass) {
            /* frost the already-painted backdrop (desktop + lower windows) behind
             * this window. A glass WIDGET renders a translucent tint -> composite
             * per-pixel so the frosted aurora shows through it; an acrylic app
             * window renders opaque -> lay it over at a uniform translucency. */
            fb_blur_region(sx0, sy0, sx1 - sx0, sy1 - sy0, GLASS_BLUR);
            if (w->widget)
                fb_blit_over(sx0, sy0, sx1 - sx0, sy1 - sy0, src, w->cw);
            else
                fb_blit_uniform(sx0, sy0, sx1 - sx0, sy1 - sy0, src, w->cw, GLASS_ALPHA);
        } else if (w->desktop || w->translucent) {
            /* the home desktop and a translucent bar render a transparent canvas
             * with only some opaque pixels; composite per-pixel over the SHARP
             * backdrop (no blur), so empty regions reveal the desktop below and a
             * thin bar can carry a tall invisible area for its dropdowns.
             * Where the window DECLARED an opaque sub-rect, frost that part of
             * the backdrop first -- real glass for the bar, sharp desktop
             * everywhere its canvas is empty. */
            if (w->blur_set) {
                int bx0 = imax(sx0, cx0 + w->blur_x);
                int by0 = imax(sy0, cy0 + w->blur_y);
                int bx1 = imin(sx1, cx0 + w->blur_x + w->blur_w);
                int by1 = imin(sy1, cy0 + w->blur_y + w->blur_h);
                if (bx1 > bx0 && by1 > by0)
                    fb_blur_region(bx0, by0, bx1 - bx0, by1 - by0, GLASS_BLUR);
            }
            fb_blit_over(sx0, sy0, sx1 - sx0, sy1 - sy0, src, w->cw);
        } else {
            fb_blit(sx0, sy0, sx1 - sx0, sy1 - sy0, src, w->cw);
        }
    }

    /* --- 1px border ring (not on the desktop, nor app-chromed windows) --- */
    if (!w->desktop && !w->chromeless) {
        fb_color_t ring = focused ? FB_RGB(0x7c, 0x84, 0xf0) : FB_RGB(0x3a, 0x3f, 0x55);
        fill_clip(fx0, fy0, fx1 - fx0, 1, ring, rx0, ry0, rx1, ry1);
        fill_clip(fx0, fy1 - 1, fx1 - fx0, 1, ring, rx0, ry0, rx1, ry1);
        fill_clip(fx0, fy0, 1, fy1 - fy0, ring, rx0, ry0, rx1, ry1);
        fill_clip(fx1 - 1, fy0, 1, fy1 - fy0, ring, rx0, ry0, rx1, ry1);
    }

    /* --- and finally round the corners off everything drawn above --- */
    if (round) corners_carve(w, focused);
}

/* Modern desktop arrow, rasterised from two polygons at 4x4 coverage. This
 * keeps the diagonal and the neck smooth at 1:1 display scale; the previous
 * hand-authored bitmap visibly turned into a star where the head met the stem.
 * Coordinates are quarter-pixels, avoiding floating point in the kernel. */
struct cursor_pt { int x, y; };
static const struct cursor_pt cursor_outer[] = {
    { 4,  4}, { 4, 84}, {24, 64}, {40,100},
    {56, 92}, {40, 60}, {72, 60}
};
static const struct cursor_pt cursor_inner[] = {
    {12, 14}, {12, 67}, {26, 53}, {42, 88},
    {47, 85}, {32, 52}, {57, 52}
};
static uint32_t cursor_shadow[CURSOR_LAYER_W * CURSOR_LAYER_H];
static uint32_t cursor_outer_layer[CURSOR_LAYER_W * CURSOR_LAYER_H];
static uint32_t cursor_inner_layer[CURSOR_LAYER_W * CURSOR_LAYER_H];
static int cursor_layers_ready;

static int cursor_inside(int x, int y, const struct cursor_pt *p, int n) {
    int inside = 0;
    for (int i = 0, j = n - 1; i < n; j = i++) {
        int yi = p[i].y, yj = p[j].y;
        if ((yi > y) != (yj > y)) {
            int64_t cross = (int64_t)(p[j].x - p[i].x) * (y - yi);
            int edge_x = p[i].x + (int)(cross / (yj - yi));
            if (x < edge_x) inside = !inside;
        }
    }
    return inside;
}

static void cursor_polygon_layer(const struct cursor_pt *p, int n,
                                 uint8_t r, uint8_t g, uint8_t b,
                                 uint8_t max_alpha, uint32_t *layer) {
    for (int y = 0; y < CURSOR_LAYER_H; y++) {
        for (int x = 0; x < CURSOR_LAYER_W; x++) {
            int hits = 0;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++)
                    hits += cursor_inside(x * 4 + sx, y * 4 + sy, p, n);
            uint8_t a = (uint8_t)((hits * max_alpha + 8) / 16);
            /* fb_blit_blend expects straight (non-premultiplied) ARGB and
             * applies alpha itself. Premultiplying here applies alpha twice,
             * producing the dark blocks/trails seen while moving the cursor. */
            layer[y * CURSOR_LAYER_W + x] =
                a ? FB_ARGB(a, r, g, b) : 0;
        }
    }
}

static void cursor_prepare_layers(void) {
    if (cursor_layers_ready) return;
    cursor_polygon_layer(cursor_outer, 7, 0x08, 0x0a, 0x10, 72,
                         cursor_shadow);
    cursor_polygon_layer(cursor_outer, 7, 0x08, 0x0a, 0x10, 255,
                         cursor_outer_layer);
    cursor_polygon_layer(cursor_inner, 7, 0xf8, 0xf8, 0xf5, 255,
                         cursor_inner_layer);
    cursor_layers_ready = 1;
}

static void draw_cursor(int cx, int cy) {
    cursor_prepare_layers();
    fb_blit_blend(cx + 2, cy + 2, CURSOR_LAYER_W, CURSOR_LAYER_H,
                  cursor_shadow, CURSOR_LAYER_W);

    fb_blit_blend(cx, cy, CURSOR_LAYER_W, CURSOR_LAYER_H,
                  cursor_outer_layer, CURSOR_LAYER_W);

    fb_blit_blend(cx, cy, CURSOR_LAYER_W, CURSOR_LAYER_H,
                  cursor_inner_layer, CURSOR_LAYER_W);
}

/* Software-cursor save-under.  Cursor pixels live only in the framebuffer,
 * never in a window backing store.  Restoring this small snapshot avoids
 * recomposing the complete desktop for every pointer movement. */
static void cursor_restore_under(void) {
    if (!g_cursor_valid || !g_cursor_under_valid) return;
    /* One clipped blit also produces one dirty-region update.  The old
     * per-pixel restore acquired the framebuffer dirty lock hundreds of times
     * per mouse packet and was the main source of pointer latency. */
    fb_blit(g_cursor_x, g_cursor_y, CURSOR_W, CURSOR_H,
            g_cursor_under, CURSOR_W);
    g_cursor_under_valid = 0;
}

static void cursor_capture_and_draw(void) {
    if (!g_cursor_valid) return;
    const fb_info_t *fi = fb_get_info();
    if (!fi) return;
    for (int y = 0; y < CURSOR_H; y++) {
        int sy = g_cursor_y + y;
        for (int x = 0; x < CURSOR_W; x++) {
            int sx = g_cursor_x + x;
            g_cursor_under[y * CURSOR_W + x] =
                (sx >= 0 && sy >= 0 &&
                 sx < (int)fi->width && sy < (int)fi->height)
                    ? fb_get_pixel((uint32_t)sx, (uint32_t)sy) : 0;
        }
    }
    g_cursor_under_valid = 1;
    draw_cursor(g_cursor_x, g_cursor_y);
}

/* Repaint screen region [rx0,ry0)-(rx1,ry1): desktop, then all windows that
 * intersect it, bottom-to-top. Caller holds g_comp_lock. Presents the region. */
static void region_grow_corners(int *rx0, int *ry0, int *rx1, int *ry1) {
    const int r = WIN_RADIUS;
    /* growing may newly clip another window's corner, so settle it */
    for (int pass = 0; pass < 4; pass++) {
        int grew = 0;
        for (int i = 0; i < COMP_MAX_WINDOWS; i++) {
            struct comp_window *w = &g_wins[i];
            if (!w->used || !w->visible || !win_shadows(w)) continue;
            int bx[4], by[4], cx[4], cy[4];
            if (!corner_boxes(w, bx, by, cx, cy)) continue;
            for (int k = 0; k < 4; k++) {
                if (bx[k] + r <= *rx0 || bx[k] >= *rx1 ||
                    by[k] + r <= *ry0 || by[k] >= *ry1) continue;   /* no touch */
                if (bx[k]     < *rx0) { *rx0 = bx[k];     grew = 1; }
                if (by[k]     < *ry0) { *ry0 = by[k];     grew = 1; }
                if (bx[k] + r > *rx1) { *rx1 = bx[k] + r; grew = 1; }
                if (by[k] + r > *ry1) { *ry1 = by[k] + r; grew = 1; }
            }
        }
        if (!grew) break;
    }
}

static void paint_region(int rx0, int ry0, int rx1, int ry1) {
    const fb_info_t *fi = fb_get_info();
    if (!fi) return;
    int W = (int)fi->width, H = (int)fi->height;
    region_grow_corners(&rx0, &ry0, &rx1, &ry1);
    if (rx0 < 0) rx0 = 0; if (ry0 < 0) ry0 = 0;
    if (rx1 > W) rx1 = W; if (ry1 > H) ry1 = H;
    if (rx1 <= rx0 || ry1 <= ry0) return;

    /* Remove the software cursor before changing the composed framebuffer.
     * This also guarantees that its saved background never contains a previous
     * cursor image. */
    cursor_restore_under();

    /* 1) desktop background: the aurora field (built once, then blitted). */
    aurora_build(W, H);
    if (g_aurora && g_aurora_w == W && g_aurora_h == H) {
        fb_blit(rx0, ry0, rx1 - rx0, ry1 - ry0, g_aurora + (size_t)ry0 * W + rx0, (uint32_t)W);
    } else {
        for (int y = ry0; y < ry1; y++)
            fb_fill_rect(rx0, y, rx1 - rx0, 1, desktop_color(y, H));
    }

    /* 2) windows, ascending z: repeatedly find the lowest not-yet-drawn z.
     *    O(N^2) over <=8 windows -- trivially cheap and keeps the code obvious. */
    int tz = top_z();
    int drawn = 0;
    int last_z = -1;
    while (drawn < COMP_MAX_WINDOWS) {
        int best = -1;
        for (int i = 0; i < COMP_MAX_WINDOWS; i++) {
            if (!g_wins[i].used || !g_wins[i].visible) continue;
            if (g_wins[i].z <= last_z) continue;
            if (best < 0 || g_wins[i].z < g_wins[best].z) best = i;
        }
        if (best < 0) break;
        paint_window(&g_wins[best], g_wins[best].z == tz, rx0, ry0, rx1, ry1);
        last_z = g_wins[best].z;
        drawn++;
    }

    /* 2b) the flying ghost of a window being opened or parked. Drawn HERE --
     *     above the windows, below the cursor -- so every repaint composes it
     *     correctly, whatever triggered the repaint. */
    if (g_anim.kind != ANIM_NONE) {
        const uint32_t *src = 0; int sw_ = 0, sh_ = 0;
        if (g_anim.snap) {
            src = g_anim.snap; sw_ = g_anim.snap_w; sh_ = g_anim.snap_h;
        } else {
            struct comp_window *aw = win_by_id(g_anim.win_id);
            if (aw && aw->content) { src = aw->content; sw_ = (int)aw->cw; sh_ = (int)aw->ch; }
        }
        if (src) {
            int r[4], alpha; anim_frame_rect(r, &alpha);
            fb_blit_scaled_uniform(r[0], r[1], r[2], r[3], src, sw_, sh_,
                                   (uint32_t)sw_, (uint32_t)alpha, rx0, ry0, rx1, ry1);
        }
    }

    /* 3) save the fresh pixels below the cursor, then put it back on top. */
    cursor_capture_and_draw();

    fb_present();
}

/* Repaint just a window's title-bar strip (so its focus tint updates without
 * touching its content). */
static void paint_titlebar(struct comp_window *w) {
    if (w->desktop || w->chromeless) return;   /* no kernel bar to repaint */
    paint_region(w->x, w->y, w->x + (int)w->cw, w->y + COMP_TITLEBAR_H);
}

/* After any z-order change, keep the focus highlight honest: the front window
 * gets the accent title bar, everyone else the muted one. Only the two windows
 * whose focus actually flipped need repainting. Caller holds g_comp_lock. */
static void enforce_focus(void) {
    struct comp_window *top = front_window();
    uint32_t nt = top ? top->id : 0;
    if (nt == g_top_id) return;
    struct comp_window *old = win_by_id(g_top_id);   /* previously focused */
    g_top_id = nt;
    /* Repaint both windows' footprints (grown by the shadow band) so DEPTH
     * follows focus: the window losing focus redraws with the shallow shadow,
     * the one gaining it with the deep one. That difference is now the focus
     * indicator, so it has to be repainted as carefully as the title tint.
     * paint_titlebar covers the kernel-chrome case; the region repaint covers
     * chromeless/glass windows (whose shadow lives outside their frame). */
    if (old && old->visible) {
        paint_titlebar(old);
        if (win_shadows(old)) { int x0, y0, x1, y1; win_repaint_rect(old, &x0, &y0, &x1, &y1); paint_region(x0, y0, x1, y1); }
    }
    if (top) {
        paint_titlebar(top);
        if (win_shadows(top)) { int x0, y0, x1, y1; win_repaint_rect(top, &x0, &y0, &x1, &y1); paint_region(x0, y0, x1, y1); }
    }
}

/* Release a window's pixel backing. For a shared (zero-copy) window this MUST
 * run while the client address space is still alive (before its teardown), so
 * the pages -- which the compositor owns, not the client -- are unmapped from
 * the client first and freed exactly once here (not again by the client's
 * address-space teardown). */
static void win_free_backing(struct comp_window *w) {
    if (w->shared) {
        if (w->client_pml4 && w->client_va) {
            for (uint32_t i = 0; i < w->npages; i++)
                vmm_unmap_in(w->client_pml4, w->client_va + (uint64_t)i * PAGE_SIZE);
        }
        if (w->kview) vmm_kunmap_pages(w->kview, w->npages);
        if (w->phys) {
            for (uint32_t i = 0; i < w->npages; i++) pmm_free_page(w->phys[i]);
            kfree(w->phys);
        }
        w->phys = 0; w->kview = 0; w->client_va = 0; w->client_pml4 = 0; w->shared = 0;
    } else if (w->content) {
        kfree(w->content);
    }
    w->content = 0;
}

/* ---- public API --------------------------------------------------------- */

int64_t compositor_win_create(int pid, uint32_t cw, uint32_t ch,
                              int32_t x, int32_t y, const char *title) {
    const fb_info_t *fi = fb_get_info();
    if (!fi) return -EMBK_ENODEV;
    if (cw == 0 || ch == 0 || cw > fi->width || ch + COMP_TITLEBAR_H > fi->height)
        return -EMBK_EINVAL;

    spin_lock(&g_comp_lock);
    int slot = -1;
    for (int i = 0; i < COMP_MAX_WINDOWS; i++)
        if (!g_wins[i].used) { slot = i; break; }
    if (slot < 0) { spin_unlock(&g_comp_lock); return -EMBK_ENOMEM; }

    uint32_t *buf = (uint32_t *)kmalloc((size_t)cw * ch * 4);
    if (!buf) { spin_unlock(&g_comp_lock); return -EMBK_ENOMEM; }
    /* start opaque black so an un-presented window isn't garbage */
    for (size_t i = 0; i < (size_t)cw * ch; i++) buf[i] = 0xFF000000u;

    struct comp_window *w = &g_wins[slot];
    w->used = 1;
    w->id = g_next_id++;
    w->pid = pid;
    w->x = x; w->y = y;
    w->cw = cw; w->ch = ch;
    w->z = g_next_z++;
    w->visible = 1;
    w->desktop = 0;
    w->chromeless = 0;
    w->widget = 0;
    w->glass = 0;
    w->translucent = 0;
    w->pend_click = 0;
    w->content = buf;
    int n = 0;
    if (title) { while (n < COMP_TITLE_MAX && title[n]) { w->title[n] = title[n]; n++; } }
    w->title[n] = 0;

    uint32_t id = w->id;

    if (!g_active) {
        /* first window: take over the whole screen with the desktop */
        g_active = 1;
        paint_region(0, 0, (int)fi->width, (int)fi->height);
    } else {
        int fx0, fy0, fx1, fy1; win_repaint_rect(w, &fx0, &fy0, &fx1, &fy1);
        paint_region(fx0, fy0, fx1, fy1);
    }
    enforce_focus();   /* the new window is now front; demote the old front */
    spin_unlock(&g_comp_lock);
    kprintf("compositor: window %u created (%ux%u) for pid %d\n",
            (unsigned)id, (unsigned)cw, (unsigned)ch, pid);
    return (int64_t)id;
}

/* Zero-copy variant: the compositor allocates page-aligned pixel pages, maps
 * them into a flat cached kernel view (for compositing) AND into the CLIENT
 * (so it renders directly into shared memory). No per-present pixel copy --
 * win_present becomes damage-only. `out_client_va` receives the client mapping. */
static int64_t win_create_shared_impl(struct process *client, uint32_t cw, uint32_t ch,
                                      int32_t x, int32_t y, const char *title,
                                      int desktop, int chromeless, int widget, int glass,
                                      int translucent,
                                      uint64_t *out_client_va) {
    const fb_info_t *fi = fb_get_info();
    if (!fi || !client) return -EMBK_EINVAL;
    if (widget) chromeless = 1;                  /* widgets are always chromeless */
    if (glass)  chromeless = 1;                  /* glass windows own their chrome */
    if (translucent) chromeless = 1;             /* translucent windows own their chrome */
    uint32_t bar = (desktop || chromeless) ? 0 : COMP_TITLEBAR_H;
    if (cw == 0 || ch == 0 || cw > fi->width || ch + bar > fi->height)
        return -EMBK_EINVAL;

    uint64_t bytes = (uint64_t)cw * ch * 4;
    uint32_t npages = (uint32_t)((bytes + PAGE_SIZE - 1) / PAGE_SIZE);
    uint64_t *phys = (uint64_t *)kmalloc((size_t)npages * sizeof(uint64_t));
    if (!phys) return -EMBK_ENOMEM;
    uint32_t got = 0;
    for (; got < npages; got++) { uint64_t p = pmm_alloc_page(); if (!p) break; phys[got] = p; }
    if (got < npages) {
        for (uint32_t i = 0; i < got; i++) pmm_free_page(phys[i]);
        kfree(phys); return -EMBK_ENOMEM;
    }

    uint64_t kview = vmm_kmap_pages(phys, npages);   /* compositor's flat view */
    if (!kview) {
        for (uint32_t i = 0; i < npages; i++) pmm_free_page(phys[i]);
        kfree(phys); return -EMBK_ENOMEM;
    }

    /* map the SAME pages into the client so it renders directly (zero-copy) */
    uint64_t cva = client->shared_next_va, va = cva;
    int ok = 1;
    for (uint32_t i = 0; i < npages; i++) {
        if (vmm_map_in(client->pml4_phys, va, phys[i], VMM_USER | VMM_WRITABLE | VMM_NX) < 0) { ok = 0; break; }
        va += PAGE_SIZE;
    }
    if (!ok) {
        for (uint64_t u = cva; u < va; u += PAGE_SIZE) vmm_unmap_in(client->pml4_phys, u);
        vmm_kunmap_pages(kview, npages);
        for (uint32_t i = 0; i < npages; i++) pmm_free_page(phys[i]);
        kfree(phys); return -EMBK_ENOMEM;
    }
    client->shared_next_va = cva + (uint64_t)npages * PAGE_SIZE;

    spin_lock(&g_comp_lock);
    int slot = -1;
    for (int i = 0; i < COMP_MAX_WINDOWS; i++) if (!g_wins[i].used) { slot = i; break; }
    if (slot < 0) {
        spin_unlock(&g_comp_lock);
        for (uint32_t i = 0; i < npages; i++) vmm_unmap_in(client->pml4_phys, cva + (uint64_t)i * PAGE_SIZE);
        vmm_kunmap_pages(kview, npages);
        for (uint32_t i = 0; i < npages; i++) pmm_free_page(phys[i]);
        kfree(phys); return -EMBK_ENOMEM;
    }
    struct comp_window *w = &g_wins[slot];
    w->used = 1; w->id = g_next_id++; w->pid = (int)client->pid;
    w->x = x; w->y = y; w->cw = cw; w->ch = ch; w->visible = 1;
    w->desktop = desktop;
    w->chromeless = chromeless;
    w->widget = widget;
    w->glass = glass && !desktop;
    w->translucent = translucent && !desktop;
    w->pend_click = 0;
    w->z = desktop ? 0 : widget ? g_widget_z++ : g_next_z++;   /* z band per kind */
    w->content = (uint32_t *)kview;
    w->shared = 1; w->phys = phys; w->npages = npages;
    w->kview = kview; w->client_va = cva; w->client_pml4 = client->pml4_phys;
    /* the desktop (home) and glass WIDGETS start TRANSPARENT so the aurora shows
     * through wherever they render nothing / a translucent tint; opaque app
     * windows (incl. acrylic, which is composited at a uniform alpha) start black */
    { uint32_t init_px = (desktop || (glass && widget) || translucent) ? 0x00000000u : 0xFF000000u;
      for (size_t i = 0; i < (size_t)cw * ch; i++) w->content[i] = init_px; }
    int n = 0;
    if (title) { while (n < COMP_TITLE_MAX && title[n]) { w->title[n] = title[n]; n++; } }
    w->title[n] = 0;
    uint32_t id = w->id;
    if (!g_active) { g_active = 1; paint_region(0, 0, (int)fi->width, (int)fi->height); }
    else { int fx0, fy0, fx1, fy1; win_repaint_rect(w, &fx0, &fy0, &fx1, &fy1); paint_region(fx0, fy0, fx1, fy1); }
    enforce_focus();
    spin_unlock(&g_comp_lock);
    if (out_client_va) *out_client_va = cva;
    kprintf("compositor: %s window %u created (%ux%u, %u pages) for pid %d\n",
            desktop ? "desktop" : "shared",
            (unsigned)id, (unsigned)cw, (unsigned)ch, (unsigned)npages, (int)client->pid);
    return (int64_t)id;
}

/* Zero-copy floating window (public). */
int64_t compositor_win_create_shared(struct process *client, uint32_t cw, uint32_t ch,
                                     int32_t x, int32_t y, const char *title,
                                     uint64_t *out_client_va) {
    return win_create_shared_impl(client, cw, ch, x, y, title, 0, 0, 0, 0, 0, out_client_va);
}

/* Zero-copy floating window with NO kernel chrome: the app draws its own bar
 * and close control (EmUI V4 Window/WindowBar) and moves itself via win_move.
 * Normal z-order otherwise (raisable, focusable, killable). */
int64_t compositor_win_create_chromeless(struct process *client, uint32_t cw, uint32_t ch,
                                         int32_t x, int32_t y, const char *title,
                                         uint64_t *out_client_va) {
    return win_create_shared_impl(client, cw, ch, x, y, title, 0, 1, 0, 0, 0, out_client_va);
}

/* Resize a SHARED window's content to (nw,nh): allocate a fresh page backing,
 * map it into the client at a new VA (returned via out_client_va), swap it in,
 * and repaint the union of the old+new footprints. The OLD backing is unmapped
 * and freed after the swap; the client must stop using its old pointer as soon
 * as this returns (the EmApp runtime does the swap synchronously). */
int64_t compositor_win_resize(struct process *client, uint32_t id,
                              uint32_t nw, uint32_t nh, uint64_t *out_client_va) {
    const fb_info_t *fi = fb_get_info();
    if (!fi || !client) return -EMBK_EINVAL;
    if (nw < 120 || nh < 90 || nw > fi->width || nh > fi->height) return -EMBK_EINVAL;

    /* new backing first (outside the lock: pmm/vmm work) */
    uint64_t bytes = (uint64_t)nw * nh * 4;
    uint32_t npages = (uint32_t)((bytes + PAGE_SIZE - 1) / PAGE_SIZE);
    uint64_t *phys = (uint64_t *)kmalloc((size_t)npages * sizeof(uint64_t));
    if (!phys) return -EMBK_ENOMEM;
    uint32_t got = 0;
    for (; got < npages; got++) { uint64_t p = pmm_alloc_page(); if (!p) break; phys[got] = p; }
    if (got < npages) {
        for (uint32_t i = 0; i < got; i++) pmm_free_page(phys[i]);
        kfree(phys); return -EMBK_ENOMEM;
    }
    uint64_t kview = vmm_kmap_pages(phys, npages);
    if (!kview) {
        for (uint32_t i = 0; i < npages; i++) pmm_free_page(phys[i]);
        kfree(phys); return -EMBK_ENOMEM;
    }
    uint64_t cva = client->shared_next_va, va = cva;
    int ok = 1;
    for (uint32_t i = 0; i < npages; i++) {
        if (vmm_map_in(client->pml4_phys, va, phys[i], VMM_USER | VMM_WRITABLE | VMM_NX) < 0) { ok = 0; break; }
        va += PAGE_SIZE;
    }
    if (!ok) {
        for (uint64_t u = cva; u < va; u += PAGE_SIZE) vmm_unmap_in(client->pml4_phys, u);
        vmm_kunmap_pages(kview, npages);
        for (uint32_t i = 0; i < npages; i++) pmm_free_page(phys[i]);
        kfree(phys); return -EMBK_ENOMEM;
    }
    client->shared_next_va = cva + (uint64_t)npages * PAGE_SIZE;
    for (size_t i = 0; i < (size_t)nw * nh; i++) ((uint32_t *)kview)[i] = 0xFF000000u;

    /* swap under the lock; remember the old backing to free after */
    uint64_t *ophys = 0; uint32_t onpages = 0; uint64_t okview = 0, ocva = 0, opml4 = 0;
    int ox0, oy0, ox1, oy1, nx0, ny0, nx1, ny1;
    spin_lock(&g_comp_lock);
    struct comp_window *w = win_find((int)client->pid, id);
    if (!w || !w->shared) {
        spin_unlock(&g_comp_lock);
        for (uint32_t i = 0; i < npages; i++) vmm_unmap_in(client->pml4_phys, cva + (uint64_t)i * PAGE_SIZE);
        vmm_kunmap_pages(kview, npages);
        for (uint32_t i = 0; i < npages; i++) pmm_free_page(phys[i]);
        kfree(phys); return -EMBK_EINVAL;
    }
    win_repaint_rect(w, &ox0, &oy0, &ox1, &oy1);   /* old footprint */
    ophys = w->phys; onpages = w->npages; okview = w->kview; ocva = w->client_va; opml4 = w->client_pml4;
    w->cw = nw; w->ch = nh;
    w->content = (uint32_t *)kview;
    w->phys = phys; w->npages = npages; w->kview = kview;
    w->client_va = cva; w->client_pml4 = client->pml4_phys;
    win_repaint_rect(w, &nx0, &ny0, &nx1, &ny1);   /* new footprint */
    paint_region(imin(ox0, nx0), imin(oy0, ny0), imax(ox1, nx1), imax(oy1, ny1));
    spin_unlock(&g_comp_lock);

    /* free the OLD backing (nobody references it any more) */
    if (ophys) {
        for (uint32_t i = 0; i < onpages; i++) vmm_unmap_in(opml4, ocva + (uint64_t)i * PAGE_SIZE);
        vmm_kunmap_pages(okview, onpages);
        for (uint32_t i = 0; i < onpages; i++) pmm_free_page(ophys[i]);
        kfree(ophys);
    }
    if (out_client_va) *out_client_va = cva;
    kprintf("compositor: win %u resized to %ux%u for pid %d\n",
            (unsigned)id, (unsigned)nw, (unsigned)nh, (int)client->pid);
    return (int64_t)id;
}

/* DESKTOP WIDGET (EmUI V5): chromeless, and z-banded ABOVE the desktop but
 * BELOW every app window -- an always-visible tile apps float over. */
int64_t compositor_win_create_widget(struct process *client, uint32_t cw, uint32_t ch,
                                     int32_t x, int32_t y, const char *title,
                                     int glass, uint64_t *out_client_va) {
    return win_create_shared_impl(client, cw, ch, x, y, title, 0, 1, 1, glass, 0, out_client_va);
}

/* Full-screen chromeless HOME/desktop window: sized to the framebuffer, pinned
 * at the back (z=0), no title bar. Returns the screen dims via *out_w/*out_h so
 * the client needn't hardcode them. Zero-copy like the floating variant. */
int64_t compositor_win_create_desktop(struct process *client, uint64_t *out_client_va,
                                      uint32_t *out_w, uint32_t *out_h) {
    const fb_info_t *fi = fb_get_info();
    if (!fi) return -EMBK_ENODEV;
    int64_t id = win_create_shared_impl(client, fi->width, fi->height, 0, 0,
                                        "", 1, 0, 0, 0, 0, out_client_va);
    if (id > 0) { if (out_w) *out_w = fi->width; if (out_h) *out_h = fi->height; }
    return id;
}

/* Zero-copy GLASS window (EmUI V8): chromeless, but the compositor blurs the
 * backdrop behind it and composites its translucent pixels over the frost. */
int64_t compositor_win_create_glass(struct process *client, uint32_t cw, uint32_t ch,
                                     int32_t x, int32_t y, const char *title,
                                     uint64_t *out_client_va) {
    return win_create_shared_impl(client, cw, ch, x, y, title, 0, 1, 0, 1, 0, out_client_va);
}

/* Zero-copy TRANSLUCENT window: chromeless, per-pixel transparent, NO blur --
 * composited over the sharp backdrop like the desktop but raisable. A thin bar
 * with a tall invisible canvas for its dropdowns (EmUI menu bar). */
int64_t compositor_win_create_translucent(struct process *client, uint32_t cw, uint32_t ch,
                                          int32_t x, int32_t y, const char *title,
                                          uint64_t *out_client_va) {
    return win_create_shared_impl(client, cw, ch, x, y, title, 0, 1, 0, 0, 1, out_client_va);
}

int compositor_win_is_shared(int pid, uint32_t id) {
    spin_lock(&g_comp_lock);
    struct comp_window *w = win_find(pid, id);
    int s = w ? w->shared : 0;
    spin_unlock(&g_comp_lock);
    return s;
}

uint32_t *compositor_win_content(int pid, uint32_t id, uint32_t *cw, uint32_t *ch) {
    spin_lock(&g_comp_lock);
    struct comp_window *w = win_find(pid, id);
    uint32_t *buf = 0;
    if (w) { buf = w->content; if (cw) *cw = w->cw; if (ch) *ch = w->ch; }
    spin_unlock(&g_comp_lock);
    return buf;
}

int64_t compositor_win_damage(int pid, uint32_t id,
                              uint32_t rx, uint32_t ry, uint32_t rw, uint32_t rh) {
    spin_lock(&g_comp_lock);
    struct comp_window *w = win_find(pid, id);
    if (!w) { spin_unlock(&g_comp_lock); return -EMBK_ENOENT; }
    if (rw == 0 || rh == 0) { rx = 0; ry = 0; rw = w->cw; rh = w->ch; }
    if (rx > w->cw) rx = w->cw; if (ry > w->ch) ry = w->ch;
    if (rx + rw > w->cw) rw = w->cw - rx;
    if (ry + rh > w->ch) rh = w->ch - ry;
    /* content coords -> screen coords (content sits below the title bar, or at
     * the frame origin for the chromeless desktop) */
    int sx0 = w->x + (int)rx;
    int sy0 = w->y + win_titlebar_h(w) + (int)ry;
    if (!w->presented) {
        /* first real frame: this is the moment the window becomes something
         * worth showing, so this -- not create -- is where it grows in. */
        w->presented = 1;
        anim_start(w, ANIM_OPEN);
        int fx0, fy0, fx1, fy1; win_repaint_rect(w, &fx0, &fy0, &fx1, &fy1);
        paint_region(fx0, fy0, fx1, fy1);
        spin_unlock(&g_comp_lock);
        return 0;
    }
    paint_region(sx0, sy0, sx0 + (int)rw, sy0 + (int)rh);
    spin_unlock(&g_comp_lock);
    return 0;
}

int64_t compositor_win_move(int pid, uint32_t id, int32_t x, int32_t y) {
    spin_lock(&g_comp_lock);
    struct comp_window *w = win_find(pid, id);
    if (!w) { spin_unlock(&g_comp_lock); return -EMBK_ENOENT; }
    int ox0, oy0, ox1, oy1; win_repaint_rect(w, &ox0, &oy0, &ox1, &oy1);
    const fb_info_t *fi = fb_get_info();
    /* A translucent bar IS the top-strip chrome, so it may sit above WORK_TOP
     * (a menu bar at y=6). Ordinary app windows still stay in the work area. */
    if (!w->desktop && !w->widget && !w->translucent && fi) {
        int maxx = (int)fi->width - (int)w->cw;
        int maxy = (int)fi->height - WORK_BOTTOM - win_titlebar_h(w) - (int)w->ch;
        if (maxx < 0) maxx = 0;
        if (maxy < WORK_TOP) maxy = WORK_TOP;
        if (x < 0) x = 0;
        if (x > maxx) x = maxx;
        if (y < WORK_TOP) y = WORK_TOP;
        if (y > maxy) y = maxy;
    }
    w->x = x; w->y = y;
    w->z = w->widget ? g_widget_z++ : g_next_z++;   /* raise within its own band */
    int nx0, ny0, nx1, ny1; win_repaint_rect(w, &nx0, &ny0, &nx1, &ny1);
    /* repaint the union of old and new footprints in one region */
    paint_region(imin(ox0, nx0), imin(oy0, ny0), imax(ox1, nx1), imax(oy1, ny1));
    enforce_focus();   /* moving raises to front; demote whoever was front */
    spin_unlock(&g_comp_lock);
    return 0;
}

int64_t compositor_win_destroy(int pid, uint32_t id) {
    spin_lock(&g_comp_lock);
    struct comp_window *w = win_find(pid, id);
    if (!w) { spin_unlock(&g_comp_lock); return -EMBK_ENOENT; }
    int fx0, fy0, fx1, fy1; win_repaint_rect(w, &fx0, &fy0, &fx1, &fy1);
    win_free_backing(w);                       /* frees copy buf OR unmaps+frees shared pages */
    w->used = 0; w->visible = 0;
    if (id == g_top_id) g_top_id = 0;          /* force focus recompute below */
    paint_region(fx0, fy0, fx1, fy1);          /* expose whatever was behind */
    enforce_focus();                           /* promote the new front window */
    spin_unlock(&g_comp_lock);
    return 0;
}

/* A process is EXITING (normally, by its own hand): hide its windows and
 * repaint the pixels they covered, NOW, while we are in the dying process's
 * ordinary syscall context. The reap below cannot do this -- it runs later,
 * under the scheduler lock, where framebuffer work is off-limits -- which is
 * why a self-closed app's window used to stay painted on screen, hover-lit
 * and dead, until something else happened to repaint that region. Death
 * should be visible immediately, whatever the parent gets around to. */
void compositor_exit_pid(int pid) {
    spin_lock(&g_comp_lock);
    int found = 0;
    int animated = 0;
    for (int i = 0; i < COMP_MAX_WINDOWS; i++) {
        struct comp_window *w = &g_wins[i];
        if (!w->used || w->pid != pid || !w->visible) continue;
        /* Collapse the FIRST real window on the way out (one motion at a time,
         * and an app's second window is chrome or a widget anyway). */
        if (!animated) { anim_start(w, ANIM_CLOSE); animated = (g_anim.kind == ANIM_CLOSE); }
        w->visible = 0;
        if (w->id == g_top_id) g_top_id = 0;
        int x0, y0, x1, y1;
        win_repaint_rect(w, &x0, &y0, &x1, &y1);
        paint_region(x0, y0, x1, y1);
        found = 1;
    }
    if (found) enforce_focus();
    spin_unlock(&g_comp_lock);
}

/* An app parking ITSELF. Chromeless windows (every EmUI V4 app) have no kernel
 * title bar to click, so the toolkit's own MinimizeButton needs a way in --
 * otherwise minimize would exist only for the handful of apps still using
 * kernel chrome. */
int compositor_win_minimize(int pid, uint32_t id) {
    spin_lock(&g_comp_lock);
    struct comp_window *w = win_find(pid, id);
    if (!w || w->desktop) { spin_unlock(&g_comp_lock); return -1; }
    if (!w->minimized) {
        w->minimized = 1;
        if (w->id == g_top_id) g_top_id = 0;
        anim_start(w, ANIM_PARK);          /* hides it; the ghost flies down */
        int x0, y0, x1, y1;
        win_repaint_rect(w, &x0, &y0, &x1, &y1);
        paint_region(x0, y0, x1, y1);
        enforce_focus();
    }
    spin_unlock(&g_comp_lock);
    return 0;
}

/* Lift the desktop layer above every app window, or drop it back to the ground.
 * Only the layer's own process may ask -- this puts a full-screen surface over
 * everything the user has open, which is not a thing one app may do to another.
 * A MODE, not a raise: clearing it restores z=0 exactly, so the ground does not
 * end up parked somewhere in the middle of the stack. */
int compositor_desktop_front(int pid, int on) {
    spin_lock(&g_comp_lock);
    int rc = -1;
    for (int i = 0; i < COMP_MAX_WINDOWS; i++) {
        struct comp_window *w = &g_wins[i];
        if (!w->used || !w->desktop) continue;
        if (w->pid != pid) break;                  /* not yours to move */
        int want = on ? DESKTOP_FRONT_Z : 0;
        rc = 0;
        if (w->z != want) {
            w->z = want;
            /* THE MENU BAR STAYS ON TOP. It is a window like any other, so a
             * full-screen surface in front of every window is in front of it
             * too -- and the bar simply vanished, which is not what a shell
             * surface does on any system that has one. On a Mac the bar is
             * above Launchpad; here that means the translucent bars ride one
             * band higher than the lifted desktop, and go back where they were
             * when it drops. */
            for (int k = 0; k < COMP_MAX_WINDOWS; k++) {
                struct comp_window *b = &g_wins[k];
                if (!b->used || !b->translucent || b->desktop) continue;
                if (on) { b->z_saved = b->z; b->z = DESKTOP_FRONT_Z + 1; }
                else if (b->z == DESKTOP_FRONT_Z + 1) b->z = b->z_saved;
            }
            /* Every window's meaning in the stack just changed -- what was in
             * front is now behind, or the reverse -- so the whole screen is
             * what has to be recomposed, not the layer's own rect. */
            const struct fb_info *fi = fb_get_info();
            if (fi) paint_region(0, 0, (int)fi->width, (int)fi->height);
            enforce_focus();
        }
        break;
    }
    spin_unlock(&g_comp_lock);
    return rc;
}

/* Bring a process's windows back to the user: un-park anything it minimized,
 * and raise its windows to the front. Both halves are what "click the dock
 * icon of a running app" has to mean -- a parked window comes back, and an app
 * that is merely buried comes forward. Returns 1 if anything changed. */
int compositor_restore_pid(int pid) {
    spin_lock(&g_comp_lock);
    int changed = 0;
    for (int i = 0; i < COMP_MAX_WINDOWS; i++) {
        struct comp_window *w = &g_wins[i];
        if (!w->used || w->pid != pid) continue;
        if (w->desktop) continue;            /* the back layer is never raised */
        if (w->minimized) {
            w->minimized = 0;
            w->z = g_next_z++;             /* it comes back in front */
            anim_start(w, ANIM_UNPARK);    /* flies back up out of the dock */
            changed = 1;
            continue;                      /* anim_finish makes it visible */
        }
        if (!w->visible) continue;
        w->z = w->widget ? g_widget_z++ : g_next_z++;
        changed = 1;
        int x0, y0, x1, y1;
        win_repaint_rect(w, &x0, &y0, &x1, &y1);
        paint_region(x0, y0, x1, y1);
    }
    if (changed) enforce_focus();
    spin_unlock(&g_comp_lock);
    return changed;
}

/* Average perceived luminance (0-255) of the composed framebuffer over a screen
 * rect, or -1. A window that paints NO background of its own -- the menu bar --
 * cannot know whether its text will land on a dark wallpaper or a light one,
 * and it has no other way to find out: the wallpaper is another process's
 * window content. Sampling is strided; this is asked once or twice a second,
 * not per frame. */
int compositor_backdrop_luma(int x, int y, int w, int h) {
    const fb_info_t *fi = fb_get_info();
    if (!fi || w <= 0 || h <= 0) return -1;
    int x0 = imax(x, 0), y0 = imax(y, 0);
    int x1 = imin(x + w, (int)fi->width), y1 = imin(y + h, (int)fi->height);
    if (x1 <= x0 || y1 <= y0) return -1;
    int stepx = ((x1 - x0) / 64) + 1, stepy = ((y1 - y0) / 8) + 1;
    uint64_t sum = 0; uint32_t n = 0;
    spin_lock(&g_comp_lock);
    for (int yy = y0; yy < y1; yy += stepy) {
        for (int xx = x0; xx < x1; xx += stepx) {
            uint32_t c = fb_get_pixel((uint32_t)xx, (uint32_t)yy);
            uint32_t r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
            sum += (r * 54 + g * 183 + b * 19) >> 8;   /* Rec.601-ish, integer */
            n++;
        }
    }
    spin_unlock(&g_comp_lock);
    return n ? (int)(sum / n) : -1;
}

/* Declare (or clear, w<=0) the window-local sub-rect whose backdrop should be
 * frosted. Repaints the window so the change is visible at once. */
int compositor_win_blur_rect(int pid, uint32_t id, int x, int y, int w_, int h_) {
    spin_lock(&g_comp_lock);
    struct comp_window *w = win_find(pid, id);
    if (!w) { spin_unlock(&g_comp_lock); return -1; }
    w->blur_set = (w_ > 0 && h_ > 0);
    w->blur_x = x; w->blur_y = y; w->blur_w = w_; w->blur_h = h_;
    int x0, y0, x1, y1; win_repaint_rect(w, &x0, &y0, &x1, &y1);
    paint_region(x0, y0, x1, y1);
    spin_unlock(&g_comp_lock);
    return 0;
}

void compositor_reap_pid(int pid) {
    /* Reclaim a dead client's windows. MUST run BEFORE its address space is torn
     * down (process_reap_slot calls us pre-vmm_destroy): a shared window's pages
     * are unmapped from the client here so the address-space teardown doesn't
     * double-free them. No framebuffer work (safe under the scheduler lock); a
     * crashed client's pixels linger until the next repaint. */
    spin_lock(&g_comp_lock);
    for (int i = 0; i < COMP_MAX_WINDOWS; i++) {
        if (g_wins[i].used && g_wins[i].pid == pid) {
            win_free_backing(&g_wins[i]);
            g_wins[i].used = 0; g_wins[i].visible = 0;
            if (g_wins[i].id == g_top_id) g_top_id = 0;
        }
    }
    spin_unlock(&g_comp_lock);
}

/* Advance any running window motion by one frame. Called from the boot CPU's
 * main loop beside compositor_pointer_tick -- schedulable context, so the
 * compositor spinlock is never taken from an IRQ, and the ~100Hz timer that
 * wakes that loop is a fine frame clock for a 200ms move. No-op when idle,
 * which is almost always. */
void compositor_anim_tick(void) {
    if (g_anim.kind == ANIM_NONE) return;        /* cheap unlocked pre-check */
    spin_lock(&g_comp_lock);
    if (g_anim.kind == ANIM_NONE) { spin_unlock(&g_comp_lock); return; }

    int num, den; anim_progress(&num, &den);
    if (num >= den) { anim_finish(); spin_unlock(&g_comp_lock); return; }

    int x0, y0, x1, y1;
    anim_dirty(&x0, &y0, &x1, &y1);
    int r[4], a; anim_frame_rect(r, &a);
    for (int i = 0; i < 4; i++) g_anim.last[i] = r[i];
    g_anim.have_last = 1;
    paint_region(x0, y0, x1, y1);            /* draws the ghost itself */
    spin_unlock(&g_comp_lock);
}

void compositor_pointer_tick(void) {
    if (!g_active) return;               /* no desktop yet -> nothing to route */
    int32_t x, y; uint32_t b;
    mouse_get_state(&x, &y, &b);

    spin_lock(&g_comp_lock);
    int left = (b & MOUSE_BTN_LEFT) != 0, prev = (g_prev_buttons & MOUSE_BTN_LEFT) != 0;

    /* one accumulated dirty bbox for the whole tick -> a single paint_region */
    int have = 0, bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
    #define DIRTY(x0, y0, x1, y1) do {                                       \
        if (!have) { bx0=(x0); by0=(y0); bx1=(x1); by1=(y1); have=1; }       \
        else { if ((x0)<bx0)bx0=(x0); if ((y0)<by0)by0=(y0);                 \
               if ((x1)>bx1)bx1=(x1); if ((y1)>by1)by1=(y1); }               \
    } while (0)

    if (!g_cursor_valid) {
        g_cursor_x = x; g_cursor_y = y; g_cursor_valid = 1;
        cursor_capture_and_draw();
        fb_present();
    }

    int raised = 0;
    int close_pid = 0;   /* set when a close button is clicked */
    /* press edge: raise the window under the pointer; title-bar press = drag,
     * close-button press = close. The desktop (home) layer is never raised or
     * dragged -- it stays pinned at the back; its clicks flow through to the app
     * as content input below. */
    if (left && !prev) {
        struct comp_window *w = topmost_at(x, y);
        /* latch a content-area press for the owner (desktop included): if the
         * app is busy rendering when this press+release happens, win_input
         * replays it (press then release) so the click is never eaten. */
        if (w) {
            int clx = x - w->x, cly = y - (w->y + win_titlebar_h(w));
            if (clx >= 0 && clx < (int)w->cw && cly >= 0 && cly < (int)w->ch) {
                w->pend_click = 1; w->pend_lx = clx; w->pend_ly = cly;
            }
        }
        if (w && !w->desktop) {
            int title_control = 0;
            int cbx0, cby0, cbx1, cby1;
            int mbx0, mby0, mbx1, mby1;
            if (win_close_rect(w, &cbx0, &cby0, &cbx1, &cby1) &&
                x >= cbx0 && x < cbx1 && y >= cby0 && y < cby1) {
                /* close: HIDE the window now so it disappears immediately (the
                 * killed process is reaped asynchronously; don't wait on it),
                 * repaint what's behind, then kill its process below. Its shared
                 * pixel backing is freed later by compositor_reap_pid. */
                close_pid = w->pid;
                title_control = 1;
                if (w->id == g_top_id) g_top_id = 0;
                anim_start(w, ANIM_CLOSE);   /* hides it; the ghost collapses */
                w->visible = 0;              /* (and stays hidden if it declined) */
                int fx0, fy0, fx1, fy1; win_repaint_rect(w, &fx0, &fy0, &fx1, &fy1);
                DIRTY(fx0, fy0, fx1, fy1);
                raised = 1;   /* re-run enforce_focus() to promote the new front */
            } else if (win_min_rect(w, &mbx0, &mby0, &mbx1, &mby1) &&
                       x >= mbx0 && x < mbx1 && y >= mby0 && y < mby1) {
                /* minimize: park the window. The process keeps running (its dock
                 * dot stays lit); clicking its dock icon brings it back. */
                w->minimized = 1;
                if (w->id == g_top_id) g_top_id = 0;
                title_control = 1;
                anim_start(w, ANIM_PARK);      /* hides it; the ghost flies down */
                { int fx0, fy0, fx1, fy1; win_repaint_rect(w, &fx0, &fy0, &fx1, &fy1);
                  DIRTY(fx0, fy0, fx1, fy1); }
                raised = 1;   /* promote whatever is now in front */
            } else if (win_max_rect(w, &mbx0, &mby0, &mbx1, &mby1) &&
                       x >= mbx0 && x < mbx1 && y >= mby0 && y < mby1) {
                w->pending_action = 1; /* EMBK_WIN_ACTION_MAXIMIZE */
                title_control = 1;
                if (w != front_window()) w->z = g_next_z++;
                raised = 1;
                int fx0, fy0, fx1, fy1;
                win_repaint_rect(w, &fx0, &fy0, &fx1, &fy1);
                DIRTY(fx0, fy0, fx1, fy1);
            } else if (w != front_window()) {
                /* Raising a window that is already in front changes no pixels.
                 * Repainting its complete footprint made fullscreen login/setup
                 * flash on every click. */
                w->z = w->widget ? g_widget_z++ : g_next_z++;   /* raise within band */
                raised = 1;
                int fx0, fy0, fx1, fy1; win_repaint_rect(w, &fx0, &fy0, &fx1, &fy1);
                DIRTY(fx0, fy0, fx1, fy1);
            }
            if (!title_control && w->visible && y < w->y + win_titlebar_h(w)) {
                /* grabbed the title bar (also valid when already front) */
                g_dragging = 1; g_drag_id = w->id;
                g_drag_dx = x - w->x; g_drag_dy = y - w->y;
            }
        }
    }
    if (!left && prev) g_dragging = 0;   /* release ends any drag */

    /* drag: move the grabbed window to follow the pointer */
    if (g_dragging && left) {
        struct comp_window *w = win_by_id(g_drag_id);
        if (w) {
            int nx = x - g_drag_dx, ny = y - g_drag_dy;
            if (nx != w->x || ny != w->y) {
                int ox0, oy0, ox1, oy1; win_repaint_rect(w, &ox0, &oy0, &ox1, &oy1);
                const fb_info_t *fi = fb_get_info();
                if (fi && !w->widget && !w->translucent) {
                    int maxx = (int)fi->width - (int)w->cw;
                    int maxy = (int)fi->height - WORK_BOTTOM -
                               win_titlebar_h(w) - (int)w->ch;
                    if (maxx < 0) maxx = 0;
                    if (maxy < WORK_TOP) maxy = WORK_TOP;
                    if (nx < 0) nx = 0;
                    if (nx > maxx) nx = maxx;
                    if (ny < WORK_TOP) ny = WORK_TOP;
                    if (ny > maxy) ny = maxy;
                }
                w->x = nx; w->y = ny;
                int nx0, ny0, nx1, ny1; win_repaint_rect(w, &nx0, &ny0, &nx1, &ny1);
                DIRTY(ox0, oy0, ox1, oy1);
                DIRTY(nx0, ny0, nx1, ny1);
            }
        }
    }

    /* cursor motion: fold the old + new cursor rects into the dirty bbox so
     * the old cursor is erased and the new one drawn (paint_region stamps the
     * cursor at g_cursor_* last). */
    int cursor_moved = (x != g_cursor_x || y != g_cursor_y);
    if (cursor_moved) {
        cursor_restore_under();
        g_cursor_x = x; g_cursor_y = y;
    }

    if (have) {
        paint_region(bx0, by0, bx1, by1);
    } else if (cursor_moved) {
        cursor_capture_and_draw();
        fb_present();
    }
    if (raised) enforce_focus();   /* demote whoever used to be front */

    /* Record the content-local pointer for the topmost window under the cursor
     * (skipping its title bar) so its owning app can read it via sys_win_input.
     * A drag in progress keeps routing to no app (the compositor owns it). */
    g_ptr_pid = 0;
    if (!g_dragging) {
        if (!(b & MOUSE_BTN_LEFT)) g_cap_pid = 0;      /* release ends capture */
        struct comp_window *cap = g_cap_pid ? win_find(g_cap_pid, g_cap_win) : 0;
        if (cap && cap->visible) {
            /* captured: route to the press-owner regardless of containment */
            g_ptr_pid = cap->pid; g_ptr_win = cap->id;
            g_ptr_lx = x - cap->x;
            g_ptr_ly = y - (cap->y + win_titlebar_h(cap));
            g_ptr_buttons = b;
        } else {
            g_cap_pid = 0;
            struct comp_window *h = topmost_at(x, y);
            if (h) {
                int lx = x - h->x;
                int ly = y - (h->y + win_titlebar_h(h));
                if (lx >= 0 && lx < (int)h->cw && ly >= 0 && ly < (int)h->ch) {
                    g_ptr_pid = h->pid; g_ptr_win = h->id;
                    g_ptr_lx = lx; g_ptr_ly = ly; g_ptr_buttons = b;
                    if ((b & MOUSE_BTN_LEFT) && !(g_prev_buttons & MOUSE_BTN_LEFT)) {
                        g_cap_pid = h->pid; g_cap_win = h->id;   /* press starts capture */
                    }
                }
            }
        }
    }
    /* drain the scroll wheel every tick; route it to the hovered window's app */
    int32_t wheel = mouse_take_wheel();
    if (wheel && g_ptr_pid) g_ptr_wheel += wheel;

    g_prev_buttons = b;
    #undef DIRTY
    spin_unlock(&g_comp_lock);

    /* A close-button click terminates the owning process OUTSIDE g_comp_lock:
     * process_kill -> process_reap_slot -> compositor_reap_pid re-takes
     * g_comp_lock, so holding it here would deadlock. The window was already
     * hidden + repainted above, so it's gone regardless of when the reap runs. */
    if (close_pid) process_kill((uint32_t)close_pid);
}

/* Deliver the content-local pointer to the calling process IF it owns the window
 * currently under the cursor. Returns 1 (focused) with the out-params filled, or
 * 0 (the pointer isn't over this process's window content). This is how an app
 * inside a compositor window reads its mouse -- the home/desktop app gets every
 * click that falls through the floating windows onto it. */
int compositor_win_input(int pid, int32_t *lx, int32_t *ly,
                         uint32_t *buttons, uint32_t *win, int32_t *wheel) {
    spin_lock(&g_comp_lock);
    int focused = (g_ptr_pid != 0 && g_ptr_pid == pid);
    if (focused) {
        if (lx) *lx = g_ptr_lx;
        if (ly) *ly = g_ptr_ly;
        if (buttons) *buttons = g_ptr_buttons;
        if (win) *win = g_ptr_win;
        if (wheel) { *wheel = g_ptr_wheel; g_ptr_wheel = 0; }
    }

    /* Latched-click replay: if one of this pid's windows recorded a press the
     * app hasn't seen (it was busy rendering when press+release happened),
     * synthesize the click across two polls -- press first, release next --
     * at the latched content coords. If the app is polling LIVE while the
     * button is still physically down on that window, drop the latch instead
     * (the real-time path is already delivering it; replaying would double-
     * fire), which also keeps slider drags on real button state. */
    for (int i = 0; i < COMP_MAX_WINDOWS; i++) {
        struct comp_window *w = &g_wins[i];
        if (!w->used || w->pid != pid || !w->pend_click) continue;
        if (w->pend_click == 1) {
            if (focused && w->id == g_ptr_win && (g_ptr_buttons & MOUSE_BTN_LEFT)) {
                w->pend_click = 0;              /* live path saw the press */
            } else {
                if (lx) *lx = w->pend_lx;
                if (ly) *ly = w->pend_ly;
                if (buttons) *buttons = MOUSE_BTN_LEFT;
                if (win) *win = w->id;
                focused = 1;
                w->pend_click = 2;              /* release replays next poll */
            }
        } else {                                 /* == 2 */
            if (lx) *lx = w->pend_lx;
            if (ly) *ly = w->pend_ly;
            if (buttons) *buttons = 0;
            if (win) *win = w->id;
            focused = 1;
            w->pend_click = 0;
        }
        break;
    }
    /* Window-manager commands share the otherwise ordinary `win` result.
     * The high bit distinguishes them without changing the stable syscall ABI. */
    for (int i = 0; i < COMP_MAX_WINDOWS; i++) {
        if (g_wins[i].used && g_wins[i].pid == pid && g_wins[i].pending_action) {
            if (win) *win = 0x80000000u | (uint32_t)g_wins[i].pending_action;
            g_wins[i].pending_action = 0;
            break;
        }
    }
    spin_unlock(&g_comp_lock);
    return focused;
}

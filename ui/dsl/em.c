/* ui/dsl/em.c -- EmUI V2 implementation.
 *
 * Two layers:
 *   1. Containers (VStack/Card/...) emit immediately as brace scopes.
 *   2. Leaves (Text/Button/...) are CHAINABLE: each stages a "pending" element
 *      and returns a vtable of modifier function pointers; the modifiers mutate
 *      the pending element's props; it is emitted ("flushed") at the next
 *      element/container boundary. Deferring the emit is what lets a chain like
 *      Text("Hi").caption().secondary() apply style BEFORE the node is built,
 *      so it composes cleanly with the dirty-rect renderer (no re-style churn). */

#include "em.h"
#include "kit.h"
#include "font.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TH (ui_theme())

/* ---- small helpers ----------------------------------------------------- */

static struct paint solid(Color c) {
    struct paint p; p.kind = PAINT_SOLID; p.solid = c; p.n_stops = 0; return p;
}

/* 2- and 3-stop linear gradients (angle in degrees). Usable as a fill or, via
 * GradientBorder, as a border stroke. */
struct paint em_lgrad(Color a, Color b, float angle_deg) {
    struct paint p = {0};
    p.kind = PAINT_LINEAR_GRADIENT; p.solid = a;
    p.stops[0].offset = 0.0f; p.stops[0].color = a;
    p.stops[1].offset = 1.0f; p.stops[1].color = b;
    p.n_stops = 2; p.angle_deg = angle_deg;
    return p;
}
struct paint em_lgrad3(Color a, Color b, Color c, float angle_deg) {
    struct paint p = {0};
    p.kind = PAINT_LINEAR_GRADIENT; p.solid = a;
    p.stops[0].offset = 0.0f; p.stops[0].color = a;
    p.stops[1].offset = 0.5f; p.stops[1].color = b;
    p.stops[2].offset = 1.0f; p.stops[2].color = c;
    p.n_stops = 3; p.angle_deg = angle_deg;
    return p;
}
static Color shade(Color c, float k) {
    Color o = { c.r * k, c.g * k, c.b * k, c.a };
    if (o.r > 1) o.r = 1;
    if (o.g > 1) o.g = 1;
    if (o.b > 1) o.b = 1;
    return o;
}
static Color tint(Color c, float a) { Color o = c; o.a = a; return o; }

static struct layout_size sz_fixed(float v)  { return (struct layout_size){ .mode = SIZE_FIXED, .fixed_value = v }; }
static struct layout_size sz_grow(void)      { return (struct layout_size){ .mode = SIZE_FLEX, .flex_grow = 1 }; }
static struct layout_size sz_intrinsic(void) { return (struct layout_size){ .mode = SIZE_INTRINSIC }; }

static void utf8_enc(int cp, char *out) {
    unsigned c = (unsigned)cp;
    if (c < 0x80) { out[0] = (char)c; out[1] = 0; }
    else if (c < 0x800) { out[0] = 0xC0 | (c >> 6); out[1] = 0x80 | (c & 0x3F); out[2] = 0; }
    else if (c < 0x10000) { out[0] = 0xE0 | (c >> 12); out[1] = 0x80 | ((c >> 6) & 0x3F); out[2] = 0x80 | (c & 0x3F); out[3] = 0; }
    else { out[0] = 0xF0 | (c >> 18); out[1] = 0x80 | ((c >> 12) & 0x3F); out[2] = 0x80 | ((c >> 6) & 0x3F); out[3] = 0x80 | (c & 0x3F); out[4] = 0; }
}

/* PAGE ZOOM, not UI zoom.
 *
 * The toolkit's theme scale moves everything, chrome included -- which is what
 * you want when the whole desktop is too small, and NOT what a browser means
 * by zoom. A browser scales the DOCUMENT and leaves its own address bar alone.
 *
 * So the scale is a bracket the caller opens around the content it is
 * emitting, rather than a global setting: the browser turns it on before
 * rendering the page and off afterwards, and the chrome emitted outside those
 * brackets never sees it. */
static float g_text_scale = 1.0f;
void em_set_text_scale(float s) { g_text_scale = (s > 0.05f && s < 20.0f) ? s : 1.0f; }
float em_text_scale(void) { return g_text_scale; }

/* Resolve a length prop: 0 means "unset, use the default", EmZero means an
 * explicit zero. See EmProps. */
float em_len(float v, float dflt) { return v < 0.0f ? 0.0f : (v > 0.0f ? v : dflt); }

static void em_resolve_font(EmFont role, uint32_t *fh, float *sz) {
    const struct ui_theme *t = TH;
    switch (role) {
        case Title:    *fh = t->font_bold;    *sz = t->text_title;   break;
        case Subtitle: *fh = t->font_regular; *sz = t->text_title;   break;
        case Heading:  *fh = t->font_bold;    *sz = t->text_heading; break;
        case Caption:  *fh = t->font_regular; *sz = t->text_caption; break;
        case BodyBold: *fh = t->font_bold;    *sz = t->text_body;    break;
        default:       *fh = t->font_regular; *sz = t->text_body;    break;
    }
    *sz *= g_text_scale;
}
static enum layout_align  map_align(EmAlign a) {
    switch (a) { case Leading: return ALIGN_START; case Center: return ALIGN_CENTER;
                 case Trailing: return ALIGN_END; case Fill: return ALIGN_STRETCH; default: return ALIGN_START; }
}
static enum layout_justify map_justify(EmAlign a) {
    switch (a) { case Leading: return JUSTIFY_START; case Center: return JUSTIFY_CENTER;
                 case Trailing: return JUSTIFY_END; case SpaceBetween: return JUSTIFY_SPACE_BETWEEN; default: return JUSTIFY_START; }
}

/* True when the active theme is dark (glass tint/edge differ by ground). */
static int em_theme_is_dark(void) {
    Color b = TH->bg;
    return (0.299f * b.r + 0.587f * b.g + 0.114f * b.b) < 0.5f;
}

/* The GLASS material: blur whatever is behind this box, lay a translucent tint
 * over the blur (theme surface nudged toward the EmbLink accent so it reads as
 * ours, not neutral frosted glass), and rim it with a light edge highlight for
 * depth. The tint is <1.0 alpha so it takes cpu_draw_rect's constant-alpha
 * integer-LUT fast path; the blur is the only costly part and, because the
 * app runs a retained loop, a static panel over a static backdrop blurs once
 * and then idles. */
static void em_glass_apply(float blur) {
    const struct ui_theme *t = TH;
    int dark = em_theme_is_dark();
    Color base = t->surface_alt, acc = t->accent;
    Color tint = { base.r * 0.90f + acc.r * 0.10f,
                   base.g * 0.90f + acc.g * 0.10f,
                   base.b * 0.90f + acc.b * 0.10f,
                   dark ? 0.55f : 0.66f };
    Color edge = dark ? (Color){ 1, 1, 1, 0.18f } : (Color){ 1, 1, 1, 0.85f };
    ui_set_backdrop_blur(true, blur > 0 ? blur : 12.0f);
    ui_set_paint(solid(tint));
    ui_set_border(1.0f, edge);
}

static void em_apply_box(EmProps p) {
    if (p.spacing > 0) ui_set_spacing(p.spacing);
    int any_pad = (p.padding > 0 || p.px > 0 || p.py > 0 || p.pt > 0 || p.pr > 0 || p.pb > 0 || p.pl > 0);
    if (any_pad) {
        float t = p.padding, r = p.padding, b = p.padding, l = p.padding;
        if (p.px > 0) l = r = p.px;
        if (p.py > 0) t = b = p.py;
        if (p.pt > 0) t = p.pt;
        if (p.pr > 0) r = p.pr;
        if (p.pb > 0) b = p.pb;
        if (p.pl > 0) l = p.pl;
        ui_set_padding(t, r, b, l);
    }
    if (p.width > 0 || p.height > 0 || p.grow) {
        struct layout_size w = p.width > 0 ? sz_fixed(p.width) : (p.grow ? sz_grow() : sz_intrinsic());
        struct layout_size h = p.height > 0 ? sz_fixed(p.height) : sz_intrinsic();
        ui_set_size(w, h);
    }
    if (p.minw > 0 || p.maxw > 0 || p.minh > 0 || p.maxh > 0)
        ui_set_size_bounds(p.minw, p.maxw, p.minh, p.maxh);
    if (p.span > 0) ui_set_grid_span(p.span);   /* grid-cell column span */
    /* ALWAYS set a paint, even when there is no background.
     *
     * This used to be `if (a > 0)`, which meant a container with a transparent
     * background set NOTHING -- and the reconciler reuses instances, so the box
     * kept whatever fill it had last frame. Any highlight that toggles was
     * therefore one-way: press a sidebar row and it lit, press another and BOTH
     * stayed lit, because the row losing selection never said "no fill". It cost
     * us the same bug twice (the dock's running dot, then the Settings and Files
     * sidebars) before it was worth fixing at the source.
     *
     * EmProps cannot distinguish "unset" from "transparent" -- both are alpha 0 --
     * so the fix is that they MEAN THE SAME THING: no fill. Containers that want
     * a default (Card, Sidebar, Window...) now pass it through p.background
     * above rather than pre-painting behind this function's back. */
    ui_set_paint(p.background.a > 0 ? solid(p.background) : (struct paint){ 0 });
    if (p.corner > 0)       ui_set_corner_radius(p.corner);
    if (p.border > 0)       ui_set_border(p.border, p.border_color.a > 0 ? p.border_color : TH->border);
    if (p.shadow > 0) {
        const struct ui_theme *t = TH;
        struct ui_shadow_spec s = p.shadow == 1 ? t->shadow_sm : p.shadow == 2 ? t->shadow_md : t->shadow_lg;
        ui_set_shadow(true, s.dx, s.dy, s.blur, s.color);
    } else if (p.shadow < 0) {
        ui_set_shadow(false, 0, 0, 0, (Color){0,0,0,0});
    }
    if (p.glass) em_glass_apply(p.blur);   /* overrides fill+border, adds blur */
    if (p.align)   ui_set_align(map_align(p.align));
    if (p.justify) ui_set_justify(map_justify(p.justify));
    if (p.clip)    ui_set_clip_children(true);
    if (p.opacity > 0.0f && p.opacity < 1.0f) ui_set_opacity(p.opacity);
}

/* ---- tokens ------------------------------------------------------------ */

const EmTokens *em_tokens_(void) {
    static EmTokens tok;
    const struct ui_theme *t = TH;
    tok.accent = t->accent; tok.accent_soft = t->accent_soft; tok.on_accent = t->on_accent;
    tok.text = t->text; tok.secondary = t->text_secondary; tok.tertiary = t->text_tertiary;
    tok.surface = t->surface; tok.surface_alt = t->surface_alt; tok.bg = t->bg;
    tok.border = t->border; tok.border_strong = t->border_strong;
    tok.success = t->success; tok.warning = t->warning; tok.danger = t->danger;
    tok.clear = (Color){ 0, 0, 0, 0 };
    return &tok;
}

/* ---- pending element + emit dispatch (declared up front) --------------- */
void em_flush(void);

/* ---- containers (flush the pending leaf, then open) -------------------- */

void em_vstack_(EmProps p) { em_flush(); ui_begin_vstack(em_key_hash(p.key)); em_apply_box(p); }
void em_hstack_(EmProps p) { em_flush(); ui_begin_hstack(em_key_hash(p.key)); if (!p.align) ui_set_align(ALIGN_CENTER); em_apply_box(p); }
/* Flow: a horizontal stack whose children wrap onto new lines (flex-wrap). */
/* Flow FILLS its width, like Grid right below it. A wrap container sized to
 * its content has nothing to wrap against -- the sum of the children IS its
 * width, so the overflow that would cause a line break can never happen. It
 * looked fine for chips and tags, which never overflow; the browser's inline
 * runs, which exist to overflow, ran off the edge of the page. */
void em_flow_(EmProps p)   {
    em_flush(); ui_begin_hstack(0); ui_set_wrap(true);
    if (!p.align) ui_set_align(ALIGN_START);
    em_apply_box(p);
    if (p.width <= 0 && !p.grow) ui_set_size(sz_grow(), sz_intrinsic());
}
/* Grid: N equal columns, children auto-flow with optional .span; fills width. */
void em_grid_(int cols, EmProps p) {
    em_flush();
    float gap = p.spacing > 0 ? p.spacing : TH->sp3;
    ui_begin_vstack(0);
    em_apply_box(p);
    ui_set_grid(cols, gap, gap);
    if (p.width <= 0) ui_set_size(sz_grow(), sz_intrinsic());
}
void em_zstack_(EmProps p) { em_flush(); ui_begin_vstack(0); em_apply_box(p); }
void em_glass_(EmProps p)  { em_flush(); ui_begin_vstack(0); p.glass = 1;
                             if (p.corner == 0) p.corner = TH->radius_lg;
                             em_apply_box(p); }
void em_row_(EmProps p)    { em_flush(); ui_begin_hstack(0); ui_set_align(ALIGN_CENTER); if (!p.spacing) ui_set_spacing(TH->sp3); em_apply_box(p); }
void em_end_(void)         { em_flush(); ui_end_stack(); }

/* Container whose border is stroked with a gradient. Box props (bg, corner,
 * padding, ...) arrive via EmProps; the gradient border is applied last so it
 * wins over any solid .border. */
void em_gborder_(float width, struct paint g, EmProps p) {
    em_flush();
    ui_begin_vstack(0);
    em_apply_box(p);
    ui_set_border_gradient(width, &g);
}
void em_gborder_end_(void) { em_flush(); ui_end_stack(); }

/* The page-transition transform (opacity/slide), resolved by em_nav each frame.
 * Applied to the PAGE'S OWN root Screen -- em_nav no longer wraps the page in a
 * second full-size box for this (the Screen is already full-size; that wrapper
 * was redundant). 1.0f/0.0f = settled = a guarded no-op on every non-nav Screen. */
static float g_nav_cur_op = 1.0f, g_nav_cur_slide = 0.0f;

void em_screen_(EmProps p) {
    em_flush();
    const struct ui_theme *t = TH;
    ui_begin_vstack(0);
    if (p.background.a <= 0) p.background = t->bg;
    ui_set_size(sz_grow(), sz_grow());
    ui_set_padding(t->sp6, t->sp6, t->sp6, t->sp6);
    if (p.padding < 0) ui_set_padding(0, 0, 0, 0);
    ui_set_spacing(t->sp5);
    em_apply_box(p);
    /* carry the active page transition on the Screen itself (see g_nav_cur_*).
     * Applied every frame (incl. the settled 1.0/0.0) so the fade resets cleanly
     * when the transition ends; both setters are guarded so 1.0/0.0 costs nothing. */
    ui_set_opacity(g_nav_cur_op);
    ui_set_offset(g_nav_cur_slide, 0.0f);
}
void em_card_(EmProps p) {
    em_flush();
    const struct ui_theme *t = TH;
    ui_box_begin(0);
    if (p.background.a <= 0) p.background = t->surface;
    ui_set_corner_radius(p.corner > 0 ? p.corner : t->radius_lg);
    ui_set_border(1.0f, t->border);
    if (p.shadow >= 0) ui_set_shadow(true, t->shadow_md.dx, t->shadow_md.dy, t->shadow_md.blur, t->shadow_md.color);
    ui_set_padding(t->sp5, t->sp5, t->sp5, t->sp5);
    ui_set_spacing(t->sp4);
    ui_set_axis(AXIS_COLUMN);
    ui_set_align(ALIGN_STRETCH);
    em_apply_box(p);
}
void em_section_(const char *title, EmProps p) {
    em_flush();
    const struct ui_theme *t = TH;
    ui_begin_vstack(0);
    ui_set_spacing(p.spacing > 0 ? p.spacing : t->sp2);
    ui_set_align(ALIGN_STRETCH);
    em_apply_box(p);
    if (title && title[0]) {
        ui_set_font(t->font_bold); ui_set_text_size(t->text_caption); ui_set_text_color(t->text_tertiary);
        ui_text("%s", title);
    }
}
void em_navbar_(const char *title, EmProps p) {
    em_flush();
    const struct ui_theme *t = TH;
    ui_begin_hstack(0);
    ui_set_align(ALIGN_CENTER);
    ui_set_spacing(t->sp3);
    em_apply_box(p);
    if (title && title[0]) {
        ui_set_font(t->font_bold); ui_set_text_size(t->text_title); ui_set_text_color(t->text);
        ui_text("%s", title);
    }
    ui_spacer();
}
void em_scroll_(float *scroll_y, float viewport_h, EmProps p) { em_flush(); ui_scroll_begin(em_key_hash(p.key), viewport_h, scroll_y); em_apply_box(p); }
void em_scroll_end_(void) { em_flush(); ui_scroll_end(); }

/* ======================================================================= */
/* the chainable leaf layer                                                */
/* ======================================================================= */

typedef enum { PK_NONE, PK_TEXT, PK_ICON, PK_LABEL, PK_BADGE, PK_TAG, PK_AVATAR,
               PK_BANNER, PK_PROGRESS, PK_BUTTON, PK_ICONBTN, PK_TOGGLE, PK_CHECK,
               PK_SLIDER, PK_STEPPER, PK_FIELD, PK_PASSWORD, PK_SEGMENTED, PK_LISTROW,
               PK_CLOSEBTN, PK_MINBTN, PK_SEARCH, PK_SPINNER, PK_DROPDOWN } PKind;

static struct {
    int active; PKind kind; EmProps props; const char *id;
    const char *str, *str2; int cp; void *bind; int lo, hi; float frac;
    const char *const *labels; int count; size_t cap; char *buf;
    bool result;                 /* interactive result after flush */
} P;

#define ID_MAX 64
static struct { const char *id; bool clicked, hovered; } g_ids[ID_MAX];
static int g_id_n;

/* forward: the actual emitters */
static void em_text_impl(const char *s, EmProps p);
static void em_icon_impl(int cp, EmProps p);
static void em_label_impl(int cp, const char *s, EmProps p);
static void em_badge_impl(const char *s, EmProps p);
static void em_tag_impl(const char *s, EmProps p);
static void em_avatar_impl(const char *s, EmProps p);
static void em_banner_impl(int cp, const char *s, EmProps p);
static void em_progress_impl(float frac, EmProps p);
static bool em_button_impl(const char *s, EmProps p, bool *hov);
static bool em_iconbtn_impl(int cp, EmProps p, bool *hov);
static void em_toggle_impl(const char *l, bool *b, EmProps p);
static void em_checkbox_impl(const char *l, bool *b, EmProps p);
static void em_slider_impl(float *b, EmProps p);
static void em_stepper_impl(const char *l, int *b, int lo, int hi, EmProps p);
static bool em_field_impl(char *buf, size_t cap, const char *ph, EmProps p, bool *hov);
static bool em_password_impl(char *buf, size_t cap, const char *ph, EmProps p, bool *hov);
static void em_segmented_impl(const char *const *labels, int count, int *b, EmProps p);
static bool em_listrow_impl(int cp, const char *title, const char *value, EmProps p, bool *hov);
static bool em_closebtn_impl(bool *hov);
static bool em_minbtn_impl(bool *hov);
static bool em_search_impl(char *buf, size_t cap, const char *ph, bool *hov);
static void em_spinner_impl(void);
static bool em_dropdown_impl(const char *const *labels, int count, int *sel, bool *hov);

void em_flush(void) {
    if (!P.active) return;
    PKind k = P.kind; EmProps pr = P.props; const char *id = P.id;
    P.active = 0;                /* clear first: emitters may create nested elements */
    bool clicked = false, hovered = false;
    switch (k) {
        case PK_TEXT:     em_text_impl(P.str, pr); break;
        case PK_ICON:     em_icon_impl(P.cp, pr); break;
        case PK_LABEL:    em_label_impl(P.cp, P.str, pr); break;
        case PK_BADGE:    em_badge_impl(P.str, pr); break;
        case PK_TAG:      em_tag_impl(P.str, pr); break;
        case PK_AVATAR:   em_avatar_impl(P.str, pr); break;
        case PK_BANNER:   em_banner_impl(P.cp, P.str, pr); break;
        case PK_PROGRESS: em_progress_impl(P.frac, pr); break;
        case PK_BUTTON:   clicked = em_button_impl(P.str, pr, &hovered); break;
        case PK_ICONBTN:  clicked = em_iconbtn_impl(P.cp, pr, &hovered); break;
        case PK_TOGGLE:   em_toggle_impl(P.str, (bool *)P.bind, pr); break;
        case PK_CHECK:    em_checkbox_impl(P.str, (bool *)P.bind, pr); break;
        case PK_SLIDER:   em_slider_impl((float *)P.bind, pr); break;
        case PK_STEPPER:  em_stepper_impl(P.str, (int *)P.bind, P.lo, P.hi, pr); break;
        case PK_FIELD:    clicked = em_field_impl(P.buf, P.cap, P.str, pr, &hovered); break;  /* clicked == focused */
        case PK_PASSWORD: clicked = em_password_impl(P.buf, P.cap, P.str, pr, &hovered); break;
        case PK_SEGMENTED:em_segmented_impl(P.labels, P.count, (int *)P.bind, pr); break;
        case PK_LISTROW:  clicked = em_listrow_impl(P.cp, P.str, P.str2, pr, &hovered); break;
        case PK_CLOSEBTN: clicked = em_closebtn_impl(&hovered); break;
        case PK_MINBTN:   clicked = em_minbtn_impl(&hovered); break;
        case PK_SEARCH:   clicked = em_search_impl(P.buf, P.cap, P.str, &hovered); break;
        case PK_SPINNER:  em_spinner_impl(); break;
        case PK_DROPDOWN: clicked = em_dropdown_impl(P.labels, P.count, (int *)P.bind, &hovered); break;
        default: break;
    }
    P.result = clicked;
    if (id && g_id_n < ID_MAX) { g_ids[g_id_n].id = id; g_ids[g_id_n].clicked = clicked; g_ids[g_id_n].hovered = hovered; g_id_n++; }
}

/* ---- animation clock + eased-scalar animator --------------------------- */
/* Structural-change epoch: bumped whenever a V4 component changes the tree in
 * a way the APP can't observe from its own state (dropdown menu open/close,
 * toast raise/expiry). Apps compare it across frames to force a full repaint
 * (the dirty-rect renderer doesn't erase a removed subtree's pixels). */
static int g_em_epoch;
int em_ui_epoch(void) { return g_em_epoch; }
void em_structure_changed(void) { g_em_epoch++; }

/* Retained updates: live animations ask for the NEXT frame while active; the
 * app runtime skips all UI work on frames nobody asked for (see em_app.c). */
static int g_frame_req = 1;   /* first frame always builds */
void em_request_frame(void) { g_frame_req = 1; }
int  em_take_frame_request(void) { int r = g_frame_req; g_frame_req = 0; return r; }

static uint64_t (*g_clock)(void);
static uint64_t g_now_ms, g_prev_ms;
static int g_dt_ms;
static int g_ov_now, g_ov_prev, g_ov_dismissed, g_ov_frames;   /* modal-overlay tracking */

void em_set_clock(uint64_t (*fn)(void)) { g_clock = fn; }
uint64_t em_now_ms(void) { return g_clock ? g_clock() : 0; }
int em_dt_ms(void) { return g_dt_ms; }

#define ANIM_MAX 96
static struct { const char *id; float cur, target; int used; } g_anim[ANIM_MAX];

float em_animate(const char *id, float target, float rate) {
    int slot = -1, freei = -1;
    for (int i = 0; i < ANIM_MAX; i++) {
        if (g_anim[i].used) {
            if (g_anim[i].id == id || (g_anim[i].id && id && strcmp(g_anim[i].id, id) == 0)) { slot = i; break; }
        } else if (freei < 0) freei = i;
    }
    if (slot < 0) { slot = freei >= 0 ? freei : 0; g_anim[slot].used = 1; g_anim[slot].id = id; g_anim[slot].cur = target; }
    g_anim[slot].target = target;
    float dt = g_dt_ms / 1000.0f;                 /* exponential approach, frame-rate independent-ish */
    float step = rate * dt;
    if (step > 1.0f) step = 1.0f;
    if (step < 0.0f) step = 0.0f;
    g_anim[slot].cur += (g_anim[slot].target - g_anim[slot].cur) * step;
    float d = g_anim[slot].target - g_anim[slot].cur;
    if (d < 0.0008f && d > -0.0008f) g_anim[slot].cur = g_anim[slot].target;   /* settle */
    else em_request_frame();               /* still easing -> keep frames coming */
    return g_anim[slot].cur;
}

void em_new_frame(void) {
    g_now_ms = em_now_ms();
    g_dt_ms = g_prev_ms ? (int)(g_now_ms - g_prev_ms) : 0;
    if (g_dt_ms < 0) g_dt_ms = 0;
    if (g_dt_ms > 100) g_dt_ms = 100;             /* clamp stalls / first frame */
    g_prev_ms = g_now_ms;
    g_ov_frames = g_ov_now ? g_ov_frames + 1 : 0;   /* consecutive frames shown */
    g_ov_prev = g_ov_now; g_ov_now = 0;             /* overlay-shown tracking for force_full */
    P.active = 0; g_id_n = 0;
}

/* ---- navigation (a page stack of view functions) ----------------------- */
#define NAV_DUR_MS 220.0f
static EmPage g_pages[16];
static int    g_nav_top = -1;
static float  g_nav_t = 1.0f;   /* transition progress: 1 = settled, <1 = fading in */
static int    g_nav_dir = 1;    /* +1 push (slide from right), -1 pop (from left) */
void em_push(EmPage p) { if (p && g_nav_top < 15) { g_pages[++g_nav_top] = p; g_nav_t = 0.0f; g_nav_dir = 1; } }
void em_pop(void)      { if (g_nav_top > 0) { g_nav_top--; g_nav_t = 0.0f; g_nav_dir = -1; } }
int  em_nav_depth(void){ return g_nav_top + 1; }

/* ---- modal overlay (Sheet / Alert) ------------------------------------- *
 * A full-surface dimming scrim with a centred dialog card, declared LAST in the
 * screen so it paints on top. Like a page transition, showing/hiding it is a big
 * structural change the dirty-rect renderer won't fully erase, so the host loop
 * force-repaints while em_overlay_active(). em_overlay_end_ records whether the
 * bare scrim (not the dialog) was clicked -> OverlayDismissed(). */
void em_overlay_(void)      { em_flush(); g_ov_now = 1; ui_overlay_begin(0); }
void em_overlay_end_(void)  {
    em_flush();
    /* Debounce scrim-dismiss for the first few frames after the modal opens: the
     * click that opens it lands on the freshly-created full-screen scrim, and a
     * mid-session tree rebuild can register one more spurious click -- either
     * would open-and-instantly-dismiss the modal. Only honour a real scrim click
     * once the overlay has been stably up for a few frames. */
    g_ov_dismissed = (ui_overlay_end() && g_ov_frames >= 3) ? 1 : 0;
}
int  em_overlay_dismissed(void) { return g_ov_dismissed; }
int  em_overlay_active(void)    { return g_ov_now || g_ov_prev; }
void em_dialog_(EmProps p)  { em_flush(); ui_dialog_begin(0); em_apply_box(p); }
void em_dialog_end_(void)   { em_flush(); ui_dialog_end(); }
/* True while a page transition is fading in. The host loop should force a full
 * surface repaint on these frames: a page swap removes nodes, and the dirty-rect
 * renderer doesn't erase a removed node's vacated pixels, so without a full clear
 * the outgoing page bleeds through the (shorter) incoming one. */
int  em_nav_transitioning(void) { return g_nav_t < 1.0f; }
void em_nav(EmPage root) {
    if (g_nav_top < 0 && root) { g_pages[0] = root; g_nav_top = 0; }
    if (!(g_nav_top >= 0 && g_pages[g_nav_top])) return;

    /* advance the fade-in transition. With no clock set (host render) there's no
     * dt, so snap to settled -- pages are always fully opaque off-device. */
    if (em_now_ms() == 0) {
        g_nav_t = 1.0f;
    } else if (g_nav_t < 1.0f) {
        g_nav_t += (float)em_dt_ms() / NAV_DUR_MS;
        if (g_nav_t > 1.0f) g_nav_t = 1.0f;
        em_request_frame();                /* transition in flight */
    }

    /* Resolve the transition transform. It is applied to the page's OWN root
     * Screen (via g_nav_cur_*), NOT to an extra full-size wrapper box -- the page
     * is already a full-size box, so the second one was redundant. Push/Pop just
     * swaps g_pages[g_nav_top]; the page is called directly as the root here. */
    g_nav_cur_op = 1.0f; g_nav_cur_slide = 0.0f;
    if (g_nav_t < 1.0f) {
        float e = 1.0f - (1.0f - g_nav_t) * (1.0f - g_nav_t);   /* easeOutQuad */
        g_nav_cur_op = 0.12f + 0.88f * e;   /* floor avoids a fully-blank first frame */
        g_nav_cur_slide = (1.0f - e) * 44.0f * (float)g_nav_dir; /* right on push, left on pop */
    }
    em_flush();
    g_pages[g_nav_top]();     /* the page IS the root -- no wrapper rectangle */
    em_flush();
    g_nav_cur_op = 1.0f; g_nav_cur_slide = 0.0f;   /* consumed; don't leak past the page */
}

bool Clicked(const char *id) {
    em_flush();
    for (int i = 0; i < g_id_n; i++)
        if (g_ids[i].id == id || (g_ids[i].id && strcmp(g_ids[i].id, id) == 0)) return g_ids[i].clicked;
    return false;
}
bool Hovered(const char *id) {
    em_flush();
    for (int i = 0; i < g_id_n; i++)
        if (g_ids[i].id == id || (g_ids[i].id && strcmp(g_ids[i].id, id) == 0)) return g_ids[i].hovered;
    return false;
}

/* ---- modifier functions (mutate P; kind-aware where names overlap) ----- */

static const EmV em_v;   /* the singleton vtable, defined below */

static EmV m_font(EmFont f){ P.props.font = f; return em_v; }
static EmV m_title(void){ P.props.font = Title; return em_v; }
static EmV m_heading(void){ P.props.font = Heading; return em_v; }
static EmV m_body(void){ P.props.font = Body; return em_v; }
static EmV m_bold(void){ P.props.font = BodyBold; return em_v; }
static EmV m_caption(void){ P.props.font = Caption; return em_v; }
static EmV m_color(Color c){ P.props.color = c; return em_v; }
static EmV m_secondary(void){ if (P.kind == PK_BUTTON) P.props.style = Secondary; else P.props.color = TH->text_secondary; return em_v; }
static EmV m_tertiary(void){ P.props.color = TH->text_tertiary; return em_v; }
static EmV m_accent(void){ if (P.kind==PK_BADGE||P.kind==PK_TAG||P.kind==PK_BANNER) P.props.tone = Accent; else P.props.color = TH->accent; return em_v; }
static EmV m_primary(void){ P.props.style = Primary; return em_v; }
static EmV m_ghost(void){ P.props.style = Ghost; return em_v; }
static EmV m_destructive(void){ if (P.kind==PK_BUTTON) P.props.style = Destructive; else P.props.tone = Danger; return em_v; }
static EmV m_tone(EmTone t){ P.props.tone = t; return em_v; }
static EmV m_success(void){ P.props.tone = Success; return em_v; }
static EmV m_warning(void){ P.props.tone = Warning; return em_v; }
static EmV m_danger(void){ P.props.tone = Danger; return em_v; }
static EmV m_bg(Color c){ P.props.background = c; return em_v; }
static EmV m_padding(float v){ P.props.padding = v; return em_v; }
static EmV m_px(float v){ P.props.px = v; return em_v; }
static EmV m_py(float v){ P.props.py = v; return em_v; }
static EmV m_frame(float w, float h){ P.props.width = w; P.props.height = h; return em_v; }
static EmV m_width(float w){ P.props.width = w; return em_v; }
static EmV m_height(float h){ P.props.height = h; return em_v; }
static EmV m_grow(void){ P.props.grow = 1; return em_v; }
static EmV m_corner(float r){ P.props.corner = r; return em_v; }
static EmV m_border(float w){ P.props.border = w; return em_v; }
static EmV m_shadow(int n){ P.props.shadow = n; return em_v; }
static EmV m_center(void){ P.props.align = Center; return em_v; }
static EmV m_leading(void){ P.props.align = Leading; return em_v; }
static EmV m_trailing(void){ P.props.align = Trailing; return em_v; }
static EmV m_align(EmAlign a){ P.props.align = a; return em_v; }
static EmV m_id(const char *s){ P.id = s; return em_v; }
static bool m_clicked(void){ em_flush(); return P.result; }
static bool m_focused(void){ em_flush(); return P.result; }

static const EmV em_v = {
    .title=m_title, .heading=m_heading, .body=m_body, .bold=m_bold, .caption=m_caption, .font=m_font,
    .color=m_color, .secondary=m_secondary, .tertiary=m_tertiary, .accent=m_accent,
    .primary=m_primary, .ghost=m_ghost, .destructive=m_destructive,
    .tone=m_tone, .success=m_success, .warning=m_warning, .danger=m_danger,
    .bg=m_bg, .padding=m_padding, .px=m_px, .py=m_py, .frame=m_frame, .width=m_width, .height=m_height, .grow=m_grow,
    .corner=m_corner, .border=m_border, .shadow=m_shadow,
    .center=m_center, .leading=m_leading, .trailing=m_trailing, .align=m_align,
    .id=m_id, .clicked=m_clicked, .focused=m_focused,
};

/* ---- creators (stage a pending element, return the chain) -------------- */

static EmV stage(PKind k) { em_flush(); memset(&P, 0, sizeof P); P.active = 1; P.kind = k; return em_v; }

EmV em_text(const char *s){ EmV v = stage(PK_TEXT); P.str = s; return v; }
EmV em_icon(int cp){ EmV v = stage(PK_ICON); P.cp = cp; return v; }
EmV em_label(int cp, const char *s){ EmV v = stage(PK_LABEL); P.cp = cp; P.str = s; return v; }
EmV em_badge(const char *s){ EmV v = stage(PK_BADGE); P.str = s; return v; }
EmV em_tag(const char *s){ EmV v = stage(PK_TAG); P.str = s; return v; }
EmV em_avatar(const char *s){ EmV v = stage(PK_AVATAR); P.str = s; return v; }
EmV em_banner(int cp, const char *s){ EmV v = stage(PK_BANNER); P.cp = cp; P.str = s; return v; }
EmV em_progress(float f){ EmV v = stage(PK_PROGRESS); P.frac = f; return v; }
EmV em_button(const char *s){ EmV v = stage(PK_BUTTON); P.str = s; return v; }
EmV em_icon_button(int cp){ EmV v = stage(PK_ICONBTN); P.cp = cp; return v; }
EmV em_toggle(const char *l, bool *b){ EmV v = stage(PK_TOGGLE); P.str = l; P.bind = b; return v; }
EmV em_checkbox(const char *l, bool *b){ EmV v = stage(PK_CHECK); P.str = l; P.bind = b; return v; }
EmV em_slider(float *b){ EmV v = stage(PK_SLIDER); P.bind = b; return v; }
EmV em_stepper(const char *l, int *b, int lo, int hi){ EmV v = stage(PK_STEPPER); P.str = l; P.bind = b; P.lo = lo; P.hi = hi; return v; }
EmV em_text_field(char *buf, size_t cap, const char *ph){ EmV v = stage(PK_FIELD); P.buf = buf; P.cap = cap; P.str = ph; return v; }
EmV em_password_field(char *buf, size_t cap, const char *ph){ EmV v = stage(PK_PASSWORD); P.buf = buf; P.cap = cap; P.str = ph; return v; }
EmV em_segmented(const char *const *labels, int count, int *b){ EmV v = stage(PK_SEGMENTED); P.labels = labels; P.count = count; P.bind = b; return v; }
EmV em_listrow(int icon, const char *title, const char *value){ EmV v = stage(PK_LISTROW); P.cp = icon; P.str = title; P.str2 = value; return v; }
EmV em_close_button(void){ EmV v = stage(PK_CLOSEBTN); P.id = "__em_win_close"; return v; }
EmV em_min_button(void){ EmV v = stage(PK_MINBTN); P.id = "__em_win_min"; return v; }
EmV em_search_field(char *buf, size_t cap, const char *ph){ EmV v = stage(PK_SEARCH); P.buf = buf; P.cap = cap; P.str = ph; return v; }
EmV em_spinner(void){ return stage(PK_SPINNER); }
EmV em_dropdown(const char *const *labels, int count, int *sel){ EmV v = stage(PK_DROPDOWN); P.labels = labels; P.count = count; P.bind = sel; return v; }
void em_spacer_(void){ em_flush(); ui_spacer(); }

/* A key string to the integer the declare layer matches on. FNV-1a, and never
 * zero -- zero is how ui_begin_* spells "no key", so a string that hashed to it
 * would silently go back to positional matching. */
uint64_t em_key_hash(const char *k) {
    if (!k || !k[0]) return 0;
    uint64_t h = 1469598103934665603ULL;
    for (const unsigned char *p = (const unsigned char *)k; *p; p++) {
        h ^= *p; h *= 1099511628211ULL;
    }
    return h ? h : 1;
}
void em_divider_(void){ em_flush(); ui_divider(); }
void em_divider_k_(const char *key) {
    if (!key || !key[0]) { em_divider_(); return; }
    /* A divider is a one-pixel box, so it is the easiest thing in a column for
     * an inserted row to be mistaken for. Wrapping it in a keyed box is what
     * stops that. */
    em_flush();
    ui_box_begin(em_key_hash(key));
    ui_divider();
    ui_box_end();
}

/* ======================================================================= */
/* emitters                                                                */
/* ======================================================================= */

static int props_wrap(EmProps p) {
    return (p.padding > 0 || p.px > 0 || p.py > 0 || p.pt > 0 || p.pr > 0 || p.pb > 0 || p.pl > 0 ||
            p.background.a > 0 || p.corner > 0 || p.border > 0 || p.width > 0 || p.grow);
}

static void em_text_impl(const char *s, EmProps p) {
    uint32_t fh; float sz; em_resolve_font(p.font, &fh, &sz);
    Color col = p.color.a > 0 ? p.color : TH->text;
    if (props_wrap(p)) {
        ui_box_begin(0);
        em_apply_box(p);
        ui_set_align(ALIGN_CENTER);
        ui_set_font(fh); ui_set_text_size(sz); ui_set_text_color(col);
        ui_text("%s", s);
        ui_box_end();
    } else {
        ui_set_font(fh); ui_set_text_size(sz); ui_set_text_color(col);
        if (p.background.a > 0) ui_set_text_bg(p.background);
        ui_text("%s", s);
    }
}
static void em_icon_impl(int cp, EmProps p) {
    char g[5]; utf8_enc(cp, g);
    if (!p.font) p.font = Body;
    em_text_impl(g, p);
}
/* Gradient text / icon: glyphs filled with a gradient over the run's own box.
 * One-shot -- ui_set_text_gradient is consumed by the ui_text() that follows. */
void em_gtext(const char *s, struct paint g, EmProps p) {
    em_flush();
    uint32_t fh; float sz; em_resolve_font(p.font, &fh, &sz);
    Color col = p.color.a > 0 ? p.color : TH->text;
    ui_set_font(fh); ui_set_text_size(sz); ui_set_text_color(col);
    ui_set_text_gradient(&g);
    ui_text("%s", s);
}
void em_gicon(int cp, struct paint g, EmProps p) {
    char buf[5]; utf8_enc(cp, buf);
    if (!p.font) p.font = Body;
    em_gtext(buf, g, p);
}
static void em_label_impl(int cp, const char *s, EmProps p) {
    ui_begin_hstack(0);
    ui_set_align(ALIGN_CENTER);
    ui_set_spacing(6);
    em_apply_box(p);
    EmProps ip = { .font = p.font ? p.font : Body, .color = p.color };
    em_icon_impl(cp, ip);
    EmProps tp = { .font = p.font ? p.font : Body, .color = p.color };
    em_text_impl(s, tp);
    ui_end_stack();
}

static enum ui_badge_tone map_tone(EmTone tn) {
    switch (tn) { case Success: return BADGE_SUCCESS; case Warning: return BADGE_WARNING;
                  case Danger: return BADGE_DANGER; case Neutral: return BADGE_NEUTRAL; default: return BADGE_ACCENT; }
}
static void em_badge_impl(const char *s, EmProps p) { ui_badge(s, map_tone(p.tone)); }

static void em_tag_impl(const char *s, EmProps p) {
    const struct ui_theme *t = TH;
    Color fg = t->text_secondary;
    switch (p.tone) { case Accent: fg = t->accent; break; case Success: fg = t->success; break;
                      case Warning: fg = t->warning; break; case Danger: fg = t->danger; break; default: break; }
    ui_begin_hstack(0);
    ui_set_paint(solid(t->surface_alt));
    ui_set_border(1.0f, t->border);
    ui_set_corner_radius(t->radius_pill);
    ui_set_padding(t->sp1, t->sp2 + 2, t->sp1, t->sp2 + 2);
    ui_set_align(ALIGN_CENTER);
    ui_set_font(t->font_bold); ui_set_text_size(t->text_caption); ui_set_text_color(fg);
    ui_text("%s", s);
    ui_end_stack();
}
static void em_avatar_impl(const char *s, EmProps p) { (void)p; ui_avatar(s); }

static void em_banner_impl(int cp, const char *msg, EmProps p) {
    const struct ui_theme *t = TH;
    Color accent = t->accent;
    switch (p.tone) { case Success: accent = t->success; break; case Warning: accent = t->warning; break;
                      case Danger: accent = t->danger; break; default: accent = t->accent; break; }
    ui_begin_hstack(0);
    ui_set_paint(solid(tint(accent, 0.14f)));
    ui_set_corner_radius(t->radius_md);
    ui_set_border(1.0f, tint(accent, 0.35f));
    ui_set_padding(t->sp3, t->sp4, t->sp3, t->sp4);
    ui_set_align(ALIGN_CENTER);
    ui_set_spacing(t->sp3);
    { EmProps ip = { .font = Body, .color = accent }; em_icon_impl(cp, ip); }
    { EmProps tp = { .font = Body, .color = t->text }; em_text_impl(msg, tp); }
    ui_spacer();
    ui_end_stack();
}
static void em_progress_impl(float frac, EmProps p) { (void)p; ui_progress(frac); }

static bool em_button_impl(const char *s, EmProps p, bool *out_hov) {
    const struct ui_theme *t = TH;
    /* An hstack, not a box: justify positions a container's children along the
     * main axis, and a box does not lay children out that way -- so .leading()
     * set the prop and the label stayed stubbornly centred. Every file name in
     * a list column was centred in its column because of this. */
    ui_begin_hstack(0);
    struct instance_handle self = ui_open();
    bool hov = ui_is_hovered(), pressed = ui_is_pressed();
    if (out_hov) *out_hov = hov;
    Color fill = t->accent, txt = t->on_accent, bcol = t->border_strong;
    int has_fill = 1, has_border = 0;
    switch (p.style) {
        case Secondary:   fill = t->surface; txt = t->text; has_border = 1; break;
        case Ghost:       has_fill = 0; txt = t->accent; break;
        case Destructive: fill = t->danger; txt = t->on_accent; break;
        default: break;
    }
    if (p.background.a > 0) { fill = p.background; has_fill = 1; }
    if (p.color.a > 0) txt = p.color;
    if (has_fill) { Color f = pressed ? shade(fill, 0.86f) : hov ? shade(fill, 1.10f) : fill; ui_set_paint(solid(f)); }
    else if (hov) ui_set_paint(solid(shade(t->accent_soft, pressed ? 0.9f : 1.0f)));
    else          ui_set_paint(solid((Color){0, 0, 0, 0}));   /* reset: a ghost button un-hovers cleanly */
    ui_set_corner_radius(p.corner > 0 ? p.corner : t->radius_md);
    if (has_border || p.border > 0) ui_set_border(p.border > 0 ? p.border : 1.0f, hov ? t->accent : bcol);
    /* Padding is overridable. A button's default is sized for a control you
     * aim at, which is right for a dialog and wrong for a dense list row --
     * and since the button's padding IS the row's height, a caller that could
     * not change it could not make a compact list at all, however tight the
     * container asked to be. */
    { float pv = em_len(p.py, p.padding > 0 ? p.padding : (float)(t->sp2 + 1));
      float ph = em_len(p.px, p.padding > 0 ? p.padding : (float)t->sp4);
      ui_set_padding(pv, ph, pv, ph); }
    /* And so is alignment. .leading() sets the ALIGN prop, but a button's
     * label sits on the MAIN axis, which justify controls -- so a caller
     * asking for a left-aligned label got a centred one and no way to say
     * otherwise. A label in a list column must start where the column does. */
    ui_set_align(p.align ? map_align(p.align) : ALIGN_CENTER);
    ui_set_justify(p.justify ? map_justify(p.justify)
                             : p.align ? map_justify(p.align) : JUSTIFY_CENTER);
    if (p.grow || p.width > 0) { struct layout_size w = p.width > 0 ? sz_fixed(p.width) : sz_grow(); ui_set_size(w, sz_intrinsic()); }
    uint32_t fh; float sz; em_resolve_font(p.font ? p.font : BodyBold, &fh, &sz);
    ui_set_font(fh); ui_set_text_size(sz); ui_set_text_color(txt);
    ui_text("%s", s);
    ui_end_stack();
    return ui_consume_click(self);
}
static bool em_iconbtn_impl(int cp, EmProps p, bool *out_hov) {
    const struct ui_theme *t = TH;
    ui_begin_hstack(0);
    struct instance_handle self = ui_open();
    bool hov = ui_is_hovered(), pressed = ui_is_pressed();
    if (out_hov) *out_hov = hov;
    Color bg = p.background.a > 0 ? p.background : t->surface_alt;
    ui_set_paint(solid(pressed ? shade(bg, 0.9f) : hov ? shade(bg, 1.12f) : bg));
    ui_set_corner_radius(p.corner > 0 ? p.corner : t->radius_md);
    ui_set_padding(t->sp2, t->sp2, t->sp2, t->sp2);
    ui_set_align(ALIGN_CENTER);
    ui_set_justify(JUSTIFY_CENTER);
    if (p.width > 0 || p.height > 0)
        ui_set_size(p.width > 0 ? sz_fixed(p.width) : sz_intrinsic(),
                    p.height > 0 ? sz_fixed(p.height) : sz_intrinsic());
    EmProps ip = { .font = p.font ? p.font : Body, .color = p.color.a > 0 ? p.color : t->text_secondary };
    em_icon_impl(cp, ip);
    ui_end_stack();
    return ui_consume_click(self);
}
static void em_toggle_impl(const char *label, bool *bind, EmProps p) {
    ui_begin_hstack(0); ui_set_align(ALIGN_CENTER); ui_set_spacing(TH->sp3);
    em_apply_box(p);
    if (label && label[0]) { EmProps tp = { .font = Body }; em_text_impl(label, tp); }
    ui_spacer();
    if (ui_toggle(bind ? *bind : false) && bind) *bind = !*bind;
    ui_end_stack();
}
static void em_checkbox_impl(const char *label, bool *bind, EmProps p) {
    ui_begin_hstack(0); ui_set_align(ALIGN_CENTER); ui_set_spacing(TH->sp3);
    em_apply_box(p);
    if (ui_checkbox(bind ? *bind : false) && bind) *bind = !*bind;
    /* The label takes the caller's colour when it gave one. Without this a
     * checkbox inside a rendered document kept the DESKTOP's text colour and
     * its label vanished into the page's white canvas. */
    if (label && label[0]) {
        EmProps tp = { .font = Body, .color = p.color };
        em_text_impl(label, tp);
    }
    ui_spacer();
    ui_end_stack();
}
static void em_slider_impl(float *bind, EmProps p) {
    (void)p; float v = ui_slider(bind ? *bind : 0.0f); if (bind) *bind = v;
}
static void em_stepper_impl(const char *label, int *bind, int lo, int hi, EmProps p) {
    const struct ui_theme *t = TH;
    ui_begin_hstack(0); ui_set_align(ALIGN_CENTER); ui_set_spacing(t->sp3);
    em_apply_box(p);
    if (label && label[0]) { EmProps tp = { .font = Body }; em_text_impl(label, tp); }
    ui_spacer();
    ui_begin_hstack(0);
    ui_set_paint(solid(t->surface_alt));
    ui_set_corner_radius(t->radius_md);
    ui_set_padding(2, t->sp2, 2, t->sp2);
    ui_set_align(ALIGN_CENTER);
    ui_set_spacing(t->sp2);
    if (em_iconbtn_impl(IconMinus, (EmProps){0}, 0) && bind && *bind > lo) (*bind)--;
    { char b[16]; snprintf(b, sizeof b, "%d", bind ? *bind : 0); EmProps vp = { .font = BodyBold }; em_text_impl(b, vp); }
    if (em_iconbtn_impl(IconPlus, (EmProps){0}, 0) && bind && *bind < hi) (*bind)++;
    ui_end_stack();
    ui_end_stack();
}
static bool em_field_impl(char *buf, size_t cap, const char *ph, EmProps p, bool *out_hov) {
    (void)p; if (out_hov) *out_hov = false;
    return ui_text_field(buf, cap, ph);
}
static bool em_password_impl(char *buf, size_t cap, const char *ph, EmProps p, bool *out_hov) {
    (void)p; if (out_hov) *out_hov = false;
    return ui_password_field(buf, cap, ph);
}
static void em_segmented_impl(const char *const *labels, int count, int *bind, EmProps p) {
    (void)p; int cur = bind ? *bind : 0; int nv = ui_segmented(labels, count, cur); if (bind) *bind = nv;
}

/* ---- richer components ------------------------------------------------- */

/* Bar chart: values scaled to the max, bottom-aligned, last bar emphasised. */
void em_chart(const float *vals, int n, EmProps p) {
    em_flush();
    const struct ui_theme *t = TH;
    float mx = 0.00001f;
    for (int i = 0; i < n; i++) if (vals[i] > mx) mx = vals[i];
    float H = p.height > 0 ? p.height : 84.0f;
    Color col = p.color.a > 0 ? p.color : t->accent;
    ui_begin_hstack(0);
    ui_set_size(sz_grow(), sz_fixed(H));
    ui_set_align(ALIGN_END);                          /* bars grow up from the baseline */
    ui_set_spacing(p.spacing > 0 ? p.spacing : 5);
    for (int i = 0; i < n; i++) {
        float bh = 4.0f + (vals[i] / mx) * (H - 4.0f);
        ui_box_begin((uint64_t)(i + 1));
        ui_set_paint(solid(i == n - 1 ? col : tint(col, 0.45f)));   /* emphasise the latest */
        ui_set_corner_radius(3);
        ui_set_size(sz_grow(), sz_fixed(bh));
        ui_box_end();
    }
    ui_end_stack();
}

/* ---- line / area chart ------------------------------------------------- *
 * A software line rasteriser (the "line primitive") draws a polyline -- and,
 * for an area chart, the fill beneath it -- into a persistent premultiplied
 * BGRA bitmap, shown via an IMAGE node (draw_image scales it to the row width).
 * One shared buffer -> one line chart on screen at a time; data is treated as
 * fixed (scene_set_image guards on the pointer, so a redraw of the same buffer
 * doesn't re-dirty -- fine for static series). */
#define LC_W 260
#define LC_H 100
static uint32_t g_lc_buf[LC_W * LC_H];

static uint32_t argb_premul(Color c, float a) {
    if (a < 0) a = 0;
    if (a > 1) a = 1;
    uint32_t A = (uint32_t)(a * 255.0f + 0.5f);
    uint32_t R = (uint32_t)(c.r * a * 255.0f + 0.5f);
    uint32_t G = (uint32_t)(c.g * a * 255.0f + 0.5f);
    uint32_t B = (uint32_t)(c.b * a * 255.0f + 0.5f);
    return (A << 24) | (R << 16) | (G << 8) | B;
}
static inline void lc_plot(int x, int y, uint32_t c) {
    if (x >= 0 && x < LC_W && y >= 0 && y < LC_H) g_lc_buf[y * LC_W + x] = c;
}

/* filled=1 -> area chart (fill under the line); filled=0 -> plain line. */
void em_linechart(const float *vals, int n, int filled, EmProps p) {
    em_flush();
    if (n < 2) return;
    const struct ui_theme *t = TH;
    Color ac = p.color.a > 0 ? p.color : t->accent;
    uint32_t line = argb_premul(ac, 1.0f);
    uint32_t dot  = argb_premul(ac, 1.0f);
    uint32_t fill = argb_premul(ac, 0.26f);

    for (int i = 0; i < LC_W * LC_H; i++) g_lc_buf[i] = 0;   /* transparent */

    float mx = -1e30f, mn = 1e30f;
    for (int i = 0; i < n; i++) { if (vals[i] > mx) mx = vals[i]; if (vals[i] < mn) mn = vals[i]; }
    if (mx <= mn) mx = mn + 1.0f;
    const int pad = 8, base = LC_H - pad;
    #define LC_Y(v) ((float)base - ((v) - mn) / (mx - mn) * (float)(LC_H - 2 * pad))

    for (int i = 0; i < n - 1; i++) {
        float fx0 = (float)i / (n - 1) * (LC_W - 1), fx1 = (float)(i + 1) / (n - 1) * (LC_W - 1);
        float fy0 = LC_Y(vals[i]), fy1 = LC_Y(vals[i + 1]);
        int x0 = (int)(fx0 + 0.5f), x1 = (int)(fx1 + 0.5f);
        for (int x = x0; x <= x1 && x < LC_W; x++) {
            float tt = x1 > x0 ? (float)(x - x0) / (float)(x1 - x0) : 0.0f;
            int y = (int)(fy0 + (fy1 - fy0) * tt + 0.5f);
            if (filled) for (int yy = y; yy < base; yy++) lc_plot(x, yy, fill);
            lc_plot(x, y, line); lc_plot(x, y - 1, line);   /* 2px line */
        }
    }
    /* sample dots */
    for (int i = 0; i < n; i++) {
        int cx = (int)((float)i / (n - 1) * (LC_W - 1) + 0.5f), cy = (int)(LC_Y(vals[i]) + 0.5f);
        for (int oy = -2; oy <= 2; oy++) for (int ox = -2; ox <= 2; ox++)
            if (ox*ox + oy*oy <= 4) lc_plot(cx + ox, cy + oy, dot);
    }
    #undef LC_Y

    float H = p.height > 0 ? p.height : 100.0f;
    ui_image((uint64_t)(uintptr_t)&g_lc_buf, g_lc_buf, LC_W, LC_H, H);
}

/* Grouped inset list container. */
void em_list_(EmProps p) {
    em_flush();
    const struct ui_theme *t = TH;
    ui_begin_vstack(0);
    if (p.background.a <= 0) p.background = t->surface_alt;
    ui_set_corner_radius(p.corner > 0 ? p.corner : t->radius_md);
    ui_set_border(1.0f, t->border);
    ui_set_clip_children(true);
    ui_set_align(ALIGN_STRETCH);
    ui_set_spacing(0);
    em_apply_box(p);
}
void em_list_end_(void) { em_flush(); ui_end_stack(); }

/* A tappable list row: [icon] title ........ value  ›   */
static bool em_listrow_impl(int cp, const char *title, const char *value, EmProps p, bool *out_hov) {
    const struct ui_theme *t = TH;
    ui_begin_hstack(0);
    struct instance_handle self = ui_open();
    bool hov = ui_is_hovered(), pressed = ui_is_pressed();
    if (out_hov) *out_hov = hov;
    ui_set_paint(solid(pressed ? shade(t->surface_alt, 0.94f) : hov ? shade(t->surface, 1.06f) : t->surface));
    ui_set_padding(t->sp3, t->sp4, t->sp3, t->sp4);
    ui_set_align(ALIGN_CENTER);
    ui_set_spacing(t->sp3);
    if (cp) { EmProps ip = { .font = Body, .color = p.color.a > 0 ? p.color : t->accent }; em_icon_impl(cp, ip); }
    { EmProps tp = { .font = Body }; em_text_impl(title, tp); }
    ui_spacer();
    if (value && value[0]) { EmProps vp = { .font = Body, .color = t->text_secondary }; em_text_impl(value, vp); }
    { EmProps cp2 = { .font = Body, .color = t->text_tertiary }; em_icon_impl(IconChevronR, cp2); }
    ui_end_stack();
    return ui_consume_click(self);
}

/* ======================================================================= */
/* EmUI V4 implementation                                                  */
/* ======================================================================= */

/* ---- app-owned window chrome ------------------------------------------ */
/* The toolkit stays syscall-free: the app registers HOW to move its window
 * (em_window_set_mover, mirroring em_set_clock) and its id + current screen
 * position (em_window_bind). WindowBar's drag then just calls the mover. */
static void   (*g_win_mover)(int win, int32_t x, int32_t y);
static int      g_win_bound;
static int      g_win_id;
static int32_t  g_win_x, g_win_y;      /* window's current screen top-left */
static int      g_win_dragging;
static float    g_win_grab_x, g_win_grab_y;   /* pointer-at-grab, content-local */
static int      g_win_moved;                  /* set whenever the window moved -> force a full repaint */

void em_window_set_mover(void (*mover)(int win, int32_t x, int32_t y)) { g_win_mover = mover; }
void em_window_bind(int win, int32_t x, int32_t y) {
    g_win_id = win; g_win_x = x; g_win_y = y; g_win_bound = 1;
}
/* Read-and-clear: did the bound window move since the last poll? The app runtime
 * force-repaints on a move so a drag/snap can't leave dirty-rect ghost trails. */
int em_window_moved(void) { int m = g_win_moved; g_win_moved = 0; return m; }
/* Programmatically move the bound window (e.g. a pin/snap-to-anchor control). */
void em_window_move_to(int32_t x, int32_t y) {
    if (!g_win_bound || !g_win_mover || (x == g_win_x && y == g_win_y)) return;
    g_win_mover(g_win_id, x, y);
    g_win_x = x; g_win_y = y; g_win_moved = 1;
}
void em_window_pos(int32_t *x, int32_t *y) { if (x) *x = g_win_x; if (y) *y = g_win_y; }

/* resizable-window plumbing (V5): the grip accumulates a drag delta and, on
 * RELEASE, parks it here for the runtime to apply (live re-backing every frame
 * would thrash the page allocator; commit-on-release keeps it one realloc). */
static int g_win_resizable;
static int g_win_glass;
static int g_rz_active, g_rz_pend;
static float g_rz_grab_x, g_rz_grab_y, g_rz_dx, g_rz_dy;
void em_window_set_resizable(int on) { g_win_resizable = on; }
void em_window_set_glass(int on) { g_win_glass = on; }
int em_window_take_resize(int *dw, int *dh) {
    if (!g_rz_pend) return 0;
    g_rz_pend = 0;
    if (dw) *dw = (int)g_rz_dx;
    if (dh) *dh = (int)g_rz_dy;
    return 1;
}

/* ---- drag-to-dismiss close (EmbLink's own close gesture) ----------------- *
 * Instead of a fixed X button, the window has a GRIP you pull: as you drag it
 * the window fades + slides toward the pull (a "peel away" preview); release
 * past the threshold and it closes, release early and it springs back. Same
 * commit-on-release shape as the resize grip. The runtime reads the commit via
 * em_window_take_close(); the visual (fade/slide) is applied by em_window_. */
#define CLOSE_PULL 95.0f           /* px of drag that commits the close */
static int   g_cl_active, g_cl_pend;
static float g_cl_grab_x, g_cl_grab_y, g_cl_dx, g_cl_dy, g_cl_progress;

int em_window_take_close(void) { if (!g_cl_pend) return 0; g_cl_pend = 0; return 1; }

bool em_close_grip(void) {
    const struct ui_theme *t = TH;
    float pr = g_cl_progress;
    ui_begin_hstack(0xC105E);   /* stable key so the drag-owner identity survives rebuilds */
    struct instance_handle self = ui_open(); (void)self;
    bool active = ui_is_active();
    /* a pull HANDLE (wider than a button, so it reads as draggable), tinting from
     * the surface toward danger-red as the pull progresses. */
    Color bg = { t->surface.r + (t->danger.r - t->surface.r) * pr,
                 t->surface.g + (t->danger.g - t->surface.g) * pr,
                 t->surface.b + (t->danger.b - t->surface.b) * pr, 1.0f };
    ui_set_paint(solid(bg));
    ui_set_corner_radius(t->radius_pill);
    ui_set_border(1.0f, (active || pr > 0.05f) ? t->danger : t->border);
    ui_set_size(sz_fixed(48), sz_fixed(28));
    ui_set_align(ALIGN_CENTER);
    ui_set_justify(JUSTIFY_CENTER);
    { EmProps ip = { .font = BodyBold, .color = pr > 0.5f ? t->on_accent : t->text_secondary };
      em_icon_impl(IconClose, ip); }
    ui_end_stack();

    if (active) {
        float px, py; ui_pointer_pos(&px, &py);
        if (!g_cl_active) { g_cl_active = 1; g_cl_grab_x = px; g_cl_grab_y = py; }
        g_cl_dx = px - g_cl_grab_x; g_cl_dy = py - g_cl_grab_y;
        float dist = (g_cl_dx < 0 ? -g_cl_dx : g_cl_dx) + (g_cl_dy < 0 ? -g_cl_dy : g_cl_dy);
        g_cl_progress = dist / CLOSE_PULL;
        if (g_cl_progress >= 1.0f) { g_cl_progress = 1.0f; g_cl_pend = 1; }  /* pulled far enough -> close */
        em_request_frame();
    } else if (g_cl_active) {
        g_cl_active = 0;                               /* released before the threshold */
    }
    /* spring back when released below the threshold */
    if (!g_cl_active && !g_cl_pend && g_cl_progress > 0.001f) {
        g_cl_progress *= 0.72f; g_cl_dx *= 0.72f; g_cl_dy *= 0.72f;
        if (g_cl_progress < 0.02f) { g_cl_progress = 0; g_cl_dx = g_cl_dy = 0; }
        em_request_frame();
    }
    return g_cl_pend != 0;
}

/* True while a close pull is in progress (or springing back) -- the app runtime
 * force-repaints so the fade/slide don't ghost under the dirty-rect present. */
int em_window_pulling(void) { return g_cl_active || g_cl_progress > 0.001f; }

/* Window: the full-bleed top-level surface of a chromeless app window. Fills
 * the whole pixel buffer with the theme background (rectangular -- the OS
 * window has no per-pixel alpha), lays children in a stretched column. */
void em_window_(const char *title, EmProps p) {
    (void)title;
    em_flush();
    const struct ui_theme *t = TH;
    ui_begin_vstack(0);
    /* the app renders opaque (the compositor makes a glass window translucent
     * and frosts the desktop behind it); a glass window just gets a faint accent
     * cast in its bg so the frost reads as EmbLink's, not a neutral gray. */
    Color bg = p.background.a > 0 ? p.background : t->bg;
    if (g_win_glass) { Color a = t->accent;
        bg = (Color){ bg.r * 0.92f + a.r * 0.08f, bg.g * 0.92f + a.g * 0.08f,
                      bg.b * 0.92f + a.b * 0.08f, 1.0f }; }
    p.background = bg;          /* em_apply_box owns the paint (see its note) */
    ui_set_size(sz_grow(), sz_grow());
    ui_set_align(ALIGN_STRETCH);
    ui_set_spacing(0);
    em_apply_box(p);
    /* drag-to-dismiss preview: fade + slide the whole window toward the pull as
     * the close grip is dragged (springs back if released early). */
    if (g_cl_progress > 0.001f) {
        ui_set_opacity(1.0f - g_cl_progress * 0.88f);
        ui_set_offset(g_cl_dx * 0.30f, g_cl_dy * 0.30f);
    }
}
void em_window_end_(void) {
    em_flush();
    if (g_win_resizable) {
        /* corner grip: a zero-height anchor at the window's very bottom, its
         * glyph raised into the corner with a scene offset (same trick as the
         * Toast). While held it tracks the pointer; on release it parks the
         * delta for the runtime. */
        const struct ui_theme *t = TH;
        ui_begin_hstack(0);
        ui_set_size(sz_grow(), (struct layout_size){ .mode = SIZE_FIXED, .fixed_value = 0 });
        ui_set_justify(JUSTIFY_END);
        {
            ui_begin_hstack(1);
            struct instance_handle grip = ui_open(); (void)grip;
            bool active = ui_is_active();
            ui_set_size(sz_fixed(20), sz_fixed(20));
            ui_set_align(ALIGN_CENTER);
            ui_set_justify(JUSTIFY_CENTER);
            ui_set_offset(0, -20.0f);
            { char g[5]; utf8_enc(0x25E2, g);   /* black lower-right triangle */
              ui_set_font(t->font_regular); ui_set_text_size(t->text_caption);
              ui_set_text_color(active ? t->accent : t->text_tertiary);
              ui_text("%s", g); }
            ui_end_stack();
            if (active) {
                float px, py; ui_pointer_pos(&px, &py);
                if (!g_rz_active) { g_rz_active = 1; g_rz_grab_x = px; g_rz_grab_y = py; g_rz_dx = 0; g_rz_dy = 0; }
                else { g_rz_dx = px - g_rz_grab_x; g_rz_dy = py - g_rz_grab_y; }
                em_request_frame();               /* keep tracking while held */
            } else if (g_rz_active) {
                g_rz_active = 0;
                if (g_rz_dx > 4.0f || g_rz_dx < -4.0f || g_rz_dy > 4.0f || g_rz_dy < -4.0f)
                    g_rz_pend = 1;                /* commit on release */
            }
        }
        ui_end_stack();
    }
    ui_end_stack();
}

/* The drag zone's per-frame move logic. Pointer is content-local; because the
 * app tracks its own origin, moving the window by (ptr_now - grab) converges in
 * one step (after the move the kernel re-references the pointer to the new
 * origin, so the grab point stays put). See the plan's drag derivation. */
static void em_window_drag_(void) {
    if (!g_win_bound) return;
    if (ui_is_active()) {          /* pressed on the drag zone, button still held */
        float px, py; ui_pointer_pos(&px, &py);
        if (!g_win_dragging) { g_win_dragging = 1; g_win_grab_x = px; g_win_grab_y = py; }
        else {
            int nx = g_win_x + (int)(px - g_win_grab_x);
            int ny = g_win_y + (int)(py - g_win_grab_y);
            if ((nx != g_win_x || ny != g_win_y) && g_win_mover) {
                g_win_mover(g_win_id, nx, ny);
                g_win_x = nx; g_win_y = ny; g_win_moved = 1;
            }
        }
    } else {
        g_win_dragging = 0;
    }
}

/* WindowBar: a draggable strip. Its interior is a growing "drag zone" that
 * carries the title and absorbs the drag; the scope's children append AFTER it
 * as siblings (right side), so pressing a control -- e.g. CloseButton -- never
 * starts a drag. Fully restyleable through EmProps/theme. */
void em_windowbar_(const char *title, EmProps p) {
    em_flush();
    const struct ui_theme *t = TH;
    ui_begin_hstack(0);                              /* the bar */
    if (p.background.a <= 0) p.background = t->surface_alt;
    ui_set_padding(t->sp2, t->sp3, t->sp2, t->sp3);
    ui_set_align(ALIGN_CENTER);
    ui_set_spacing(t->sp2);
    ui_set_size(sz_grow(), sz_intrinsic());
    ui_set_border(0, t->border);                     /* a hairline under the bar reads as chrome */
    em_apply_box(p);

    ui_begin_hstack(0);                              /* drag zone (grows, grabbable) */
    ui_open();
    ui_set_size(sz_grow(), sz_intrinsic());
    ui_set_align(ALIGN_CENTER);
    ui_set_spacing(t->sp2);
    em_window_drag_();
    { EmProps dp = { .font = Body, .color = t->text_tertiary }; em_icon_impl(IconDot, dp); }
    { EmProps tp = { .font = BodyBold, .color = t->text }; em_text_impl(title && title[0] ? title : " ", tp); }
    ui_spacer();
    ui_end_stack();                                  /* close drag zone; controls follow as siblings */
}
void em_windowbar_end_(void) { em_flush(); ui_end_stack(); }

/* The standard application title bar (see em.h). Deliberately NOT a variant of
 * WindowBar: WindowBar is the general "put what you like in a bar" container,
 * while this one is a house style with opinions -- lights leading in Mac
 * order, a centred title, a hairline under it -- and the point of a house
 * style is that apps do not get to disagree about it. */
void em_appbar_(const char *title, EmProps p) {
    em_flush();
    const struct ui_theme *t = TH;
    ui_begin_hstack(0);                              /* the bar */
    if (p.background.a <= 0) p.background = t->surface_alt;
    ui_set_padding(t->sp2, t->sp3, t->sp2, t->sp2);
    ui_set_align(ALIGN_CENTER);
    ui_set_spacing(t->sp2);
    ui_set_size(sz_grow(), sz_intrinsic());
    ui_set_border(0, t->border);
    em_apply_box(p);

    /* the lights, leading */
    em_close_button(); em_flush();
    em_min_button();   em_flush();

    /* Title, centred in whatever space is left between the lights and the
     * app's own controls, and the drag zone at the same time -- so the bar is
     * grabbable everywhere a control is not. */
    ui_begin_hstack(0);
    ui_open();
    ui_set_size(sz_grow(), sz_intrinsic());
    ui_set_align(ALIGN_CENTER);
    ui_set_justify(JUSTIFY_CENTER);
    ui_set_spacing(t->sp2);
    em_window_drag_();
    { EmProps tp = { .font = BodyBold, .color = t->text }; em_text_impl(title && title[0] ? title : " ", tp); }
    ui_end_stack();
}
void em_appbar_end_(void) { em_flush(); ui_end_stack(); }

/* DragHandle: a placeable draggable strip -- press-and-move anywhere on it drags
 * the bound window (like WindowBar's zone, but the app positions it: e.g. the
 * empty middle of a menu bar). Grows by default; put controls to either side. */
void em_drag_handle_(EmProps p) {
    em_flush();
    ui_begin_hstack(0);
    ui_open();
    if (p.width <= 0) ui_set_size(sz_grow(), sz_intrinsic());
    ui_set_align(ALIGN_CENTER);
    em_apply_box(p);
    em_window_drag_();
}
void em_drag_handle_end_(void) { em_flush(); ui_end_stack(); }

/* ---- window controls: traffic lights ------------------------------------ *
 * Colour IS the affordance, which is the whole point of the Mac design: red
 * means this window goes away, green means it comes back. That reads instantly
 * and at any size, where two identical grey pills distinguished only by a tiny
 * glyph do not -- you had to look at the symbol to know which was which.
 *
 * So the glyph is only shown on hover, and the resting state is pure colour.
 * The visible dot is small (13px, Mac's is 12) but the CONTROL is 24px: the
 * case is deliberately larger than the dot and completely transparent, so
 * Fitts' law is satisfied by the hit area while the eye sees a small tidy
 * light. That separation is why an oversized case is correct here -- it just
 * has to be invisible, and the dot has to be genuinely centred in it. */
#define TL_DOT  13
#define TL_HIT  24

static bool em_light_impl(int cp, Color base, bool *out_hov) {
    ui_begin_hstack(0);
    struct instance_handle self = ui_open();
    bool hov = ui_is_hovered(), pressed = ui_is_pressed();
    if (out_hov) *out_hov = hov;
    ui_set_size(sz_fixed(TL_HIT), sz_fixed(TL_HIT));
    ui_set_paint(solid((Color){ 0.f, 0.f, 0.f, 0.f }));   /* the case is invisible */
    ui_set_align(ALIGN_CENTER);
    ui_set_justify(JUSTIFY_CENTER);

    ui_box_begin(1);
    ui_set_size(sz_fixed(TL_DOT), sz_fixed(TL_DOT));
    ui_set_paint(solid(pressed ? shade(base, 0.80f) : hov ? shade(base, 1.10f) : base));
    ui_set_corner_radius((float)TL_DOT / 2.0f);
    ui_set_align(ALIGN_CENTER);
    ui_set_justify(JUSTIFY_CENTER);
    /* the symbol appears only under the pointer -- at rest the colour says it */
    if (hov) { EmProps ip = { .font = Caption, .color = { 0.f, 0.f, 0.f, 0.68f } };
               em_icon_impl(cp, ip); }
    ui_box_end();

    ui_end_stack();
    return ui_consume_click(self);
}

/* #FF5F57 / #28C840 -- close is red, minimize green, as asked. */
static bool em_closebtn_impl(bool *out_hov) {
    return em_light_impl(IconClose, (Color){ 1.00f, 0.373f, 0.341f, 1.f }, out_hov);
}
static bool em_minbtn_impl(bool *out_hov) {
    return em_light_impl(IconMinus, (Color){ 0.157f, 0.784f, 0.251f, 1.f }, out_hov);
}
int em_window_closed(void) { return Clicked("__em_win_close"); }
int em_window_minimized(void) { return Clicked("__em_win_min"); }

/* ---- Spinner: phase-animated dots (indeterminate activity) ------------- */
static void em_spinner_impl(void) {
    const struct ui_theme *t = TH;
    em_request_frame();                    /* indeterminate: animates every frame */
    const int N = 8;
    float now = (float)em_now_ms();
    float phase = now > 0 ? (now / 90.0f) : 0.0f;   /* advance ~11 steps/sec */
    ui_begin_hstack(0);
    ui_set_align(ALIGN_CENTER);
    ui_set_spacing(t->sp1 + 1);
    for (int i = 0; i < N; i++) {
        /* a lit dot travels around the row */
        float d = phase - (float)i;
        int k = ((int)d) % N; if (k < 0) k += N;
        float a = (k == 0) ? 1.0f : (k == 1 || k == N - 1) ? 0.55f : 0.20f;
        ui_box_begin((uint64_t)(i + 1));
        ui_set_paint(solid(tint(t->accent, a)));
        ui_set_corner_radius(t->radius_pill);
        ui_set_size(sz_fixed(7), sz_fixed(7));
        ui_box_end();
    }
    ui_end_stack();
}

/* ---- Gauge: a rasterised ring (0..1), like LineChart --------------------*/
#define GA_SZ 120
static uint32_t g_ga_buf[GA_SZ * GA_SZ];

/* fast atan2 (~0.01 rad) -> avoids a libm dependency in the toolkit. */
static float em_atan2(float y, float x) {
    const float PI = 3.14159265f, HALF = 1.57079633f;
    float ax = x < 0 ? -x : x, ay = y < 0 ? -y : y, r;
    if (ax >= ay) { float z = ay / (ax + 1e-9f); r = z * (0.9724f - 0.1919f * z * z); }
    else          { float z = ax / (ay + 1e-9f); r = HALF - z * (0.9724f - 0.1919f * z * z); }
    if (x < 0) r = PI - r;
    if (y < 0) r = -r;
    return r;
}

void em_gauge(float frac, const char *center, EmProps p) {
    em_flush();
    const struct ui_theme *t = TH;
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    Color ac = p.color.a > 0 ? p.color : t->accent;
    uint32_t on  = argb_premul(ac, 1.0f);
    uint32_t off = argb_premul(t->border, 1.0f);
    /* Re-rasterise ONLY when the ring actually changes: per-pixel float math
     * every frame is poison under TCG (it alone pushed v4demo's frame time to
     * ~0.5s). Keyed on the quantised fraction + both colours. */
    static uint32_t ga_key;
    uint32_t key = ((uint32_t)(frac * 1024.0f) << 8) ^ on ^ (off * 2654435761u);
    if (key != ga_key || ga_key == 0) {
        ga_key = key;
        const float TWO_PI = 6.2831853f;
        float cx = GA_SZ / 2.0f, cy = GA_SZ / 2.0f;
        float r_out = GA_SZ / 2.0f - 2.0f, r_in = r_out - 12.0f;
        for (int y = 0; y < GA_SZ; y++) {
            for (int x = 0; x < GA_SZ; x++) {
                float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
                float dist = dx * dx + dy * dy;
                uint32_t px = 0;                               /* transparent */
                if (dist <= r_out * r_out && dist >= r_in * r_in) {
                    float turn = em_atan2(dx, -dy) / TWO_PI;   /* 0 at top, CW */
                    if (turn < 0) turn += 1.0f;
                    px = (turn <= frac) ? on : off;
                }
                g_ga_buf[y * GA_SZ + x] = px;
            }
        }
    }
    float H = p.height > 0 ? p.height : (float)GA_SZ;
    /* ring + a centred readout beneath it (a column keeps layout predictable in
     * the pure-translation scene -- no fragile absolute overlay). */
    ui_begin_vstack(0);
    ui_set_align(ALIGN_CENTER);
    ui_set_spacing(t->sp1);
    ui_image((uint64_t)(uintptr_t)&g_ga_buf, g_ga_buf, GA_SZ, GA_SZ, H);
    if (center && center[0]) {
        ui_set_font(t->font_bold); ui_set_text_size(t->text_title); ui_set_text_color(t->text);
        ui_text("%s", center);
    }
    ui_end_stack();
}

/* ---- ColorPicker: an HSV square + hue bar, rasterised like the Gauge ------ *
 * Binds to float hsv[3] = { hue, saturation, value }, all 0..1. The square
 * (saturation x value, for the current hue) re-rasterises only when the hue
 * changes; the rainbow hue bar is rasterised once. Cursors are composited into
 * per-frame display copies (cheap integer memcpy + ring), so moving a cursor
 * never triggers the expensive float rasterisation. Drag either surface. */
#define CP_W    176
#define CP_SVH  140
#define CP_HUEH 18
static uint32_t g_cp_sv[CP_W * CP_SVH];    /* SV square base (cached on hue)   */
static uint32_t g_cp_svd[CP_W * CP_SVH];   /* + SV cursor, per frame           */
static uint32_t g_cp_hue[CP_W * CP_HUEH];  /* rainbow hue bar (rasterised once)*/
static uint32_t g_cp_hued[CP_W * CP_HUEH]; /* + hue cursor, per frame          */

/* HSV (all 0..1) -> straight-alpha Color. */
Color em_hsv(float h, float s, float v) {
    h -= (float)(int)h;
    if (h < 0) h += 1.0f;
    if (s < 0) s = 0;
    if (s > 1) s = 1;
    if (v < 0) v = 0;
    if (v > 1) v = 1;
    float hh = h * 6.0f; int i = (int)hh; float f = hh - i;
    float p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
    float r, g, b;
    switch (i % 6) {
        case 0:  r = v; g = t; b = p; break;
        case 1:  r = q; g = v; b = p; break;
        case 2:  r = p; g = v; b = t; break;
        case 3:  r = p; g = q; b = v; break;
        case 4:  r = t; g = p; b = v; break;
        default: r = v; g = p; b = q; break;
    }
    return (Color){ r, g, b, 1.0f };
}

/* 2px ring (distance test in a bounding box), used for the SV cursor. */
static void cp_ring(uint32_t *buf, int w, int h, int cx, int cy, int rad, uint32_t col) {
    for (int y = cy - rad - 1; y <= cy + rad + 1; y++) {
        if (y < 0 || y >= h) continue;
        for (int x = cx - rad - 1; x <= cx + rad + 1; x++) {
            if (x < 0 || x >= w) continue;
            int dx = x - cx, dy = y - cy, d2 = dx * dx + dy * dy;
            if (d2 <= (rad + 1) * (rad + 1) && d2 >= (rad - 1) * (rad - 1)) buf[y * w + x] = col;
        }
    }
}

/* Map the live pointer into the currently-open box while it is pressed;
 * writes normalised (fx,fy) in [0,1]. Returns true when it updated. */
static bool cp_drag(float *fx, float *fy) {
    if (!(ui_is_pressed() || ui_is_active())) return false;
    float px, py, bx, by, bw, bh;
    ui_pointer_pos(&px, &py);
    if (!ui_open_rect(&bx, &by, &bw, &bh) || bw <= 0 || bh <= 0) return false;
    float x = (px - bx) / bw, y = (py - by) / bh;
    if (x < 0) x = 0;
    if (x > 1) x = 1;
    if (y < 0) y = 0;
    if (y > 1) y = 1;
    *fx = x; *fy = y;
    return true;
}

void em_colorpicker(float *hsv, EmProps p) {
    em_flush();
    const struct ui_theme *t = TH;
    float H = hsv ? hsv[0] : 0.0f, S = hsv ? hsv[1] : 1.0f, V = hsv ? hsv[2] : 1.0f;

    /* hue bar: rasterise once (never changes). */
    static int hue_ready;
    if (!hue_ready) {
        hue_ready = 1;
        for (int x = 0; x < CP_W; x++) {
            uint32_t c = argb_premul(em_hsv((float)x / (CP_W - 1), 1.0f, 1.0f), 1.0f);
            for (int y = 0; y < CP_HUEH; y++) g_cp_hue[y * CP_W + x] = c;
        }
    }
    /* SV square: rasterise only when the hue actually changes. */
    static uint32_t sv_key = 0xFFFFFFFFu;
    uint32_t hk = (uint32_t)(H * 4096.0f);
    if (hk != sv_key) {
        sv_key = hk;
        for (int y = 0; y < CP_SVH; y++) {
            float v = 1.0f - (float)y / (CP_SVH - 1);
            for (int x = 0; x < CP_W; x++) {
                float s = (float)x / (CP_W - 1);
                g_cp_sv[y * CP_W + x] = argb_premul(em_hsv(H, s, v), 1.0f);
            }
        }
    }
    /* per-frame display copies + cursors. */
    for (int i = 0; i < CP_W * CP_SVH; i++) g_cp_svd[i] = g_cp_sv[i];
    cp_ring(g_cp_svd, CP_W, CP_SVH, (int)(S * (CP_W - 1)), (int)((1.0f - V) * (CP_SVH - 1)),
            6, (V > 0.6f && S < 0.5f) ? 0xFF000000u : 0xFFFFFFFFu);
    for (int i = 0; i < CP_W * CP_HUEH; i++) g_cp_hued[i] = g_cp_hue[i];
    int hcx = (int)(H * (CP_W - 1));
    for (int y = 0; y < CP_HUEH; y++) {
        uint32_t *row = &g_cp_hued[y * CP_W];
        if (hcx > 0)        row[hcx - 1] = 0xFF000000u;
        row[hcx] = 0xFFFFFFFFu;
        if (hcx + 1 < CP_W) row[hcx + 1] = 0xFF000000u;
    }

    ui_begin_vstack(0);
    ui_set_spacing(t->sp2);
    em_apply_box(p);

    /* SV square (draggable) */
    ui_box_begin(0xC010D1);
    ui_set_size(sz_fixed(CP_W), sz_fixed(CP_SVH));
    ui_set_corner_radius(t->radius_md);
    ui_image((uint64_t)(uintptr_t)&g_cp_svd, g_cp_svd, CP_W, CP_SVH, (float)CP_SVH);
    { float fx, fy; if (hsv && cp_drag(&fx, &fy)) { hsv[1] = fx; hsv[2] = 1.0f - fy; } }
    ui_box_end();

    /* hue bar (draggable) */
    ui_box_begin(0xC010D2);
    ui_set_size(sz_fixed(CP_W), sz_fixed(CP_HUEH));
    ui_set_corner_radius(t->radius_md);
    ui_image((uint64_t)(uintptr_t)&g_cp_hued, g_cp_hued, CP_W, CP_HUEH, (float)CP_HUEH);
    { float fx, fy; if (hsv && cp_drag(&fx, &fy)) { (void)fy; hsv[0] = fx; } }
    ui_box_end();

    /* preview: swatch + hex readout */
    ui_begin_hstack(0);
    ui_set_align(ALIGN_CENTER);
    ui_set_spacing(t->sp2);
    ui_box_begin(0xC010D3);
    ui_set_size(sz_fixed(30), sz_fixed(30));
    ui_set_paint(solid(em_hsv(H, S, V)));
    ui_set_corner_radius(t->radius_md);
    ui_set_border(1.0f, t->border);
    ui_box_end();
    { Color c = em_hsv(H, S, V);
      char hex[10];
      snprintf(hex, sizeof hex, "#%02X%02X%02X",
               (int)(c.r * 255 + 0.5f), (int)(c.g * 255 + 0.5f), (int)(c.b * 255 + 0.5f));
      EmProps tp = { .font = BodyBold };
      em_text_impl(hex, tp); }
    ui_end_stack();

    ui_end_stack();
}

/* ---- Calendar / DatePicker: an inline month grid ------------------------- *
 * Binds to int date[3] = { year, month(1..12), day(1..31) }. Header with
 * prev/next month, a weekday row, then a 6x7 grid of day cells; the selected
 * day is filled with the accent. Month navigation carries its own view state
 * (defaults to the selected month), so paging months doesn't move the pick. */
static const char *const CAL_MON[12] = {
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December" };
static const char *const CAL_WD[7] = { "S", "M", "T", "W", "T", "F", "S" };

static int cal_leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
static int cal_dim(int y, int m) {
    static const int d[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    return (m == 2 && cal_leap(y)) ? 29 : d[(m - 1) % 12];
}
/* Sakamoto's algorithm: weekday of the 1st (0 = Sunday). */
static int cal_dow1(int y, int m) {
    static const int t[12] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    int yy = y;
    if (m < 3) yy -= 1;
    return (yy + yy / 4 - yy / 100 + yy / 400 + t[(m - 1) % 12] + 1) % 7;
}

void em_calendar(int *date, EmProps p) {
    em_flush();
    const struct ui_theme *t = TH;
    int sy = date ? date[0] : 2026, sm = date ? date[1] : 1, sd = date ? date[2] : 1;
    if (sm < 1) sm = 1;
    if (sm > 12) sm = 12;
    static int vy, vm, inited;
    if (!inited) { inited = 1; vy = sy; vm = sm; }

    ui_begin_vstack(0);
    ui_set_spacing(t->sp2);
    em_apply_box(p);

    /* header: ‹  Month Year  › */
    ui_begin_hstack(0);
    ui_set_align(ALIGN_CENTER);
    ui_set_size(sz_grow(), sz_intrinsic());
    if (em_iconbtn_impl(IconChevronL, (EmProps){0}, 0)) { if (--vm < 1) { vm = 12; vy--; } }
    ui_spacer();
    { char hdr[32];
      snprintf(hdr, sizeof hdr, "%s %d", CAL_MON[vm - 1], vy);
      EmProps hp = { .font = BodyBold };
      em_text_impl(hdr, hp); }
    ui_spacer();
    if (em_iconbtn_impl(IconChevronR, (EmProps){0}, 0)) { if (++vm > 12) { vm = 1; vy++; } }
    ui_end_stack();

    /* weekday labels */
    ui_begin_hstack(0);
    ui_set_spacing(0);
    for (int i = 0; i < 7; i++) {
        ui_box_begin(0);
        ui_set_size(sz_fixed(34), sz_fixed(22));
        ui_set_align(ALIGN_CENTER);
        ui_set_justify(JUSTIFY_CENTER);
        EmProps wp = { .font = Caption, .color = t->text_tertiary };
        em_text_impl(CAL_WD[i], wp);
        ui_box_end();
    }
    ui_end_stack();

    /* 6 weeks x 7 days */
    int dow = cal_dow1(vy, vm), dim = cal_dim(vy, vm);
    int day = 1 - dow;                       /* first cell's number (<=0 -> blank) */
    for (int w = 0; w < 6; w++) {
        ui_begin_hstack(0);
        ui_set_spacing(0);
        for (int c = 0; c < 7; c++, day++) {
            ui_box_begin((uint64_t)(0xDA7E0000u + w * 7 + c));
            struct instance_handle self = ui_open();
            bool inmonth = (day >= 1 && day <= dim);
            bool sel = inmonth && (vy == sy && vm == sm && day == sd);
            bool hov = inmonth && ui_is_hovered();
            ui_set_size(sz_fixed(34), sz_fixed(30));
            ui_set_align(ALIGN_CENTER);
            ui_set_justify(JUSTIFY_CENTER);
            if (sel)      { ui_set_paint(solid(t->accent));      ui_set_corner_radius(t->radius_md); }
            else if (hov) { ui_set_paint(solid(t->surface_alt)); ui_set_corner_radius(t->radius_md); }
            else          { ui_set_paint(solid((Color){0, 0, 0, 0})); }   /* reset when neither */
            if (inmonth) {
                char ds[12];
                snprintf(ds, sizeof ds, "%d", day);
                EmProps dp = { .font = sel ? BodyBold : Body, .color = sel ? t->on_accent : t->text };
                em_text_impl(ds, dp);
            }
            ui_box_end();
            if (inmonth && ui_consume_click(self) && date) { date[0] = vy; date[1] = vm; date[2] = day; }
        }
        ui_end_stack();
    }
    ui_end_stack();
}

/* ---- Combobox: an editable field with a filtered option menu ------------- *
 * Binds an editable text buffer + a list of options; the menu shows only the
 * options containing the typed text (case-insensitive), and picking one fills
 * the buffer. `open` (app-owned, like Disclosure) controls the menu; the
 * chevron toggles it. */
static char cb_lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
static int  cb_contains(const char *hay, const char *ndl) {
    if (!ndl || !ndl[0]) return 1;
    for (const char *h = hay; *h; h++) {
        const char *a = h, *b = ndl;
        while (*a && *b && cb_lc(*a) == cb_lc(*b)) { a++; b++; }
        if (!*b) return 1;
    }
    return 0;
}

void em_combobox(char *buf, size_t cap, const char *const *labels, int count,
                 const char *placeholder, bool *open, EmProps p) {
    em_flush();
    const struct ui_theme *t = TH;
    bool is_open = open ? *open : false;

    ui_begin_vstack(0);
    ui_set_align(ALIGN_STRETCH);
    ui_set_spacing(t->sp1);
    em_apply_box(p);

    /* field row: text input (grows) + chevron toggle */
    ui_begin_hstack(0);
    ui_set_align(ALIGN_CENTER);
    ui_set_spacing(t->sp2);
    ui_set_size(sz_grow(), sz_intrinsic());
    ui_begin_hstack(0);
    ui_set_size(sz_grow(), sz_intrinsic());
    ui_text_field(buf, cap, placeholder);
    ui_end_stack();
    if (em_iconbtn_impl(is_open ? IconChevronU : IconChevronD, (EmProps){0}, 0) && open) {
        *open = !is_open;
        is_open = *open;
        g_em_epoch++;
    }
    ui_end_stack();

    /* filtered menu */
    if (is_open) {
        ui_begin_vstack(0);
        ui_set_paint(solid(t->surface));
        ui_set_corner_radius(t->radius_md);
        ui_set_border(1.0f, t->border);
        ui_set_clip_children(true);
        ui_set_align(ALIGN_STRETCH);
        ui_set_spacing(0);
        ui_set_shadow(true, t->shadow_md.dx, t->shadow_md.dy, t->shadow_md.blur, t->shadow_md.color);
        int shown = 0;
        for (int i = 0; i < count; i++) {
            if (!cb_contains(labels[i], buf)) continue;
            shown++;
            ui_begin_hstack((uint64_t)(i + 1));
            struct instance_handle row = ui_open();
            bool rhov = ui_is_hovered(), rpr = ui_is_pressed();
            ui_set_paint(solid(rpr ? shade(t->accent_soft, 0.94f) : rhov ? t->accent_soft : t->surface));
            ui_set_padding(t->sp2 + 1, t->sp3, t->sp2 + 1, t->sp3);
            ui_set_align(ALIGN_CENTER);
            ui_set_size(sz_grow(), sz_intrinsic());
            { EmProps lp = { .font = Body, .color = t->text }; em_text_impl(labels[i], lp); }
            ui_end_stack();
            if (ui_consume_click(row)) {
                size_t n = 0;
                for (const char *s = labels[i]; *s && n + 1 < cap; s++) buf[n++] = *s;
                buf[n] = 0;
                if (open) *open = false;
                is_open = false;
                g_em_epoch++;
            }
        }
        if (shown == 0) {
            ui_begin_hstack(0);
            ui_set_padding(t->sp2 + 1, t->sp3, t->sp2 + 1, t->sp3);
            EmProps lp = { .font = Body, .color = t->text_tertiary };
            em_text_impl("No matches", lp);
            ui_end_stack();
        }
        ui_end_stack();
    }
    ui_end_stack();
}

/* ---- TagInput: removable chips + an entry field -------------------------- *
 * tags is char[max][EM_TAG_LEN]; *count is the live tag count; `entry` is the
 * text buffer for a new tag. The + button (or a non-empty entry) commits a
 * tag; each chip's ✕ removes it. */
static void ti_copy(char *dst, const char *src, size_t cap) {
    size_t n = 0;
    for (; src[n] && n + 1 < cap; n++) dst[n] = src[n];
    dst[n] = 0;
}

void em_taginput(char (*tags)[EM_TAG_LEN], int *count, int max,
                 char *entry, size_t ecap, EmProps p) {
    em_flush();
    const struct ui_theme *t = TH;
    int n = count ? *count : 0;

    ui_begin_vstack(0);
    ui_set_align(ALIGN_STRETCH);
    ui_set_spacing(t->sp2);
    em_apply_box(p);

    /* chips: a real flex-wrap row -- the layout engine flows them onto new lines
     * (pixel-accurate, no width estimate). Removal is deferred to after render so
     * the list isn't mutated mid-layout. */
    int remove_idx = -1;
    if (n > 0) {
        ui_begin_hstack(0);
        ui_set_wrap(true);
        ui_set_spacing(t->sp2);
        ui_set_align(ALIGN_CENTER);
        ui_set_size(sz_grow(), sz_intrinsic());
        for (int i = 0; i < n; i++) {
            ui_begin_hstack((uint64_t)(0x7A6C0000u + i));
            ui_set_paint(solid(t->accent_soft));
            ui_set_corner_radius(t->radius_pill);
            ui_set_padding(t->sp1, t->sp3, t->sp1, t->sp2 + 2);
            ui_set_align(ALIGN_CENTER);
            ui_set_spacing(t->sp1);
            { EmProps lp = { .font = Body, .color = t->accent }; em_text_impl(tags[i], lp); }
            ui_box_begin((uint64_t)(0x7A6F0000u + i));
            struct instance_handle rm = ui_open();
            { EmProps xp = { .font = Caption, .color = t->accent }; em_icon_impl(IconClose, xp); }
            ui_box_end();
            ui_end_stack();
            if (ui_consume_click(rm)) remove_idx = i;
        }
        ui_end_stack();
    }
    if (remove_idx >= 0 && count) {
        for (int j = remove_idx; j < n - 1; j++) ti_copy(tags[j], tags[j + 1], EM_TAG_LEN);
        (*count)--;
        n--;
        g_em_epoch++;
    }

    /* entry row: field (grows) + add button */
    ui_begin_hstack(0);
    ui_set_align(ALIGN_CENTER);
    ui_set_spacing(t->sp2);
    ui_begin_hstack(0);
    ui_set_size(sz_grow(), sz_intrinsic());
    ui_text_field(entry, ecap, "Add tag...");
    ui_end_stack();
    if (em_iconbtn_impl(IconPlus, (EmProps){0}, 0) && count && entry && entry[0] && n < max) {
        ti_copy(tags[n], entry, EM_TAG_LEN);
        (*count)++;
        entry[0] = 0;
        g_em_epoch++;
    }
    ui_end_stack();
    ui_end_stack();
}

/* ---- Dock: a drag-REORDER + drag-OUT-to-remove row of chips --------------- *
 * `ids` holds the display order (length *n); render(id) draws one chip's inner
 * content. Press-drag a chip: it lifts (shadow), snaps between slots as the
 * pointer crosses them (the others flow), and if pulled below the row it dims
 * and, on release, is removed. This is EmUI's first real drag-and-drop. */
static int   g_dock_id = -1;      /* id being dragged, -1 = none */
static float g_dock_grab_x, g_dock_grab_y;   /* pointer at grab (screen) */
static float g_dock_ptr_y;        /* live pointer y (for drag-out) */
static int   g_dock_out;          /* pulled below the row -> pending remove */

int em_dock_dragging(void) { return g_dock_id; }   /* id being dragged, or -1 */

void em_dock(int *ids, int *n, void (*render)(int id), EmProps p) {
    em_flush();
    const struct ui_theme *t = TH;
    int cnt = n ? *n : 0;

    ui_begin_hstack(0);
    (void)ui_open();
    ui_set_spacing(p.spacing > 0 ? p.spacing : t->sp2);
    ui_set_align(ALIGN_CENTER);
    em_apply_box(p);
    float dx0, dy0, dw, dh;
    int have = ui_open_rect(&dx0, &dy0, &dw, &dh);

    int any_active = 0, active_i = -1, target = -1;

    for (int i = 0; i < cnt; i++) {
        int id = ids[i];
        int dragged = (id == g_dock_id);
        ui_box_begin((uint64_t)(0xD0C00000u + (unsigned)id));   /* stable key by id */
        struct instance_handle self = ui_open();
        int active = ui_is_active();
        bool hov = ui_is_hovered();
        /* A status item is a GLYPH, not a button: boxing each one in its own
         * grey chip turned a menu bar into a row of widgets. The surface
         * appears only when the pointer is on it (or while dragging), which is
         * also the only time it means anything. */
        Color chip = dragged ? shade(t->surface_alt, 1.18f)
                   : hov     ? t->surface_alt
                             : (Color){ 0.f, 0.f, 0.f, 0.f };
        ui_set_paint(solid(chip));
        ui_set_corner_radius(t->radius_md);
        ui_set_padding(t->sp1, t->sp2, t->sp1, t->sp2);
        ui_set_align(ALIGN_CENTER);
        if (dragged) {
            ui_set_shadow(true, 0, 4, 12, t->shadow_md.color);      /* lift */
            if (g_dock_out) ui_set_offset(0, g_dock_ptr_y - g_dock_grab_y);  /* follow out */
        }
        render(id);
        ui_box_end();
        (void)self;

        if (active) {
            any_active = 1; active_i = i;
            float px, py; ui_pointer_pos(&px, &py);
            g_dock_ptr_y = py;
            if (g_dock_id != id) { g_dock_id = id; g_dock_grab_x = px; g_dock_grab_y = py; g_dock_out = 0; }
            if (have && dw > 0) {
                int tgt = (int)(((px - dx0) / dw) * cnt);
                if (tgt < 0) tgt = 0;
                if (tgt >= cnt) tgt = cnt - 1;
                target = tgt;
            }
            g_dock_out = (have && py > dy0 + dh + 16.0f);
        }
    }
    ui_end_stack();

    /* reorder (after the loop, so no mid-loop mutation); not while removing */
    if (active_i >= 0 && target >= 0 && target != active_i && !g_dock_out) {
        int tmp = ids[active_i];
        if (target > active_i) for (int k = active_i; k < target; k++) ids[k] = ids[k + 1];
        else                   for (int k = active_i; k > target; k--) ids[k] = ids[k - 1];
        ids[target] = tmp;
        g_em_epoch++;
    }
    /* release: remove if it was pulled out */
    if (g_dock_id != -1 && !any_active) {
        if (g_dock_out && n) {
            for (int i = 0; i < *n; i++) if (ids[i] == g_dock_id) {
                for (int k = i; k < *n - 1; k++) ids[k] = ids[k + 1];
                (*n)--;
                break;
            }
        }
        g_dock_id = -1; g_dock_out = 0;
        g_em_epoch++;
    }
}

/* ---- StatCard: label / big value / signed delta / mini sparkline -------- */
void em_stat_card(const char *label, const char *value, const char *delta,
                  const float *vals, int n) {
    em_flush();
    const struct ui_theme *t = TH;
    ui_box_begin(0);
    ui_set_paint(solid(t->surface));
    ui_set_corner_radius(t->radius_lg);
    ui_set_border(1.0f, t->border);
    ui_set_padding(t->sp4, t->sp4, t->sp4, t->sp4);
    ui_set_axis(AXIS_COLUMN);
    ui_set_align(ALIGN_STRETCH);
    ui_set_spacing(t->sp1);
    ui_set_size(sz_grow(), sz_intrinsic());

    { EmProps lp = { .font = Caption, .color = t->text_secondary }; em_text_impl(label, lp); }
    { EmProps vp = { .font = Title, .color = t->text }; em_text_impl(value, vp); }
    if (delta && delta[0]) {
        int neg = (delta[0] == '-');
        Color dc = neg ? t->danger : t->success;
        EmProps dp = { .font = Caption, .color = dc };
        em_text_impl(delta, dp);
    }
    if (vals && n >= 2) {
        EmProps cp = { .height = 34, .color = t->accent };
        em_linechart(vals, n, 1, cp);
    }
    ui_box_end();
}

/* ---- EmptyState: centred icon + title + subtitle ----------------------- */
void em_empty_state(int icon, const char *title, const char *subtitle) {
    em_flush();
    const struct ui_theme *t = TH;
    ui_begin_vstack(0);
    ui_set_align(ALIGN_CENTER);
    ui_set_justify(JUSTIFY_CENTER);
    ui_set_spacing(t->sp2);
    ui_set_padding(t->sp6, t->sp5, t->sp6, t->sp5);
    ui_set_size(sz_grow(), sz_intrinsic());
    { char g[5]; utf8_enc(icon, g);
      ui_set_font(t->font_regular); ui_set_text_size(t->text_heading * 1.6f);
      ui_set_text_color(t->text_tertiary); ui_text("%s", g); }
    { EmProps tp = { .font = Heading, .color = t->text }; em_text_impl(title, tp); }
    if (subtitle && subtitle[0]) {
        EmProps sp = { .font = Body, .color = t->text_secondary };
        em_text_impl(subtitle, sp);
    }
    ui_end_stack();
}

/* ---- DividerLabel: line -- LABEL -- line -------------------------------- */
void em_divider_label(const char *label) {
    em_flush();
    const struct ui_theme *t = TH;
    ui_begin_hstack(0);
    ui_set_align(ALIGN_CENTER);
    ui_set_spacing(t->sp3);
    ui_set_size(sz_grow(), sz_intrinsic());
    ui_box_begin(1); ui_set_paint(solid(t->border)); ui_set_size(sz_grow(), sz_fixed(1)); ui_box_end();
    { ui_set_font(t->font_bold); ui_set_text_size(t->text_caption); ui_set_text_color(t->text_tertiary);
      ui_text("%s", label); }
    ui_box_begin(2); ui_set_paint(solid(t->border)); ui_set_size(sz_grow(), sz_fixed(1)); ui_box_end();
    ui_end_stack();
}

/* ---- Toast: transient message; ToastHost() renders the active one ------ */
static struct { const char *msg; EmTone tone; uint64_t raised; int active; } g_toast;
#define TOAST_MS 2500

void em_toast(const char *msg, EmTone tone) {
    g_toast.msg = msg; g_toast.tone = tone; g_toast.raised = em_now_ms(); g_toast.active = 1;
    g_em_epoch++;
}
void em_toast_host(void) {
    if (!g_toast.active) return;
    em_request_frame();                    /* fading/expiring: keep frames coming */
    const struct ui_theme *t = TH;
    uint64_t now = em_now_ms();
    /* with a real clock, auto-expire; on host (clock 0) stay visible for render */
    float age = (now && g_toast.raised) ? (float)(now - g_toast.raised) : 0.0f;
    if (now && age > TOAST_MS) { g_toast.active = 0; g_em_epoch++; return; }
    /* fade+rise in over the first 180ms and out over the last 300ms */
    float op = 1.0f;
    if (now) {
        if (age < 180.0f) op = age / 180.0f;
        else if (age > TOAST_MS - 300.0f) op = (TOAST_MS - age) / 300.0f;
    }
    if (op < 0) op = 0;
    if (op > 1) op = 1;
    Color accent = t->accent;
    int cp = IconInfo;
    switch (g_toast.tone) {
        case Success: accent = t->success; cp = IconCheck; break;
        case Warning: accent = t->warning; cp = IconWarn;  break;
        case Danger:  accent = t->danger;  cp = IconClose; break;
        default: break;
    }
    /* A ZERO-HEIGHT in-flow anchor at the very bottom of the window column;
     * the pill is raised into view with a pure scene offset. Floats over the
     * content without taking layout space or intercepting clicks elsewhere
     * (unlike an overlay layer, which hit-tests across the whole window). */
    ui_begin_hstack(0);
    ui_set_align(ALIGN_CENTER);
    ui_set_justify(JUSTIFY_CENTER);
    ui_set_size(sz_grow(), (struct layout_size){ .mode = SIZE_FIXED, .fixed_value = 0 });
    ui_set_offset(0, -62.0f + (1.0f - op) * 16.0f);   /* rise above the tab bar */
    ui_set_opacity(op < 1.0f ? op : 1.0f);
    {
        ui_begin_hstack(1);
        ui_set_paint(solid(t->text));                 /* dark pill, light text (modern toast) */
        ui_set_corner_radius(t->radius_pill);
        ui_set_shadow(true, t->shadow_lg.dx, t->shadow_lg.dy, t->shadow_lg.blur, t->shadow_lg.color);
        ui_set_padding(t->sp2, t->sp4, t->sp2, t->sp4);
        ui_set_align(ALIGN_CENTER);
        ui_set_spacing(t->sp2);
        { EmProps ip = { .font = BodyBold, .color = accent }; em_icon_impl(cp, ip); }
        { EmProps mp = { .font = Body, .color = t->bg }; em_text_impl(g_toast.msg ? g_toast.msg : "", mp); }
        ui_end_stack();
    }
    ui_end_stack();
}

/* ---- SearchField: field + leading search glyph + trailing clear --------- */
static bool em_search_impl(char *buf, size_t cap, const char *ph, bool *out_hov) {
    const struct ui_theme *t = TH;
    ui_begin_hstack(0);
    struct instance_handle self = ui_open();
    bool hov = ui_is_hovered();
    if (out_hov) *out_hov = hov;
    ui_set_paint(solid(t->surface_alt));
    ui_set_corner_radius(t->radius_pill);
    ui_set_border(1.0f, hov ? t->border_strong : t->border);
    ui_set_padding(t->sp2, t->sp3, t->sp2, t->sp3);
    ui_set_align(ALIGN_CENTER);
    ui_set_spacing(t->sp2);
    ui_set_size(sz_grow(), sz_intrinsic());
    { EmProps ip = { .font = Body, .color = t->text_tertiary }; em_icon_impl(IconMagnify, ip); }
    /* the actual editable field, borderless, grows */
    { EmProps fp = { .grow = 1 }; (void)fp; em_field_impl(buf, cap, ph, (EmProps){ .grow = 1 }, 0); }
    if (buf && buf[0]) {
        EmProps xp = { .font = Body, .color = t->text_tertiary };
        if (em_iconbtn_impl(IconClose, xp, 0)) buf[0] = '\0';   /* clear */
    }
    ui_end_stack();
    return ui_consume_click(self);
}

/* ---- Disclosure: a tappable header revealing its scope children -------- */
static int g_disc_open;   /* passed from em_disclosure_ to _end_ */
void em_disclosure_(const char *title, bool *open, EmProps p) {
    em_flush();
    const struct ui_theme *t = TH;
    ui_begin_vstack(0);                 /* wrapper: header + (optional) body */
    ui_set_align(ALIGN_STRETCH);
    ui_set_spacing(t->sp2);
    em_apply_box(p);
    /* header row (tappable) */
    ui_begin_hstack(0);
    struct instance_handle self = ui_open();
    bool hov = ui_is_hovered(), pressed = ui_is_pressed();
    ui_set_paint(solid(pressed ? shade(t->surface_alt, 0.94f) : hov ? t->surface_alt : t->surface));
    ui_set_corner_radius(t->radius_md);
    ui_set_border(1.0f, t->border);
    ui_set_padding(t->sp3, t->sp4, t->sp3, t->sp4);
    ui_set_align(ALIGN_CENTER);
    ui_set_spacing(t->sp3);
    { EmProps tp = { .font = BodyBold, .color = t->text }; em_text_impl(title, tp); }
    ui_spacer();
    { EmProps cp = { .font = Body, .color = t->text_secondary };
      em_icon_impl((open && *open) ? IconChevronU : IconChevronD, cp); }
    ui_end_stack();
    if (ui_consume_click(self) && open) { *open = !*open; g_em_epoch++; }
    g_disc_open = (open && *open) ? 1 : 0;
    if (!g_disc_open) return;           /* body suppressed: children still emit but into a hidden box */
    ui_begin_vstack(0);                 /* the body -- children append here */
    ui_set_align(ALIGN_STRETCH);
    ui_set_spacing(t->sp2);
    ui_set_padding(0, t->sp2, t->sp2, t->sp2);
}
void em_disclosure_end_(void) {
    em_flush();
    if (g_disc_open) ui_end_stack();    /* close body */
    ui_end_stack();                     /* close wrapper */
}

/* ---- Dropdown / Picker ------------------------------------------------- */
/* One dropdown open at a time, keyed by the sel pointer. The menu is drawn
 * inline right under the field (a simple, robust anchor -- no overlay needed
 * for a first cut; it participates in normal layout/scroll). */
static const int *g_dd_open;   /* which dropdown (by sel ptr) is expanded */

static bool em_dropdown_impl(const char *const *labels, int count, int *sel, bool *out_hov) {
    const struct ui_theme *t = TH;
    int cur = sel ? *sel : 0;
    if (cur < 0) cur = 0;
    if (cur >= count) cur = count - 1;
    bool is_open = (g_dd_open == (const int *)sel);

    ui_begin_vstack(0);                 /* field + (optional) menu */
    ui_set_align(ALIGN_STRETCH);
    ui_set_spacing(t->sp1);

    /* the field */
    ui_begin_hstack(0);
    struct instance_handle self = ui_open();
    bool hov = ui_is_hovered(), pressed = ui_is_pressed();
    if (out_hov) *out_hov = hov;
    /* Through the control palette, so a <select> inside a rendered document
     * is a page control and not a piece of the desktop -- see kit.h. */
    { struct color face = ui_ctl_color_(UI_CTL_SURFACE, t->surface);
      ui_set_paint(solid(pressed ? shade(face, 0.94f) : hov ? shade(face, 1.06f) : face)); }
    ui_set_corner_radius(t->radius_md);
    ui_set_border(1.0f, is_open ? ui_ctl_color_(UI_CTL_FOCUS, t->accent)
                                : ui_ctl_color_(UI_CTL_BORDER, t->border));
    ui_set_padding(t->sp2 + 1, t->sp3, t->sp2 + 1, t->sp3);
    ui_set_align(ALIGN_CENTER);
    ui_set_spacing(t->sp2);
    ui_set_size(sz_grow(), sz_intrinsic());
    { EmProps vp = { .font = Body, .color = ui_ctl_color_(UI_CTL_TEXT, t->text) };
      em_text_impl(count > 0 ? labels[cur] : "", vp); }
    ui_spacer();
    { EmProps cp = { .font = Body, .color = ui_ctl_color_(UI_CTL_PLACEHOLDER, t->text_secondary) };
      em_icon_impl(is_open ? IconChevronU : IconChevronD, cp); }
    ui_end_stack();
    if (ui_consume_click(self)) {
        g_dd_open = is_open ? 0 : (const int *)sel;   /* toggle */
        is_open = !is_open;
        g_em_epoch++;
    }

    /* the menu (inline, under the field) */
    if (is_open) {
        ui_begin_vstack(0);
        ui_set_paint(solid(t->surface));
        ui_set_corner_radius(t->radius_md);
        ui_set_border(1.0f, t->border);
        ui_set_clip_children(true);
        ui_set_align(ALIGN_STRETCH);
        ui_set_spacing(0);
        ui_set_shadow(true, t->shadow_md.dx, t->shadow_md.dy, t->shadow_md.blur, t->shadow_md.color);
        for (int i = 0; i < count; i++) {
            ui_begin_hstack((uint64_t)(i + 1));
            struct instance_handle row = ui_open();
            bool rhov = ui_is_hovered(), rpr = ui_is_pressed();
            ui_set_paint(solid(rpr ? shade(t->accent_soft, 0.94f)
                                   : rhov ? t->accent_soft
                                          : (i == cur ? t->surface_alt : t->surface)));
            ui_set_padding(t->sp2 + 1, t->sp3, t->sp2 + 1, t->sp3);
            ui_set_align(ALIGN_CENTER);
            ui_set_spacing(t->sp2);
            { EmProps lp = { .font = Body, .color = (i == cur) ? t->accent : t->text }; em_text_impl(labels[i], lp); }
            ui_spacer();
            if (i == cur) { EmProps ck = { .font = Body, .color = t->accent }; em_icon_impl(IconCheck, ck); }
            ui_end_stack();
            if (ui_consume_click(row)) {
                if (sel) *sel = i;
                g_dd_open = 0; is_open = 0;
                g_em_epoch++;
            }
        }
        ui_end_stack();
    }
    ui_end_stack();
    return false;
}

/* ---- TabView: page + bottom tab bar with an eased selection pill -------- */
void em_tabview(int *sel, const EmTab *items, int count) {
    em_flush();
    const struct ui_theme *t = TH;
    int cur = sel ? *sel : 0;
    if (cur < 0) cur = 0;
    if (cur >= count) cur = count - 1;

    ui_begin_vstack(0);
    ui_set_size(sz_grow(), sz_grow());
    ui_set_align(ALIGN_STRETCH);
    ui_set_spacing(0);

    /* the active page fills the space above the bar */
    ui_begin_vstack(0);
    ui_set_size(sz_grow(), sz_grow());
    ui_set_align(ALIGN_STRETCH);
    em_flush();
    if (count > 0 && items[cur].page) items[cur].page();
    em_flush();
    ui_end_stack();

    /* the bottom tab bar */
    ui_begin_hstack(0);
    ui_set_paint(solid(t->surface));
    ui_set_border(1.0f, t->border);
    ui_set_padding(t->sp1, t->sp2, t->sp1, t->sp2);
    ui_set_align(ALIGN_CENTER);
    ui_set_justify(JUSTIFY_SPACE_BETWEEN);
    ui_set_size(sz_grow(), sz_intrinsic());
    for (int i = 0; i < count; i++) {
        ui_begin_vstack((uint64_t)(i + 1));
        struct instance_handle tab = ui_open();
        bool hov = ui_is_hovered();
        int on = (i == cur);
        Color fg = on ? t->accent : (hov ? t->text_secondary : t->text_tertiary);
        ui_set_paint(solid(on ? t->accent_soft : t->surface));
        ui_set_corner_radius(t->radius_md);
        ui_set_padding(t->sp1, t->sp3, t->sp1, t->sp3);
        ui_set_align(ALIGN_CENTER);
        ui_set_spacing(2);
        ui_set_size(sz_grow(), sz_intrinsic());
        { char g[5]; utf8_enc(items[i].icon, g);
          ui_set_font(t->font_regular); ui_set_text_size(t->text_body_lg);
          ui_set_text_color(fg); ui_text("%s", g); }
        { ui_set_font(t->font_bold); ui_set_text_size(t->text_caption);
          ui_set_text_color(fg); ui_text("%s", items[i].label); }
        ui_end_stack();
        if (ui_consume_click(tab) && sel && *sel != i) { *sel = i; g_em_epoch++; }
    }
    ui_end_stack();     /* bar */
    ui_end_stack();     /* tabview */
}

/* ---- SplitView: fixed sidebar surface + growing content ---------------- */
static float g_split_w;
void em_split_(float sidebar_w, EmProps p) {
    em_flush();
    g_split_w = sidebar_w > 0 ? sidebar_w : 220.0f;
    ui_begin_hstack(0);
    ui_set_size(sz_grow(), sz_grow());
    ui_set_align(ALIGN_STRETCH);
    ui_set_spacing(0);
    em_apply_box(p);
}
void em_split_end_(void) { em_flush(); ui_end_stack(); }
void em_sidebar_(EmProps p) {
    em_flush();
    const struct ui_theme *t = TH;
    ui_begin_vstack(0);
    if (p.background.a <= 0) p.background = t->surface;
    ui_set_border(1.0f, t->border);
    ui_set_size(sz_fixed(g_split_w), sz_grow());
    ui_set_align(ALIGN_STRETCH);
    ui_set_spacing(t->sp1);
    ui_set_padding(t->sp3, t->sp2, t->sp3, t->sp2);
    em_apply_box(p);
}
void em_content_(EmProps p) {
    em_flush();
    const struct ui_theme *t = TH;
    ui_begin_vstack(0);
    ui_set_size(sz_grow(), sz_grow());
    ui_set_align(ALIGN_STRETCH);
    ui_set_spacing(t->sp3);
    ui_set_padding(t->sp5, t->sp5, t->sp5, t->sp5);
    em_apply_box(p);
}

/* ======================================================================= */
/* resources (V4.1) -- path-keyed caches over an injected loader           */
/* ======================================================================= */

static uint8_t *(*g_res_load)(const char *path, size_t *out_len);
void em_res_set_loader(uint8_t *(*load)(const char *path, size_t *out_len)) {
    g_res_load = load;
}

/* One slot per distinct image path. A desktop with a real icon per app blows
 * straight past the old 8 -- and a full cache used to mean re-decoding every
 * frame forever, so this bound is load-bearing, not cosmetic. */
#define EM_RES_MAX 32

uint32_t em_font(const char *path) {
    static struct { char path[128]; int used; uint32_t handle; } cache[EM_RES_MAX];
    static int installed;
    if (!path) return 0;
    for (int i = 0; i < EM_RES_MAX; i++)
        if (cache[i].used && strcmp(cache[i].path, path) == 0) return cache[i].handle;
    if (!g_res_load) return 0;
    size_t len = 0;
    uint8_t *data = g_res_load(path, &len);       /* kept alive: font parses in place */
    uint32_t h = (data && len) ? font_load(data, len) : 0;
    if (h && !installed) { font_install_backend(); installed = 1; }
    for (int i = 0; i < EM_RES_MAX; i++)
        if (!cache[i].used) {
            snprintf(cache[i].path, sizeof cache[i].path, "%s", path);
            cache[i].used = 1; cache[i].handle = h; break;
        }
    return h;
}

/* Minimal P6 RGB + P7 RGB_ALPHA decoder into BGRA-premul, cached by path. */
const uint32_t *em_image(const char *path, uint32_t *out_w, uint32_t *out_h) {
    /* Own a COPY of the path: callers may pass a reused stack/heap buffer (e.g.
     * an app's icon field), so caching the pointer would alias distinct icons. */
    static struct { char path[128]; int used; uint32_t *px, w, h; } cache[EM_RES_MAX];
    if (!path) return 0;
    for (int i = 0; i < EM_RES_MAX; i++)
        if (cache[i].used && strcmp(cache[i].path, path) == 0) {
            if (out_w) *out_w = cache[i].w;
            if (out_h) *out_h = cache[i].h;
            return cache[i].px;
        }
    if (!g_res_load) return 0;
    size_t len = 0;
    uint8_t *d = g_res_load(path, &len);
    if (!d || len < 16 || d[0] != 'P') return 0;
    size_t o = 0; uint32_t w = 0, h = 0, depth = 3;
    if (d[1] == '6') {
        o = 2; uint32_t vals[3] = {0,0,0}; int nv = 0;
        while (o < len && nv < 3) {                    /* width height maxval */
            while (o < len && (d[o]==' '||d[o]=='\n'||d[o]=='\r'||d[o]=='\t')) o++;
            if (o < len && d[o] == '#') { while (o < len && d[o] != '\n') o++; continue; }
            uint32_t v = 0; int any = 0;
            while (o < len && d[o] >= '0' && d[o] <= '9') { v = v*10 + (d[o]-'0'); o++; any = 1; }
            if (!any) return 0;
            vals[nv++] = v;
        }
        o++;                                           /* whitespace after maxval */
        w = vals[0]; h = vals[1];
        if (vals[2] == 0) return 0;
    } else if (d[1] == '7') {
        /* PAM headers are textual and small. Copy only the header so string
         * parsing never walks into the binary (often NUL-filled) pixel data. */
        size_t hn = len < 255 ? len : 255;
        char hdr[256];
        memcpy(hdr, d, hn); hdr[hn] = 0;
        char *end = strstr(hdr, "ENDHDR\n");
        char *pw = strstr(hdr, "WIDTH ");
        char *ph = strstr(hdr, "HEIGHT ");
        char *pd = strstr(hdr, "DEPTH ");
        char *pm = strstr(hdr, "MAXVAL ");
        if (!end || !pw || !ph || !pd || !pm) return 0;
        w = (uint32_t)strtoul(pw + 6, 0, 10);
        h = (uint32_t)strtoul(ph + 7, 0, 10);
        depth = (uint32_t)strtoul(pd + 6, 0, 10);
        if (strtoul(pm + 7, 0, 10) != 255 || (depth != 3 && depth != 4)) return 0;
        o = (size_t)(end - hdr) + 7;
    } else {
        return 0;
    }
    if (!w || !h || o + (size_t)w*h*depth > len) return 0;
    /* Claim the cache slot BEFORE decoding: with no free slot the old code
     * still malloc'd, returned an uncached buffer, and did it again on the
     * NEXT frame -- an unbounded per-frame leak once the desktop had more
     * distinct images than slots. Refusing to draw is the honest failure. */
    int slot = -1;
    for (int i = 0; i < EM_RES_MAX; i++) if (!cache[i].used) { slot = i; break; }
    if (slot < 0) return 0;
    uint32_t *px = (uint32_t *)malloc((size_t)w*h*4);
    if (!px) return 0;
    for (size_t i = 0; i < (size_t)w*h; i++) {
        uint8_t r = d[o+i*depth], g = d[o+i*depth+1], b = d[o+i*depth+2];
        uint8_t a = depth == 4 ? d[o+i*depth+3] : 255;
        uint8_t pr = (uint8_t)(((uint32_t)r * a) / 255);
        uint8_t pg = (uint8_t)(((uint32_t)g * a) / 255);
        uint8_t pb = (uint8_t)(((uint32_t)b * a) / 255);
        px[i] = ((uint32_t)a<<24) | ((uint32_t)pr<<16) | ((uint32_t)pg<<8) | pb;
    }
    snprintf(cache[slot].path, sizeof cache[slot].path, "%s", path);
    cache[slot].used = 1; cache[slot].px = px; cache[slot].w = w; cache[slot].h = h;
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    return px;
}

/* ---- .eic multi-resolution icons (docs/ICONS.md) ------------------------ */

#define EM_ICON_MAX 24

static uint32_t eic_rd16(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static uint32_t eic_rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Resolve an icon at the size actually being drawn.
 *
 * A .eic holds the same icon at several sizes; we hand back the level that
 * best fits `want_px` so the renderer blits it 1:1 instead of resampling one
 * oversized master down to whatever the widget asked for. The pixels point
 * straight INTO the cached file image -- .eic stores premultiplied BGRA, the
 * exact layout the renderer consumes, so there is no decode and no second
 * allocation. Anything that isn't a .eic falls through to the .ppm/.pam
 * decoder, so existing art keeps working. */
const uint32_t *em_image_at(const char *path, uint32_t want_px,
                            uint32_t *out_w, uint32_t *out_h) {
    static struct { char path[128]; int used; uint8_t *file; size_t len; } cache[EM_ICON_MAX];
    if (!path) return 0;
    size_t n = strlen(path);
    if (n < 4 || strcmp(path + n - 4, ".eic") != 0)
        return em_image(path, out_w, out_h);

    uint8_t *f = 0; size_t len = 0;
    int slot = -1;
    for (int i = 0; i < EM_ICON_MAX; i++) {
        if (cache[i].used && strcmp(cache[i].path, path) == 0) {
            f = cache[i].file; len = cache[i].len; break;
        }
        if (slot < 0 && !cache[i].used) slot = i;
    }
    if (!f) {
        if (slot < 0 || !g_res_load) return 0;
        f = g_res_load(path, &len);
        if (!f || len < 32) return 0;
        snprintf(cache[slot].path, sizeof cache[slot].path, "%s", path);
        cache[slot].used = 1; cache[slot].file = f; cache[slot].len = len;
    }
    if (f[0] != 'E' || f[1] != 'I' || f[2] != 'C' || f[3] != 'O') return 0;
    uint32_t nlev = eic_rd16(f + 6);
    if (!nlev || 32 + (size_t)nlev * 16 > len) return 0;

    /* Smallest level that still covers the request: shrinking a bigger level a
     * little stays sharp, stretching a smaller one never does. */
    const uint8_t *best = 0;
    for (uint32_t i = 0; i < nlev; i++) {
        const uint8_t *e = f + 32 + (size_t)i * 16;
        best = e;                                  /* table is ascending */
        if (eic_rd16(e) >= want_px) break;         /* first that covers it */
    }
    if (!best) return 0;

    uint32_t w = eic_rd16(best), h = eic_rd16(best + 2);
    uint32_t off = eic_rd32(best + 8), nb = eic_rd32(best + 12);
    if (!w || !h || nb != w * h * 4 || (size_t)off + nb > len || (off & 3)) return 0;

    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    return (const uint32_t *)(const void *)(f + off);
}

void em_image_view(const char *path, EmProps p) {
    em_flush();
    uint32_t w = 0, h = 0;
    const uint32_t *px = em_image(path, &w, &h);
    if (!px) {   /* missing resource: a quiet placeholder box, not a crash */
        ui_box_begin(0);
        ui_set_paint(solid(TH->surface_alt));
        ui_set_corner_radius(TH->radius_md);
        ui_set_size(sz_grow(), sz_fixed(p.height > 0 ? p.height : 64));
        ui_box_end();
        return;
    }
    ui_image((uint64_t)(uintptr_t)px, px, w, h, p.height > 0 ? p.height : (float)h);
}

bool em_image_button_key(const char *path, float size, uint64_t key) {
    em_flush();
    uint32_t w = 0, h = 0;
    /* The glyph is drawn into size-8 (the padding below), so ask for the level
     * that matches THAT -- asking for `size` would fetch one level too large
     * and reintroduce the downscale this format exists to avoid. */
    float inner = size - 8;
    const uint32_t *px = em_image_at(path, inner > 1 ? (uint32_t)inner : 1, &w, &h);
    if (!px) return false;
    ui_begin_vstack(key ? key : (uint64_t)(uintptr_t)path);
    struct instance_handle self = ui_open();
    bool hov = ui_is_hovered(), pressed = ui_is_pressed();
    ui_set_size(sz_fixed(size), sz_fixed(size));
    ui_set_padding(4, 4, 4, 4);
    ui_set_align(ALIGN_CENTER);
    ui_set_justify(JUSTIFY_CENTER);
    ui_set_corner_radius(12);
    /* ALWAYS set the paint: a hover highlight set only on hover is retained and
     * would stay lit after the pointer leaves. Transparent when not hovered. */
    ui_set_paint(hov ? solid(shade(TH->surface_alt, pressed ? 0.86f : 1.12f))
                     : solid((Color){0, 0, 0, 0}));
    /* The leaf's key must be STABLE, and the pixel pointer is not: it names
     * the mip level, and an icon that magnifies swaps levels as it swells. A
     * key that changes destroys and recreates the instance -- and a press edge
     * captures LAST frame's instance, so a click landing on the same frame as
     * a level flip held a handle to an instance the build then threw away.
     * ui_is_active() walked up from a dead handle and found nothing.
     *
     * That was the dock: hover engages the magnifier, the magnifier animates
     * levels, and clicks died exactly while it animated -- which is exactly
     * when a person clicks. The launcher grid never magnifies, so it never
     * missed. Locked by declare-test T8. The leaf is the box's only image
     * child, so any nonzero constant is unique here. */
    ui_image_sized(0x1CA7, px, w, h, size - 8, size - 8);
    ui_end_stack();
    return ui_consume_click(self);
}

bool em_image_button(const char *path, float size) {
    return em_image_button_key(path, size, (uint64_t)(uintptr_t)path);
}

/* An icon button drawn from real art but coloured by the THEME: the image is
 * used as a stencil, so it sits beside glyph-based controls (which take their
 * colour from the palette) without looking like a foreign object, and it
 * follows a theme change instead of staying whatever colour it was authored. */
bool em_image_button_tinted(const char *path, float size, Color tint) {
    em_flush();
    uint32_t w = 0, h = 0;
    float inner = size - 8;
    const uint32_t *px = em_image_at(path, inner > 1 ? (uint32_t)inner : 1, &w, &h);
    if (!px) return false;
    ui_begin_vstack((uint64_t)(uintptr_t)path ^ 0x71E70000ULL);
    struct instance_handle self = ui_open();
    bool hov = ui_is_hovered(), pressed = ui_is_pressed();
    ui_set_size(sz_fixed(size), sz_fixed(size));
    ui_set_padding(4, 4, 4, 4);
    ui_set_align(ALIGN_CENTER);
    ui_set_justify(JUSTIFY_CENTER);
    ui_set_corner_radius(10);
    ui_set_paint(hov ? solid(shade(TH->surface_alt, pressed ? 0.86f : 1.12f))
                     : solid((Color){0, 0, 0, 0}));
    /* key on the tint too: the same pixels drawn in a new colour must not be
     * mistaken for an unchanged node and skipped by the dirty tracker */
    uint64_t k = (uint64_t)(uintptr_t)px
               ^ ((uint64_t)(uint32_t)(tint.r * 255.0f) << 16)
               ^ ((uint64_t)(uint32_t)(tint.g * 255.0f) << 8)
               ^ ((uint64_t)(uint32_t)(tint.b * 255.0f));
    ui_image_sized_tinted(k, px, w, h, inner, inner, tint);
    ui_end_stack();
    return ui_consume_click(self);
}

void em_background_image(const char *path) {
    em_flush();
    uint32_t w = 0, h = 0;
    const uint32_t *px = em_image(path, &w, &h);
    if (px) ui_image_fill((uint64_t)(uintptr_t)px, px, w, h);
}

void em_theme_use(EmTheme t) { ui_theme_use_dark(t != Light); }

/* ======================================================================= */
/* EmUI V6 -- menus (menu bar, dropdown menus, context menus)              */
/* ======================================================================= */

/* One menu is open at a time, keyed by the Menu's label pointer. The open
 * menu's items float in an out-of-flow overlay anchored where the button was
 * clicked (ui_pointer_pos at open time -- no layout query needed). */
/* the window's content size, owned HERE so em.c never depends on the ring-3
 * app runtime (host tools set it directly) */
static float g_vp_w, g_vp_h;
void  em_set_viewport(float w, float h) { g_vp_w = w; g_vp_h = h; }

/* THE input feed. Every host loop routes its pointer state through here --
 * em_app_run and the desktop's own loop alike -- because "what the toolkit
 * needs told each frame" is a fact about the toolkit, not about any one loop.
 * Two hand-written copies drifted twice: the desktop silently lacked
 * right-button delivery (context menus dead) and, before that, viewport
 * mirroring. One function, one truth. */
void em_feed_pointer(float x, float y, int left_down, int right_down,
                     int wheel, int focused) {
    if (focused) {
        ui_pointer(x, y, left_down != 0);
        em_feed_right_button(x, y, right_down != 0);
        if (wheel) ui_wheel((float)wheel);
    } else {
        ui_pointer(-100.0f, -100.0f, false);
        em_feed_right_button(0, 0, false);
    }
}
float em_viewport_width(void)  { return g_vp_w; }
float em_viewport_height(void) { return g_vp_h; }

static const void *g_menu_open;          /* label ptr of the open Menu, or NULL */
static float g_menu_ax, g_menu_ay;       /* anchor (window-content coords) */
static int   g_menu_cur_open;            /* is the Menu being emitted right now open? */
static int   g_menu_item_chosen;         /* a MenuItem was clicked this frame */
static struct instance_handle g_menu_scrim;

/* right-click edge, fed by em_feed_right_button (em_app_run). */
static int   g_rclick_pending;
static float g_rclick_x, g_rclick_y;
static int   g_rbtn_prev;

void em_feed_right_button(float x, float y, bool down) {
    if (down && !g_rbtn_prev) { g_rclick_pending = 1; g_rclick_x = x; g_rclick_y = y; }
    g_rbtn_prev = down ? 1 : 0;
}
int em_right_clicked(float *ox, float *oy) {
    if (!g_rclick_pending) return 0;
    g_rclick_pending = 0;
    if (ox) *ox = g_rclick_x;
    if (oy) *oy = g_rclick_y;
    return 1;
}

void em_menubar_(EmProps p) {
    em_flush();
    const struct ui_theme *t = TH;
    ui_begin_hstack(0);
    /* No surface of its own, and no growing. A menu bar is a ROW OF MENUS --
     * whatever hosts it owns the material and decides how wide the row is. It
     * used to paint surface_alt and sz_grow(), so inside the system bar it
     * claimed half the width (an even split with the trailing Spacer) and
     * filled it with an opaque slab. That was invisible while the bar had its
     * own dark fill; the moment the bar went transparent it became a grey
     * rectangle over the left half of the screen. A caller that does want a
     * surface still passes .background. */
    ui_set_border(0, t->border);
    ui_set_padding(t->sp1, t->sp2, t->sp1, t->sp2);
    ui_set_align(ALIGN_CENTER);
    ui_set_spacing(t->sp1);
    em_apply_box(p);            /* paints only if the caller asked for a fill */
}
void em_menubar_end_(void) { em_flush(); ui_end_stack(); }

/* One container per menu, SAME EXPLICIT KEY in both states.
 *
 * Open and closed used to emit structurally different children (a keyed
 * overlay vs an anonymous hidden box), so toggling a menu changed the child
 * LIST SHAPE. Positional (key-0) siblings then slid one slot in the
 * reconciler, and because reused instances RETAIN scene props a widget
 * doesn't explicitly set, the drift smeared stale 0x0 sizes and clip flags
 * across the neighbouring menus -- squashed buttons, item lists escaping
 * their clipped hidden box. The corruption was latent for as long as the
 * menus existed; z-layer paint deferral merely changed who covered whom and
 * made it visible.
 *
 * With one keyed container per menu the list shape never changes, and every
 * property that DIFFERS between the two states is set explicitly in BOTH
 * branches, so a state flip can never inherit the other state's leftovers. */
static void em_menu_panel_open(uint64_t key, float ax, float ay) {
    const struct ui_theme *t = TH;
    ui_begin_vstack(key);                 /* the out-of-flow overlay layer */
    g_menu_scrim = ui_open();
    ui_set_overlay(true);
    ui_set_layer(1);                      /* elevated: paints above + hits above the flow */
    ui_set_clip_children(false);
    ui_set_paint(solid((Color){0,0,0,0}));        /* transparent: catches outside clicks, no dim */
    /* WINDOW-sized, not parent-sized: the dismiss surface must cover the
     * window, and this overlay's parent is a 28px menu strip. Overlays honour
     * an explicit fixed size (layout.c) precisely for this. */
    ui_set_size(sz_fixed(em_viewport_width()), sz_fixed(em_viewport_height()));
    /* The panel's offset below is measured from THIS overlay's origin, but
     * callers pass WINDOW coordinates -- em_right_clicked reports them, and so
     * does any hit test. Emitted inside a pane, the two disagreed by that
     * pane's origin and the menu opened down and right of the pointer.
     * Subtracting the overlay's own resolved origin makes the anchor
     * window-absolute wherever the menu is emitted. One frame stale, which is
     * invisible: the menu is not on screen the frame it opens. */
    float ovx = 0, ovy = 0, ovw, ovh;
    if (!ui_open_rect(&ovx, &ovy, &ovw, &ovh)) { ovx = 0; ovy = 0; }
    /* drop-in motion: the panel fades in while settling down its last 8px.
     * Keyed on WHICH menu is open, so switching between menus re-plays it;
     * with no clock (host renders) it snaps to settled, like em_nav. */
    static uint64_t s_anim_key, s_anim_t0;
    uint64_t mnow = em_now_ms();
    if (s_anim_key != key) { s_anim_key = key; s_anim_t0 = mnow; }
    float mt = 1.0f;
    if (mnow && s_anim_t0) {
        float e = (float)(mnow - s_anim_t0) / 150.0f;
        mt = e < 0 ? 0 : e > 1.0f ? 1.0f : e;
        float inv = 1.0f - mt;
        mt = 1.0f - inv * inv * inv;
        if (mt < 1.0f) em_request_frame();
    }
    ui_begin_vstack(1);                   /* the menu panel -- frosted glass */
    ui_set_opacity(mt);
    ui_set_offset(ax - ovx, ay - ovy - 8.0f * (1.0f - mt));
    ui_set_corner_radius(t->radius_md);
    ui_set_shadow(true, t->shadow_lg.dx, t->shadow_lg.dy, t->shadow_lg.blur, t->shadow_lg.color);
    em_glass_apply(12.0f);                 /* blur behind + tint + edge highlight */
    ui_set_clip_children(true);
    ui_set_align(ALIGN_STRETCH);
    ui_set_spacing(0);
    ui_set_padding(t->sp1, t->sp1, t->sp1, t->sp1);
    ui_set_size(sz_fixed(200), sz_intrinsic());
}
/* returns 1 if the transparent scrim (outside the panel) was clicked */
static int em_menu_panel_close(void) {
    ui_end_stack();                       /* panel */
    int scrim_hit = ui_consume_click(g_menu_scrim);
    ui_end_stack();                       /* overlay */
    return scrim_hit;
}
/* the hidden container a CLOSED menu's items emit into (built but invisible).
 * SAME key as the open overlay -- one instance per menu, two dressings -- and
 * every open-state prop explicitly reversed (overlay, layer, paint, offset). */
static void em_menu_hidden_open(uint64_t key) {
    ui_begin_vstack(key);
    ui_set_overlay(false);
    ui_set_layer(0);
    ui_set_paint(solid((Color){0,0,0,0}));
    ui_set_size(sz_fixed(0), sz_fixed(0));
    ui_set_clip_children(true);
    ui_begin_vstack(1);                   /* mirror the panel level */
    ui_set_offset(0, 0);
    ui_set_shadow(false, 0, 0, 0, (Color){0,0,0,0});
    ui_set_backdrop_blur(false, 0);
    ui_set_paint(solid((Color){0,0,0,0}));
    ui_set_clip_children(true);
    ui_set_size(sz_fixed(0), sz_fixed(0));
}

void em_menu_(const char *label, EmProps p) {
    em_flush();
    const struct ui_theme *t = TH;
    int is_open = (g_menu_open == (const void *)label);
    ui_begin_hstack(0);                   /* the menu button in the bar */
    struct instance_handle btn = ui_open();
    bool hov = ui_is_hovered(), pressed = ui_is_pressed();
    ui_set_paint(solid(is_open ? t->accent_soft : pressed ? shade(t->surface_alt, 0.94f)
                                : hov ? t->surface_alt : t->surface_alt));
    if (!is_open && !hov && !pressed) ui_set_paint(solid((Color){0,0,0,0}));   /* flat until hovered */
    ui_set_corner_radius(t->radius_sm);
    ui_set_padding(t->sp1, t->sp3, t->sp1, t->sp3);
    ui_set_align(ALIGN_CENTER);
    /* .font honoured: a menu bar bolds the ACTIVE APP's name and leaves the
     * rest regular -- that weight difference is what tells you which
     * application the menus belong to. */
    { EmProps lp = { .font = p.font ? p.font : Body,
                     .color = is_open ? t->accent
                            : (p.color.a > 0 ? p.color : t->text) };
      em_text_impl(label, lp); }
    ui_end_stack();
    if (ui_consume_click(btn)) {
        if (is_open) g_menu_open = 0;
        else { g_menu_open = (const void *)label; float px, py; ui_pointer_pos(&px, &py);
               g_menu_ax = px; g_menu_ay = py + 8.0f; }
        g_em_epoch++;
        is_open = (g_menu_open == (const void *)label);
    }
    g_menu_cur_open = is_open;
    if (is_open) em_menu_panel_open((uint64_t)(uintptr_t)label, g_menu_ax, g_menu_ay);
    else         em_menu_hidden_open((uint64_t)(uintptr_t)label);
}
void em_menu_end_(void) {
    em_flush();
    if (g_menu_cur_open) {
        if (em_menu_panel_close()) { g_menu_open = 0; g_em_epoch++; }
    }
    else {
        ui_end_stack();                       /* inner mirror */
        /* Swallow clicks resolved against LAST frame's stale scrim. Hit tests
         * run on the last-built tree, and the frame that DISMISSES a menu
         * still built the open tree -- so a click in that one-frame window
         * lands on a scrim whose menu is already closed. Same keyed instance
         * in both states, so consuming it here is exact, and the stray click
         * dies instead of leaking into whatever sat beneath the scrim. */
        struct instance_handle box = ui_open();
        ui_end_stack();                       /* hidden box */
        (void)ui_consume_click(box);
    }
}

/* Is any MenuBar dropdown currently open? The app runtime uses this to grow a
 * thin translucent menu-bar window tall enough to show the dropdown, then
 * shrink it back on close -- so the bar stays a thin strip when idle. */
int em_menu_any_open(void) { return g_menu_open != 0; }

bool em_menu_item(const char *label, const char *shortcut) {
    em_flush();
    const struct ui_theme *t = TH;
    ui_begin_hstack(0);
    struct instance_handle self = ui_open();
    bool hov = ui_is_hovered(), pressed = ui_is_pressed();
    ui_set_paint(solid(pressed ? shade(t->accent_soft, 0.94f) : hov ? t->accent_soft : t->surface));
    ui_set_corner_radius(t->radius_sm);
    ui_set_padding(t->sp2, t->sp3, t->sp2, t->sp3);
    ui_set_align(ALIGN_CENTER);
    ui_set_spacing(t->sp3);
    ui_set_size(sz_grow(), sz_intrinsic());
    { EmProps lp = { .font = Body, .color = hov ? t->accent : t->text }; em_text_impl(label, lp); }
    if (shortcut && shortcut[0]) {
        ui_spacer();
        EmProps sp = { .font = Caption, .color = t->text_tertiary }; em_text_impl(shortcut, sp);
    }
    ui_end_stack();
    if (ui_consume_click(self)) { g_menu_open = 0; g_menu_item_chosen = 1; g_em_epoch++; return true; }
    return false;
}

void em_menu_separator(void) {
    em_flush();
    const struct ui_theme *t = TH;
    ui_box_begin(0);
    ui_set_paint(solid(t->border));
    ui_set_size(sz_grow(), sz_fixed(1));
    ui_box_end();
}

/* ContextMenu: a popover at (x,y) while *open; item-click or outside-click
 * clears *open. Same panel machinery as Menu. g_menu_item_chosen (set by
 * MenuItem on click) is the "an item was picked" signal both Menu and
 * ContextMenu use to dismiss. */
static int   g_ctx_cur_open;
static bool *g_ctx_open_flag;
void em_context_menu_(bool *open, float x, float y, EmProps p) {
    (void)p;
    em_flush();
    g_ctx_cur_open = (open && *open) ? 1 : 0;
    g_ctx_open_flag = open;
    g_menu_item_chosen = 0;               /* fresh: only THIS frame's item clicks count */
    if (g_ctx_cur_open) em_menu_panel_open((uint64_t)(uintptr_t)open, x, y);
    else                em_menu_hidden_open((uint64_t)(uintptr_t)open);
}
void em_context_menu_end_(void) {
    em_flush();
    if (g_ctx_cur_open) {
        int scrim_hit = em_menu_panel_close();
        if ((scrim_hit || g_menu_item_chosen) && g_ctx_open_flag) {
            *g_ctx_open_flag = false;
            g_em_epoch++;
        }
    } else {
        ui_end_stack(); ui_end_stack();   /* inner mirror + hidden box */
    }
}

/* ======================================================================= */
/* EmUI V7 -- multi-line text editor                                       */
/* ======================================================================= */

/* Private key codes the kernel keyboard driver emits for the extended nav keys
 * (kept in lockstep with kernel/drivers/input/keyboard.c EK_* and embk.h
 * EMBK_KEY_*). Defined here so em.c stays SDK-free and host-testable. */
#define EMK_LEFT  0x11
#define EMK_RIGHT 0x12
#define EMK_UP    0x13
#define EMK_DOWN  0x14
#define EMK_HOME  0x02
#define EMK_END   0x05
#define EMK_DEL   0x7F

/* ---- editing primitives on a NUL-terminated buffer + byte cursor -------- */
static void te_insert(char *buf, size_t cap, int *len, int *cur, char c) {
    if (*len + 1 >= (int)cap) return;
    memmove(buf + *cur + 1, buf + *cur, (size_t)(*len - *cur + 1));  /* incl. NUL */
    buf[*cur] = c;
    (*cur)++; (*len)++;
}
static void te_backspace(char *buf, int *len, int *cur) {
    if (*cur <= 0) return;
    memmove(buf + *cur - 1, buf + *cur, (size_t)(*len - *cur + 1));
    (*cur)--; (*len)--;
}
static void te_delete(char *buf, int *len, int *cur) {
    if (*cur >= *len) return;
    memmove(buf + *cur, buf + *cur + 1, (size_t)(*len - *cur));
    (*len)--;
}
static int te_line_start(const char *buf, int cur) {
    int i = cur;
    while (i > 0 && buf[i - 1] != '\n') i--;
    return i;
}
static int te_line_end(const char *buf, int len, int cur) {
    int i = cur;
    while (i < len && buf[i] != '\n') i++;
    return i;
}

bool em_text_editor(char *buf, size_t cap, int *cursor, float height) {
    em_flush();                       /* emit any pending staged leaf first */
    const struct ui_theme *t = TH;
    int len = (int)strlen(buf);
    int cur = cursor ? *cursor : 0;
    if (cur > len) cur = len;
    if (cur < 0) cur = 0;

    ui_begin_vstack(0);
    struct instance_handle self = ui_open();
    if (ui_consume_click(self)) ui_request_focus(self);
    bool focused = ui_has_focus(self);

    if (focused) {
        char in[64];
        int n = ui_input_take(in, (int)sizeof in);
        for (int i = 0; i < n; i++) {
            unsigned char c = (unsigned char)in[i];
            switch (c) {
                case '\b':      te_backspace(buf, &len, &cur); break;
                case EMK_DEL:   te_delete(buf, &len, &cur); break;
                case '\n': case '\r': te_insert(buf, cap, &len, &cur, '\n'); break;
                case '\t':      te_insert(buf, cap, &len, &cur, ' ');
                                te_insert(buf, cap, &len, &cur, ' '); break;
                case EMK_LEFT:  if (cur > 0) cur--; break;
                case EMK_RIGHT: if (cur < len) cur++; break;
                case EMK_HOME:  cur = te_line_start(buf, cur); break;
                case EMK_END:   cur = te_line_end(buf, len, cur); break;
                case EMK_UP: {
                    int ls = te_line_start(buf, cur), col = cur - ls;
                    if (ls > 0) { int pls = te_line_start(buf, ls - 1), ple = ls - 1;
                                  cur = pls + (col < ple - pls ? col : ple - pls); }
                    break;
                }
                case EMK_DOWN: {
                    int ls = te_line_start(buf, cur), col = cur - ls;
                    int le = te_line_end(buf, len, cur);
                    if (le < len) { int nls = le + 1, nle = te_line_end(buf, len, nls);
                                    cur = nls + (col < nle - nls ? col : nle - nls); }
                    break;
                }
                default:
                    if (c >= 32 && c < 127) te_insert(buf, cap, &len, &cur, (char)c);
                    break;
            }
        }
        if (cursor) *cursor = cur;
        em_request_frame();   /* keep the loop live while typing */
    }

    /* the editor surface */
    ui_set_paint(solid(t->surface_alt));
    ui_set_corner_radius(t->radius_md);
    ui_set_border(focused ? 1.5f : 1.0f, focused ? t->accent : t->border);
    ui_set_padding(t->sp2, t->sp3, t->sp2, t->sp3);
    ui_set_clip_children(true);
    ui_set_align(ALIGN_STRETCH);
    ui_set_size(sz_grow(), sz_fixed(height));

    /* auto-scroll: shift the lines up so the cursor line stays visible */
    float line_h = t->text_body + 5.0f;
    int cur_line = 0; for (int i = 0; i < cur; i++) if (buf[i] == '\n') cur_line++;
    float caret_y = (cur_line + 1) * line_h;
    float view_h = height - 2 * t->sp2;
    float scroll = caret_y > view_h ? caret_y - view_h : 0.0f;

    ui_begin_vstack(0);
    ui_set_align(ALIGN_STRETCH);
    ui_set_spacing(5);
    ui_set_offset(0, -scroll);

    if (len == 0 && !focused) {
        EmProps pp = { .font = Body, .color = t->text_tertiary };
        em_text_impl("Type here...", pp);
    } else {
        /* render each line; the cursor line splits around a caret box */
        int line = 0, i = 0;
        while (i <= len) {
            int e = i; while (e < len && buf[e] != '\n') e++;
            int is_cur = (cur >= i && cur <= e);
            char tmp[512];
            if (is_cur && focused) {
                ui_begin_hstack((uint64_t)(line + 1));
                ui_set_align(ALIGN_CENTER);
                ui_set_spacing(0);
                int bn = cur - i; if (bn > (int)sizeof tmp - 1) bn = sizeof tmp - 1;
                memcpy(tmp, buf + i, (size_t)bn); tmp[bn] = 0;
                /* skip empty text nodes: an empty string in an ALIGN_CENTER hstack
                 * collapses the row's layout (same reason the plain-line path below
                 * substitutes a space). Cursor at line start -> no before-text;
                 * cursor at line end -> no after-text; the caret always draws. */
                if (bn > 0) { EmProps tp = { .font = Body, .color = t->text }; em_text_impl(tmp, tp); }
                ui_box_begin(0);              /* caret */
                ui_set_paint(solid(t->accent));
                ui_set_size(sz_fixed(2), sz_fixed(t->text_body));
                ui_box_end();
                int an = e - cur; if (an > (int)sizeof tmp - 1) an = sizeof tmp - 1;
                memcpy(tmp, buf + cur, (size_t)an); tmp[an] = 0;
                if (an > 0) { EmProps tp = { .font = Body, .color = t->text }; em_text_impl(tmp, tp); }
                ui_spacer();
                ui_end_stack();
            } else {
                int ln = e - i; if (ln > (int)sizeof tmp - 1) ln = sizeof tmp - 1;
                memcpy(tmp, buf + i, (size_t)ln); tmp[ln] = 0;
                ui_begin_hstack((uint64_t)(line + 1));
                ui_set_align(ALIGN_CENTER);
                { EmProps tp = { .font = Body, .color = t->text }; em_text_impl(tmp[0] ? tmp : " ", tp); }
                ui_spacer();
                ui_end_stack();
            }
            line++;
            i = e + 1;
            if (e == len) break;
        }
    }
    ui_end_stack();     /* lines */
    ui_end_stack();     /* editor surface */
    return focused;
}

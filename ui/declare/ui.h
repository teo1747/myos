#ifndef __EMBLINK_UI_UI_H__
#define __EMBLINK_UI_UI_H__

/* ui/declare/ui.h -- EmbLink UI Piece 7: the declarative API an app author
 * writes against. Begin/end imperative scoping (Dear ImGui family); positional
 * identity by default, explicit keys for dynamic lists. */

#include "instance.h"

/* --- driver / frame loop glue --- */
void ui_init(struct scene_arena *sa, struct layout_arena *la);
struct instance_handle ui_root(void);
void ui_frame_begin(void);
void ui_frame_end(void);
void ui_run_layout(float W, float H);   /* Piece 5 arrange on the root */
void ui_set_font(uint32_t font_handle);
void ui_set_text_size(float px);

/* --- containers --- */
void ui_box_begin(uint64_t key);
void ui_box_end(void);
void ui_begin_vstack(uint64_t key);
void ui_begin_hstack(uint64_t key);
void ui_end_stack(void);

/* --- properties (apply to the currently open box) --- */
void ui_set_paint(struct paint p);
void ui_set_corner_radius(float r);
void ui_set_padding(float top, float right, float bottom, float left);
void ui_set_spacing(float s);
void ui_set_size(struct layout_size w, struct layout_size h);
void ui_set_size_bounds(float min_w, float max_w, float min_h, float max_h);  /* 0 = unset */
void ui_set_shadow(bool enabled, float dx, float dy, float blur, struct color color);
void ui_set_opacity(float opacity);   /* 0..1; wraps the subtree in a group */
void ui_set_offset(float x, float y);  /* post-layout translate (transitions/slides) */
void ui_set_backdrop_blur(bool enabled, float radius);
void ui_set_clip_children(bool clip);
void ui_set_overlay(bool on);   /* fill parent, out of flow (modal/popover layer) */
/* CSS top/right/bottom/left on the open box. `set` is a bitmask -- bit 0 top,
 * 1 right, 2 bottom, 3 left -- because 0 is an ordinary offset and has to be
 * distinguishable from unset. `relative` keeps the box IN flow and offsets it
 * afterwards, so its siblings never notice. */
void ui_set_insets(float top, float right, float bottom, float left,
                   unsigned set, bool relative);
void ui_set_layer(int l);       /* z-layer 0..3: elevated paints above + hits first */
void ui_set_border(float width, struct color color);
void ui_set_border_gradient(float width, const struct paint *paint);
void ui_set_text_gradient(const struct paint *paint);   /* one-shot: next ui_text() */
void ui_set_axis(enum layout_axis a);
void ui_set_wrap(bool wrap);   /* flex-wrap: overflowing children flow onto new lines */
void ui_set_grid(int cols, float col_gap, float row_gap);   /* container: 2D grid */
/* Explicit track sizes for that grid -- see layout.h. Pass n == 0 (or never
 * call it) to keep tracks sized from their content. */
void ui_set_grid_tracks(const unsigned char *mode, const float *val, int n);
void ui_set_grid_span(int span);                            /* child: columns to span */
void ui_set_justify(enum layout_justify j);
void ui_set_align(enum layout_align a);
void ui_set_text_color(struct color c);   /* colour for subsequent ui_text calls */
/* Background painted behind the NEXT ui_text's glyphs (one-shot, a==0 = none).
 * This is how a selection highlight is expressed: a property of the run, so it
 * lands wherever layout actually put the words. */
void ui_set_text_bg(struct color c);

/* --- leaves --- */
void ui_text(const char *fmt, ...);
void ui_image(uint64_t key, const void *pixels, uint32_t iw, uint32_t ih, float height_px);
void ui_image_sized(uint64_t key, const void *pixels, uint32_t iw, uint32_t ih,
                    float width_px, float height_px);
/* draws the image as a stencil filled with `tint` (its alpha = coverage), so a
 * single-colour icon follows the theme exactly as a glyph does */
void ui_image_sized_tinted(uint64_t key, const void *pixels, uint32_t iw, uint32_t ih,
                           float width_px, float height_px, struct color tint);
void ui_image_fill(uint64_t key, const void *pixels, uint32_t iw, uint32_t ih);
void ui_text_keyed(uint64_t key, const char *fmt, ...);
void ui_spacer(void);
bool ui_button(const char *label);
bool ui_button_keyed(uint64_t key, const char *label);

void ui_component(void (*fn)(void *props), const void *props, size_t props_size, uint64_t key);

/* --- input dispatch (§7): hit-test the retained tree, set clicked pulse --- */
void ui_dispatch_click(float px, float py);

/* Handle of the currently open box (for widget authors that need to read back
 * interaction state), and read+clear its one-frame click pulse. */
struct instance_handle ui_open(void);
bool ui_consume_click(struct instance_handle h);

/* Live pointer input for the event loop: feed the pointer each frame (coords
 * are surface-local), then widgets query hover/press for the currently open
 * box. A click fires on the press edge and is read via ui_consume_click. */
void ui_pointer(float x, float y, bool down);
void ui_pointer_pos(float *x, float *y);   /* live pointer position (surface-local) */
bool ui_is_hovered(void);   /* is the pointer over the currently open box? */
bool ui_is_pressed(void);   /* ...and is the button held down? */
bool ui_pointer_down(void); /* the raw button state, not scoped to a widget */
bool ui_is_active(void);    /* is the open box the drag owner (pointer capture)? */

/* World rect (last frame's arranged geometry) of an instance / the open box.
 * Widgets map the pointer into their own box with these (sliders, scrollables). */
bool ui_rect_of(struct instance_handle h, float *x, float *y, float *w, float *ht);
bool ui_open_rect(float *x, float *y, float *w, float *ht);

/* --- keyboard focus + typed input (text fields) --- */
void ui_input_char(int c);                        /* loop feeds each key byte here */
int  ui_input_take(char *dst, int max);           /* focused field drains queued chars */
void ui_request_focus(struct instance_handle h);
bool ui_has_focus(struct instance_handle h);
bool ui_any_focus(void);       /* is ANY text field focused? (app key hooks) */

/* --- scroll --- */
void  ui_set_scroll_offset(float dy);             /* shift open box's children up by dy */
bool  ui_open_content_extent(float *content_h, float *viewport_h);
void  ui_wheel(float dy);                         /* loop feeds this frame's wheel delta */
float ui_take_wheel(void);                        /* open box consumes wheel if hovered */

/* --- test/diagnostic --- */
uint32_t ui_debug_mutation_count(void);
struct instance_handle ui_first_child(struct instance_handle h);
struct instance_handle ui_next_sibling(struct instance_handle h);
struct node_handle     ui_scene_of(struct instance_handle h);

/* Did a tree this frame nest deeper than the reconciler can hold? Deeper
 * containers are CLAMPED to the deepest level that fits -- their children
 * attach one level up rather than corrupting the stack. Reported rather than
 * silent, because a page that trips it is laid out slightly wrong and that is
 * worth being able to see. */
int                    ui_depth_overflowed(void);

/* How many children the reconciler re-linked. Re-declaring an unchanged list
 * must relink NOTHING: the unlink walks the parent's child list to find a
 * predecessor, so doing it per child is quadratic in the number of siblings.
 * Exposed so that cost can be asserted on instead of timed. */
unsigned long          ui_relink_walks(void);
void                   ui_relink_walks_reset(void);
struct scene_arena    *ui_scene_arena(void);
/* Instance-pool telemetry. `overflow` counts allocations the pool REFUSED --
 * any non-zero value means views were silently dropped and whatever is on
 * screen is incomplete. */
uint32_t               ui_instance_overflow(void);
uint32_t               ui_instance_used(void);
/* Instance-pool telemetry. `overflow` counts allocations the pool REFUSED --
 * any non-zero value means views were silently dropped and whatever is on
 * screen is incomplete. */
struct layout_handle   ui_layout_of(struct instance_handle h);

#endif /* __EMBLINK_UI_UI_H__ */

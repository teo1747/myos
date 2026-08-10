/* ports/netsurf/frontend/emblink.h -- what the frontend's files agree about.
 *
 * NetSurf hands a frontend a set of TABLES to fill in: one for windows, one
 * for fetching, one for plotting, and so on. Each of those is a separate
 * concern and lives in its own file here; this header is the only thing they
 * share, which keeps the seams where the tables already put them.
 *
 * The frontend's job in one sentence: give the core somewhere to draw, tell it
 * how wide text is, and pump its clock.
 */
#ifndef EMBLINK_FRONTEND_H
#define EMBLINK_FRONTEND_H

#include <stdbool.h>
#include <stdint.h>

#include "utils/errors.h"
#include "netsurf/types.h"

struct gui_window_table;
struct gui_misc_table;
struct gui_fetch_table;
struct gui_layout_table;
struct gui_bitmap_table;
struct plotter_table;

/* Each file exports exactly one table. */
extern struct gui_window_table *emblink_window_table;
extern struct gui_misc_table   *emblink_misc_table;
extern struct gui_fetch_table  *emblink_fetch_table;
extern struct gui_layout_table *emblink_layout_table;
extern struct gui_bitmap_table *emblink_bitmap_table;
extern const struct plotter_table emblink_plotters;

/* THE SURFACE the plotters draw into: 32-bit pixels, one buffer, no windowing.
 * A window on this OS is a shared framebuffer the compositor already knows how
 * to present (see kernel/gfx/compositor.c and the win syscalls), so the same
 * struct serves both the headless render and the windowed one -- the only
 * difference is who owns the memory. */
struct emblink_surface {
    uint32_t *px;            /* xRGB, row-major, `stride` pixels per row */
    int width, height, stride;
    /* the clip the core last asked for; every plot is bounded by it */
    int cx0, cy0, cx1, cy1;
};

/* The surface the plotters are currently drawing into. Set before the core is
 * asked to redraw and never while it is: the plotter table has no context
 * parameter of its own in this version of the API. */
extern struct emblink_surface *emblink_target;
extern int g_textdump;   /* TEXTDUMP=1: print every drawn run (see plot.c) */

void emblink_surface_clip(struct emblink_surface *s, int x0, int y0, int x1, int y1);
void emblink_surface_fill(struct emblink_surface *s, int x0, int y0, int x1, int y1,
                          uint32_t argb);

/* --- the viewport --------------------------------------------------------
 * window.c owns the gui_window; these are the handles main.c needs to reach
 * into it. Declared here rather than re-declared at the use site, because two
 * copies of a prototype are one rename away from disagreeing silently. */
struct gui_window;
struct browser_window;
struct gui_window     *emblink_window_get(void);
struct emblink_surface *emblink_window_surface(struct gui_window *gw);
struct browser_window *emblink_window_bw(struct gui_window *gw);
const char            *emblink_window_title(struct gui_window *gw);
void                   emblink_window_set_size(int w, int h);
bool                   emblink_window_settled(struct gui_window *gw);
bool                   emblink_window_resize(struct gui_window *gw, int w, int h);

/* --- the clock ------------------------------------------------------------
 * NetSurf schedules callbacks in milliseconds and expects someone to run them.
 * There is no event loop library here, so the frontend keeps the list and the
 * main loop drains it. */
nserror emblink_schedule(int tms, void (*cb)(void *p), void *p);
int  emblink_schedule_run(void);     /* run what is due; returns ms until next, -1 = none */
void emblink_schedule_finalise(void);

/* --- text -----------------------------------------------------------------
 * Measuring text is the one thing the core cannot do without the frontend, and
 * it asks constantly: every line break is a width query. Implemented over the
 * OS's own font metrics. */
struct plot_font_style;
int emblink_text_advance(const struct plot_font_style *fstyle,
                         const char *utf8, size_t len);
/* `y` is the BASELINE, which is what the core passes and what a glyph's
 * bearing is measured from. */
void emblink_text_draw(struct emblink_surface *s, int x, int y,
                       const struct plot_font_style *fstyle,
                       const char *utf8, size_t len);

#endif /* EMBLINK_FRONTEND_H */

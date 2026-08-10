/* ports/netsurf/frontend/app.c -- NetSurf as a window on the desktop.
 *
 * Everything else in this frontend was written so that this file is the only
 * one that knows a screen exists. window.c owns a pixel buffer and an
 * invalidation rect and has no idea whether anyone is looking at it; plot.c
 * draws into a `struct emblink_surface` and does not care where it came from.
 * So becoming an application is: get the buffer from the compositor instead of
 * from malloc, and pump input.
 *
 * The window is ZERO-COPY. embk_win_create_shared maps the compositor's own
 * pixel pages into this process, so the surface IS the window -- a redraw
 * lands directly in what the screen scans out, and present() is a damage call
 * rather than a copy. That matters here more than in most apps: a browser
 * repaints a whole page, and a 1100x800 memcpy per frame under TCG is the kind
 * of cost this project has measured before (docs/PROJECT_STATUS.md, the 4946ms
 * frame).
 *
 * SCROLLING is done by redrawing at an offset rather than by moving pixels.
 * The core is asked to redraw with a negative y, which is what every NetSurf
 * frontend does and what makes position:fixed and sticky headers come out
 * right -- blitting the old image up and filling the gap would freeze them.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "utils/errors.h"
#include "utils/nsurl.h"
#include "netsurf/browser_window.h"
#include "netsurf/plotters.h"
#include "netsurf/mouse.h"
#include "netsurf/keypress.h"
#include "netsurf/types.h"

#include "emblink.h"
#include "embk.h"

/* The desktop is 800x600 by default (FB_W/FB_H in the root Makefile), and the
 * compositor refuses a window bigger than the screen -- which is where the
 * first attempt at this died, with a bare -22. Sized to fit under the top bar
 * with room for the dock, and overridable for a bigger framebuffer. */
#ifndef APP_W
#define APP_W 760
#endif
#ifndef APP_H
#define APP_H 500
#endif

struct app {
    int       win;
    uint32_t *px;
    int        w, h;
    int        scroll;          /* document pixels hidden above the viewport */
    int        doc_h;
    bool       dirty;
    uint32_t   last_buttons;
};

static struct app g_app;

/* The core's redraw, into the window's own pixels, at the current scroll. */
static void repaint(struct gui_window *gw)
{
    struct emblink_surface *s = emblink_window_surface(gw);
    struct redraw_context ctx = {
        .interactive = true,
        .background_images = true,
        .plot = &emblink_plotters,
    };
    struct rect clip = { 0, 0, g_app.w, g_app.h };

    /* White first: the core paints a document's own background, and whatever
     * it does not cover has to be the canvas rather than the last frame. */
    for (size_t i = 0; i < (size_t)g_app.w * g_app.h; i++) g_app.px[i] = 0x00FFFFFF;

    emblink_target = s;
    emblink_surface_clip(s, 0, 0, g_app.w, g_app.h);
    browser_window_redraw(emblink_window_bw(gw), 0, -g_app.scroll, &clip, &ctx);
    emblink_target = NULL;

    embk_win_present(g_app.win, g_app.px, (uint32_t)g_app.w, (uint32_t)g_app.h);
    g_app.dirty = false;
}

static void clamp_scroll(void)
{
    int max = g_app.doc_h - g_app.h;
    if (max < 0) max = 0;
    if (g_app.scroll > max) g_app.scroll = max;
    if (g_app.scroll < 0) g_app.scroll = 0;
}

/* Pointer and wheel. Coordinates arrive in WINDOW space and the core wants
 * DOCUMENT space, which differ by the scroll -- forgetting that is a browser
 * whose links stop working the moment you scroll. */
static void pump_pointer(struct gui_window *gw)
{
    struct embk_win_input in;
    if (embk_win_input(&in) != 0) return;
    if (!in.focused) return;

    struct browser_window *bw = emblink_window_bw(gw);
    int dx = in.x, dy = in.y + g_app.scroll;

    if (in.wheel != 0) {
        g_app.scroll -= in.wheel * 48;      /* three lines, roughly */
        clamp_scroll();
        g_app.dirty = true;
    }

    browser_window_mouse_track(bw, (browser_mouse_state)0, dx, dy);

    /* A CLICK IS AN EDGE, not a state. Sending PRESS_1 every frame the button
     * is down makes one click open a link as many times as the loop runs. */
    bool now = (in.buttons & 1) != 0;
    bool was = (g_app.last_buttons & 1) != 0;
    if (now && !was) {
        browser_window_mouse_click(bw, BROWSER_MOUSE_PRESS_1, dx, dy);
    } else if (!now && was) {
        browser_window_mouse_click(bw, BROWSER_MOUSE_CLICK_1, dx, dy);
        g_app.dirty = true;                 /* a click may navigate */
    }
    g_app.last_buttons = in.buttons;
}

static void pump_keys(struct gui_window *gw)
{
    struct embk_key_event ev;
    /* poll returns 1 for "got one", 0 for empty -- and only PRESSES matter
     * here; a release that scrolled would double every key. */
    while (embk_key_event_poll(&ev) == 1) {
        if (!ev.pressed) continue;
        struct browser_window *bw = emblink_window_bw(gw);
        switch (ev.code) {
        case EMBK_KEY_PGDN: g_app.scroll += g_app.h - 40; g_app.dirty = true; break;
        case EMBK_KEY_PGUP: g_app.scroll -= g_app.h - 40; g_app.dirty = true; break;
        case EMBK_KEY_DOWN:     g_app.scroll += 48;           g_app.dirty = true; break;
        case EMBK_KEY_UP:       g_app.scroll -= 48;           g_app.dirty = true; break;
        case EMBK_KEY_HOME:     g_app.scroll = 0;             g_app.dirty = true; break;
        case EMBK_KEY_END:      g_app.scroll = g_app.doc_h;   g_app.dirty = true; break;
        default:
            /* Anything else belongs to the page -- a form field, a shortcut
             * the document installed. The core decides, not this loop. */
            if (ev.code != 0 && ev.code < 0x80 &&
                browser_window_key_press(bw, (uint32_t)ev.code))
                g_app.dirty = true;
            break;
        }
        clamp_scroll();
    }
}

/* The application loop. Returns when the window closes. */
int emblink_app_run(struct gui_window *gw, const char *title);
int emblink_app_run(struct gui_window *gw, const char *title)
{
    void *pixels = NULL;
    g_app.win = embk_win_create_shared(APP_W, APP_H, 60, 60,
                                       title != NULL ? title : "NetSurf", &pixels);
    if (g_app.win < 0 || pixels == NULL) {
        fprintf(stderr, "nsemblink: no window (%d)\n", g_app.win);
        return 1;
    }
    g_app.px = pixels;
    g_app.w = APP_W;
    g_app.h = APP_H;

    /* THE SURFACE IS THE WINDOW. window.c allocated a buffer when the core
     * asked for a viewport; hand it the compositor's pages instead and free
     * the one it made. Everything downstream is unchanged, which is the whole
     * point of the core never learning where its pixels live. */
    if (!emblink_window_adopt(gw, g_app.px, APP_W, APP_H)) {
        fprintf(stderr, "nsemblink: could not adopt the window surface\n");
        embk_win_destroy(g_app.win);
        return 1;
    }

    int dw = 0, dh = 0;
    if (browser_window_get_extents(emblink_window_bw(gw), true, &dw, &dh) == NSERROR_OK)
        g_app.doc_h = dh;

    repaint(gw);

    for (;;) {
        int next = emblink_schedule_run();       /* fetches, animations, reflow */

        pump_pointer(gw);
        pump_keys(gw);

        /* The core invalidates when a fetch lands or a reflow finishes; that
         * is the signal to draw, not a timer. */
        if (emblink_window_take_invalid(gw)) {
            if (browser_window_get_extents(emblink_window_bw(gw), true, &dw, &dh) == NSERROR_OK)
                g_app.doc_h = dh;
            clamp_scroll();
            g_app.dirty = true;
        }

        if (g_app.dirty) repaint(gw);

        /* Sleep for whatever the scheduler asked for, bounded so input stays
         * responsive: a page with nothing pending must not make the window
         * ignore the mouse for a second. */
        if (next < 0 || next > 16) next = 16;
        if (next < 1) next = 1;
        usleep((useconds_t)next * 1000);
    }
}

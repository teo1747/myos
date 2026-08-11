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

#include "desktop/browser_history.h"

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

/* THE CHROME. A browser without it opens one page and can go nowhere, which
 * is what "not usable" means precisely. NetSurf draws no chrome of its own --
 * every frontend supplies it -- so this is a toolbar strip above the page:
 * back, forward, reload, and an address field you can type into.
 *
 * Drawn with the same plotters and the same font as the document, because
 * there is no second drawing stack here and inventing one for four buttons
 * would be the wrong kind of work. */
#define TB_H     30            /* toolbar height, in pixels */
#define BTN_W    28
#define SB_W     10            /* scrollbar */
#define URL_MAX  512

struct app {
    int       win;
    uint32_t *px;
    int        w, h;
    int        scroll;          /* document pixels hidden above the viewport */
    int        doc_h;
    bool       dirty;
    uint32_t   last_buttons;
    /* the address field */
    char       url[URL_MAX];
    int        url_len;
    int        url_caret;       /* insertion point, in bytes */
    bool       url_focus;
    bool       loading;
};

static struct app g_app;

/* One label, drawn through the same text path the document uses. */
static void chrome_text(struct emblink_surface *s, int x, int y,
                        const char *str, uint32_t colour)
{
    plot_font_style_t f;
    memset(&f, 0, sizeof f);
    f.family = PLOT_FONT_FAMILY_SANS_SERIF;
    f.size = 11 * PLOT_STYLE_SCALE;
    f.weight = 400;
    f.foreground = colour;
    f.background = 0x00FFFFFF;
    emblink_text_draw(s, x, y, &f, str, strlen(str));
}

static void draw_chrome(struct emblink_surface *s)
{
    /* NetSurf's colour word is 0xAABBGGRR -- red LOW -- so these read
     * backwards from an HTML hex triple. Same convention as plot.c. */
    const uint32_t bar   = 0x00E8E8E8;
    const uint32_t line  = 0x00B0B0B0;
    const uint32_t ink   = 0x00202020;
    const uint32_t field = 0x00FFFFFF;

    emblink_surface_clip(s, 0, 0, g_app.w, g_app.h);
    emblink_surface_fill(s, 0, 0, g_app.w, TB_H, bar);
    emblink_surface_fill(s, 0, TB_H - 1, g_app.w, TB_H, line);

    /* back / forward / reload, as glyphs rather than icons: the font is
     * already here and three .eic files are three more things to ship. */
    chrome_text(s, 10,          20, "<", ink);
    chrome_text(s, 10 + BTN_W,  20, ">", ink);
    chrome_text(s, 10 + BTN_W*2, 20, "R", ink);

    int fx = 10 + BTN_W * 3 + 6;
    emblink_surface_fill(s, fx, 4, g_app.w - 8, TB_H - 5, field);
    emblink_surface_fill(s, fx, 4, g_app.w - 8, 5, line);        /* top edge */
    chrome_text(s, fx + 6, 20, g_app.url, ink);

    /* LOADING, said rather than guessed at. A browser that looks identical
     * whether it is fetching or finished is one you press Enter on twice. */
    if (g_app.loading) {
        const uint32_t busy = 0x0020A0F0;                 /* 0xAABBGGRR */
        emblink_surface_fill(s, fx, TB_H - 4, g_app.w - 8, TB_H - 1, busy);
    }

    if (g_app.url_focus) {
        /* The caret sits after the text BEFORE it, so it tracks the insertion
         * point rather than always sitting at the end. */
        char save = g_app.url[g_app.url_caret];
        g_app.url[g_app.url_caret] = '\0';
        int cx = fx + 6 + emblink_text_advance_str(g_app.url);
        g_app.url[g_app.url_caret] = save;
        emblink_surface_fill(s, cx, 7, cx + 1, TB_H - 7, ink);
    }
}

/* THE SCROLLBAR. Not decoration: without it a long page gives no clue that
 * there is more of it, or how far down you are -- which is most of what makes
 * a window feel like a browser rather than a picture. */
static void draw_scrollbar(struct emblink_surface *s)
{
    if (g_app.doc_h <= g_app.h - TB_H) return;      /* it all fits */

    /* Dark enough to SEE. The first version used a near-white track and a pale
     * thumb, which on a white page is an affordance you cannot find -- the
     * scrollbar existed and told nobody anything. */
    const uint32_t track = 0x00D8D8D8, thumb = 0x00707070;
    int top = TB_H, height = g_app.h - TB_H;
    int x0 = g_app.w - SB_W;

    emblink_surface_clip(s, 0, 0, g_app.w, g_app.h);
    emblink_surface_fill(s, x0, top, g_app.w, g_app.h, track);
    emblink_surface_fill(s, x0, top, x0 + 1, g_app.h, 0x00A0A0A0);   /* an edge */

    int span = g_app.doc_h;
    int th = height * height / span;
    if (th < 20) th = 20;
    int ty = top + (int)((long long)g_app.scroll * (height - th) /
                         (span - height > 0 ? span - height : 1));
    emblink_surface_fill(s, x0 + 1, ty, g_app.w - 1, ty + th, thumb);
}

/* The core's redraw, into the window's own pixels, below the toolbar. */
static void repaint(struct gui_window *gw)
{
    struct emblink_surface *s = emblink_window_surface(gw);
    struct redraw_context ctx = {
        .interactive = true,
        .background_images = true,
        .plot = &emblink_plotters,
    };
    /* The page lives BELOW the toolbar, so it is clipped to that band and the
     * document origin is pushed down by it. Getting this wrong draws the page
     * over the address bar, which looks like the chrome failing to paint. */
    struct rect clip = { 0, TB_H, g_app.w, g_app.h };

    /* White first: the core paints a document's own background, and whatever
     * it does not cover has to be the canvas rather than the last frame. */
    for (size_t i = 0; i < (size_t)g_app.w * g_app.h; i++) g_app.px[i] = 0x00FFFFFF;

    emblink_target = s;
    emblink_surface_clip(s, 0, TB_H, g_app.w, g_app.h);
    browser_window_redraw(emblink_window_bw(gw), 0, TB_H - g_app.scroll, &clip, &ctx);
    draw_chrome(s);
    draw_scrollbar(s);
    emblink_target = NULL;

    embk_win_present(g_app.win, g_app.px, (uint32_t)g_app.w, (uint32_t)g_app.h);
    g_app.dirty = false;
}

/* Follow whatever is in the address field. */
static void go(struct gui_window *gw)
{
    struct nsurl *u = NULL;
    char buf[URL_MAX + 8];
    const char *t = g_app.url;

    /* What people type is not a URL. A bare path is a file and a bare host is
     * http -- guessing here is the difference between an address bar and a
     * field that only accepts what a program would have written. */
    if (t[0] == '/') snprintf(buf, sizeof buf, "file://%s", t);
    else if (strstr(t, "://") == NULL) snprintf(buf, sizeof buf, "http://%s", t);
    else snprintf(buf, sizeof buf, "%s", t);

    if (nsurl_create(buf, &u) != NSERROR_OK) return;
    browser_window_navigate(emblink_window_bw(gw), u, NULL,
                            BW_NAVIGATE_HISTORY, NULL, NULL, NULL);
    nsurl_unref(u);
    g_app.scroll = 0;
    g_app.dirty = true;
}

/* Keep the field showing where we actually are, unless it is being edited. */
static void sync_url(struct gui_window *gw)
{
    if (g_app.url_focus) return;
    struct nsurl *u = browser_window_access_url(emblink_window_bw(gw));
    if (u == NULL) return;
    snprintf(g_app.url, sizeof g_app.url, "%s", nsurl_access(u));
    g_app.url_len = (int)strlen(g_app.url);
    g_app.url_caret = g_app.url_len;
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
    /* Window space -> DOCUMENT space: minus the toolbar, plus the scroll. */
    int dx = in.x, dy = in.y - TB_H + g_app.scroll;

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

    if (now && !was && in.y < TB_H) {
        /* THE TOOLBAR gets the click, not the page. */
        if (in.x < 10 + BTN_W) {
            browser_window_history_back(bw, false);
            g_app.scroll = 0; g_app.dirty = true;
        } else if (in.x < 10 + BTN_W * 2) {
            browser_window_history_forward(bw, false);
            g_app.scroll = 0; g_app.dirty = true;
        } else if (in.x < 10 + BTN_W * 3) {
            browser_window_reload(bw, false);
            g_app.dirty = true;
        } else {
            g_app.url_focus = true;         /* clicked the address field */
            g_app.url_caret = g_app.url_len;
            g_app.dirty = true;
        }
        g_app.last_buttons = in.buttons;
        return;
    }

    if (now && !was) {
        g_app.url_focus = false;            /* clicking the page leaves the field */
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

        /* THE ADDRESS FIELD TAKES EVERY KEY while it is focused -- including
         * the arrows and PgDn that would otherwise scroll. A field that
         * scrolls the page while you type in it is not a field. */
        if (g_app.url_focus) {
            if (ev.code == '\r' || ev.code == '\n') {
                g_app.url_focus = false;
                go(gw);
            } else if (ev.code == '\b' || ev.code == 0x7F) {
                /* delete BEFORE the caret and close the gap -- an editor that
                 * only truncates the end is a field you cannot correct. */
                if (g_app.url_caret > 0) {
                    memmove(g_app.url + g_app.url_caret - 1,
                            g_app.url + g_app.url_caret,
                            (size_t)(g_app.url_len - g_app.url_caret) + 1);
                    g_app.url_caret--;
                    g_app.url_len--;
                }
                g_app.dirty = true;
            } else if (ev.code == 0x1B) {          /* escape: give up editing */
                g_app.url_focus = false;
                sync_url(gw);
                g_app.dirty = true;
            } else if (ev.code == EMBK_KEY_LEFT) {
                if (g_app.url_caret > 0) g_app.url_caret--;
                g_app.dirty = true;
            } else if (ev.code == EMBK_KEY_RIGHT) {
                if (g_app.url_caret < g_app.url_len) g_app.url_caret++;
                g_app.dirty = true;
            } else if (ev.code == EMBK_KEY_HOME) {
                g_app.url_caret = 0; g_app.dirty = true;
            } else if (ev.code == EMBK_KEY_END) {
                g_app.url_caret = g_app.url_len; g_app.dirty = true;
            } else if (ev.code >= 0x20 && ev.code < 0x7F &&
                       g_app.url_len < URL_MAX - 1) {
                memmove(g_app.url + g_app.url_caret + 1,
                        g_app.url + g_app.url_caret,
                        (size_t)(g_app.url_len - g_app.url_caret) + 1);
                g_app.url[g_app.url_caret++] = (char)ev.code;
                g_app.url_len++;
                g_app.dirty = true;
            }
            continue;
        }

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
    /* THE PROGRAM'S NAME FIRST, then the page's. Titling the window with the
     * document alone put "Vellum" on NetSurf's window when it opened the OS's
     * start page -- correct, and unreadable as anything but a bug. */
    char wtitle[128];
    if (title != NULL && title[0] != '\0')
        snprintf(wtitle, sizeof wtitle, "NetSurf - %s", title);
    else
        snprintf(wtitle, sizeof wtitle, "NetSurf");

    void *pixels = NULL;
    /* x=20, not 60: at 760 wide a window placed at 60 has its right-hand
     * 20 pixels off an 800-wide screen -- which is exactly where the
     * scrollbar is, so the one affordance that tells you a page is longer
     * than the window was the part that could not be seen. */
    g_app.win = embk_win_create_shared(APP_W, APP_H, 20, 46, wtitle, &pixels);
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

    sync_url(gw);
    repaint(gw);

    for (;;) {
        int next = emblink_schedule_run();       /* fetches, animations, reflow */

        pump_pointer(gw);
        pump_keys(gw);

        /* The core invalidates when a fetch lands or a reflow finishes; that
         * is the signal to draw, not a timer. */
        sync_url(gw);

        bool busy = emblink_window_loading(gw);
        if (busy != g_app.loading) { g_app.loading = busy; g_app.dirty = true; }

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

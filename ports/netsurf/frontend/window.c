/* ports/netsurf/frontend/window.c -- somewhere for the page to be.
 *
 * A `gui_window` is the frontend's idea of a viewport: the core asks it how
 * big it is, tells it when a region needs repainting, and reads its scroll
 * position. It is NOT a window on the screen yet -- this one owns a pixel
 * buffer and nothing else, which is exactly what a headless render needs and
 * exactly the part the compositor version will replace (the win syscalls hand
 * back a shared framebuffer; that becomes `surf.px` and the rest is unchanged).
 *
 * Keeping the two apart is deliberate. The core must never learn whether its
 * pixels are on a screen.
 */
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "utils/errors.h"
#include "netsurf/types.h"
#include "netsurf/window.h"
#include "netsurf/browser_window.h"
#include "utils/nsurl.h"
#include "emblink.h"


struct gui_window {
    struct browser_window *bw;
    struct emblink_surface surf;
    /* the region the core said was stale; a headless render just redraws all
     * of it, but recording it is what makes the windowed version cheap */
    struct rect invalid;
    bool has_invalid;
    int scroll_x, scroll_y;
    /* THE ONLY SIGNAL that a load has finished. There is no "is it done"
     * question to ask the core; it TELLS the frontend, by stopping the
     * throbber it told it to start. A headless render has no throbber and
     * still needs the fact. */
    bool loading, loaded;
    char title[128];
};

static struct gui_window *g_only;      /* one viewport for now, and only one */

struct gui_window *emblink_window_get(void) { return g_only; }
struct emblink_surface *emblink_window_surface(struct gui_window *gw)
{
    return gw != NULL ? &gw->surf : NULL;
}
struct browser_window *emblink_window_bw(struct gui_window *gw)
{
    return gw != NULL ? gw->bw : NULL;
}

/* The viewport size. Set once, before anything is loaded. */
static int g_width = 1100, g_height = 900;
void emblink_window_set_size(int w, int h) { g_width = w; g_height = h; }

static struct gui_window *win_create(struct browser_window *bw,
                                     struct gui_window *existing,
                                     gui_window_create_flags flags)
{
    (void)existing; (void)flags;
    struct gui_window *gw = calloc(1, sizeof *gw);
    if (gw == NULL) return NULL;

    gw->bw = bw;
    gw->surf.width = g_width;
    gw->surf.height = g_height;
    gw->surf.stride = g_width;
    gw->surf.px = calloc((size_t)g_width * g_height, sizeof(uint32_t));
    if (gw->surf.px == NULL) { free(gw); return NULL; }
    /* white, because that is the canvas a page is written against -- the same
     * reasoning as PAGE_CANVAS in the other browser's style.h */
    for (size_t i = 0; i < (size_t)g_width * g_height; i++) gw->surf.px[i] = 0x00FFFFFF;
    emblink_surface_clip(&gw->surf, 0, 0, g_width, g_height);

    if (g_only == NULL) g_only = gw;
    return gw;
}

static void win_destroy(struct gui_window *gw)
{
    if (gw == NULL) return;
    if (g_only == gw) g_only = NULL;
    free(gw->surf.px);
    free(gw);
}

static nserror win_invalidate(struct gui_window *gw, const struct rect *rect)
{
    if (gw == NULL) return NSERROR_OK;
    if (rect == NULL) {                       /* NULL means the whole thing */
        gw->invalid = (struct rect){ 0, 0, gw->surf.width, gw->surf.height };
        gw->has_invalid = true;
        return NSERROR_OK;
    }
    if (!gw->has_invalid) {
        gw->invalid = *rect;
        gw->has_invalid = true;
    } else {                                  /* union, so nothing is forgotten */
        if (rect->x0 < gw->invalid.x0) gw->invalid.x0 = rect->x0;
        if (rect->y0 < gw->invalid.y0) gw->invalid.y0 = rect->y0;
        if (rect->x1 > gw->invalid.x1) gw->invalid.x1 = rect->x1;
        if (rect->y1 > gw->invalid.y1) gw->invalid.y1 = rect->y1;
    }
    return NSERROR_OK;
}

/* SCROLL POSITION, which is the frontend's to own: the core asks where the
 * viewport is and tells it where to move to, and both are MANDATORY -- a table
 * without them is refused outright (netsurf_register returns BadParameter, and
 * that is the whole error you get). A headless render never scrolls, but the
 * page still asks: an anchor in the URL, or a script calling scrollTo. */
static bool win_get_scroll(struct gui_window *gw, int *sx, int *sy)
{
    if (gw == NULL) return false;
    *sx = gw->scroll_x;
    *sy = gw->scroll_y;
    return true;
}

static nserror win_set_scroll(struct gui_window *gw, const struct rect *rect)
{
    if (gw == NULL || rect == NULL) return NSERROR_BAD_PARAMETER;
    /* The core passes the RECTANGLE it wants brought into view, not an
     * offset. Its top-left is where the viewport should start. */
    gw->scroll_x = rect->x0;
    gw->scroll_y = rect->y0;
    return NSERROR_OK;
}

static nserror win_get_dimensions(struct gui_window *gw, int *width, int *height)
{
    if (gw == NULL) return NSERROR_BAD_PARAMETER;
    *width = gw->surf.width;
    *height = gw->surf.height;
    return NSERROR_OK;
}

static nserror win_event(struct gui_window *gw, enum gui_window_event event)
{
    if (gw == NULL) return NSERROR_OK;
    switch (event) {
    case GW_EVENT_START_THROBBER: gw->loading = true;  break;
    case GW_EVENT_STOP_THROBBER:  gw->loading = false;
                                  gw->loaded  = true;  break;
    default: break;   /* the rest update chrome this frontend does not have */
    }
    return NSERROR_OK;
}

/* True once the core has said it finished -- however it finished. A page that
 * failed still stops its throbber, and waiting for a SUCCESSFUL load would
 * hang forever on a 404. */
bool emblink_window_settled(struct gui_window *gw)
{
    return gw != NULL && gw->loaded && !gw->loading;
}

static void win_set_title(struct gui_window *gw, const char *title)
{
    if (gw == NULL || title == NULL) return;
    /* A GUARD, AND IT IS NOT A FIX -- it is a way to keep going past a bug
     * that is not ours to fix yet, without pretending it is gone.
     *
     * On the OS build this is handed a pointer that is neither in the heap
     * (which starts at 0x600000000000, see USER_HEAP_VA_BASE) nor in the
     * program image, so dereferencing it kills the process before anything
     * can be seen. The value is wrong by the first call, while the nsurl the
     * title falls back to is verifiably sound at creation -- so something
     * corrupts it in between, and that is still open.
     *
     * Refusing the pointer costs a window title and buys every diagnostic
     * after it. It is LOUD on purpose: a silent guard here would turn an
     * unexplained crash into an unexplained missing title, which is worse. */
    /* 0x400000 is where newlib.ld places the image, so nothing valid -- not a
     * string literal, not a heap pointer -- can be below it. An earlier
     * version of this guard used 1MB and the bad pointer was at 2MB, which
     * tested nothing and cost a boot. */
    if ((uintptr_t)title < 0x400000ULL) {
        fprintf(stderr, "nsemblink: REFUSED a title pointer at %p "
                        "(not heap, not image) -- see docs/TODO.md\n",
                (const void *)title);
        fflush(stderr);
        return;
    }
    snprintf(gw->title, sizeof gw->title, "%s", title);
}

/* GROW THE SURFACE TO THE DOCUMENT. The core plots only what falls inside the
 * viewport it is given, so a headless render of a long page silently stops at
 * the fold -- which measures the window rather than the engine. Asked for
 * after the load, when the document's height is finally known.
 *
 * Reallocates rather than resizing in place: the buffer is width*height and
 * both change. Returns false and keeps the old surface if the new one cannot
 * be had, because a short render beats no render. */
bool emblink_window_resize(struct gui_window *gw, int w, int h)
{
    if (gw == NULL || w <= 0 || h <= 0) return false;
    if (w == gw->surf.width && h == gw->surf.height) return true;

    /* A page can be a hundred thousand pixels tall. The cap is a real limit
     * on the picture, not on the layout -- the core still laid the whole
     * document out; this is how much of it we are prepared to paint. */
    if ((long long)w * h > 64LL * 1024 * 1024) return false;

    uint32_t *px = calloc((size_t)w * h, sizeof(uint32_t));
    if (px == NULL) return false;
    for (size_t i = 0; i < (size_t)w * h; i++) px[i] = 0x00FFFFFF;

    free(gw->surf.px);
    gw->surf.px = px;
    gw->surf.width = w;
    gw->surf.height = h;
    gw->surf.stride = w;
    emblink_surface_clip(&gw->surf, 0, 0, w, h);
    return true;
}

/* declared in emblink.h */
const char *emblink_window_title(struct gui_window *gw)
{
    return gw != NULL ? gw->title : "";
}

static nserror win_set_url(struct gui_window *gw, struct nsurl *url)
{
    (void)gw; (void)url;
    return NSERROR_OK;
}

static void win_set_status(struct gui_window *gw, const char *text)
{
    (void)gw; (void)text;
}

static void win_set_pointer(struct gui_window *gw, enum gui_pointer_shape shape)
{
    (void)gw; (void)shape;
}

static void win_place_caret(struct gui_window *gw, int x, int y, int height,
                            const struct rect *clip)
{
    (void)gw; (void)x; (void)y; (void)height; (void)clip;
}

static struct gui_window_table window_table = {
    .create = win_create,
    .destroy = win_destroy,
    .invalidate = win_invalidate,
    .get_scroll = win_get_scroll,
    .set_scroll = win_set_scroll,
    .get_dimensions = win_get_dimensions,
    .event = win_event,
    .set_title = win_set_title,
    .set_url = win_set_url,
    .set_status = win_set_status,
    .set_pointer = win_set_pointer,
    .place_caret = win_place_caret,
};

struct gui_window_table *emblink_window_table = &window_table;

/* ports/netsurf/frontend/main.c -- start the core, load one page, draw it.
 *
 * The smallest thing that is a browser: register the tables, initialise the
 * core, ask it to navigate, pump the scheduler until the page settles, then
 * ask it to redraw into our surface and write the result out as a PPM.
 *
 * Headless on purpose for this milestone. The pipeline being proved here is
 * fetch -> parse -> cascade -> LAYOUT -> plot, and a window adds nothing to
 * that proof while adding a compositor, an event loop and input handling to
 * the list of things that can be wrong. The image it writes is the same format
 * `make web-shots` already grades, so the port is measurable against Firefox
 * from its first frame -- the same instrument, pointed at a different renderer.
 *
 *   nsemblink <file-or-url> [width] [height] [out.ppm]
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "utils/errors.h"
#include "utils/messages.h"
#include "netsurf/netsurf.h"
#include "netsurf/browser_window.h"
#include "netsurf/misc.h"
#include "netsurf/window.h"
#include "netsurf/fetch.h"
#include "netsurf/layout.h"
#include "netsurf/bitmap.h"
#include "netsurf/plotters.h"
#include "netsurf/content.h"
#include "utils/nsurl.h"
#include "utils/nsoption.h"
#include "desktop/gui_table.h"
#include "emblink.h"


static struct netsurf_table emblink_table = {
    .misc   = NULL,       /* filled in main: the tables live in their own files */
    .window = NULL,
    .fetch  = NULL,
    .bitmap = NULL,
    .layout = NULL,
};

static bool write_ppm(const struct emblink_surface *s, const char *path)
{
    FILE *f = fopen(path, "wb");
    if (f == NULL) return false;
    fprintf(f, "P6\n%d %d\n255\n", s->width, s->height);
    for (int y = 0; y < s->height; y++) {
        const uint32_t *row = s->px + (size_t)y * s->stride;
        for (int x = 0; x < s->width; x++) {
            uint32_t p = row[x];
            unsigned char rgb[3] = {
                (unsigned char)((p >> 16) & 0xFF),
                (unsigned char)((p >> 8) & 0xFF),
                (unsigned char)(p & 0xFF),
            };
            if (fwrite(rgb, 1, 3, f) != 3) { fclose(f); return false; }
        }
    }
    fclose(f);
    return true;
}

int main(int argc, char **argv)
{
    const char *target = argc > 1 ? argv[1] : "about:blank";
    int width  = argc > 2 ? atoi(argv[2]) : 1100;
    int height = argc > 3 ? atoi(argv[3]) : 900;
    const char *out = argc > 4 ? argv[4] : "nsrender.ppm";
    nserror err;

    emblink_table.misc   = emblink_misc_table;
    emblink_table.window = emblink_window_table;
    emblink_table.fetch  = emblink_fetch_table;
    emblink_table.bitmap = emblink_bitmap_table;
    emblink_table.layout = emblink_layout_table;

    err = netsurf_register(&emblink_table);
    if (err != NSERROR_OK) {
        fprintf(stderr, "nsemblink: table rejected: %s\n", messages_get_errorcode(err));
        return 1;
    }

    err = nsoption_init(NULL, NULL, NULL);
    if (err != NSERROR_OK) {
        fprintf(stderr, "nsemblink: options: %s\n", messages_get_errorcode(err));
        return 1;
    }

    emblink_window_set_size(width, height);

    err = netsurf_init(NULL);
    if (err != NSERROR_OK) {
        fprintf(stderr, "nsemblink: core init: %s\n", messages_get_errorcode(err));
        return 1;
    }

    /* A BARE PATH IS NOT A URL, and typing one is what people do. Every
     * browser turns a leading slash into file://, and the alternative here is
     * an error message that tells a user their own filename is malformed. */
    char urlbuf[1024];
    if (target[0] == '/') {
        snprintf(urlbuf, sizeof urlbuf, "file://%s", target);
        target = urlbuf;
    }

    struct nsurl *url = NULL;
    err = nsurl_create(target, &url);
    if (err != NSERROR_OK) {
        fprintf(stderr, "nsemblink: bad url [%s]\n", target);
        return 1;
    }

    struct browser_window *bw = NULL;
    err = browser_window_create(BW_CREATE_HISTORY, url, NULL, NULL, &bw);
    nsurl_unref(url);
    if (err != NSERROR_OK) {
        fprintf(stderr, "nsemblink: navigate: %s\n", messages_get_errorcode(err));
        return 1;
    }

    /* PUMP UNTIL IT SETTLES. There is no event loop to block in: the only
     * things that make progress are scheduled callbacks, so run them until
     * nothing is pending. Bounded, because a page that never settles (an
     * animation, a poll) would otherwise never render -- an animation is not
     * a reason to refuse to draw the first frame. */
    for (int spin = 0; spin < 20000; spin++) {
        int next = emblink_schedule_run();
        if (next < 0) break;
    }

    struct gui_window *gw = emblink_window_get();
    struct emblink_surface *surf = emblink_window_surface(gw);
    if (gw == NULL || surf == NULL) {
        fprintf(stderr, "nsemblink: the core never asked for a window\n");
        return 1;
    }

    struct redraw_context ctx = {
        .interactive = false,
        .background_images = true,
        .plot = &emblink_plotters,
    };
    struct rect clip = { 0, 0, surf->width, surf->height };

    emblink_target = surf;
    emblink_surface_clip(surf, 0, 0, surf->width, surf->height);
    err = browser_window_redraw(emblink_window_bw(gw), 0, 0, &clip, &ctx);
    emblink_target = NULL;
    if (err != NSERROR_OK) {
        fprintf(stderr, "nsemblink: redraw: %s\n", messages_get_errorcode(err));
        return 1;
    }

    printf("nsemblink: [%s] %dx%d -> %s\n", emblink_window_title(gw),
           surf->width, surf->height, out);
    if (!write_ppm(surf, out)) {
        fprintf(stderr, "nsemblink: cannot write %s\n", out);
        return 1;
    }

    browser_window_destroy(bw);
    netsurf_exit();
    emblink_schedule_finalise();
    return 0;
}

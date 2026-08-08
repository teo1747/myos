/* user/web/cssref.c -- see cssref.h. */
#include <string.h>
#include <stdio.h>

#include "cssref.h"
#include "html.h"
#include "fetchjob.h"
#include "net.h"
#include "url.h"

/* our tag on the shared worker; the document, the images and fetch() differ */
#define CSS_TAG 4

#define CSSREF_MAX   8
/* Sized from what real sites actually ship, counted rather than guessed:
 * python.org 513KB of CSS across four files, Wikipedia 223KB in one,
 * MDN 123KB across seventeen. The old 64KB-per-sheet meant Wikipedia's
 * stylesheet could not even be held, and the old 128KB total meant it was
 * dropped from the concatenation ENTIRELY -- which is not "some styling is
 * missing", it is the page rendering as though it had no stylesheet at all.
 * That is exactly what Wikipedia did, and it read as a layout bug.
 *
 * github ships 6.9MB and will not fit. That is a bound being hit, and hitting
 * one is now reported (cssref_dropped) rather than silent. */
#define CSS_TEXT_MAX (1024 * 1024)    /* the whole page's author CSS */
#define CSS_ONE_MAX  (512 * 1024)     /* one sheet's download buffer  */

static char   g_url[CSSREF_MAX][512];
static int    g_n;                    /* how many sheets this page names */
static int    g_next;                 /* the next one to fetch          */
static int    g_inflight;

static char   g_text[CSS_TEXT_MAX];   /* every sheet, concatenated */
static int    g_dropped;              /* sheets that did not fit -- see cssref.h */
static size_t g_len;
static char   g_one[CSS_ONE_MAX];     /* the sheet currently arriving */

void cssref_reset(void) {
    g_n = g_next = 0;
    g_inflight = 0;
    g_len = 0;
    g_text[0] = 0;
    g_dropped = 0;
}

int cssref_start(struct html_doc *doc, const char *base) {
    cssref_reset();
    if (!doc) return 0;
    for (int i = 0; i < doc->n_cssref && g_n < CSSREF_MAX; i++) {
        if (!doc->cssref[i] || !doc->cssref[i][0]) continue;
        /* Resolved against the DOCUMENT, not the browser's idea of "here": a
         * sheet at /css/site.css on a page fetched over https means that host,
         * and the same href in a local file means the file beside it. Same one
         * rule the link handler uses. */
        if (url_resolve(base, doc->cssref[i], g_url[g_n], sizeof g_url[0]) != 0)
            continue;
        g_n++;
    }
    return g_n;
}

int cssref_pending(void) { return g_inflight || g_next < g_n; }

int cssref_pump(void) {
    int changed = 0;

    if (g_inflight) {
        struct vnet_result res;
        int r = fetchjob_poll(CSS_TAG, &res);
        if (r == 1) {
            g_inflight = 0;
            changed = 1;
            /* A sheet that 404s or times out is not an error worth stopping
             * for: the page still renders, with whatever styling did arrive.
             * A browser that refuses a document because one of its stylesheets
             * is missing is worse than one that shows it plain. */
            if (res.len > 0 && res.status >= 200 && res.status < 300) {
                size_t n = res.len;
                if (n > sizeof g_one - 1) { n = sizeof g_one - 1; g_dropped++; }
                if (g_len + n + 2 < sizeof g_text) {
                    /* a newline between sheets: a file that ends mid-comment or
                     * without its final newline must not weld onto the next */
                    if (g_len) g_text[g_len++] = '\n';
                    memcpy(g_text + g_len, g_one, n);
                    g_len += n;
                    g_text[g_len] = 0;
                } else {
                    /* NOT SILENT. A sheet that does not fit is dropped whole,
                     * and a page missing a whole stylesheet does not look
                     * slightly wrong -- it looks unstyled, for a reason
                     * nothing on screen explains. */
                    g_dropped++;
                }
            }
        } else if (r < 0) {
            g_inflight = 0;              /* the job vanished; move on */
            changed = 1;
        }
    }

    if (!g_inflight && g_next < g_n && !fetchjob_busy()) {
        if (fetchjob_start(g_url[g_next], g_one, sizeof g_one, CSS_TAG) == 0) {
            g_next++;
            g_inflight = 1;
        }
    }
    return changed;
}

const char *cssref_text(size_t *len) {
    if (len) *len = g_len;
    return g_len ? g_text : 0;
}

/* How many stylesheets were dropped for want of room. Reported so a page that
 * renders unstyled says why. */
int cssref_dropped(void) { return g_dropped; }

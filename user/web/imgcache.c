/* user/web/imgcache.c -- see imgcache.h.
 *
 * One fetch in flight at a time, on the worker fetchjob already owns. That is
 * a deliberate limit rather than a missing feature: a second concurrent fetch
 * would need a second TLS context (net.c keeps one static, on purpose), and
 * pictures arriving one after another is a browser loading a page, not a bug.
 */
#include <string.h>
#include <stdio.h>

#include "imgcache.h"
#include "svg.h"
#include "fetchjob.h"
#include "net.h"
#include "png.h"
#include "jpeg.h"

/* our tag on the shared worker; the document uses a different one */
#define IMG_TAG 2

/* The pixel arena. Sized for a documentation page: a few diagrams, not a photo
 * gallery. Blown deliberately rather than grown -- see the header. */
#define IMG_ARENA_PX IMG_MAX_PX               /* 6.4 MB of BGRA */
static uint32_t g_arena[IMG_ARENA_PX];
static size_t   g_used;

/* One download + decode buffer, reused per image: only one is ever in flight,
 * so a second copy would be memory spent to hold nothing. */
#define IMG_SRC_MAX (512 * 1024)
static char    g_src[IMG_SRC_MAX];
/* inflated scanlines: pixels*4 plus one filter byte per row */
static uint8_t g_scratch[IMG_MAX_PX * 4 + IMG_MAX_DIM + 64];

static struct img_slot g_slot[IMG_SLOTS];
static int g_inflight = -1;                   /* slot index, or -1 */
static int g_refused;                         /* asked for, no slot left */

void imgcache_reset(void) {
    memset(g_slot, 0, sizeof g_slot);
    g_used = 0;
    /* A fetch may still be running for the PREVIOUS page. It cannot be
     * cancelled mid-flight, so its slot is gone but the job is left to finish
     * and be reaped by the next pump -- which then finds no slot and drops the
     * bytes. Forgetting the slot while the worker still writes into g_src is
     * safe: the worker owns that buffer until it reports done. */
    g_inflight = -1;
    g_refused = 0;
}

struct img_slot *imgcache_want(const char *url) {
    if (!url || !url[0]) return 0;
    for (int i = 0; i < IMG_SLOTS; i++)
        if (g_slot[i].state != IMG_EMPTY && !strcmp(g_slot[i].url, url))
            return &g_slot[i];
    for (int i = 0; i < IMG_SLOTS; i++) {
        if (g_slot[i].state != IMG_EMPTY) continue;
        snprintf(g_slot[i].url, sizeof g_slot[i].url, "%s", url);
        g_slot[i].state = IMG_WANTED;
        return &g_slot[i];
    }
    /* FULL. Counted rather than shrugged at: a page with more pictures than
     * slots draws grey boxes that look exactly like failures, and the two want
     * completely different fixes. */
    g_refused++;
    return 0;
}

void imgcache_stats(int *ready, int *failed, int *pending, int *refused) {
    int r = 0, f = 0, p = 0;
    for (int i = 0; i < IMG_SLOTS; i++) {
        if (g_slot[i].state == IMG_READY)  r++;
        else if (g_slot[i].state == IMG_FAILED) f++;
        else if (g_slot[i].state == IMG_WANTED || g_slot[i].state == IMG_LOADING) p++;
    }
    if (ready) *ready = r;
    if (failed) *failed = f;
    if (pending) *pending = p;
    if (refused) *refused = g_refused;
}

int imgcache_pending(void) {
    for (int i = 0; i < IMG_SLOTS; i++)
        if (g_slot[i].state == IMG_WANTED || g_slot[i].state == IMG_LOADING) return 1;
    return 0;
}

static void finish(int slot, const struct vnet_result *res) {
    struct img_slot *s = &g_slot[slot];
    if (s->state != IMG_LOADING) return;       /* its page went away */

    uint32_t w = 0, h = 0;
    if (res->err[0] && res->len == 0) { s->state = IMG_FAILED; return; }

    /* Which decoder, decided by the BYTES and not by the URL's extension. A
     * server is free to send a JPEG from a path ending .png, and a browser
     * that trusts the name renders garbage; the signature is the only thing
     * that actually knows. */
    const uint8_t *bytes = (const uint8_t *)g_src;
    int is_jpeg = res->len > 3 && bytes[0] == 0xFF && bytes[1] == 0xD8;
    int is_png  = res->len > 8 && bytes[0] == 0x89 && bytes[1] == 'P';
    /* SVG is TEXT, so it has no signature byte -- it is whatever is neither of
     * the other two and contains an <svg. Checked last for that reason. */
    int is_svg  = !is_jpeg && !is_png && svg_probe(bytes, res->len, &w, &h) == 0;
    int probed = is_svg  ? 0
               : is_jpeg ? jpeg_probe(bytes, res->len, &w, &h)
                         : png_probe(bytes, res->len, &w, &h);
    if (probed != 0) { s->state = IMG_FAILED; return; }
    /* Reject on DIMENSIONS before spending the arena -- a 20000x20000 header
     * costs nothing to send and would otherwise cost everything to honour. */
    if (w > IMG_MAX_DIM || h > IMG_MAX_DIM) { s->state = IMG_FAILED; return; }
    size_t need = (size_t)w * h;
    if (need > IMG_MAX_PX) { s->state = IMG_FAILED; return; }
    if (need > IMG_ARENA_PX - g_used) { s->state = IMG_FAILED; return; }

    uint32_t *dst = g_arena + g_used;
    int rc = is_svg
        ? svg_decode(bytes, res->len, dst, need * 4, g_scratch, sizeof g_scratch, &w, &h)
        : is_jpeg
        ? jpeg_decode(bytes, res->len, dst, need * 4, g_scratch, sizeof g_scratch, &w, &h)
        : png_decode(bytes, res->len, dst, need * 4, g_scratch, sizeof g_scratch, &w, &h);
    if (rc != 0) { s->state = IMG_FAILED; return; }
    g_used += need;
    s->px = dst; s->w = w; s->h = h;
    s->state = IMG_READY;
}

int imgcache_pump(void) {
    int changed = 0;
    /* 1. reap a finished fetch */
    if (g_inflight >= 0) {
        struct vnet_result res;
        int r = fetchjob_poll(IMG_TAG, &res);
        if (r == 1) { finish(g_inflight, &res); g_inflight = -1; changed = 1; }
        else if (r < 0) {
            /* genuinely nothing running: our job vanished (a new document
             * reset us mid-flight). Put the slot back so it can be retried. */
            if (g_slot[g_inflight].state == IMG_LOADING) g_slot[g_inflight].state = IMG_WANTED;
            g_inflight = -1; changed = 1;
        }
    }

    /* 2. start the next one, but never while the DOCUMENT is loading -- the
     *    page the user asked for outranks the pictures on the page they are
     *    leaving. */
    if (g_inflight < 0 && !fetchjob_busy()) {
        for (int i = 0; i < IMG_SLOTS; i++) {
            if (g_slot[i].state != IMG_WANTED) continue;
            if (fetchjob_start(g_slot[i].url, g_src, sizeof g_src, IMG_TAG) == 0) {
                g_slot[i].state = IMG_LOADING;
                g_inflight = i;
                changed = 1;
            }
            break;
        }
    }
    return changed;
}

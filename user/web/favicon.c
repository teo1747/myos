/* user/web/favicon.c -- see favicon.h. Mirrors imgcache.c's fetch loop, which
 * is the module this one is a sibling of; the differences are all consequences
 * of an icon belonging to a SITE rather than to a page. */

#include "favicon.h"
#include "fetchjob.h"
#include "net.h"
#include "png.h"
#include "jpeg.h"
#include "ico.h"
#include "url.h"
#include <string.h>
#include <stdio.h>

/* Its own tag, so a landed icon is never mistaken for the document or for a
 * picture on the page -- fetchjob hands a result to whoever asks with the
 * matching tag and leaves everyone else's alone. */
#define FAV_TAG 4

/* Big enough for what favicons actually are: the largest among the sites this
 * is tested against is lobste.rs at 11KB. */
#define FAV_SRC_MAX (128 * 1024)
/* The largest picture we will decode before shrinking it. Hacker News ships a
 * 256x256 PNG inside its .ico and that is the realistic ceiling. */
#define FAV_DEC_DIM 256

static char     g_src[FAV_SRC_MAX];
static uint32_t g_dec[FAV_DEC_DIM * FAV_DEC_DIM];
static uint8_t  g_scratch[FAV_DEC_DIM * FAV_DEC_DIM * 4 + FAV_DEC_DIM + 64];

static struct favicon g_slot[FAVICON_SLOTS];
static uint32_t       g_px[FAVICON_SLOTS][FAVICON_PX * FAVICON_PX];
/* Which candidate each slot is on: 0 = the first URL, 1 = the fallback. */
static unsigned char  g_try[FAVICON_SLOTS];
static char           g_cand[FAVICON_SLOTS][2][256];
static int            g_inflight = -1;

/* scheme://host of a URL, or 0 if it has none (a local path). */
static int origin_of(const char *url, char *out, size_t cap) {
    const char *p = strstr(url, "://");
    if (!p) return -1;
    const char *h = p + 3;
    const char *e = h;
    while (*e && *e != '/' && *e != '?' && *e != '#') e++;
    if (e == h) return -1;
    size_t n = (size_t)(e - url);
    if (n + 1 > cap) return -1;
    memcpy(out, url, n);
    out[n] = 0;
    return 0;
}

/* A declared href this browser has no decoder for is worse than no declaration:
 * following it costs a fetch and then fails, when /favicon.ico was going to
 * work. SVG is the case that actually happens -- Hacker News declares one. */
static int undecodable(const char *href) {
    if (!href || !href[0]) return 1;
    if (!strncmp(href, "data:", 5)) return 1;
    size_t n = strlen(href);
    if (n >= 4 && !strcmp(href + n - 4, ".svg")) return 1;
    /* ...and with a query string after it */
    const char *q = strstr(href, ".svg?");
    return q != 0;
}

struct favicon *favicon_want(const char *page_url, const char *declared) {
    char origin[192];
    if (!page_url || origin_of(page_url, origin, sizeof origin) != 0) return 0;

    for (int i = 0; i < FAVICON_SLOTS; i++)
        if (g_slot[i].state != FAV_EMPTY && !strcmp(g_slot[i].origin, origin))
            return &g_slot[i];

    int slot = -1;
    for (int i = 0; i < FAVICON_SLOTS; i++)
        if (g_slot[i].state == FAV_EMPTY) { slot = i; break; }
    if (slot < 0) return 0;              /* full: the tab shows no icon, which is fine */

    memset(&g_slot[slot], 0, sizeof g_slot[slot]);
    snprintf(g_slot[slot].origin, sizeof g_slot[slot].origin, "%s", origin);

    /* The two candidates, in the order they will be tried. */
    char dec[256]; dec[0] = 0;
    if (declared && declared[0] && !undecodable(declared))
        if (url_resolve(page_url, declared, dec, sizeof dec) != 0) dec[0] = 0;
    char ico[256];
    snprintf(ico, sizeof ico, "%s/favicon.ico", origin);
    if (dec[0]) {
        snprintf(g_cand[slot][0], sizeof g_cand[0][0], "%s", dec);
        snprintf(g_cand[slot][1], sizeof g_cand[0][0], "%s", ico);
    } else {
        snprintf(g_cand[slot][0], sizeof g_cand[0][0], "%s", ico);
        g_cand[slot][1][0] = 0;
    }
    g_try[slot] = 0;
    g_slot[slot].state = FAV_WANTED;
    return &g_slot[slot];
}

int favicon_pending(void) {
    for (int i = 0; i < FAVICON_SLOTS; i++)
        if (g_slot[i].state == FAV_WANTED || g_slot[i].state == FAV_LOADING) return 1;
    return 0;
}

/* Box-average `sw x sh` BGRA down to FAVICON_PX square. A favicon is drawn at
 * sixteen pixels and every one of them matters, so this averages the whole
 * source block rather than picking a nearest sample -- point-sampling a 256px
 * logo down to 32 throws away 98% of it and lands on whichever pixel happened
 * to be under the grid. Premultiplied input averages correctly component-wise,
 * which is the whole reason the decoders produce it. */
static void downsample(const uint32_t *s, uint32_t sw, uint32_t sh, uint32_t *d) {
    for (uint32_t y = 0; y < FAVICON_PX; y++) {
        uint32_t y0 = y * sh / FAVICON_PX, y1 = (y + 1) * sh / FAVICON_PX;
        if (y1 <= y0) y1 = y0 + 1;
        for (uint32_t x = 0; x < FAVICON_PX; x++) {
            uint32_t x0 = x * sw / FAVICON_PX, x1 = (x + 1) * sw / FAVICON_PX;
            if (x1 <= x0) x1 = x0 + 1;
            uint32_t b = 0, g = 0, r = 0, a = 0, n = 0;
            for (uint32_t yy = y0; yy < y1 && yy < sh; yy++)
                for (uint32_t xx = x0; xx < x1 && xx < sw; xx++) {
                    uint32_t p = s[yy * sw + xx];
                    b += p & 255u; g += (p >> 8) & 255u; r += (p >> 16) & 255u; a += p >> 24;
                    n++;
                }
            if (!n) n = 1;
            d[y * FAVICON_PX + x] = (b / n) | ((g / n) << 8) | ((r / n) << 16) | ((a / n) << 24);
        }
    }
}

/* Decode whatever arrived, by SIGNATURE -- a server serves a PNG from
 * /favicon.ico as readily as an .ico, and MDN actually does. */
static int decode_into(int slot, size_t len) {
    const uint8_t *b = (const uint8_t *)g_src;
    uint32_t w = 0, h = 0;
    int rc;
    if (ico_is(b, len)) {
        rc = ico_decode(b, len, FAVICON_PX, g_dec, sizeof g_dec,
                        g_scratch, sizeof g_scratch, &w, &h);
        if (rc != ICO_OK) return -1;
    } else if (len > 3 && b[0] == 0xFF && b[1] == 0xD8) {
        if (jpeg_probe(b, len, &w, &h) != 0) return -1;
        if (w > FAV_DEC_DIM || h > FAV_DEC_DIM) return -1;
        if (jpeg_decode(b, len, g_dec, sizeof g_dec, g_scratch, sizeof g_scratch, &w, &h) != 0)
            return -1;
    } else {
        if (png_probe(b, len, &w, &h) != 0) return -1;
        if (w > FAV_DEC_DIM || h > FAV_DEC_DIM) return -1;
        if (png_decode(b, len, g_dec, sizeof g_dec, g_scratch, sizeof g_scratch, &w, &h) != PNG_OK)
            return -1;
    }
    if (!w || !h) return -1;
    downsample(g_dec, w, h, g_px[slot]);
    g_slot[slot].px = g_px[slot];
    g_slot[slot].w = g_slot[slot].h = FAVICON_PX;
    return 0;
}

static void finish(int slot, const struct vnet_result *res) {
    struct favicon *s = &g_slot[slot];
    if (s->state != FAV_LOADING) return;

    int ok = res->len > 0 && res->status >= 200 && res->status < 300 &&
             decode_into(slot, res->len) == 0;
    if (ok) { s->state = FAV_READY; return; }

    /* The other candidate, once. A site with a broken /favicon.ico and a real
     * declared PNG (or the reverse) is common enough that one retry is the
     * difference between an icon and a blank. */
    if (g_try[slot] == 0 && g_cand[slot][1][0]) {
        g_try[slot] = 1;
        s->state = FAV_WANTED;
        return;
    }
    s->state = FAV_FAILED;
}

int favicon_pump(void) {
    int changed = 0;

    if (g_inflight >= 0) {
        struct vnet_result res;
        int r = fetchjob_poll(FAV_TAG, &res);
        if (r == 1)      { finish(g_inflight, &res); g_inflight = -1; changed = 1; }
        else if (r < 0)  {
            if (g_slot[g_inflight].state == FAV_LOADING) g_slot[g_inflight].state = FAV_FAILED;
            g_inflight = -1; changed = 1;
        }
    }

    /* An icon is the LAST thing anyone is waiting for. It never starts while
     * the document, its stylesheets or its pictures are still coming: the tab
     * decoration must not take a turn on the worker ahead of the page. */
    if (g_inflight < 0 && !fetchjob_busy()) {
        for (int i = 0; i < FAVICON_SLOTS; i++) {
            if (g_slot[i].state != FAV_WANTED) continue;
            const char *u = g_cand[i][g_try[i]];
            if (!u[0]) { g_slot[i].state = FAV_FAILED; changed = 1; break; }
            if (fetchjob_start(u, g_src, sizeof g_src, FAV_TAG) == 0) {
                g_slot[i].state = FAV_LOADING;
                g_inflight = i;
                changed = 1;
            }
            break;
        }
    }
    return changed;
}

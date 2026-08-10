/* ports/netsurf/frontend/text.c -- the OS's own typeface, in NetSurf.
 *
 * The frontend owes the core two things about text and they must agree: how
 * WIDE a string is (layout.c asks, at every line break) and what it LOOKS like
 * (plot.c draws it). Answer them from two different sources and the caret
 * lands beside the glyph it is supposed to be in.
 *
 * So both come from here, and here is ui/backend/font.c -- the OS's TrueType
 * parser, rasteriser and glyph cache, compiled straight into the browser. That
 * file needs nothing but malloc and memcpy (its one line of toolkit coupling
 * is behind FONT_NO_BACKEND), so a browser with no EmUI in it renders in the
 * same typeface as the rest of the system, from the same file on the image.
 *
 * The atlas is 8-bit coverage. Drawing is therefore a per-pixel blend of one
 * colour, not a bitmap copy -- and it is the only per-pixel work in the plot
 * path, which is why the loop below is written with the integer arithmetic
 * this project has twice had to learn to use (docs/PROJECT_STATUS.md, the
 * 4946ms frame).
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "utils/errors.h"
#include "netsurf/plot_style.h"

#include "emblink.h"
#include "font.h"

/* Where the OS keeps its typeface. Overridable at run time for the host
 * build, the same way the resources are -- see fetch.c. */
#ifndef EMBLINK_FONT_PATH
#define EMBLINK_FONT_PATH "/system/fonts/font.ttf"
#endif

static struct font *g_font;
static struct glyph_atlas *g_atlas;
static bool g_tried;

/* Loaded ONCE, on the first question anyone asks about text. Not at startup:
 * the core asks for text metrics long before it draws anything, and a browser
 * that fails to start because a font is missing is worse than one that lays
 * out with fallback numbers. */
static bool font_ready(void)
{
    if (g_tried) return g_font != NULL;
    g_tried = true;

    const char *path = getenv("NSFONT");
    if (path == NULL || path[0] == '\0') path = EMBLINK_FONT_PATH;

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "nsemblink: no font at %s -- text will be measured "
                        "but not drawn\n", path);
        return false;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return false; }

    uint8_t *buf = malloc((size_t)n);
    if (buf == NULL) { fclose(f); return false; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return false; }
    fclose(f);

    /* The font keeps the buffer -- it parses tables out of it lazily, so this
     * is deliberately never freed. */
    uint32_t h = font_load(buf, (size_t)n);
    if (h == 0) { free(buf); return false; }
    g_font = font_for_handle(h);
    g_atlas = font_global_atlas();
    return g_font != NULL && g_atlas != NULL;
}

/* plot_font_style carries its size in points, scaled by PLOT_STYLE_SCALE. */
static float px_of(const struct plot_font_style *fstyle)
{
    float px = (float)fstyle->size * 4.0f / (3.0f * (float)PLOT_STYLE_SCALE);
    return px > 1.0f ? px : 1.0f;
}

/* --- measurement ---------------------------------------------------------- */

int emblink_text_advance(const struct plot_font_style *fstyle,
                         const char *utf8, size_t len)
{
    float px = px_of(fstyle);

    if (!font_ready()) {
        /* No typeface: count CHARACTERS, not bytes -- a two-byte e-acute is
         * one glyph, and charging it two advances wraps a page of accents
         * early. Half an em is the measured average of the real face, so the
         * layout is wrong by a fraction rather than by a factor. */
        size_t chars = 0;
        for (size_t i = 0; i < len; i++)
            if ((utf8[i] & 0xC0) != 0x80) chars++;
        int num = (fstyle->family == PLOT_FONT_FAMILY_MONOSPACE) ? 6 : 5;
        return (int)((chars * (size_t)px * (size_t)num) / 10);
    }

    float w = 0;
    size_t i = 0;
    while (i < len) {
        uint32_t cp = 0;
        int used = font_utf8_decode(utf8 + i, &cp);
        if (used <= 0) break;
        if ((size_t)used > len - i) break;
        i += (size_t)used;
        struct glyph_cache_entry *g =
            glyph_cache_lookup_or_rasterize(g_atlas, g_font, cp, px);
        if (g != NULL) w += g->advance_px;
    }
    return (int)(w + 0.5f);
}

/* --- drawing -------------------------------------------------------------- */

/* `y` is the BASELINE, which is what NetSurf passes and what a font's
 * bearing is measured from. Drawing at the top instead puts every line one
 * ascent too low, which reads as "the text is offset" rather than as a
 * baseline mistake. */
void emblink_text_draw(struct emblink_surface *s, int x, int y,
                       const struct plot_font_style *fstyle,
                       const char *utf8, size_t len)
{
    if (s == NULL || !font_ready()) return;

    float px = px_of(fstyle);
    /* NetSurf's colour is 0xAABBGGRR with red LOW; the surface is 0x00RRGGBB.
     * Same exchange as plot.c's fills -- see ns_to_surface there. */
    uint32_t c = fstyle->foreground;
    unsigned fr = c & 0xFF, fg = (c >> 8) & 0xFF, fb = (c >> 16) & 0xFF;

    float pen = (float)x;
    size_t i = 0;
    while (i < len) {
        uint32_t cp = 0;
        int used = font_utf8_decode(utf8 + i, &cp);
        if (used <= 0) break;
        if ((size_t)used > len - i) break;
        i += (size_t)used;

        struct glyph_cache_entry *g =
            glyph_cache_lookup_or_rasterize(g_atlas, g_font, cp, px);
        if (g == NULL) continue;

        int gx = (int)(pen + 0.5f) + g->bearing_x;
        int gy = y - g->bearing_y;
        pen += g->advance_px;
        if (g->atlas_w == 0 || g->atlas_h == 0) continue;    /* a space */

        int x0 = gx, y0 = gy, x1 = gx + g->atlas_w, y1 = gy + g->atlas_h;
        if (x0 < s->cx0) x0 = s->cx0;
        if (y0 < s->cy0) y0 = s->cy0;
        if (x1 > s->cx1) x1 = s->cx1;
        if (y1 > s->cy1) y1 = s->cy1;
        if (x1 <= x0 || y1 <= y0) continue;

        for (int py = y0; py < y1; py++) {
            const uint8_t *cov = &g_atlas->coverage[g->atlas_y + (py - gy)][g->atlas_x];
            uint32_t *row = s->px + (size_t)py * s->stride;
            for (int pxx = x0; pxx < x1; pxx++) {
                unsigned a = cov[pxx - gx];
                if (a == 0) continue;
                if (a == 255) {
                    row[pxx] = (fr << 16) | (fg << 8) | fb;
                    continue;
                }
                uint32_t d = row[pxx];
                unsigned dr = (d >> 16) & 0xFF, dg = (d >> 8) & 0xFF, db = d & 0xFF;
                row[pxx] = (uint32_t)(((fr * a + dr * (255 - a)) / 255) << 16) |
                           (uint32_t)(((fg * a + dg * (255 - a)) / 255) << 8) |
                           (uint32_t)((fb * a + db * (255 - a)) / 255);
            }
        }
    }
}

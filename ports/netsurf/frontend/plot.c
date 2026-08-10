/* ports/netsurf/frontend/plot.c -- NetSurf's drawing, in our pixels.
 *
 * This is the whole output side of the port. The core lays a page out and then
 * calls exactly these twelve functions to draw it; everything a reader sees
 * comes through here. There is no clever surface abstraction underneath -- a
 * span of 32-bit pixels and a clip rectangle -- because the OS's compositor
 * takes exactly that, and anything in between would be a format conversion per
 * frame for nothing (kernel/gfx/compositor.c, and the lesson in
 * docs/PROJECT_STATUS.md about a per-pixel float blend costing 4946ms).
 *
 * Text is drawn by text.c, from the OS's own typeface -- the same font file
 * the desktop uses, through the same rasteriser. Measurement lives there too,
 * deliberately: the two have to agree exactly or a caret lands beside the
 * glyph it is supposed to be inside.
 */
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "utils/errors.h"
#include "netsurf/bitmap.h"
#include "netsurf/plotters.h"
#include "netsurf/plot_style.h"
#include "emblink.h"


struct emblink_surface *emblink_target;

/* --- the surface ---------------------------------------------------------- */

void emblink_surface_clip(struct emblink_surface *s, int x0, int y0, int x1, int y1)
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > s->width)  x1 = s->width;
    if (y1 > s->height) y1 = s->height;
    /* An inverted clip is not an error the core should have to avoid making;
     * it means "nothing", and expressing that as an empty rect keeps every
     * loop below trivially correct. */
    if (x1 < x0) x1 = x0;
    if (y1 < y0) y1 = y0;
    s->cx0 = x0; s->cy0 = y0; s->cx1 = x1; s->cy1 = y1;
}

/* NetSurf's colour word is 0xAABBGGRR: RED in the LOW byte, and an alpha where
 * 0 means OPAQUE -- both the inverse of what everything else here uses. The
 * surface is 0x00RRGGBB, which is what the compositor takes, so the channels
 * have to be exchanged rather than copied. Copying them straight through is a
 * bug that looks like a design decision: the first page rendered came out with
 * its three columns in believable-but-wrong colours, and the fastest way to be
 * sure was to read the pixel back and compare it to the stylesheet. */
static inline uint32_t ns_to_surface(uint32_t c)
{
    return ((c & 0x000000FFu) << 16) |      /* R */
           ( c & 0x0000FF00u)        |      /* G */
           ((c & 0x00FF0000u) >> 16);       /* B */
}

/* One solid rectangle, clipped, with the alpha of `argb` honoured as
 * source-over. */
void emblink_surface_fill(struct emblink_surface *s, int x0, int y0, int x1, int y1,
                          uint32_t argb)
{
    if (x0 < s->cx0) x0 = s->cx0;
    if (y0 < s->cy0) y0 = s->cy0;
    if (x1 > s->cx1) x1 = s->cx1;
    if (y1 > s->cy1) y1 = s->cy1;
    if (x1 <= x0 || y1 <= y0) return;

    unsigned a = 255 - ((argb >> 24) & 0xFF);      /* NetSurf: 0 == opaque */
    if (a == 0) return;

    uint32_t rgb = ns_to_surface(argb);
    if (a == 255) {                                 /* the common case: no blend */
        for (int y = y0; y < y1; y++) {
            uint32_t *row = s->px + (size_t)y * s->stride;
            for (int x = x0; x < x1; x++) row[x] = rgb;
        }
        return;
    }
    unsigned sr = (rgb >> 16) & 0xFF, sg = (rgb >> 8) & 0xFF, sb = rgb & 0xFF;
    for (int y = y0; y < y1; y++) {
        uint32_t *row = s->px + (size_t)y * s->stride;
        for (int x = x0; x < x1; x++) {
            uint32_t d = row[x];
            unsigned dr = (d >> 16) & 0xFF, dg = (d >> 8) & 0xFF, db = d & 0xFF;
            row[x] = (uint32_t)(((sr * a + dr * (255 - a)) / 255) << 16) |
                     (uint32_t)(((sg * a + dg * (255 - a)) / 255) << 8) |
                     (uint32_t)(((sb * a + db * (255 - a)) / 255));
        }
    }
}

/* --- the plotters --------------------------------------------------------- */

static nserror p_clip(const struct redraw_context *ctx, const struct rect *clip)
{
    (void)ctx;
    if (emblink_target == NULL) return NSERROR_OK;
    emblink_surface_clip(emblink_target, clip->x0, clip->y0, clip->x1, clip->y1);
    return NSERROR_OK;
}

static nserror p_rectangle(const struct redraw_context *ctx,
                           const plot_style_t *style, const struct rect *r)
{
    (void)ctx;
    if (emblink_target == NULL) return NSERROR_OK;

    if (style->fill_type != PLOT_OP_TYPE_NONE)
        emblink_surface_fill(emblink_target, r->x0, r->y0, r->x1, r->y1,
                             style->fill_colour);

    if (style->stroke_type != PLOT_OP_TYPE_NONE) {
        int w = style->stroke_width / PLOT_STYLE_SCALE;
        if (w < 1) w = 1;
        uint32_t c = style->stroke_colour;
        emblink_surface_fill(emblink_target, r->x0, r->y0, r->x1, r->y0 + w, c);
        emblink_surface_fill(emblink_target, r->x0, r->y1 - w, r->x1, r->y1, c);
        emblink_surface_fill(emblink_target, r->x0, r->y0, r->x0 + w, r->y1, c);
        emblink_surface_fill(emblink_target, r->x1 - w, r->y0, r->x1, r->y1, c);
    }
    return NSERROR_OK;
}

/* A line, by Bresenham, thickened across the minor axis. Rules and borders are
 * axis-aligned almost always, and those come out exact. */
static nserror p_line(const struct redraw_context *ctx,
                      const plot_style_t *style, const struct rect *line)
{
    (void)ctx;
    if (emblink_target == NULL) return NSERROR_OK;
    if (style->stroke_type == PLOT_OP_TYPE_NONE) return NSERROR_OK;

    int w = style->stroke_width / PLOT_STYLE_SCALE;
    if (w < 1) w = 1;
    int x0 = line->x0, y0 = line->y0, x1 = line->x1, y1 = line->y1;
    uint32_t c = style->stroke_colour;

    if (y0 == y1) {                                   /* horizontal */
        if (x1 < x0) { int t = x0; x0 = x1; x1 = t; }
        emblink_surface_fill(emblink_target, x0, y0, x1, y0 + w, c);
        return NSERROR_OK;
    }
    if (x0 == x1) {                                   /* vertical */
        if (y1 < y0) { int t = y0; y0 = y1; y1 = t; }
        emblink_surface_fill(emblink_target, x0, y0, x0 + w, y1, c);
        return NSERROR_OK;
    }
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        emblink_surface_fill(emblink_target, x0, y0, x0 + w, y0 + w, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
    return NSERROR_OK;
}

/* A convex-or-not polygon, by scanline. `p` is x,y pairs. */
static nserror p_polygon(const struct redraw_context *ctx,
                         const plot_style_t *style, const int *p, unsigned int n)
{
    (void)ctx;
    if (emblink_target == NULL || n < 3) return NSERROR_OK;
    if (style->fill_type == PLOT_OP_TYPE_NONE) return NSERROR_OK;

    int ymin = p[1], ymax = p[1];
    for (unsigned i = 1; i < n; i++) {
        if (p[i * 2 + 1] < ymin) ymin = p[i * 2 + 1];
        if (p[i * 2 + 1] > ymax) ymax = p[i * 2 + 1];
    }
    if (ymin < emblink_target->cy0) ymin = emblink_target->cy0;
    if (ymax > emblink_target->cy1) ymax = emblink_target->cy1;

    /* 64 crossings is more than any shape a page draws through this path; a
     * polygon with more simply loses its extra spans rather than the stack. */
    for (int y = ymin; y < ymax; y++) {
        int xs[64];
        unsigned nx = 0;
        for (unsigned i = 0; i < n && nx < 64; i++) {
            unsigned j = (i + 1) % n;
            int y0 = p[i * 2 + 1], y1 = p[j * 2 + 1];
            if ((y0 <= y && y1 > y) || (y1 <= y && y0 > y)) {
                int x0 = p[i * 2], x1 = p[j * 2];
                xs[nx++] = x0 + (y - y0) * (x1 - x0) / (y1 - y0);
            }
        }
        for (unsigned a = 1; a < nx; a++) {           /* insertion sort */
            int v = xs[a]; unsigned b = a;
            while (b > 0 && xs[b - 1] > v) { xs[b] = xs[b - 1]; b--; }
            xs[b] = v;
        }
        for (unsigned a = 0; a + 1 < nx; a += 2)
            emblink_surface_fill(emblink_target, xs[a], y, xs[a + 1], y + 1,
                                 style->fill_colour);
    }
    return NSERROR_OK;
}

static nserror p_disc(const struct redraw_context *ctx,
                      const plot_style_t *style, int x, int y, int radius)
{
    (void)ctx;
    if (emblink_target == NULL || radius <= 0) return NSERROR_OK;
    uint32_t c = (style->fill_type != PLOT_OP_TYPE_NONE) ? style->fill_colour
                                                         : style->stroke_colour;
    for (int dy = -radius; dy <= radius; dy++) {
        int span = (int)(0.5 + __builtin_sqrt((double)(radius * radius - dy * dy)));
        emblink_surface_fill(emblink_target, x - span, y + dy, x + span, y + dy + 1, c);
    }
    return NSERROR_OK;
}

/* Arcs are used for one thing in a document -- rounded corners on a form
 * control -- and drawing one as nothing is invisible; drawing one wrong is a
 * smear across the widget. Nothing, for now, and named in docs/TODO.md. */
static nserror p_arc(const struct redraw_context *ctx, const plot_style_t *style,
                     int x, int y, int radius, int angle1, int angle2)
{
    (void)ctx; (void)style; (void)x; (void)y; (void)radius; (void)angle1; (void)angle2;
    return NSERROR_OK;
}

static nserror p_path(const struct redraw_context *ctx, const plot_style_t *pstyle,
                      const float *p, unsigned int n, const float transform[6])
{
    (void)ctx; (void)pstyle; (void)p; (void)n; (void)transform;
    return NSERROR_OK;      /* SVG paths: the scanline filler comes across next */
}

/* A decoded picture, scaled with a nearest-neighbour stepper. Nearest and not
 * bilinear on purpose: this runs under TCG where a float per pixel is poison
 * (docs/PROJECT_STATUS.md), and a 16.16 integer step is what the OS's own blit
 * already uses. */
static nserror p_bitmap(const struct redraw_context *ctx, struct bitmap *bitmap,
                        int x, int y, int width, int height,
                        colour bg, bitmap_flags_t flags)
{
    (void)ctx; (void)bg; (void)flags;
    struct emblink_surface *s = emblink_target;
    if (s == NULL || bitmap == NULL || width <= 0 || height <= 0) return NSERROR_OK;

    const struct gui_bitmap_table *bt = emblink_bitmap_table;
    unsigned char *src = bt->get_buffer(bitmap);
    int sw = bt->get_width(bitmap), sh = bt->get_height(bitmap);
    size_t stride = bt->get_rowstride(bitmap);
    if (src == NULL || sw <= 0 || sh <= 0) return NSERROR_OK;

    int x0 = x < s->cx0 ? s->cx0 : x, y0 = y < s->cy0 ? s->cy0 : y;
    int x1 = x + width  > s->cx1 ? s->cx1 : x + width;
    int y1 = y + height > s->cy1 ? s->cy1 : y + height;

    uint32_t stepx = (uint32_t)(((int64_t)sw << 16) / width);
    uint32_t stepy = (uint32_t)(((int64_t)sh << 16) / height);

    for (int py = y0; py < y1; py++) {
        uint32_t sy = (uint32_t)(py - y) * stepy;
        const unsigned char *srow = src + (size_t)(sy >> 16) * stride;
        uint32_t *drow = s->px + (size_t)py * s->stride;
        uint32_t sx = (uint32_t)(x0 - x) * stepx;
        for (int px = x0; px < x1; px++, sx += stepx) {
            const unsigned char *sp = srow + (size_t)(sx >> 16) * 4;
            unsigned a = sp[3];
            if (a == 0) continue;
            /* the decoded buffer is R,G,B,A in MEMORY order (bitmap.c) */
            if (a == 255) {
                drow[px] = ((uint32_t)sp[0] << 16) | ((uint32_t)sp[1] << 8) | (uint32_t)sp[2];
            } else {
                uint32_t d = drow[px];
                unsigned dr = (d >> 16) & 0xFF, dg = (d >> 8) & 0xFF, db = d & 0xFF;
                drow[px] = (uint32_t)(((sp[0] * a + dr * (255 - a)) / 255) << 16) |
                           (uint32_t)(((sp[1] * a + dg * (255 - a)) / 255) << 8) |
                           (uint32_t)((sp[2] * a + db * (255 - a)) / 255);
            }
        }
    }
    return NSERROR_OK;
}

static nserror p_text(const struct redraw_context *ctx, const plot_font_style_t *fstyle,
                      int x, int y, const char *text, size_t length)
{
    (void)ctx;
    if (emblink_target == NULL) return NSERROR_OK;
    emblink_text_draw(emblink_target, x, y, fstyle, text, length);
    return NSERROR_OK;
}

static nserror p_group_start(const struct redraw_context *ctx, const char *name)
{
    (void)ctx; (void)name;
    return NSERROR_OK;
}

static nserror p_group_end(const struct redraw_context *ctx)
{
    (void)ctx;
    return NSERROR_OK;
}

static nserror p_flush(const struct redraw_context *ctx)
{
    (void)ctx;
    return NSERROR_OK;
}

const struct plotter_table emblink_plotters = {
    .clip = p_clip,
    .arc = p_arc,
    .disc = p_disc,
    .line = p_line,
    .rectangle = p_rectangle,
    .polygon = p_polygon,
    .path = p_path,
    .bitmap = p_bitmap,
    .text = p_text,
    .group_start = p_group_start,
    .group_end = p_group_end,
    .flush = p_flush,
    /* Knockout rendering coalesces overlapping opaque boxes before they are
     * drawn. Ours is a plain fill with no overdraw cost worth avoiding, and
     * the knockout path adds a whole second buffer. */
    .option_knockout = false,
};

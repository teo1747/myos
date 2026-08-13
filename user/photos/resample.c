/* user/photos/resample.c -- pixels at one size, pixels at another, well.
 *
 * THE ARGUMENT FOR THIS FILE EXISTING AT ALL.
 *
 * The compositor can already scale an image while blitting it, bilinearly. So
 * the viewer could hand it a 4000x3000 photo, say "draw that at 800x600", and
 * be done in one line. It would also look bad, and understanding why is the
 * whole design of this file.
 *
 * Bilinear interpolation reads FOUR source pixels per destination pixel. When
 * magnifying that is exactly right -- the four nearest samples are the four
 * that matter. When shrinking 5:1 it is wrong in a specific way: each output
 * pixel stands for 25 input pixels, bilinear looks at 4 of them, and the other
 * 21 are discarded without being consulted. Which 4 depends on where the
 * sample grid happens to land, so fine detail -- fabric, foliage, text in a
 * screenshot -- turns into a moire pattern that has nothing to do with the
 * picture. Worse, the pattern MOVES when the image does, because a different
 * subset lands under the grid, so a thumbnail crawls while you scroll.
 *
 * The correct operation for shrinking is an area average: an output pixel is
 * the mean of the input pixels it covers, weighted by how much of each it
 * covers. Every input pixel is read exactly once and contributes exactly its
 * share. There is no sample grid to alias against.
 *
 * SEPARABLE, so the cost is O(w*h) and not O(w*h*ratio^2): shrink horizontally
 * into a strip, then shrink that vertically. Two passes over the data instead
 * of one pass reading a rectangle per output pixel.
 *
 * INTEGER, throughout. This runs on every pixel of every photo, and the lesson
 * the browser taught -- at the cost of a 4946ms frame -- is that per-pixel
 * floating point under emulation is not slow, it is disqualifying. The weights
 * are 16.16 fixed point, the accumulators are 64-bit so no ratio can overflow
 * them, and the per-pixel division is replaced by one reciprocal per output
 * pixel shared across its four channels.
 *
 * PREMULTIPLIED alpha is what makes averaging channels legitimate: in
 * premultiplied form a pixel's colour is already scaled by its coverage, so
 * summing colour and alpha separately gives the right answer. In straight
 * alpha it does not -- a transparent red pixel would drag red into its
 * neighbours -- and every "why does the edge have a halo" bug in image scaling
 * is that mistake.
 */
#include <stdlib.h>
#include <string.h>

#include "photo.h"

/* One axis of the area average.
 *
 * `src` is a run of `sn` pixels with stride `sstride` words; the result is `dn`
 * pixels written with stride `dstride`. Both passes are this function -- the
 * vertical pass is the horizontal pass with the strides exchanged, which is
 * the entire benefit of writing it in terms of strides rather than rows. */
static void axis_average(const uint32_t *src, uint32_t sn, uint32_t sstride,
                         uint32_t *dst, uint32_t dn, uint32_t dstride)
{
    for (uint32_t d = 0; d < dn; d++) {
        /* The half-open source span this output pixel covers, in 16.16.
         * Computed in 64-bit before the shift: at 4-digit dimensions
         * (d * sn) << 16 overflows 32 bits long before the ratio is unusual. */
        uint64_t start = ((uint64_t)d * sn << 16) / dn;
        uint64_t end   = ((uint64_t)(d + 1) * sn << 16) / dn;
        if (end <= start) end = start + 1;          /* never an empty span */

        uint32_t s0 = (uint32_t)(start >> 16);
        uint32_t s1 = (uint32_t)((end + 0xFFFFu) >> 16);
        if (s1 > sn) s1 = sn;

        uint64_t acc[4] = { 0, 0, 0, 0 };
        uint64_t wsum = 0;

        for (uint32_t s = s0; s < s1; s++) {
            uint64_t lo = (uint64_t)s << 16, hi = lo + 65536u;
            uint64_t a = start > lo ? start : lo;
            uint64_t b = end   < hi ? end   : hi;
            if (b <= a) continue;
            uint64_t w = b - a;                     /* 1..65536 */

            uint32_t p = src[(size_t)s * sstride];
            acc[0] += (uint64_t)( p        & 0xFFu) * w;
            acc[1] += (uint64_t)((p >>  8) & 0xFFu) * w;
            acc[2] += (uint64_t)((p >> 16) & 0xFFu) * w;
            acc[3] += (uint64_t)((p >> 24) & 0xFFu) * w;
            wsum += w;
        }
        if (wsum == 0) { dst[(size_t)d * dstride] = 0; continue; }

        /* An exact rounded divide per channel.
         *
         * This started as one reciprocal per pixel and four multiplies, to
         * avoid four 64-bit divides -- and it was wrong. A 24-bit reciprocal
         * of a weight sum that can reach 2^25 loses enough that a FLAT colour
         * came back a level darker than it went in, which the test caught as
         * "the average of one value is not that value". Every resize dimming
         * the picture slightly is the kind of bug that is invisible in
         * isolation, obvious in a gradient, and cumulative when a viewer
         * rescales on every window resize.
         *
         * The divides are affordable because this runs when the VIEW changes,
         * not per frame, and over a viewport-sized output rather than the
         * whole picture. Correct first; this was never the hot path it
         * looked like. */
        uint32_t out = 0;
        for (int c = 0; c < 4; c++) {
            uint64_t v = (acc[c] + wsum / 2) / wsum;
            if (v > 255) v = 255;
            out |= (uint32_t)v << (c * 8);
        }
        dst[(size_t)d * dstride] = out;
    }
}

/* Area-average a sub-rectangle down to dw x dh. */
static uint32_t *crop_downscale(const uint32_t *src, uint32_t sw,
                                uint32_t x0, uint32_t y0, uint32_t cw, uint32_t ch,
                                uint32_t dw, uint32_t dh)
{
    /* The intermediate is dw wide and STILL ch tall: shrinking the horizontal
     * axis first means the vertical pass walks the narrow buffer, which is
     * both less memory and less work than the other order. */
    uint32_t *tmp = malloc((size_t)dw * ch * 4);
    uint32_t *dst = malloc((size_t)dw * dh * 4);
    if (!tmp || !dst) { free(tmp); free(dst); return NULL; }

    for (uint32_t y = 0; y < ch; y++)
        axis_average(src + (size_t)(y0 + y) * sw + x0, cw, 1,
                     tmp + (size_t)y * dw, dw, 1);

    for (uint32_t x = 0; x < dw; x++)
        axis_average(tmp + x, ch, dw,
                     dst + x, dh, dw);

    free(tmp);
    return dst;
}

/* Lift a sub-rectangle out at 1:1, for the magnifying case. */
static uint32_t *crop_copy(const uint32_t *src, uint32_t sw,
                           uint32_t x0, uint32_t y0, uint32_t cw, uint32_t ch)
{
    uint32_t *dst = malloc((size_t)cw * ch * 4);
    if (!dst) return NULL;
    for (uint32_t y = 0; y < ch; y++)
        memcpy(dst + (size_t)y * cw,
               src + (size_t)(y0 + y) * sw + x0, (size_t)cw * 4);
    return dst;
}

uint32_t *photo_view(const uint32_t *src, uint32_t sw, uint32_t sh,
                     uint32_t x0, uint32_t y0, uint32_t cw, uint32_t ch,
                     uint32_t dw, uint32_t dh,
                     uint32_t *out_w, uint32_t *out_h)
{
    if (!src || !sw || !sh || !cw || !ch || !dw || !dh) return NULL;
    /* Clamp the crop INSIDE the picture here rather than trusting the caller.
     * Every pan and zoom in the app produces one of these rectangles, and a
     * rounding error of one pixel at the edge is an out-of-bounds read of a
     * 48MB buffer -- a crash the user would see as "it died on that photo". */
    if (x0 >= sw) x0 = sw - 1;
    if (y0 >= sh) y0 = sh - 1;
    if (x0 + cw > sw) cw = sw - x0;
    if (y0 + ch > sh) ch = sh - y0;

    if (dw < cw || dh < ch) {
        *out_w = dw; *out_h = dh;
        return crop_downscale(src, sw, x0, y0, cw, ch, dw, dh);
    }
    *out_w = cw; *out_h = ch;
    return crop_copy(src, sw, x0, y0, cw, ch);
}

uint32_t *photo_downscale(const uint32_t *src, uint32_t sw, uint32_t sh,
                          uint32_t dw, uint32_t dh)
{
    if (!src || !sw || !sh || !dw || !dh) return NULL;
    return crop_downscale(src, sw, 0, 0, sw, sh, dw, dh);
}

/* --- orientation ---------------------------------------------------------- *
 * The eight EXIF cases as one loop over the DESTINATION, mapping each output
 * pixel back to its source. Walking the destination rather than the source is
 * what keeps the output writes sequential -- and on a rotation the reads have
 * to be scattered one way or the other, so it may as well be the reads. */
uint32_t *photo_orient(const uint32_t *src, uint32_t sw, uint32_t sh,
                       int orientation, uint32_t *out_w, uint32_t *out_h)
{
    if (orientation < 2 || orientation > 8) return NULL;

    /* 5..8 are the transposing cases, and they SWAP the dimensions. This is
     * the part a viewer gets wrong quietly: rotate the pixels, keep the old
     * width, and every portrait photo is squeezed into a landscape box. */
    bool swap = (orientation >= 5);
    uint32_t dw = swap ? sh : sw;
    uint32_t dh = swap ? sw : sh;

    uint32_t *dst = malloc((size_t)dw * dh * 4);
    if (!dst) return NULL;

    for (uint32_t y = 0; y < dh; y++) {
        for (uint32_t x = 0; x < dw; x++) {
            uint32_t sx, sy;
            switch (orientation) {
            case 2: sx = sw - 1 - x; sy = y;          break;  /* mirror       */
            case 3: sx = sw - 1 - x; sy = sh - 1 - y; break;  /* 180          */
            case 4: sx = x;          sy = sh - 1 - y; break;  /* flip         */
            case 5: sx = y;          sy = x;          break;  /* transpose    */
            case 6: sx = y;          sy = sh - 1 - x; break;  /* 90 CW        */
            case 7: sx = sw - 1 - y; sy = sh - 1 - x; break;  /* transverse   */
            default:sx = sw - 1 - y; sy = x;          break;  /* 8: 90 CCW    */
            }
            dst[(size_t)y * dw + x] = src[(size_t)sy * sw + sx];
        }
    }

    *out_w = dw;
    *out_h = dh;
    return dst;
}

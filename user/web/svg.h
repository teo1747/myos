/* user/web/svg.h -- the pictures that are DESCRIBED rather than sampled.
 *
 * PNG and JPEG arrive as pixels; an SVG arrives as instructions, and until
 * something followed them every icon on the modern web was a grey box. That is
 * 23 of the 138 images across the seventeen sites in `make web-real`, plus
 * every inline `<svg>` -- which is nearly every button on a page written this
 * decade. Brave's search header is entirely icons, and it read as a row of
 * empty rectangles for exactly this reason.
 *
 * So this is a renderer, not a decoder, and it is deliberately a SMALL one. It
 * covers what icons are actually made of:
 *
 *   - <path> with the M/L/H/V/C/S/Q/T/A/Z grammar, absolute and relative;
 *   - <rect> (with rx/ry), <circle>, <ellipse>, <line>, <polygon>, <polyline>;
 *   - <g> nesting, with transform= translate/scale/rotate/matrix, and fill
 *     inherited down;
 *   - fill (hex, the common names, none, currentColor), fill-rule, opacity,
 *     and stroke as a width along the path.
 *
 * It does NOT do gradients, filters, clip paths, masks, text or CSS inside the
 * document. Those are what a full SVG engine is; an icon uses none of them,
 * and a renderer that half-did them would produce a wrong picture instead of a
 * plain one.
 *
 * The output is what every other image source here produces -- premultiplied
 * BGRA, so the compositor blits it without knowing where it came from. No
 * allocation: the caller provides the pixels and the scratch, the same
 * contract png.c and jpeg.c already keep.
 */
#ifndef _EMBLINK_WEB_SVG_H_
#define _EMBLINK_WEB_SVG_H_

#include <stdint.h>
#include <stddef.h>

/* Do these bytes look like SVG, and how big does the document say it is?
 * Returns 0 and fills w/h, or -1. An SVG has no intrinsic pixel size the way a
 * PNG does -- width/height may be percentages, or absent with only a viewBox --
 * so this answers with the document's own numbers when it has them and a
 * sensible default when it does not. The caller may render at any size. */
int svg_probe(const uint8_t *src, size_t len, uint32_t *w, uint32_t *h);

/* Render into `out` (w*h premultiplied BGRA, transparent where nothing is
 * painted). `scratch` holds the edge list and the coverage row. Returns 0, or
 * -1 if the document could not be followed or the buffers are too small.
 *
 * `ink` is what `currentColor` and a missing fill resolve to -- for an inline
 * icon that is the surrounding text colour, which is how icons take on the
 * colour of the thing they sit in. 0xAARRGGBB. */
int svg_render(const uint8_t *src, size_t len, uint32_t *out, size_t out_bytes,
               uint8_t *scratch, size_t scratch_bytes,
               uint32_t w, uint32_t h, uint32_t ink);

/* The imgcache-shaped spelling: same signature as png_decode/jpeg_decode, so
 * the cache dispatches on the bytes without a special case. Renders at the
 * size svg_probe reported. */
int svg_decode(const uint8_t *src, size_t len, uint32_t *out, size_t out_bytes,
               uint8_t *scratch, size_t scratch_bytes, uint32_t *w, uint32_t *h);

#endif /* _EMBLINK_WEB_SVG_H_ */

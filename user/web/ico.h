/* user/web/ico.h -- the format the web's favicons are actually in.
 *
 * A favicon is very nearly always an .ico, and .ico is not one format: it is a
 * container holding several sizes of the same picture, each of which is EITHER
 * a whole PNG file OR a Windows DIB. Of the sites this browser is tested
 * against, Hacker News ships one 256x256 PNG inside it, GitHub two 32-bit DIBs,
 * Wikipedia three 4-bit palettised ones, and xkcd an 8-bit and a 4-bit. A
 * decoder that handled only one of those would show an icon for roughly one
 * site in three.
 *
 * The DIB inside an .ico differs from a standalone .bmp in two ways worth
 * stating, because both are silent if you get them wrong: the header claims
 * DOUBLE the real height (it counts the AND mask as more rows), and for
 * anything under 32 bits per pixel that AND mask -- one bit per pixel, packed
 * after the colour rows -- is the only transparency there is. Skip it and every
 * icon comes out as a rectangle of background.
 *
 * Rows are bottom-up and padded to a 4-byte boundary, both for the colour data
 * and for the mask, which are padded independently.
 *
 * Output is BGRA8888-premultiplied, the same as png.c and jpeg.c produce, so
 * everything downstream treats a decoded icon like any other picture.
 */
#ifndef _EMBLINK_WEB_ICO_H_
#define _EMBLINK_WEB_ICO_H_

#include <stdint.h>
#include <stddef.h>

enum {
    ICO_OK       =  0,
    ICO_ENOTICO  = -1,   /* not an .ico at all                          */
    ICO_EUNSUP   = -2,   /* a bit depth or compression we do not decode */
    ICO_ETOOBIG  = -3,   /* would not fit the caller's buffers          */
    ICO_EDATA    = -4,   /* truncated or self-contradictory             */
};

/* Is this an .ico? Cheap enough to call on any downloaded bytes. */
int ico_is(const uint8_t *src, size_t len);

/* The dimensions of the entry ico_decode would choose for `want_px`. Lets a
 * caller reject on size before spending an arena on it. */
int ico_probe(const uint8_t *src, size_t len, unsigned want_px,
              uint32_t *out_w, uint32_t *out_h);

/* Decode the entry closest to `want_px` into `dst` as BGRA8888-premultiplied,
 * `dst_cap` in BYTES. `scratch` is only used when the chosen entry turns out to
 * be a PNG, which needs it for inflated scanlines; DIB entries decode straight
 * out of the source. Preference is for the smallest entry that is at least
 * want_px, because scaling a picture down keeps more of it than scaling one up.
 */
int ico_decode(const uint8_t *src, size_t len, unsigned want_px,
               uint32_t *dst, size_t dst_cap,
               uint8_t *scratch, size_t scratch_cap,
               uint32_t *out_w, uint32_t *out_h);

#endif /* _EMBLINK_WEB_ICO_H_ */

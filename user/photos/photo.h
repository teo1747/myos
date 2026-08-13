/* user/photos/photo.h -- what a picture is, and who does what to it.
 *
 * The viewer is split the way the work splits, not the way the file grew:
 *
 *   decode.c    bytes on disk -> pixels in memory (sniff, decode, orient)
 *   resample.c  pixels at one size -> pixels at another, WELL
 *   album.c     one picture -> the others next to it in the directory
 *   photos.c    the window, the input, the frame
 *
 * Pixels are BGRA8888 PREMULTIPLIED everywhere in here. That is not an
 * arbitrary choice: it is what png.c and jpeg.c already emit and what the
 * toolkit blits without conversion, so the whole path from file to screen has
 * no format conversion in it at all. It is also the representation in which
 * FILTERING is a plain per-channel average -- straight alpha would need an
 * unpremultiply/repremultiply round trip around every resample, and skipping
 * that round trip is how transparent pixels bleed their colour into visible
 * edges.
 */
#ifndef _EMBLINK_PHOTO_H_
#define _EMBLINK_PHOTO_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct {
    uint32_t   *px;            /* BGRA8888 premultiplied, w*h words          */
    uint32_t    w, h;
    const char *format;        /* "PNG" / "JPEG" -- for the info line        */
    size_t      file_bytes;    /* what it cost on disk                       */
    int         orientation;   /* the EXIF value that was applied, 1 if none */
} Photo;

/* Negative return codes. Distinct from the decoders' own so a failure can say
 * which STAGE failed -- "that file is not a picture" and "that picture is too
 * big for the memory we have" are different problems and a viewer that reports
 * both as "cannot open" is a viewer you cannot debug. */
enum {
    PHOTO_OK        =  0,
    PHOTO_ENOENT    = -1,      /* no such file, or unreadable                */
    PHOTO_EFORMAT   = -2,      /* not a format we decode                     */
    PHOTO_EDECODE   = -3,      /* it is a picture, and it is corrupt         */
    PHOTO_EMEMORY   = -4,      /* it decodes, and it does not fit            */
    PHOTO_EUNSUP    = -5,      /* interlaced PNG, progressive JPEG, ...      */
};

/* --- decode.c ----------------------------------------------------------- */
int         photo_load(const char *path, Photo *out);
void        photo_free(Photo *p);
const char *photo_error(int rc);
/* Does this NAME look like something we can open? Used to filter a directory
 * listing without reading every file in it. */
bool        photo_is_image_name(const char *name);

/* --- resample.c --------------------------------------------------------- */
/* Produce exactly what the viewport should show: the sub-rectangle
 * (x0,y0,cw,ch) of the source, resampled to dw x dh, freshly allocated.
 *
 * VIEWPORT-MAPPED rather than scale-then-pan. A viewer that scales the whole
 * photo and pans around the result pays for every pixel it is not showing --
 * at 8x zoom that is 98% of the work, on a picture that may be 48MB before it
 * starts. Resampling the visible crop instead means the cost of a frame is the
 * size of the WINDOW, not the size of the file, so zooming into a 50-megapixel
 * photo costs exactly what zooming into a small one costs.
 *
 * Two regimes, and the split is the whole quality argument:
 *
 *   MINIFYING (dw < cw) -- resample here, by area average, and report the
 *   result at dw x dh for a 1:1 blit. Explained at length in resample.c: the
 *   compositor's bilinear reads 4 source pixels however many the destination
 *   covers, which aliases, and the aliasing crawls when the picture moves.
 *
 *   MAGNIFYING (dw >= cw) -- copy the crop out at 1:1 and let the compositor's
 *   bilinear do the enlarging. Four nearest samples is the RIGHT answer when
 *   magnifying, the blitter already does it well, and doing it here would cost
 *   a second full-size buffer to reach the same pixels.
 *
 * *out_w / *out_h say which regime ran, so the caller knows what size it is
 * holding. Returns NULL if the allocation fails. */
uint32_t *photo_view(const uint32_t *src, uint32_t sw, uint32_t sh,
                     uint32_t x0, uint32_t y0, uint32_t cw, uint32_t ch,
                     uint32_t dw, uint32_t dh,
                     uint32_t *out_w, uint32_t *out_h);

/* Downscale a whole picture by AREA AVERAGE into a freshly allocated buffer.
 *
 * This is the whole quality argument of the viewer. The compositor's blitter
 * rescales bilinearly, which is right for magnifying and WRONG for shrinking:
 * bilinear reads four source pixels no matter how many the destination pixel
 * actually covers, so a 4000px photo shown at 800px throws away 24 of every 25
 * pixels and keeps whatever the sample grid happened to land on. Detail turns
 * into aliasing, and the aliasing CRAWLS when the picture moves. An area
 * average reads every source pixel exactly once, weighted by how much of it
 * the destination pixel covers, which is the correct answer rather than a
 * cheaper approximation of it.
 *
 * Integer arithmetic throughout, deliberately: this runs per pixel over
 * millions of pixels, and the lesson from the browser was that per-pixel
 * floating point is what makes a frame take five seconds under emulation. */
uint32_t *photo_downscale(const uint32_t *src, uint32_t sw, uint32_t sh,
                          uint32_t dw, uint32_t dh);

/* Apply an EXIF orientation (1..8), returning a new buffer and updating the
 * dimensions, which SWAP for the four rotated cases. Returns NULL on failure;
 * orientation 1 is the identity and returns NULL with nothing to do. */
uint32_t *photo_orient(const uint32_t *src, uint32_t sw, uint32_t sh,
                       int orientation, uint32_t *out_w, uint32_t *out_h);

/* --- album.c ------------------------------------------------------------ */
/* The other pictures in the same directory, sorted, so Left/Right walk them.
 * A photo viewer that can only show the file it was given is a decoder with a
 * window; the album is the difference. */
#define ALBUM_MAX 512

typedef struct {
    char  dir[256];
    char  name[ALBUM_MAX][96];
    int   count;
    int   index;               /* which one is on screen, -1 = not in the set */
} Album;

/* Scan the directory containing `path` and position on `path` itself. */
void        album_open(Album *a, const char *path);
/* Full path of entry i into `buf`; NULL if there is no such entry. */
const char *album_path(const Album *a, int i, char *buf, size_t cap);
/* Step by +1/-1 with wraparound; returns the new index, or -1 if empty. */
int         album_step(Album *a, int delta);

#endif /* _EMBLINK_PHOTO_H_ */

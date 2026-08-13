/* user/photos/decode.c -- bytes on disk to pixels in memory.
 *
 * The decoding itself is not here. png.c and jpeg.c were written for the
 * browser and they are ours; this is the layer that decides WHICH of them a
 * file is for, gets it the memory it needs, and fixes up the one thing a
 * decoder is not allowed to fix up by itself.
 *
 * SNIFF THE BYTES, NOT THE NAME. A file called .jpg is very often a PNG --
 * every camera app and messaging client has renamed one at some point -- and a
 * viewer that trusts the extension shows an error on a file that every other
 * viewer opens. The name is used for one thing only: filtering a directory
 * listing cheaply, where being wrong costs a skipped entry rather than a
 * wrong answer.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "photo.h"
#include "png.h"
#include "jpeg.h"

/* A ceiling that exists to produce a MESSAGE instead of a failed allocation
 * halfway through decoding. 80 megapixels is larger than any camera we are
 * likely to meet and small enough that the arithmetic below cannot overflow. */
#define MAX_PIXELS (80u * 1000u * 1000u)

const char *photo_error(int rc)
{
    switch (rc) {
    case PHOTO_OK:      return "ok";
    case PHOTO_ENOENT:  return "cannot read that file";
    case PHOTO_EFORMAT: return "not a picture we can decode (PNG and JPEG)";
    case PHOTO_EDECODE: return "the picture is damaged";
    case PHOTO_EMEMORY: return "not enough memory for a picture that size";
    case PHOTO_EUNSUP:  return "an encoding we do not decode yet "
                               "(interlaced PNG, progressive JPEG)";
    default:            return "unknown error";
    }
}

bool photo_is_image_name(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    static const char *ext[] = { "png", "jpg", "jpeg", "jpe", NULL };
    for (int i = 0; ext[i]; i++) {
        const char *a = dot + 1, *b = ext[i];
        while (*a && *b) {
            char ca = *a >= 'A' && *a <= 'Z' ? (char)(*a + 32) : *a;
            if (ca != *b) break;
            a++; b++;
        }
        if (!*a && !*b) return true;
    }
    return false;
}

/* --- EXIF orientation ---------------------------------------------------- *
 * Phone cameras do not rotate the pixels they write. The sensor reads out the
 * same way whichever way the phone was held, and the fact that it was held
 * sideways is recorded as a NUMBER in the file -- so a picture whose pixels
 * are 4000x3000 is meant to be shown 3000x4000, and a viewer that ignores the
 * number shows every portrait photo lying on its side. That is the single most
 * visible difference between a viewer that feels finished and one that does
 * not, and it is about sixty lines.
 *
 * Only IFD0 and only tag 0x0112 -- this is not an EXIF parser, it is one field
 * lookup, and treating it as one field lookup is why it fits in this file. */
static uint16_t rd16(const uint8_t *p, bool be)
{
    return be ? (uint16_t)((p[0] << 8) | p[1]) : (uint16_t)((p[1] << 8) | p[0]);
}

static uint32_t rd32(const uint8_t *p, bool be)
{
    return be ? ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]
              : ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0];
}

static int exif_orientation(const uint8_t *src, size_t len)
{
    if (len < 4 || src[0] != 0xFF || src[1] != 0xD8) return 1;   /* not JPEG */

    size_t p = 2;
    while (p + 4 <= len) {
        if (src[p] != 0xFF) return 1;              /* lost sync: give up      */
        uint8_t marker = src[p + 1];
        if (marker == 0xD8 || marker == 0x01 ||
            (marker >= 0xD0 && marker <= 0xD7)) { p += 2; continue; }
        if (marker == 0xDA || marker == 0xD9) return 1;   /* image data began */

        uint32_t seglen = (uint32_t)((src[p + 2] << 8) | src[p + 3]);
        if (seglen < 2 || p + 2 + seglen > len) return 1;
        const uint8_t *seg = src + p + 4;
        uint32_t n = seglen - 2;

        if (marker == 0xE1 && n > 14 && memcmp(seg, "Exif\0\0", 6) == 0) {
            const uint8_t *tiff = seg + 6;
            uint32_t tn = n - 6;
            bool be;
            if (tiff[0] == 'M' && tiff[1] == 'M') be = true;
            else if (tiff[0] == 'I' && tiff[1] == 'I') be = false;
            else return 1;
            if (rd16(tiff + 2, be) != 42) return 1;

            uint32_t ifd = rd32(tiff + 4, be);
            if (ifd + 2 > tn) return 1;
            uint16_t entries = rd16(tiff + ifd, be);
            for (uint16_t e = 0; e < entries; e++) {
                uint32_t off = ifd + 2 + (uint32_t)e * 12;
                if (off + 12 > tn) return 1;
                const uint8_t *ent = tiff + off;
                if (rd16(ent, be) == 0x0112) {
                    /* A SHORT stored inline, and the value sits in the first
                     * two bytes of the 4-byte value field -- which on a
                     * big-endian file is not where a naive read looks. */
                    int v = (int)rd16(ent + 8, be);
                    return (v >= 1 && v <= 8) ? v : 1;
                }
            }
            return 1;
        }
        p += 2 + seglen;
    }
    return 1;
}

/* --- loading -------------------------------------------------------------- */

static uint8_t *read_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz <= 0) { fclose(f); return NULL; }
    rewind(f);

    uint8_t *buf = malloc((size_t)sz);
    if (!buf) { fclose(f); return NULL; }

    /* Read in a LOOP. fread on this libc returns what it got, and a short read
     * on a large file over the disk path is normal rather than exceptional --
     * treating one as EOF is how a big photo decodes as "damaged". */
    size_t got = 0;
    while (got < (size_t)sz) {
        size_t n = fread(buf + got, 1, (size_t)sz - got, f);
        if (n == 0) break;
        got += n;
    }
    fclose(f);
    if (got != (size_t)sz) { free(buf); return NULL; }
    *out_len = got;
    return buf;
}

int photo_load(const char *path, Photo *out)
{
    memset(out, 0, sizeof *out);

    size_t len = 0;
    uint8_t *file = read_file(path, &len);
    if (!file) return PHOTO_ENOENT;

    uint32_t w = 0, h = 0;
    bool is_png = false;

    if (png_probe(file, len, &w, &h) == PNG_OK) {
        is_png = true;
    } else if (jpeg_probe(file, len, &w, &h) == 0) {
        is_png = false;
    } else {
        free(file);
        return PHOTO_EFORMAT;
    }

    if (w == 0 || h == 0 || (uint64_t)w * h > MAX_PIXELS) {
        free(file);
        return PHOTO_EMEMORY;
    }

    size_t pixels = (size_t)w * h;
    uint32_t *dst = malloc(pixels * 4);
    /* The decoders inflate into caller-provided scratch, and each states its
     * own worst case. Honour BOTH so the same code path serves either. */
    size_t scratch_cap = is_png ? (pixels * 4 + h + 64) : (pixels * 3 + 4096);
    uint8_t *scratch = malloc(scratch_cap);
    if (!dst || !scratch) {
        free(dst); free(scratch); free(file);
        return PHOTO_EMEMORY;
    }

    int rc = is_png
        ? png_decode(file, len, dst, pixels * 4, scratch, scratch_cap, &w, &h)
        : jpeg_decode(file, len, dst, pixels * 4, scratch, scratch_cap, &w, &h);
    free(scratch);

    if (rc != 0) {
        free(dst); free(file);
        return (rc == PNG_EUNSUP) ? PHOTO_EUNSUP : PHOTO_EDECODE;
    }

    int orient = is_png ? 1 : exif_orientation(file, len);
    free(file);

    if (orient != 1) {
        uint32_t ow = w, oh = h;
        uint32_t *rot = photo_orient(dst, w, h, orient, &ow, &oh);
        if (rot) { free(dst); dst = rot; w = ow; h = oh; }
        else      orient = 1;      /* could not rotate: show it as it lies */
    }

    out->px = dst;
    out->w = w;
    out->h = h;
    out->format = is_png ? "PNG" : "JPEG";
    out->file_bytes = len;
    out->orientation = orient;
    return PHOTO_OK;
}

void photo_free(Photo *p)
{
    free(p->px);
    memset(p, 0, sizeof *p);
}

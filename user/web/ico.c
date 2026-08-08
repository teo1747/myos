/* user/web/ico.c -- .ico decoding. See ico.h for what the format is and which
 * two of its details are silent if you get them wrong. */

#include "ico.h"
#include "png.h"
#include <string.h>

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

#define ICO_DIR   6      /* bytes of file header before the directory  */
#define ICO_ENT  16      /* bytes per directory entry                  */
#define ICO_MAX_DIM 512  /* an icon larger than this is not an icon    */

struct entry {
    uint32_t w, h, off, len;
};

/* Pick the entry to decode: the smallest that is at least `want_px`, or failing
 * that the largest there is. Scaling down keeps more of a picture than scaling
 * up invents. */
static int choose(const uint8_t *src, size_t len, unsigned want_px, struct entry *out) {
    if (len < ICO_DIR) return ICO_ENOTICO;
    if (rd16(src) != 0 || rd16(src + 2) != 1) return ICO_ENOTICO;
    unsigned n = rd16(src + 4);
    if (n == 0 || len < ICO_DIR + (size_t)n * ICO_ENT) return ICO_EDATA;

    int best = -1;
    uint32_t best_w = 0;
    for (unsigned i = 0; i < n; i++) {
        const uint8_t *e = src + ICO_DIR + (size_t)i * ICO_ENT;
        /* A zero in the width or height byte means 256 -- the field is one byte
         * and 256 does not fit in it. */
        uint32_t w = e[0] ? e[0] : 256u;
        uint32_t h = e[1] ? e[1] : 256u;
        uint32_t sz = rd32(e + 8), off = rd32(e + 12);
        if (!w || !h || w > ICO_MAX_DIM || h > ICO_MAX_DIM) continue;
        if (off > len || sz > len - off || sz == 0) continue;   /* points outside the file */
        int better;
        if (best < 0)                       better = 1;
        else if (best_w >= want_px)         better = (w >= want_px && w < best_w);
        else                                better = (w > best_w);
        if (better) { best = (int)i; best_w = w; out->w = w; out->h = h; out->off = off; out->len = sz; }
    }
    return best < 0 ? ICO_EDATA : ICO_OK;
}

int ico_is(const uint8_t *src, size_t len) {
    return len >= ICO_DIR && rd16(src) == 0 && rd16(src + 2) == 1 && rd16(src + 4) != 0;
}

int ico_probe(const uint8_t *src, size_t len, unsigned want_px,
              uint32_t *out_w, uint32_t *out_h) {
    struct entry e;
    int rc = choose(src, len, want_px, &e);
    if (rc != ICO_OK) return rc;
    /* A PNG entry is authoritative about its own size; the directory byte is
     * only 8 bits and says 0 for the 256 case, so ask the PNG. */
    if (e.len > 8 && !png_probe(src + e.off, e.len, out_w, out_h)) return ICO_OK;
    if (out_w) *out_w = e.w;
    if (out_h) *out_h = e.h;
    return ICO_OK;
}

/* Premultiply and pack one BGRA pixel the way png.c and jpeg.c do. */
static uint32_t px_bgra(unsigned b, unsigned g, unsigned r, unsigned a) {
    b = b * a / 255u; g = g * a / 255u; r = r * a / 255u;
    return (uint32_t)b | ((uint32_t)g << 8) | ((uint32_t)r << 16) | ((uint32_t)a << 24);
}

/* The DIB half. `src`/`len` are the entry's bytes, starting at its
 * BITMAPINFOHEADER. */
static int dib_decode(const uint8_t *src, size_t len,
                      uint32_t *dst, size_t dst_cap,
                      uint32_t *out_w, uint32_t *out_h) {
    if (len < 40) return ICO_EDATA;
    uint32_t hdr = rd32(src);
    if (hdr < 40 || hdr > len) return ICO_EUNSUP;      /* BITMAPCOREHEADER etc. */
    int32_t  w  = (int32_t)rd32(src + 4);
    int32_t  h2 = (int32_t)rd32(src + 8);
    unsigned bpp  = rd16(src + 14);
    uint32_t comp = rd32(src + 16);
    if (comp != 0) return ICO_EUNSUP;                  /* BI_RGB only */
    /* THE DOUBLED HEIGHT: the header counts the AND mask's rows as well. */
    int32_t h = h2 / 2;
    if (w <= 0 || h <= 0 || w > ICO_MAX_DIM || h > ICO_MAX_DIM) return ICO_EDATA;
    if (bpp != 32 && bpp != 24 && bpp != 8 && bpp != 4 && bpp != 1) return ICO_EUNSUP;
    if ((size_t)w * (size_t)h * 4u > dst_cap) return ICO_ETOOBIG;

    /* palette, for the indexed depths: BGRA quads straight after the header */
    const uint8_t *pal = src + hdr;
    unsigned pal_n = rd32(src + 32);
    if (bpp <= 8) {
        if (pal_n == 0) pal_n = 1u << bpp;
        if (hdr + (size_t)pal_n * 4u > len) return ICO_EDATA;
    } else {
        pal_n = 0;
    }

    const uint8_t *bits = src + hdr + (size_t)pal_n * 4u;
    size_t row  = (((size_t)w * bpp + 31u) / 32u) * 4u;      /* padded to 4 bytes */
    size_t mrow = (((size_t)w + 31u) / 32u) * 4u;            /* AND mask, 1bpp, padded separately */
    size_t need = row * (size_t)h;
    if ((size_t)(bits - src) + need > len) return ICO_EDATA;
    /* The mask is optional in practice -- some 32-bit icons omit it, and their
     * alpha channel already carries the transparency. */
    const uint8_t *mask = bits + need;
    int have_mask = (size_t)(mask - src) + mrow * (size_t)h <= len;

    for (int32_t y = 0; y < h; y++) {
        const uint8_t *sr = bits + row * (size_t)(h - 1 - y);   /* rows are bottom-up */
        const uint8_t *mr = have_mask ? mask + mrow * (size_t)(h - 1 - y) : 0;
        uint32_t *dr = dst + (size_t)y * (size_t)w;
        for (int32_t x = 0; x < w; x++) {
            unsigned b, g, r, a;
            if (bpp == 32) {
                b = sr[x * 4]; g = sr[x * 4 + 1]; r = sr[x * 4 + 2]; a = sr[x * 4 + 3];
            } else if (bpp == 24) {
                b = sr[x * 3]; g = sr[x * 3 + 1]; r = sr[x * 3 + 2]; a = 255;
            } else {
                unsigned idx;
                if (bpp == 8)      idx = sr[x];
                else if (bpp == 4) idx = (x & 1) ? (sr[x >> 1] & 0x0F) : (sr[x >> 1] >> 4);
                else               idx = (sr[x >> 3] >> (7 - (x & 7))) & 1u;
                if (idx >= pal_n) idx = 0;
                b = pal[idx * 4]; g = pal[idx * 4 + 1]; r = pal[idx * 4 + 2]; a = 255;
            }
            /* THE AND MASK: a set bit means "leave the background alone", which
             * is this format's only way to say transparent below 32 bits. For a
             * 32-bit icon the alpha channel is the truth and an all-zero alpha
             * plane (which some icons ship) would otherwise erase the picture,
             * so the mask only ever ADDS transparency there. */
            if (mr) {
                unsigned clear = (mr[x >> 3] >> (7 - (x & 7))) & 1u;
                if (clear) a = 0;
            }
            dr[x] = px_bgra(b, g, r, a);
        }
    }
    /* An icon whose every pixel came out transparent is one whose alpha plane
     * was absent rather than empty -- 24-bit data stored as 32. Redo it opaque
     * rather than hand back an invisible picture. */
    if (bpp == 32) {
        int any = 0;
        for (size_t i = 0, n = (size_t)w * (size_t)h; i < n && !any; i++)
            if (dst[i] >> 24) any = 1;
        if (!any)
            for (int32_t y = 0; y < h; y++) {
                const uint8_t *sr = bits + row * (size_t)(h - 1 - y);
                const uint8_t *mr = have_mask ? mask + mrow * (size_t)(h - 1 - y) : 0;
                for (int32_t x = 0; x < w; x++) {
                    unsigned a = 255;
                    if (mr && ((mr[x >> 3] >> (7 - (x & 7))) & 1u)) a = 0;
                    dst[(size_t)y * (size_t)w + x] =
                        px_bgra(sr[x * 4], sr[x * 4 + 1], sr[x * 4 + 2], a);
                }
            }
    }
    if (out_w) *out_w = (uint32_t)w;
    if (out_h) *out_h = (uint32_t)h;
    return ICO_OK;
}

int ico_decode(const uint8_t *src, size_t len, unsigned want_px,
               uint32_t *dst, size_t dst_cap,
               uint8_t *scratch, size_t scratch_cap,
               uint32_t *out_w, uint32_t *out_h) {
    struct entry e;
    int rc = choose(src, len, want_px, &e);
    if (rc != ICO_OK) return rc;
    /* Whole PNG files live inside .ico containers routinely -- Hacker News
     * ships exactly one. Decided by the signature, like everywhere else. */
    if (e.len > 8 && src[e.off] == 0x89 && !memcmp(src + e.off + 1, "PNG", 3)) {
        int p = png_decode(src + e.off, e.len, dst, dst_cap, scratch, scratch_cap, out_w, out_h);
        return p == PNG_OK ? ICO_OK : (p == PNG_ETOOBIG ? ICO_ETOOBIG : ICO_EDATA);
    }
    return dib_decode(src + e.off, e.len, dst, dst_cap, out_w, out_h);
}

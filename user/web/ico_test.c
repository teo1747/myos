/* user/web/ico_test.c -- the .ico decoder, against fixtures built here.
 *
 * The fixtures are SYNTHESISED rather than downloaded, for the reason
 * tools/web_real.py does not commit the pages it fetches: someone else's bytes
 * go stale and a corpus nobody can regenerate is a corpus nobody can check.
 * Everything the real web puts in a favicon is constructible -- the four bit
 * depths, the AND mask, a whole PNG inside the container -- and building them
 * here means each test states exactly which byte it is about.
 *
 * The malformed cases matter more than the well-formed ones. A favicon is
 * bytes from a stranger, arriving with a length the file's own header is free
 * to contradict, and every field in an .ico directory is an offset into
 * somewhere else. The decoder's job on all of that is to refuse.
 */
#include "ico.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int g_fail;
static void ok(int cond, const char *what) {
    printf("  %s: %s\n", cond ? "ok  " : "FAIL", what);
    if (!cond) g_fail++;
}

/* --- fixture construction ----------------------------------------------- */

static void put16(unsigned char *p, unsigned v) { p[0] = v & 255; p[1] = (v >> 8) & 255; }
static void put32(unsigned char *p, unsigned long v) {
    p[0] = v & 255; p[1] = (v >> 8) & 255; p[2] = (v >> 16) & 255; p[3] = (v >> 24) & 255;
}

/* One-entry .ico wrapping a DIB of `bpp` bits, `w` x `h`, filled with colour
 * index/value `fill`, and an AND mask that clears the top-left pixel. */
static size_t make_dib_ico(unsigned char *out, size_t cap,
                           unsigned w, unsigned h, unsigned bpp,
                           unsigned fill, int with_mask) {
    unsigned pal_n = bpp <= 8 ? (1u << bpp) : 0;
    size_t row  = (((size_t)w * bpp + 31) / 32) * 4;
    size_t mrow = (((size_t)w + 31) / 32) * 4;
    size_t bits = 40 + (size_t)pal_n * 4;
    size_t dib  = bits + row * h + (with_mask ? mrow * h : 0);
    size_t need = 6 + 16 + dib;
    if (need > cap) return 0;
    memset(out, 0, need);

    put16(out + 0, 0); put16(out + 2, 1); put16(out + 4, 1);         /* ICONDIR */
    unsigned char *e = out + 6;
    e[0] = (unsigned char)(w == 256 ? 0 : w);
    e[1] = (unsigned char)(h == 256 ? 0 : h);
    put16(e + 6, bpp);
    put32(e + 8, dib);
    put32(e + 12, 6 + 16);

    unsigned char *d = out + 6 + 16;
    put32(d + 0, 40);
    put32(d + 4, w);
    put32(d + 8, h * 2);                  /* THE DOUBLED HEIGHT */
    put16(d + 12, 1);
    put16(d + 14, bpp);
    put32(d + 16, 0);                     /* BI_RGB */
    put32(d + 32, pal_n);
    /* palette: entry `fill` is red, everything else black */
    for (unsigned i = 0; i < pal_n; i++) {
        unsigned char *q = d + 40 + i * 4;
        if (i == fill) { q[0] = 0x00; q[1] = 0x00; q[2] = 0xFF; }   /* BGRA: red */
    }
    unsigned char *px = d + bits;
    for (unsigned y = 0; y < h; y++)
        for (unsigned x = 0; x < w; x++) {
            unsigned char *r = px + row * y;
            if (bpp == 32)      { r[x*4] = 0; r[x*4+1] = 0; r[x*4+2] = 0xFF; r[x*4+3] = 0xFF; }
            else if (bpp == 24) { r[x*3] = 0; r[x*3+1] = 0; r[x*3+2] = 0xFF; }
            else if (bpp == 8)  r[x] = (unsigned char)fill;
            else if (bpp == 4)  r[x >> 1] |= (x & 1) ? (fill & 15) : (unsigned char)((fill & 15) << 4);
            else if (bpp == 1)  r[x >> 3] |= (unsigned char)(1u << (7 - (x & 7)));
        }
    if (with_mask) {
        unsigned char *m = px + row * h;
        m[0] |= 0x80;   /* first STORED mask row = BOTTOM output row, leftmost pixel */
    }
    return need;
}

/* Two entries, 16x16 and 32x32, so entry SELECTION can be tested. */
static size_t make_two_entry(unsigned char *out, size_t cap) {
    unsigned char a[4096], b[8192];
    size_t na = make_dib_ico(a, sizeof a, 16, 16, 32, 0, 0);
    size_t nb = make_dib_ico(b, sizeof b, 32, 32, 32, 0, 0);
    if (!na || !nb) return 0;
    size_t da = na - 22, db = nb - 22;            /* just the DIB parts */
    size_t need = 6 + 32 + da + db;
    if (need > cap) return 0;
    memset(out, 0, need);
    put16(out + 0, 0); put16(out + 2, 1); put16(out + 4, 2);
    unsigned char *e0 = out + 6, *e1 = out + 22;
    e0[0] = 16; e0[1] = 16; put16(e0 + 6, 32); put32(e0 + 8, da); put32(e0 + 12, 6 + 32);
    e1[0] = 32; e1[1] = 32; put16(e1 + 6, 32); put32(e1 + 8, db); put32(e1 + 12, 6 + 32 + da);
    memcpy(out + 6 + 32, a + 22, da);
    memcpy(out + 6 + 32 + da, b + 22, db);
    return need;
}

/* --- the tests ----------------------------------------------------------- */

static uint32_t dst[512 * 512];
static uint8_t  scratch[512 * 512 * 4 + 1024];

#define A(p) ((p) >> 24)
#define R(p) (((p) >> 16) & 255)

int main(void) {
    static unsigned char buf[1 << 16];
    uint32_t w, h;
    size_t n;

    printf("ico_test\n");

    /* --- the four bit depths all produce the same red square --- */
    unsigned depths[] = { 32, 24, 8, 4 };
    for (unsigned i = 0; i < 4; i++) {
        n = make_dib_ico(buf, sizeof buf, 16, 16, depths[i], 1, 0);
        int rc = ico_decode(buf, n, 16, dst, sizeof dst, scratch, sizeof scratch, &w, &h);
        char msg[96];
        snprintf(msg, sizeof msg, "%u-bit DIB decodes to a 16x16 opaque red square", depths[i]);
        ok(rc == ICO_OK && w == 16 && h == 16 && A(dst[0]) == 255 && R(dst[0]) > 200, msg);
    }

    /* --- the doubled height is not taken at face value --- */
    n = make_dib_ico(buf, sizeof buf, 16, 16, 32, 0, 0);
    ico_decode(buf, n, 16, dst, sizeof dst, scratch, sizeof scratch, &w, &h);
    ok(h == 16, "the header's doubled height is halved, not believed");

    /* --- the AND mask is the only transparency below 32 bits --- */
    n = make_dib_ico(buf, sizeof buf, 16, 16, 8, 1, 1);
    ico_decode(buf, n, 16, dst, sizeof dst, scratch, sizeof scratch, &w, &h);
    /* The bit was set in the FIRST stored mask row, and rows are stored
     * bottom-up -- so it must come out at the BOTTOM of the picture. Checking
     * both ends is what makes this a test of the row order and not just of the
     * mask: read the rows the wrong way round and the hole moves to the top,
     * which is exactly the bug that turns an icon upside down. */
    ok(A(dst[(size_t)15 * 16]) == 0, "a set AND-mask bit clears that pixel");
    ok(A(dst[(size_t)15 * 16 + 1]) == 255, "...and only that pixel");
    ok(A(dst[0]) == 255,
       "rows are read bottom-up (the first stored row lands at the bottom)");

    /* --- entry selection prefers the smallest that is big enough --- */
    n = make_two_entry(buf, sizeof buf);
    ok(n > 0, "two-entry fixture built");
    ico_probe(buf, n, 16, &w, &h);
    ok(w == 16, "want 16 -> picks the 16x16 entry");
    ico_probe(buf, n, 32, &w, &h);
    ok(w == 32, "want 32 -> picks the 32x32 entry");
    ico_probe(buf, n, 64, &w, &h);
    ok(w == 32, "want 64 -> picks the largest there is");

    /* --- HOSTILE INPUT. Every one of these is a length or an offset the file
     *     itself is free to lie about, and the only correct answer is a
     *     refusal. A decoder that reads past the buffer here is the bug that
     *     matters; the picture coming out wrong is not. --- */
    n = make_dib_ico(buf, sizeof buf, 16, 16, 32, 0, 0);

    ok(ico_decode(buf, 3, 16, dst, sizeof dst, scratch, sizeof scratch, &w, &h) != ICO_OK,
       "refuses a file shorter than its own header");
    ok(ico_decode(buf, n / 2, 16, dst, sizeof dst, scratch, sizeof scratch, &w, &h) != ICO_OK,
       "refuses when the pixel data is cut in half");

    put32(buf + 6 + 12, 0xFFFFFF00UL);              /* entry offset past the end */
    ok(ico_decode(buf, n, 16, dst, sizeof dst, scratch, sizeof scratch, &w, &h) != ICO_OK,
       "refuses an entry whose offset points outside the file");

    n = make_dib_ico(buf, sizeof buf, 16, 16, 32, 0, 0);
    put32(buf + 6 + 8, 0xFFFFFF00UL);               /* entry length longer than the file */
    ok(ico_decode(buf, n, 16, dst, sizeof dst, scratch, sizeof scratch, &w, &h) != ICO_OK,
       "refuses an entry longer than the file that contains it");

    n = make_dib_ico(buf, sizeof buf, 16, 16, 32, 0, 0);
    put32(buf + 6 + 16 + 4, 100000UL);              /* absurd width in the DIB header */
    ok(ico_decode(buf, n, 16, dst, sizeof dst, scratch, sizeof scratch, &w, &h) != ICO_OK,
       "refuses a DIB claiming a width no icon has");

    n = make_dib_ico(buf, sizeof buf, 16, 16, 32, 0, 0);
    put16(buf + 6 + 16 + 14, 16);                   /* a bit depth we do not decode */
    ok(ico_decode(buf, n, 16, dst, sizeof dst, scratch, sizeof scratch, &w, &h) == ICO_EUNSUP,
       "says UNSUPPORTED for 16-bit rather than guessing");

    n = make_dib_ico(buf, sizeof buf, 16, 16, 32, 0, 0);
    put32(buf + 6 + 16 + 16, 3);                    /* BI_BITFIELDS */
    ok(ico_decode(buf, n, 16, dst, sizeof dst, scratch, sizeof scratch, &w, &h) == ICO_EUNSUP,
       "says UNSUPPORTED for a compressed DIB");

    n = make_dib_ico(buf, sizeof buf, 16, 16, 32, 0, 0);
    put16(buf + 4, 0);                              /* zero entries */
    ok(!ico_is(buf, n), "a directory with no entries is not an .ico");

    /* --- and a buffer too small to hold the result --- */
    n = make_dib_ico(buf, sizeof buf, 32, 32, 32, 0, 0);
    ok(ico_decode(buf, n, 32, dst, 16, scratch, sizeof scratch, &w, &h) == ICO_ETOOBIG,
       "refuses rather than overrunning a destination that is too small");

    printf("=== ico_test: %s (%d failure%s) ===\n",
           g_fail ? "FAILED" : "OK", g_fail, g_fail == 1 ? "" : "s");
    return g_fail != 0;
}

/* user/photos/photo_test.c -- prove the resampler, on the host, with numbers.
 *
 * "The photo looks good" is not a result. The claim this viewer makes is a
 * specific and checkable one -- that shrinking a picture here produces the
 * AREA AVERAGE of the pixels each output pixel covers, rather than four
 * samples of them -- and the difference between those two is measurable
 * without looking at anything.
 *
 * The centrepiece is T4. A one-pixel checkerboard is the worst case for a
 * sampling filter: it is entirely composed of the frequency that shrinking
 * cannot represent. The correct answer when you shrink it is FLAT MID-GREY,
 * because every output pixel covers equal amounts of black and white. Bilinear
 * gives you something else, and what it gives you depends on where the sample
 * grid lands -- which is what a moire pattern IS. So the test runs both over
 * the same input and reports the error of each, and the gap between the two
 * numbers is the whole argument for this file existing.
 *
 *   make test-photos
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "photo.h"

static int g_fail;

static void ok(const char *name, bool cond, const char *detail)
{
    printf("  %-46s %s%s%s\n", name, cond ? "OK" : "FAIL",
           detail && *detail ? "  -- " : "", detail ? detail : "");
    if (!cond) g_fail++;
}

static uint32_t bgra(int b, int g, int r, int a)
{
    return (uint32_t)b | ((uint32_t)g << 8) | ((uint32_t)r << 16) | ((uint32_t)a << 24);
}
static int chan(uint32_t p, int i) { return (int)((p >> (i * 8)) & 0xFFu); }

/* A reference bilinear sampler -- what the compositor's blitter does, and what
 * this viewer would be using if resample.c did not exist. Present ONLY so T4
 * can compare against it honestly rather than asserting that the alternative
 * is worse. */
static uint32_t *bilinear(const uint32_t *src, uint32_t sw, uint32_t sh,
                          uint32_t dw, uint32_t dh)
{
    uint32_t *dst = malloc((size_t)dw * dh * 4);
    for (uint32_t y = 0; y < dh; y++) {
        for (uint32_t x = 0; x < dw; x++) {
            /* Sample at the centre of the destination pixel, mapped back. */
            float fx = ((float)x + 0.5f) * (float)sw / (float)dw - 0.5f;
            float fy = ((float)y + 0.5f) * (float)sh / (float)dh - 0.5f;
            int x0 = (int)(fx < 0 ? 0 : fx), y0 = (int)(fy < 0 ? 0 : fy);
            int x1 = x0 + 1 < (int)sw ? x0 + 1 : (int)sw - 1;
            int y1 = y0 + 1 < (int)sh ? y0 + 1 : (int)sh - 1;
            float ax = fx - (float)x0, ay = fy - (float)y0;
            if (ax < 0) ax = 0;
            if (ay < 0) ay = 0;
            uint32_t out = 0;
            for (int c = 0; c < 4; c++) {
                float p00 = (float)chan(src[(size_t)y0 * sw + x0], c);
                float p10 = (float)chan(src[(size_t)y0 * sw + x1], c);
                float p01 = (float)chan(src[(size_t)y1 * sw + x0], c);
                float p11 = (float)chan(src[(size_t)y1 * sw + x1], c);
                float v = (p00 * (1 - ax) + p10 * ax) * (1 - ay)
                        + (p01 * (1 - ax) + p11 * ax) * ay;
                int iv = (int)(v + 0.5f);
                if (iv > 255) iv = 255;
                out |= (uint32_t)iv << (c * 8);
            }
            dst[(size_t)y * dw + x] = out;
        }
    }
    return dst;
}

/* Largest distance from `want` over one channel of a buffer. */
static int worst_error(const uint32_t *p, size_t n, int c, int want)
{
    int worst = 0;
    for (size_t i = 0; i < n; i++) {
        int d = chan(p[i], c) - want;
        if (d < 0) d = -d;
        if (d > worst) worst = d;
    }
    return worst;
}

int main(void)
{
    char msg[160];
    printf("=== photo resampler\n");

    /* T1 -- a flat colour must survive being shrunk. Shrinking is an average,
     * and the average of one value is that value; anything else is a rounding
     * or accumulator bug, and it would show up as the whole picture getting
     * fractionally darker every time it was resized. */
    {
        uint32_t *src = malloc(512 * 512 * 4);
        for (int i = 0; i < 512 * 512; i++) src[i] = bgra(40, 130, 200, 255);
        uint32_t *d = photo_downscale(src, 512, 512, 37, 91);   /* awkward on purpose */
        int e = 0;
        e |= worst_error(d, 37 * 91, 0, 40)  ? 1 : 0;
        e |= worst_error(d, 37 * 91, 1, 130) ? 1 : 0;
        e |= worst_error(d, 37 * 91, 2, 200) ? 1 : 0;
        e |= worst_error(d, 37 * 91, 3, 255) ? 1 : 0;
        ok("T1 flat colour survives a 512->37x91 shrink", e == 0,
           e ? "the average of one value is not that value" : "");
        free(src); free(d);
    }

    /* T2 -- the whole image to a single pixel is the exact mean, and we can
     * compute the mean independently. This is the definition the file claims
     * to implement, checked against arithmetic rather than against itself. */
    {
        uint32_t *src = malloc(64 * 64 * 4);
        long sum = 0;
        for (int i = 0; i < 64 * 64; i++) {
            int v = i % 256;
            src[i] = bgra(v, v, v, 255);
            sum += v;
        }
        int want = (int)((sum + (64 * 64) / 2) / (64 * 64));
        uint32_t *d = photo_downscale(src, 64, 64, 1, 1);
        int got = chan(d[0], 0);
        snprintf(msg, sizeof msg, "want %d, got %d", want, got);
        ok("T2 whole image to 1 pixel is the exact mean",
           got >= want - 1 && got <= want + 1, msg);
        free(src); free(d);
    }

    /* T3 -- a horizontal ramp shrunk keeps its ends and stays monotonic. An
     * off-by-one in the span arithmetic shows up here as a repeated or skipped
     * column, which a flat-colour test cannot see. */
    {
        uint32_t *src = malloc(256 * 4 * 4);
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 256; x++)
                src[y * 256 + x] = bgra(x, x, x, 255);
        uint32_t *d = photo_downscale(src, 256, 4, 32, 1);
        bool mono = true;
        for (int x = 1; x < 32; x++)
            if (chan(d[x], 0) <= chan(d[x - 1], 0)) mono = false;
        snprintf(msg, sizeof msg, "ends %d..%d", chan(d[0], 0), chan(d[31], 0));
        ok("T3 a ramp stays monotonic and keeps its ends",
           mono && chan(d[0], 0) < 8 && chan(d[31], 0) > 247, msg);
        free(src); free(d);
    }

    /* T4 -- THE ONE THAT MATTERS.
     *
     * A 1px checkerboard shrunk 32:1. Every output pixel covers exactly 512
     * black and 512 white source pixels, so the only correct answer is 128,
     * flat. Area averaging should land on it almost exactly. Bilinear reads
     * four pixels of a pattern that alternates every pixel, so it lands on
     * whatever the grid gives it -- and the error IS the aliasing you would
     * see as a moire pattern, and would see CRAWL if the image moved.
     *
     * 256 -> 9, NOT 256 -> 8, and that detail is the test. At an exact 32:1
     * reduction bilinear samples each output pixel at the precise midpoint of
     * four texels that happen to be two black and two white, averages them,
     * and lands on 128 -- perfectly. The first version of this test used 8 and
     * reported both filters as flawless, which did not mean bilinear was fine.
     * It meant the test image had been made easy by a ratio that put every
     * sample on a symmetry point. A non-divisor ratio makes the sampling phase
     * drift across the output, which is the situation a real photo in a
     * resizable window is in essentially always. */
    {
        const uint32_t N = 256, D = 9;
        uint32_t *src = malloc((size_t)N * N * 4);
        for (uint32_t y = 0; y < N; y++)
            for (uint32_t x = 0; x < N; x++) {
                int v = ((x + y) & 1) ? 255 : 0;
                src[(size_t)y * N + x] = bgra(v, v, v, 255);
            }

        uint32_t *area = photo_downscale(src, N, N, D, D);
        uint32_t *bil  = bilinear(src, N, N, D, D);
        int ea = worst_error(area, (size_t)D * D, 0, 128);
        int eb = worst_error(bil,  (size_t)D * D, 0, 128);

        snprintf(msg, sizeof msg, "area off by %d, bilinear off by %d", ea, eb);
        ok("T4 checkerboard shrinks to flat grey (no aliasing)", ea <= 2, msg);
        /* State the comparison as its own result, and require the reference to
         * be badly wrong in ABSOLUTE terms as well as relative ones. A pure
         * ratio would be satisfied by both filters being nearly perfect, which
         * is exactly how the first version of this test passed while proving
         * nothing: it would mean the input had stopped being a hard case, and
         * T4 would be green for a reason that has nothing to do with the
         * filter under test. */
        ok("T4b bilinear visibly aliases where area averaging does not",
           eb > 30 && eb > ea * 10, msg);
        free(src); free(area); free(bil);
    }

    /* T5 -- premultiplied alpha does not bleed. A transparent pixel next to an
     * opaque white one must not drag colour into it: in premultiplied form the
     * transparent pixel is all zeroes, so the average of the pair is half the
     * white AND half the alpha, and colour-over-alpha comes back to white.
     * Getting this wrong is every halo artefact in image scaling. */
    {
        uint32_t src[4] = {
            bgra(255, 255, 255, 255), bgra(0, 0, 0, 0),
            bgra(255, 255, 255, 255), bgra(0, 0, 0, 0),
        };
        uint32_t *d = photo_downscale(src, 2, 2, 1, 1);
        int a = chan(d[0], 3), b = chan(d[0], 0);
        /* Half coverage, and colour exactly tracking it -- so unpremultiplying
         * gives 255 (white), not a grey that has been darkened by the void. */
        snprintf(msg, sizeof msg, "alpha %d, colour %d", a, b);
        ok("T5 premultiplied alpha averages without a halo",
           a >= 126 && a <= 130 && b == a, msg);
        free(d);
    }

    /* T6 -- photo_view returns the crop asked for, at 1:1 when magnifying. */
    {
        uint32_t *src = malloc(16 * 16 * 4);
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 16; x++) src[y * 16 + x] = bgra(x, y, 0, 255);
        uint32_t w = 0, h = 0;
        uint32_t *v = photo_view(src, 16, 16, 4, 5, 3, 2, 300, 200, &w, &h);
        bool good = v && w == 3 && h == 2 &&
                    chan(v[0], 0) == 4 && chan(v[0], 1) == 5 &&
                    chan(v[3], 0) == 4 && chan(v[3], 1) == 6;
        snprintf(msg, sizeof msg, "%ux%u, first pixel (%d,%d)", w, h,
                 v ? chan(v[0], 0) : -1, v ? chan(v[0], 1) : -1);
        ok("T6 magnifying returns the crop at 1:1", good, msg);
        free(src); free(v);
    }

    /* T7 -- an out-of-bounds crop is clamped, not read. The app computes these
     * rectangles from floating-point pan and zoom, so one WILL eventually be a
     * pixel past the edge, and the difference between clamping and not is a
     * crash on somebody's photo. */
    {
        uint32_t *src = malloc(8 * 8 * 4);
        for (int i = 0; i < 64; i++) src[i] = bgra(9, 9, 9, 255);
        uint32_t w = 0, h = 0;
        uint32_t *v = photo_view(src, 8, 8, 6, 6, 99, 99, 99, 99, &w, &h);
        ok("T7 a crop past the edge is clamped", v && w == 2 && h == 2, "");
        free(src); free(v);
    }

    /* T8 -- EXIF rotation. 90 CW then 90 CCW is the identity, and it has to
     * put the dimensions back as well as the pixels: the transposing cases
     * SWAP width and height, and a viewer that rotates pixels while keeping
     * the old width squeezes every portrait photo into a landscape box. */
    {
        const uint32_t W = 7, H = 3;
        uint32_t *src = malloc((size_t)W * H * 4);
        for (uint32_t y = 0; y < H; y++)
            for (uint32_t x = 0; x < W; x++)
                src[y * W + x] = bgra((int)x * 30, (int)y * 60, 0, 255);

        uint32_t aw = 0, ah = 0, bw = 0, bh = 0;
        uint32_t *cw90 = photo_orient(src, W, H, 6, &aw, &ah);
        uint32_t *back = cw90 ? photo_orient(cw90, aw, ah, 8, &bw, &bh) : NULL;
        bool dims = (aw == H && ah == W && bw == W && bh == H);
        bool same = back && memcmp(back, src, (size_t)W * H * 4) == 0;
        snprintf(msg, sizeof msg, "%ux%u -> %ux%u -> %ux%u", W, H, aw, ah, bw, bh);
        ok("T8 rotate 90 CW then CCW restores pixels and dims", dims && same, msg);
        free(src); free(cw90); free(back);
    }

    printf("=== photo resampler: %s (%d failure%s)\n",
           g_fail ? "FAIL" : "OK", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}

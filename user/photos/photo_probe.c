/* user/photos/photo_probe.c -- the viewer's pipeline, on the host.
 *
 * The same decode -> orient -> resample path the app runs, driven from the
 * command line so it can be checked without booting anything. This is the
 * `make browser-render` lesson applied to pictures: a two-second loop on the
 * build machine finds the bug that a boot-and-look loop takes ten minutes to
 * find and then describes as "it looked wrong".
 *
 *   photo_probe data/pictures/chart.png                 -- what is in it
 *   photo_probe data/pictures/chart.png 256 192 out.ppm -- and what we make of it
 *
 * The PPM is the visual proof: open it next to the source and the zone plate's
 * outer field is either flat grey (correct) or full of ghost rings that are
 * not in the original (aliasing). Written as P6 because it is six lines of
 * code and every image tool on earth reads it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "photo.h"

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: photo_probe <file> [out_w out_h out.ppm]\n");
        return 2;
    }

    Photo p;
    int rc = photo_load(argv[1], &p);
    if (rc != PHOTO_OK) {
        printf("%s: FAIL -- %s\n", argv[1], photo_error(rc));
        return 1;
    }

    printf("%s: %s %ux%u, %zu bytes on disk", argv[1], p.format, p.w, p.h,
           p.file_bytes);
    if (p.orientation != 1) printf(", EXIF orientation %d applied", p.orientation);
    printf("\n");

    if (argc < 5) { photo_free(&p); return 0; }

    uint32_t dw = (uint32_t)atoi(argv[2]), dh = (uint32_t)atoi(argv[3]);
    uint32_t ow = 0, oh = 0;
    uint32_t *v = photo_view(p.px, p.w, p.h, 0, 0, p.w, p.h, dw, dh, &ow, &oh);
    if (!v) { printf("  resample FAILED\n"); photo_free(&p); return 1; }

    FILE *f = fopen(argv[4], "wb");
    if (!f) { printf("  cannot write %s\n", argv[4]); photo_free(&p); free(v); return 1; }
    fprintf(f, "P6\n%u %u\n255\n", ow, oh);
    for (size_t i = 0; i < (size_t)ow * oh; i++) {
        uint32_t px = v[i];
        /* BGRA premultiplied over WHITE, so a transparent PNG does not come
         * out as black in a format that has no alpha to explain it. */
        unsigned a = (px >> 24) & 0xFF;
        unsigned b = px & 0xFF, g = (px >> 8) & 0xFF, r = (px >> 16) & 0xFF;
        unsigned char rgb[3] = {
            (unsigned char)(r + 255 * (255 - a) / 255),
            (unsigned char)(g + 255 * (255 - a) / 255),
            (unsigned char)(b + 255 * (255 - a) / 255),
        };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    printf("  resampled to %ux%u -> %s\n", ow, oh, argv[4]);

    photo_free(&p);
    free(v);
    return 0;
}

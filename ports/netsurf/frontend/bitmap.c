/* ports/netsurf/frontend/bitmap.c -- where decoded pictures live.
 *
 * The core decodes an image into a buffer the FRONTEND owns, in the frontend's
 * preferred pixel format, so that drawing one later is a blit rather than a
 * conversion. Ours is 32 bits per pixel, 8888, which is what the compositor
 * takes (kernel/gfx/compositor.c) and what our own image cache already used --
 * so a picture arrives in the layout in the format the screen wants.
 *
 * Nothing here is clever. It is a malloc, a stride, and the honesty to say
 * when the allocation failed.
 */
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "utils/errors.h"
#include "netsurf/bitmap.h"
#include "emblink.h"


struct bitmap {
    int width, height;
    bool opaque;
    unsigned char *px;      /* width * height * 4 */
};

static void *bm_create(int width, int height, enum gui_bitmap_flags flags)
{
    if (width <= 0 || height <= 0) return NULL;
    /* A page can ask for a picture the size of a continent. Refusing a silly
     * allocation here is cheaper than discovering it as an OOM three frames
     * later, and the core copes with a NULL by not showing the image. */
    if ((long long)width * height > 64LL * 1024 * 1024) return NULL;

    struct bitmap *bm = calloc(1, sizeof *bm);
    if (bm == NULL) return NULL;
    bm->px = calloc((size_t)width * height, 4);
    if (bm->px == NULL) { free(bm); return NULL; }
    bm->width = width;
    bm->height = height;
    bm->opaque = (flags & BITMAP_OPAQUE) == BITMAP_OPAQUE;
    return bm;
}

static void bm_destroy(void *bitmap)
{
    struct bitmap *bm = bitmap;
    if (bm == NULL) return;
    free(bm->px);
    free(bm);
}

static void bm_set_opaque(void *bitmap, bool opaque)
{
    struct bitmap *bm = bitmap;
    if (bm != NULL) bm->opaque = opaque;
}

static bool bm_get_opaque(void *bitmap)
{
    struct bitmap *bm = bitmap;
    return bm != NULL ? bm->opaque : false;
}

static unsigned char *bm_get_buffer(void *bitmap)
{
    struct bitmap *bm = bitmap;
    return bm != NULL ? bm->px : NULL;
}

static size_t bm_get_rowstride(void *bitmap)
{
    struct bitmap *bm = bitmap;
    return bm != NULL ? (size_t)bm->width * 4 : 0;
}

static int bm_get_width(void *bitmap)
{
    struct bitmap *bm = bitmap;
    return bm != NULL ? bm->width : 0;
}

static int bm_get_height(void *bitmap)
{
    struct bitmap *bm = bitmap;
    return bm != NULL ? bm->height : 0;
}

static void bm_modified(void *bitmap)
{
    (void)bitmap;   /* nothing is cached from the pixels yet */
}

/* Rendering a CONTENT into a bitmap is what a thumbnail is. Not yet -- and
 * said out loud, because a thumbnail that silently comes back blank looks like
 * a rendering bug in the page rather than a missing feature. */
static nserror bm_render(struct bitmap *bitmap, struct hlcache_handle *content)
{
    (void)bitmap; (void)content;
    return NSERROR_NOT_IMPLEMENTED;
}

static struct gui_bitmap_table bitmap_table = {
    .create = bm_create,
    .destroy = bm_destroy,
    .set_opaque = bm_set_opaque,
    .get_opaque = bm_get_opaque,
    .get_buffer = bm_get_buffer,
    .get_rowstride = bm_get_rowstride,
    .get_width = bm_get_width,
    .get_height = bm_get_height,
    .modified = bm_modified,
    .render = bm_render,
};

struct gui_bitmap_table *emblink_bitmap_table = &bitmap_table;

/* user/web/imgcache.h -- the pictures a page asked for.
 *
 * A document names images; it does not contain them. Each one is a separate
 * fetch, which is the thing that makes images different from every other part
 * of a page: the HTML arrives complete, and then N more round trips have to
 * happen before it looks right.
 *
 * So this is a small state machine, not a decoder. It answers two questions
 * for the renderer -- "do you have this picture?" and "then go and get it" --
 * and drives one fetch at a time on the SAME worker the document used, so the
 * window keeps drawing throughout. Images appear as they land, which is what a
 * browser has always done and is far better than a blank page that snaps into
 * existence when the last byte of the last picture arrives.
 *
 * Bounded hard, because this is memory spent on behalf of a stranger's markup:
 * a fixed number of slots, a fixed pixel arena, and a per-image size ceiling.
 * A page asking for more than that gets the first few and alt text for the
 * rest -- which is a page, rather than an out-of-memory.
 */
#ifndef _EMBLINK_WEB_IMGCACHE_H_
#define _EMBLINK_WEB_IMGCACHE_H_

#include <stdint.h>
#include <stddef.h>

#define IMG_SLOTS      8

/* The real cost of a picture is its PIXEL COUNT, not either dimension: a
 * 1400x200 banner is ordinary and was being refused by a 900px-per-side cap,
 * while 900x900 (four times the pixels) sailed through. So the budget is in
 * pixels, with a generous per-side cap kept only to catch an absurd header
 * before the multiply can overflow anything. */
#define IMG_MAX_PX  (1600u * 1024u)     /* 6.4 MB of BGRA -- the arena size */
#define IMG_MAX_DIM  4096

enum img_state {
    IMG_EMPTY = 0,
    IMG_WANTED,      /* named by the page, not yet fetched */
    IMG_LOADING,     /* the worker is fetching it now      */
    IMG_READY,       /* decoded, pixels are valid          */
    IMG_FAILED,      /* fetch or decode failed -- show alt */
};

struct img_slot {
    char      url[512];
    int       state;
    uint32_t  w, h;
    uint32_t *px;                /* into the shared arena, BGRA premultiplied */
};

/* Forget everything: call when a new document is loaded, so one page's
 * pictures never leak into the next. */
void imgcache_reset(void);

/* Ask for `url`. Returns its slot (possibly still IMG_WANTED) or NULL when
 * the cache is full -- the renderer then draws alt text, which is what alt
 * text is for. Safe to call every frame with the same URL. */
struct img_slot *imgcache_want(const char *url);

/* Drive the queue: start at most one fetch, and finish one that has landed.
 * Call once per frame from the UI loop. Returns 1 if anything changed (so the
 * caller can request a repaint), 0 otherwise. Never blocks. */
int imgcache_pump(void);

/* Is any image still outstanding? For the status line. */
int imgcache_pending(void);

/* HOW THE PICTURES WENT: how many are ready, how many FAILED, how many are
 * still coming, and how many the page asked for that there was no slot for.
 * A failed image and a slot-less one both draw the same grey box, and telling
 * them apart from a screenshot is guesswork -- which is how "the icons do not
 * draw" stayed a mystery through three separate fixes that were not it. */
void imgcache_stats(int *ready, int *failed, int *pending, int *refused);

#endif /* _EMBLINK_WEB_IMGCACHE_H_ */

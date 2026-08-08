/* user/web/favicon.h -- the little picture that says which site a tab is.
 *
 * A favicon belongs to an ORIGIN, not to a page: every page on a site shares
 * one, and a reader who opens four Wikipedia articles should see the same W
 * four times rather than four fetches of it. So the cache is keyed by
 * scheme://host, survives navigation, and is deliberately NOT cleared when a
 * document is (which is the one thing imgcache does that this must not).
 *
 * WHERE THE ICON IS is two questions, because the web answers it two ways: a
 * page may declare one with <link rel="icon">, and /favicon.ico exists whether
 * anyone declared it or not. Neither is reliable alone -- kernel.org's
 * /favicon.ico is 146 bytes of nothing and its declared PNG is real, while
 * Hacker News declares an SVG this browser cannot draw and ships a perfectly
 * good PNG inside its /favicon.ico. So both are tried, declared first unless it
 * is something undecodable, and a failure falls through to the other.
 *
 * Icons are stored DOWNSAMPLED to FAVICON_PX and nothing else is kept: Hacker
 * News's is 256x256, which is a quarter of a megabyte to hold something drawn
 * sixteen pixels wide. Eight origins at 32x32 is 32KB, which is a cache that
 * cannot become a memory leak no matter how long the browser runs.
 */
#ifndef _EMBLINK_WEB_FAVICON_H_
#define _EMBLINK_WEB_FAVICON_H_

#include <stdint.h>
#include <stddef.h>

#define FAVICON_SLOTS 8
#define FAVICON_PX   32          /* stored size; drawn smaller */

enum favicon_state {
    FAV_EMPTY = 0,
    FAV_WANTED,     /* named, not yet fetched      */
    FAV_LOADING,    /* the worker has it now       */
    FAV_READY,      /* decoded, px is valid        */
    FAV_FAILED,     /* both candidates gave up     */
};

struct favicon {
    char      origin[192];             /* scheme://host                    */
    int       state;
    uint32_t  w, h;                    /* always FAVICON_PX when READY     */
    uint32_t *px;                      /* BGRA premultiplied, or NULL      */
};

/* The icon for the site `page_url` is on. `declared` is the page's own
 * <link rel="icon"> href (may be NULL), resolved against page_url by the
 * caller's usual rule. Returns a slot -- possibly still FAV_WANTED, possibly
 * FAV_FAILED -- or NULL if the URL has no origin (a local file) or the cache is
 * full. Cheap to call every frame: an origin already known is a string compare.
 */
struct favicon *favicon_want(const char *page_url, const char *declared);

/* Drive the queue: start at most one fetch, finish one that has landed.
 * Returns 1 when something changed and the view should be rebuilt. */
int favicon_pump(void);

/* Is an icon still outstanding? Used to keep the frame clock ticking. */
int favicon_pending(void);

#endif /* _EMBLINK_WEB_FAVICON_H_ */

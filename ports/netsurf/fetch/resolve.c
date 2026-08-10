/* ports/netsurf/fetch/resolve.c -- the two functions the OS's HTTP client
 * expects its host program to supply, answered by NetSurf instead of by Vellum.
 *
 * user/web/net.c and user/web/url.c were written for the OS's own browser and
 * call back into it for three things: resolving a relative URL against a base,
 * and reading and writing cookies. Linking Vellum's html.c to get the first
 * would drag an entire second browser engine into this one.
 *
 * NetSurf already has both, better: nsurl_join is a full RFC 3986 resolver, and
 * a cookie jar it manages itself through urldb. So these forward rather than
 * reimplement -- which is the whole point of the seam being a function pointer
 * shaped hole rather than a hard dependency.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "utils/errors.h"
#include "utils/nsurl.h"

/* Declared by user/web/html.h, which is not included here: pulling in Vellum's
 * parser header for one prototype would be the coupling this file exists to
 * avoid. The signature is copied and must match. */
int html_resolve_url(const char *base, const char *href, char *out, size_t cap);

int html_resolve_url(const char *base, const char *href, char *out, size_t cap)
{
    struct nsurl *b = NULL, *joined = NULL;
    int rc = -1;

    if (out == NULL || cap == 0) return -1;
    out[0] = '\0';
    if (href == NULL) return -1;

    /* No base, or one we cannot parse: the href is all there is. Copying it
     * through is what a browser does with an absolute URL, and refusing would
     * turn every absolute link into a dead one. */
    if (base == NULL || nsurl_create(base, &b) != NSERROR_OK) {
        snprintf(out, cap, "%s", href);
        return 0;
    }

    if (nsurl_join(b, href, &joined) == NSERROR_OK && joined != NULL) {
        snprintf(out, cap, "%s", nsurl_access(joined));
        nsurl_unref(joined);
        rc = 0;
    } else {
        snprintf(out, cap, "%s", href);
        rc = 0;
    }
    nsurl_unref(b);
    return rc;
}

/* COOKIES. NetSurf keeps its own jar (urldb) and applies it inside its own
 * fetchers; ours goes through the OS's HTTP client, which asks the host
 * program for the header. Wiring the two together means teaching the client
 * about urldb, which is a real feature and not a stub -- so these say NOTHING
 * rather than say something wrong, and a session cookie simply does not
 * persist yet. Logged in docs/TODO.md rather than left to be discovered when
 * a login does not stick. */
int cookie_header(const char *url, char *out, size_t cap);
int cookie_header(const char *url, char *out, size_t cap)
{
    (void)url;
    if (out != NULL && cap > 0) out[0] = '\0';
    return 0;
}

void cookie_take_headers(const char *url, const char *headers, size_t len);
void cookie_take_headers(const char *url, const char *headers, size_t len)
{
    (void)url; (void)headers; (void)len;
}

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
#include <stdlib.h>
#include <string.h>

#include "utils/errors.h"
#include "utils/nsurl.h"
#include "content/urldb.h"

/* THE DECLARATIONS WE ARE IMPLEMENTING, included so the compiler checks them.
 * Without this the only thing tying these definitions to their callers is the
 * linker matching a name, which is how a three-argument cookie_header came to
 * satisfy a five-argument call and corrupt the request path. A prototype that
 * is copied rather than included is a prototype that can drift. */
#include "cookie.h"

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

/* COOKIES: OFF, deliberately, and with the RIGHT signatures.
 *
 * These forwarded to NetSurf's jar (urldb) for one commit, and wiring them is
 * what broke the browser: the first version invented three-argument versions
 * of cookie.h's five- and two-argument functions, C matched them by name at
 * link time, and the arguments landed in the wrong registers -- so the
 * "output buffer" was really the request path and every request went out with
 * an empty target. Two more sessions went into the damage.
 *
 * The signatures below are cookie.h's, exactly, so the compiler checks them
 * (cookie.h is included above for that reason and no other). The bodies are
 * inert: no cookie is ever sent or stored, which is the state the browser
 * demonstrably fetched real pages in. Nothing here is hard -- urldb does the
 * work and the forwarding is fifteen lines -- but it goes back in when the
 * browser is being worked on deliberately rather than while it is the thing
 * standing between this OS and its audio stack.
 *
 * Consequence, stated so it is not discovered: a login will not stick, and a
 * site that requires a session cookie will keep asking. */

size_t cookie_header(const char *host, const char *path, int secure,
                     char *out, size_t cap)
{
    (void)host; (void)path; (void)secure;
    if (out != NULL && cap > 0) out[0] = '\0';
    return 0;
}

int cookie_take_headers(const char *host, const char *headers)
{
    (void)host; (void)headers;
    return 0;
}

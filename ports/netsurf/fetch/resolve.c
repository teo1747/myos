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

/* COOKIES, through NetSurf's own jar (urldb): expiry, domain and path
 * matching and the HttpOnly rule, rather than remembering strings.
 *
 * THE SIGNATURES ARE user/web/cookie.h's, EXACTLY. The first version of this
 * file invented three-argument versions, and C let it: net.c saw cookie.h's
 * five-argument declaration, the linker matched by name alone, and the
 * arguments landed in the wrong registers -- so `out` was really `u->path`
 * and `cap` was really the secure flag. Writing the terminating NUL then
 * emptied the request path, and every request went out as `GET  HTTP/1.0`.
 * Google parsed the bare "HTTP/1.0" as an absolute URI and answered "the
 * requested URL /1.0 was not found", which is a 404 from the far end of the
 * internet caused by a prototype in this file.
 *
 * In memory only: urldb's file is neither loaded nor saved, so a cookie lives
 * as long as the process. */

/* The `Cookie:` value for host+path, without the header name. Returns the
 * number of bytes written. */
size_t cookie_header(const char *host, const char *path, int secure,
                     char *out, size_t cap)
{
    struct nsurl *u = NULL;
    char urlbuf[1024];
    char *jar;
    size_t n;

    if (out == NULL || cap == 0) return 0;
    out[0] = '\0';
    if (host == NULL) return 0;

    /* urldb matches on a URL, not on host+path, and the SCHEME decides
     * whether a Secure cookie may be sent -- which is the entire point of the
     * `secure` argument being here. */
    snprintf(urlbuf, sizeof urlbuf, "%s://%s%s",
             secure ? "https" : "http", host,
             (path != NULL && path[0] != '\0') ? path : "/");
    if (nsurl_create(urlbuf, &u) != NSERROR_OK) return 0;

    /* true: this IS the network layer, and HttpOnly exists to hide a cookie
     * from scripts rather than from the request that carries it. */
    jar = urldb_get_cookie(u, true);
    nsurl_unref(u);
    if (jar == NULL) return 0;

    snprintf(out, cap, "%s", jar);
    free(jar);
    n = strlen(out);
    return n;
}

/* Every Set-Cookie line in a response header block. Returns how many were
 * stored -- a server may send several, and they are separate headers rather
 * than one comma-joined list. */
int cookie_take_headers(const char *host, const char *headers)
{
    struct nsurl *u = NULL;
    char urlbuf[1024];
    const char *p = headers;
    int stored = 0;

    if (host == NULL || headers == NULL) return 0;
    snprintf(urlbuf, sizeof urlbuf, "https://%s/", host);
    if (nsurl_create(urlbuf, &u) != NSERROR_OK) return 0;

    while (*p != '\0') {
        const char *eol = strchr(p, '\n');
        size_t n = eol != NULL ? (size_t)(eol - p) : strlen(p);
        while (n > 0 && (p[n - 1] == '\r' || p[n - 1] == ' ')) n--;

        if (n > 11) {                       /* case-insensitive "set-cookie:" */
            static const char key[] = "set-cookie:";
            size_t i = 0;
            while (i < 11) {
                char c = p[i];
                if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
                if (c != key[i]) break;
                i++;
            }
            if (i == 11) {
                char line[1024];
                size_t vlen = n - 11;
                const char *v = p + 11;
                while (vlen > 0 && *v == ' ') { v++; vlen--; }
                if (vlen >= sizeof line) vlen = sizeof line - 1;
                memcpy(line, v, vlen);
                line[vlen] = '\0';
                if (urldb_set_cookie(line, u, NULL)) stored++;
            }
        }
        if (eol == NULL) break;
        p = eol + 1;
    }
    nsurl_unref(u);
    return stored;
}

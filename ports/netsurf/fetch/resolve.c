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

/* COOKIES, through NetSurf's own jar. The OS's HTTP client asks its host
 * program for the Cookie header and hands back the Set-Cookie lines; NetSurf
 * keeps a real jar in urldb with expiry, domain and path matching and the
 * HttpOnly rule. Forwarding to it is how a login sticks -- and how it stays
 * scoped, which a naive "remember every header" would not be.
 *
 * In memory only: urldb's file is not loaded or saved yet, so a cookie lives
 * as long as the process. That is the difference between a session cookie and
 * a persistent one, and it is the remaining half of this feature. */
int cookie_header(const char *url, char *out, size_t cap);
int cookie_header(const char *url, char *out, size_t cap)
{
    struct nsurl *u = NULL;
    char *jar;

    if (out == NULL || cap == 0) return 0;
    out[0] = '\0';
    if (url == NULL) return 0;
    if (nsurl_create(url, &u) != NSERROR_OK) return 0;

    /* false: HttpOnly cookies are for the network layer, and this IS the
     * network layer -- but the OS's client puts the string on the wire
     * verbatim, so asking for them here is what makes HttpOnly mean anything
     * at all rather than nothing. */
    jar = urldb_get_cookie(u, true);
    nsurl_unref(u);
    if (jar == NULL) return 0;

    snprintf(out, cap, "%s", jar);
    free(jar);
    return (int)strlen(out);
}

/* Every Set-Cookie line in the response, one call. The client hands the whole
 * header block; urldb wants one header at a time. */
void cookie_take_headers(const char *url, const char *headers, size_t len);
void cookie_take_headers(const char *url, const char *headers, size_t len)
{
    struct nsurl *u = NULL;
    const char *p = headers, *end;

    if (url == NULL || headers == NULL || len == 0) return;
    if (nsurl_create(url, &u) != NSERROR_OK) return;

    end = headers + len;
    while (p < end) {
        const char *eol = memchr(p, '\n', (size_t)(end - p));
        size_t n = eol != NULL ? (size_t)(eol - p) : (size_t)(end - p);
        while (n > 0 && (p[n - 1] == '\r' || p[n - 1] == ' ')) n--;

        /* case-insensitive "set-cookie:" -- servers spell it every way */
        if (n > 11) {
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
                urldb_set_cookie(line, u, NULL);
            }
        }
        if (eol == NULL) break;
        p = eol + 1;
    }
    nsurl_unref(u);
}

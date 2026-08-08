/* user/web/net.h -- where a document's bytes come from.
 *
 * This is the seam docs/BROWSER.md put between fetch and parse, and B1 was
 * built against it deliberately: the browser above this line does not know or
 * care whether a page came off EMBKFS, off a socket, or out of a TLS session.
 * B2 is therefore this file appearing and one call in the app changing.
 *
 * One entry point on purpose. "Give me the bytes for this location" is a single
 * question, and the address bar should take a path and a URL without the user
 * telling it which is which.
 *
 * A browser is handed hostile input by definition, so the contract is bounded:
 * you supply the buffer, the fetch never exceeds it, and an oversized response
 * is TRUNCATED and reported rather than grown into.
 */
#ifndef _EMBLINK_WEB_NET_H_
#define _EMBLINK_WEB_NET_H_

#include <stddef.h>

struct vnet_result {
    int    status;          /* HTTP status; 200 synthesised for a local read  */
    size_t len;             /* bytes written to the caller's buffer           */
    int    truncated;       /* the response did not fit and was cut           */
    /* The server said how many bytes the body would be and fewer arrived. The
     * document is real but PARTIAL, so the caller may still render it -- and
     * must say so rather than present it as the whole page. */
    int    incomplete;
    int    redirects;       /* how many hops were followed                    */
    char   final_url[512];  /* where we ended up -- redirects change this     */
    char   via[64];         /* "file", "http", "https (authenticated)"        */
    char   err[160];        /* human-readable failure, empty on success       */
};

/* Fetch `url` into `out`. Returns 0 on success (including a non-2xx HTTP
 * response -- an error page is still a document, and showing the server's own
 * words beats inventing our own), or -1 if no bytes could be obtained, with
 * r->err explaining why. `r` is always filled in. */
int vnet_fetch(const char *url, char *out, size_t cap, struct vnet_result *r);

/* ...and the same with a POST body (form-urlencoded). `body` may be NULL,
 * which is exactly vnet_fetch. A form is the first thing this browser does
 * that SENDS, so POST is a real method here and not a stub -- a submit button
 * that silently performs a GET would lose the user's data to a server that
 * never sees it. */
int vnet_post(const char *url, const char *body,
              char *out, size_t cap, struct vnet_result *r);

#endif /* _EMBLINK_WEB_NET_H_ */

/* user/web/net.c -- fetching a document, over the filesystem or over the wire.
 *
 * The network path is this OS's own stack the whole way down: our DNS resolver,
 * our TCP, our TLS 1.3 with our own X25519/AES-GCM/SHA-384 and our own X.509
 * chain verification. Nothing here is a port. See docs/TLS.md and docs/NET.md.
 *
 * Deliberately HTTP/1.0 with `Connection: close`, which is the same choice wget
 * made and for the same reason: the server closing the socket frames the body,
 * so there is no chunked decoder to write and no keep-alive state machine to
 * get wrong. A browser that renders is worth more than one that pipelines.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include "embk.h"
#include "embk_socket.h"
#include "tls.h"

#include "url.h"
#include "net.h"
#include "charset.h"
#include "cookie.h"

#define MAX_REDIRECTS 5

/* A transport: a plain socket, or TLS over one. Same shape as wget's, because
 * it is the same idea -- everything above it is written once. */
struct xport { int fd; struct tls_conn *tls; };

static long x_send(struct xport *x, const void *b, size_t n) {
    return x->tls ? tls_write(x->tls, b, n) : send(x->fd, b, n, 0);
}
static long x_recv(struct xport *x, void *b, size_t n) {
    return x->tls ? tls_read(x->tls, b, n) : recv(x->fd, b, n, 0);
}
static void x_close(struct xport *x) {
    if (x->tls) tls_close(x->tls);      /* tls_close closes the fd; the conn is static */
    else if (x->fd >= 0) close(x->fd);
}

/* --- the filesystem arm -------------------------------------------------- */

static int fetch_local(const struct url *u, char *out, size_t cap,
                       struct vnet_result *r) {
    int fd = (int)embk_open(u->path, EMBK_O_RDONLY, 0);
    if (fd < 0) {
        snprintf(r->err, sizeof r->err, "No document at %.120s", u->path);
        return -1;
    }
    size_t n = 0;
    for (;;) {
        if (n + 1 >= cap) { r->truncated = 1; break; }
        int64_t got = embk_read(fd, out + n, cap - 1 - n);
        if (got <= 0) break;
        n += (size_t)got;
    }
    embk_close(fd);
    out[n] = 0;
    r->len = n;
    r->status = 200;
    snprintf(r->via, sizeof r->via, "file");
    return 0;
}

/* --- the network arm ----------------------------------------------------- */

/* Case-insensitive header lookup over a NUL-terminated header block. Returns a
 * pointer to the value (past the colon and its spaces) or NULL. */
static const char *hdr_find(const char *hdr, const char *name) {
    size_t nl = strlen(name);
    for (const char *l = hdr; l && *l; ) {
        const char *eol = strchr(l, '\n');
        if (!strncasecmp(l, name, nl) && l[nl] == ':') {
            const char *v = l + nl + 1;
            while (*v == ' ' || *v == '\t') v++;
            return v;
        }
        if (!eol) break;
        l = eol + 1;
    }
    return 0;
}

/* Copy a header value up to end-of-line, trimming the CR. */
static void hdr_value(const char *v, char *out, size_t cap) {
    size_t i = 0;
    while (v[i] && v[i] != '\r' && v[i] != '\n' && i + 1 < cap) { out[i] = v[i]; i++; }
    out[i] = 0;
}

/* One HTTP exchange, no redirect following. Returns 0 if a response was read
 * (any status), -1 if the transport failed. `location` gets the Location header
 * if there was one. */
static const char *g_post_body;      /* set for the duration of one submission */

static int http_once(const struct url *u, char *out, size_t cap,
                     struct vnet_result *r, char *location, size_t loc_cap) {
    location[0] = 0;

    struct in_addr addr;
    if (emb_resolve(u->host, &addr) != 0) {
        snprintf(r->err, sizeof r->err, "Cannot resolve %s", u->host);
        return -1;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        /* The single most likely cause, and worth naming: a process that was
         * not granted CAP_NETWORK cannot open a socket at all. */
        snprintf(r->err, sizeof r->err,
                 "No socket (%d) -- does this app hold the network capability?", fd);
        return -1;
    }

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port   = htons((unsigned short)u->port);
    sa.sin_addr   = addr;
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        snprintf(r->err, sizeof r->err, "Cannot connect to %s:%d", u->host, u->port);
        close(fd);
        return -1;
    }

    struct xport x = { fd, 0 };
    if (u->kind == URL_HTTPS) {
        /* STATIC, not malloc'd, and that is deliberate. This function runs on a
         * worker thread (fetchjob.c) so the UI does not freeze during a fetch,
         * and newlib's allocator here is not thread-safe -- __malloc_lock is a
         * stub. libtls itself allocates nothing, so keeping this one struct
         * static makes the whole fetch path allocation-free and the two threads
         * never touch the same allocator state. One fetch is in flight at a
         * time, which is what makes a single static sound. */
        /* Guard-banded because this context is STATIC: it lives in .bss beside
         * the browser's own document and scene state, where an overrun would
         * corrupt the page instead of crashing. 512 bytes turns a silent
         * corruption into a printed fact. It has stayed clean across every run
         * since -- which is how the "content vanishes during an https fetch"
         * symptom was ruled out as memory corruption. */
        static struct { uint64_t pre[32]; struct tls_conn c; uint64_t post[32]; } box;
        memset(&box, 0, sizeof box);
        x.tls = &box.c;
        int rc = tls_connect(x.tls, fd, u->host);
        if (rc != 0) {
            /* Refusing is the feature. An unauthenticated page is not a page
             * you were asked to show. */
            snprintf(r->err, sizeof r->err,
                     "TLS handshake failed (rc=%d) -- server not authenticated", rc);
            x.tls = 0; close(fd);
            return -1;
        }
        /* Did the handshake stay inside its own struct? This one lives in .bss
         * next to the browser's document and scene state, so an overrun here
         * would corrupt the page rather than crash -- exactly the shape of the
         * "content vanishes during an https fetch" symptom. Guard bands cost
         * 512 bytes and turn a silent corruption into a printed fact. */
        int spill = 0;
        for (unsigned gi = 0; gi < 32; gi++) if (box.pre[gi] || box.post[gi]) spill++;
        if (spill) {
            char gb[96];
            snprintf(gb, sizeof gb, "net: TLS CONTEXT OVERRUN -- %d guard words clobbered\n", spill);
            embk_puts(1, gb);
        }
        snprintf(r->via, sizeof r->via, "https (authenticated)");
    } else {
        snprintf(r->via, sizeof r->via, "http");
    }

    /* Whatever the jar has for this host and path. A site that cannot get its
     * cookie back is a site you are logged out of on every click. */
    char ck[1024]; ck[0] = 0;
    size_t cn = cookie_header(u->host, u->path, u->kind == URL_HTTPS, ck, sizeof ck);
    char ckhdr[1088]; ckhdr[0] = 0;
    if (cn) snprintf(ckhdr, sizeof ckhdr, "Cookie: %s\r\n", ck);

    /* 8K, and the length is CHECKED below. snprintf returns what it WOULD have
     * written, so a request that overflows this buffer used to be sent as
     * `rl` bytes out of a smaller array -- a truncated request line plus a
     * read past the end. Unreachable while the cookie header was always empty;
     * the moment cookies worked, a site with big ones (Google's are) sent a
     * malformed request and got a 404 for a path that was never there. */
    char req[8192];
    int rl;
    if (g_post_body) {
        rl = snprintf(req, sizeof req,
                      "POST %s HTTP/1.0\r\n"
                      "Host: %s\r\n"
                      "User-Agent: Vellum (EmbLinkOS)\r\n"
                      "Accept: text/html,*/*\r\n"
                      "Content-Type: application/x-www-form-urlencoded\r\n"
                      "Content-Length: %u\r\n"
                      "%s"
                      "Connection: close\r\n\r\n%s",
                      u->path, u->host,
                      (unsigned)strlen(g_post_body), ckhdr, g_post_body);
    } else {
        rl = snprintf(req, sizeof req,
                      "GET %s HTTP/1.0\r\n"
                      "Host: %s\r\n"
                      "User-Agent: Vellum (EmbLinkOS)\r\n"
                      "Accept: text/html,*/*\r\n"
                      "%s"
                      "Connection: close\r\n\r\n",
                      u->path, u->host, ckhdr);
    }
    /* If it STILL does not fit, drop the cookies and try again: a request
     * without them is a working request that loses your session, while a
     * truncated one is not a request at all. Only then give up. */
    if (rl < 0 || (size_t)rl >= sizeof req) {
        rl = snprintf(req, sizeof req,
                      "%s %s HTTP/1.0\r\n"
                      "Host: %s\r\n"
                      "User-Agent: Vellum (EmbLinkOS)\r\n"
                      "Accept: text/html,*/*\r\n"
                      "Connection: close\r\n\r\n",
                      g_post_body ? "POST" : "GET", u->path, u->host);
        if (rl < 0 || (size_t)rl >= sizeof req) {
            snprintf(r->err, sizeof r->err, "Request too large to send");
            x_close(&x);
            return -1;
        }
    }
    /* THE REQUEST LINE, on the console. A malformed one is invisible from this
     * side -- the server answers a 404 for a path you never asked for and the
     * bug looks like it is at the far end. NSREQ=1 to see it. */
    if (getenv("NSREQ") != NULL) {
        int n = 0;
        while (n < rl && req[n] != '\r') n++;
        fprintf(stderr, "net: [%.*s] pathlen=%d\n", n, req, (int)strlen(u->path));
        fflush(stderr);
    }
    if (x_send(&x, req, (size_t)rl) < 0) {
        snprintf(r->err, sizeof r->err, "Request failed");
        x_close(&x);
        return -1;
    }

    static char hdr[8192];
    char buf[4096];
    size_t n = 0;
    int hlen = 0, header_done = 0;

    int stalls = 0;
    for (;;) {
        long got = x_recv(&x, buf, sizeof buf);
        /* Nothing YET is not nothing coming -- see net_tcp_recv in the kernel.
         * The TLS path handles this inside its record reader, because it can be
         * stopped mid-record; this is the plain-HTTP arm, which can only be
         * stopped mid-body. Both used to read it as the end of the response. */
        if (got < 0 && errno == EAGAIN) {
            if (++stalls > 10) break;
            continue;
        }
        if (got <= 0) break;
        stalls = 0;
        int off = 0;
        if (!header_done) {
            for (int i = 0; i < got && !header_done; i++) {
                if (hlen < (int)sizeof hdr - 1) hdr[hlen++] = buf[i];
                if (hlen >= 4 && hdr[hlen-4]=='\r' && hdr[hlen-3]=='\n' &&
                                 hdr[hlen-2]=='\r' && hdr[hlen-1]=='\n') {
                    header_done = 1;
                    off = i + 1;
                }
            }
            if (!header_done) continue;
            hdr[hlen] = 0;
            /* Set-Cookie, before anything else looks at the response: a
             * redirect carries the session cookie that the NEXT request needs,
             * and taking it after following the hop is one hop too late. */
            cookie_take_headers(u->host, hdr);
            if (!strncmp(hdr, "HTTP/1.", 7)) r->status = atoi(hdr + 9);
            const char *loc = hdr_find(hdr, "Location");
            if (loc) hdr_value(loc, location, loc_cap);
        }
        size_t blen = (size_t)got - (size_t)off;
        if (blen) {
            if (n + blen + 1 > cap) { blen = cap - n - 1; r->truncated = 1; }
            if (blen) { memcpy(out + n, buf + off, blen); n += blen; }
            if (r->truncated) break;   /* bounded appetite: stop reading */
        }
    }
    x_close(&x);

    out[n] = 0;
    r->len = n;
    if (!header_done) {
        snprintf(r->err, sizeof r->err, "No response from %s", u->host);
        return -1;
    }
    /* DID ALL OF IT ARRIVE? The server said how long the body would be; a
     * shorter one means the connection ended early, and until this check
     * existed that was indistinguishable from a complete response. A page
     * arriving at a third of its length rendered as a page, with status 200 and
     * nothing anywhere saying otherwise -- which is how a networking bug spent
     * this long looking like a rendering one. */
    /* WHAT ENCODING THE BODY IS IN, straight from the header that states it.
     * Assuming UTF-8 turns every accent on an ISO-8859-1 page into a
     * replacement character -- which looks like a broken font and is not. */
    {
        const char *ct = hdr_find(hdr, "Content-Type");
        if (ct) {
            char v[160];
            hdr_value(ct, v, sizeof v);
            charset_from_content_type(v, r->charset, sizeof r->charset);
        }
    }
    const char *cl = hdr_find(hdr, "Content-Length");
    if (cl && !r->truncated) {
        char v[32];
        hdr_value(cl, v, sizeof v);
        unsigned long want = strtoul(v, 0, 10);
        if (want && n < want) {
            r->incomplete = 1;
            snprintf(r->err, sizeof r->err,
                     "Connection ended early: %zu of %lu bytes", n, want);
        }
    }
    return 0;
}

/* --- the one entry point ------------------------------------------------- */

int vnet_post(const char *url, const char *body,
              char *out, size_t cap, struct vnet_result *r) {
    g_post_body = body;
    int rc = vnet_fetch(url, out, cap, r);
    g_post_body = 0;         /* one submission only: a later GET must not
                              * inherit a body from a form the user has left */
    return rc;
}

int vnet_fetch(const char *url, char *out, size_t cap, struct vnet_result *r) {
    memset(r, 0, sizeof *r);
    if (!url || !out || cap < 2) {
        snprintf(r->err, sizeof r->err, "Nothing to fetch");
        return -1;
    }

    char here[512];
    snprintf(here, sizeof here, "%s", url);

    for (int hop = 0; ; hop++) {
        struct url u;
        if (url_parse(here, &u) != 0) {
            snprintf(r->err, sizeof r->err,
                     "Not a location Vellum understands: %.100s", here);
            snprintf(r->final_url, sizeof r->final_url, "%s", here);
            return -1;
        }
        snprintf(r->final_url, sizeof r->final_url, "%s", here);
        r->redirects = hop;

        if (u.kind == URL_LOCAL) return fetch_local(&u, out, cap, r);

        char location[512];
        if (http_once(&u, out, cap, r, location, sizeof location) != 0) return -1;

        int is_redirect = (r->status == 301 || r->status == 302 || r->status == 303 ||
                           r->status == 307 || r->status == 308);
        if (!is_redirect || !location[0]) return 0;

        if (hop >= MAX_REDIRECTS) {
            /* Not an error worth hiding: a redirect loop is a thing servers do,
             * and saying so is more useful than showing the last hop's body. */
            snprintf(r->err, sizeof r->err,
                     "Too many redirects (%d) -- stopped at %.100s", hop, here);
            return -1;
        }

        /* POST/redirect/GET: a 303 (and, in practice, a 301/302) after a POST
         * becomes a GET. Re-posting the body to the redirect target is how a
         * form gets submitted twice. */
        if (r->status == 301 || r->status == 302 || r->status == 303) g_post_body = 0;

        char next[512];
        if (url_resolve(here, location, next, sizeof next) != 0) {
            snprintf(r->err, sizeof r->err, "Bad redirect target: %.120s", location);
            return -1;
        }
        snprintf(here, sizeof here, "%s", next);
        /* a fresh hop reuses the buffer; clear what the last one said */
        r->status = 0; r->len = 0; r->truncated = 0; r->err[0] = 0;
    }
}

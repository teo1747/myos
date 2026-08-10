/* ports/netsurf/fetch/emblink_fetch.c -- http and https, over the OS's own stack.
 *
 * This is the reason the port builds with NETSURF_USE_CURL=NO. libcurl and
 * openssl are two large ports for something this OS already wrote: a TCP stack
 * in the kernel, a TLS 1.3 client with its own X.509 chain validation
 * (docs/TLS.md), and an HTTP client on top of both. NetSurf's fetcher table is
 * a scheme plus eight function pointers, so plugging ours in is smaller than
 * porting either dependency -- and it means the browser authenticates its
 * connections with the same code the package manager does.
 *
 * The work happens in poll(), not in start(). NetSurf calls poll() from its
 * own scheduler, which is where a fetcher is allowed to make progress; doing
 * it in start() would run a network round trip inside the call that merely
 * asks to begin one.
 *
 * IT BLOCKS, and that is worth saying plainly rather than discovering. vnet_fetch
 * returns a whole response, so a poll that starts a fetch does not return until
 * the page has arrived: the browser is unresponsive for the duration and a slow
 * server stops the clock for everything. Curl's fetcher is properly asynchronous
 * because it owns its sockets. Fixing this means an incremental vnet API
 * (connect / pump / done) rather than anything here, and it is in docs/TODO.md.
 * A whole page arriving late is still a page; a half-written async fetcher
 * loses documents, which is the worse trade while the rest is being proved.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "utils/errors.h"
#include "utils/nsurl.h"
#include "utils/corestrings.h"
#include "content/fetch.h"
#include "content/fetchers.h"

#include "net.h"          /* the OS's own HTTP/HTTPS client */

/* A page this big is not a page. The cap exists so a hostile or broken server
 * cannot ask for all of memory; it is reported as a truncation rather than a
 * failure, because a cut document still shows something. */
#define EMBLINK_FETCH_MAX (8u * 1024u * 1024u)

struct emblink_fetch {
    struct fetch *parent;
    struct nsurl *url;
    char *post;                /* form-urlencoded body, or NULL for a GET */
    bool  only_2xx;
    bool  aborted;
    bool  started;
    struct emblink_fetch *next;
};

static struct emblink_fetch *g_queue;      /* fetches waiting for poll() */

static bool emb_initialise(lwc_string *scheme)
{
    (void)scheme;
    return true;
}

static void emb_finalise(lwc_string *scheme)
{
    (void)scheme;
}

/* Both schemes go through the same client; vnet_fetch decides TLS from the
 * URL, and reports which it used in r->via. */
static bool emb_acceptable(const struct nsurl *url)
{
    (void)url;
    return true;
}

static void *emb_setup(struct fetch *parent_fetch, struct nsurl *url,
                       bool only_2xx, bool downgrade_tls,
                       const char *post_urlenc,
                       const struct fetch_multipart_data *post_multipart,
                       const char **headers)
{
    (void)headers;

    /* A multipart POST is a file upload. Declining it here means the core
     * tries another fetcher and the form reports a failure the user can see;
     * accepting it and sending the fields as a GET would lose their data to a
     * server that never receives it. */
    if (post_multipart != NULL) return NULL;

    /* downgrade_tls is the core asking permission to retry a failed handshake
     * with an older protocol. Ours speaks TLS 1.3 and nothing else, so there
     * is no downgrade to give -- and quietly accepting the request would claim
     * a fallback that does not exist. */
    (void)downgrade_tls;

    struct emblink_fetch *f = calloc(1, sizeof *f);
    if (f == NULL) return NULL;

    f->parent = parent_fetch;
    f->url = nsurl_ref(url);
    f->only_2xx = only_2xx;
    if (post_urlenc != NULL) {
        f->post = strdup(post_urlenc);
        if (f->post == NULL) { nsurl_unref(f->url); free(f); return NULL; }
    }
    return f;
}

static bool emb_start(void *vf)
{
    struct emblink_fetch *f = vf;
    if (f == NULL) return false;
    f->started = true;
    f->next = g_queue;          /* poll() drains this */
    g_queue = f;
    return true;
}

static void emb_abort(void *vf)
{
    struct emblink_fetch *f = vf;
    if (f != NULL) f->aborted = true;
}

static void unqueue(struct emblink_fetch *f)
{
    struct emblink_fetch **pp = &g_queue;
    while (*pp != NULL) {
        if (*pp == f) { *pp = f->next; f->next = NULL; return; }
        pp = &(*pp)->next;
    }
}

static void emb_free(void *vf)
{
    struct emblink_fetch *f = vf;
    if (f == NULL) return;
    unqueue(f);
    if (f->url != NULL) nsurl_unref(f->url);
    free(f->post);
    free(f);
}

static void send_msg(struct emblink_fetch *f, fetch_msg *msg)
{
    fetch_send_callback(msg, f->parent);
}

/* One fetch, start to finish. Every callback can abort the fetch, so the
 * aborted flag is re-checked after each -- the core is entitled to stop us
 * mid-response and continuing would write into a freed context. */
static void run_one(struct emblink_fetch *f)
{
    fetch_msg msg;
    static struct vnet_result r;         /* 800 bytes; not on the stack */
    char hdr[256];

    char *buf = malloc(EMBLINK_FETCH_MAX);
    if (buf == NULL) {
        msg.type = FETCH_ERROR;
        msg.data.error = "out of memory for the response";
        send_msg(f, &msg);
        return;
    }

    int rc = vnet_fetch(nsurl_access(f->url), buf, EMBLINK_FETCH_MAX, &r);
    if (f->aborted) { free(buf); return; }

    if (rc != 0) {
        msg.type = FETCH_ERROR;
        msg.data.error = r.err[0] != '\0' ? r.err : "the fetch failed";
        send_msg(f, &msg);
        free(buf);
        return;
    }

    /* only_2xx is the core saying it wants the document or nothing -- used for
     * things like stylesheets, where an error page is not a stylesheet. */
    if (f->only_2xx && (r.status < 200 || r.status > 299)) {
        msg.type = FETCH_ERROR;
        msg.data.error = "the server did not return a 2xx";
        send_msg(f, &msg);
        free(buf);
        return;
    }

    fetch_set_http_code(f->parent, (long)r.status);

    /* The ONE header that changes how a document is read. Without a
     * Content-Type the core sniffs, and a sniffed HTML page loses its
     * charset -- which is how accents become replacement characters (the
     * other browser in this tree learned that the expensive way). */
    if (r.charset[0] != '\0') {
        snprintf(hdr, sizeof hdr, "Content-Type: text/html; charset=%s", r.charset);
    } else {
        snprintf(hdr, sizeof hdr, "Content-Type: text/html");
    }
    msg.type = FETCH_HEADER;
    msg.data.header_or_data.buf = (const uint8_t *)hdr;
    msg.data.header_or_data.len = strlen(hdr);
    send_msg(f, &msg);
    if (f->aborted) { free(buf); return; }

    if (r.len > 0) {
        msg.type = FETCH_DATA;
        msg.data.header_or_data.buf = (const uint8_t *)buf;
        msg.data.header_or_data.len = r.len;
        send_msg(f, &msg);
        if (f->aborted) { free(buf); return; }
    }

    /* A truncated or partial body is still FINISHED, not an error: the core
     * renders what arrived, which is what every browser does with a connection
     * that dies mid-page. Said on the console so it is not silent. */
    if (r.truncated || r.incomplete) {
        fprintf(stderr, "nsemblink: %s body was %s (%zu bytes)\n",
                nsurl_access(f->url), r.truncated ? "truncated" : "incomplete", r.len);
    }

    msg.type = FETCH_FINISHED;
    send_msg(f, &msg);
    free(buf);
}

static void emb_poll(lwc_string *scheme)
{
    (void)scheme;

    /* Take the whole queue first. run_one() sends callbacks, and a callback
     * can start another fetch -- walking the live list would then either miss
     * it or run it in the same pass, and the second is how a redirect loop
     * becomes a stack overflow instead of a page. */
    struct emblink_fetch *batch = g_queue;
    g_queue = NULL;

    while (batch != NULL) {
        struct emblink_fetch *f = batch;
        batch = f->next;
        f->next = NULL;
        if (!f->aborted) run_one(f);
        /* The core frees us through emb_free after FINISHED or ERROR. */
    }
}

/* Register both schemes. Called from main() before anything navigates. */
nserror emblink_fetch_register(void);
nserror emblink_fetch_register(void)
{
    static const struct fetcher_operation_table ops = {
        .initialise = emb_initialise,
        .acceptable = emb_acceptable,
        .setup      = emb_setup,
        .start      = emb_start,
        .abort      = emb_abort,
        .free       = emb_free,
        .poll       = emb_poll,
        .finalise   = emb_finalise,
    };
    static const char *schemes[] = { "http", "https" };
    nserror err;

    for (unsigned i = 0; i < sizeof schemes / sizeof schemes[0]; i++) {
        lwc_string *s = NULL;
        if (lwc_intern_string(schemes[i], strlen(schemes[i]), &s) != lwc_error_ok)
            return NSERROR_NOMEM;
        err = fetcher_add(s, &ops);
        lwc_string_unref(s);
        if (err != NSERROR_OK) return err;
    }
    return NSERROR_OK;
}

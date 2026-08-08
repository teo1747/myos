/* vellum.c -- EmbLink's browser. See docs/BROWSER.md.
 *
 * B1: the pipeline end to end, minus the network. Load a document from the
 * filesystem, parse it, style it with the user-agent stylesheet, render it,
 * and follow links between local pages. Everything except where the bytes come
 * from -- which is exactly the seam the design put between fetch and parse, so
 * B2 changes one function and nothing else.
 *
 * The chrome is TWO rows, not the house AppBar. A browser's title bar has
 * nothing to put in it -- the app's name never changes -- so the lights share
 * the tab strip, and the tab carries the page title instead. Below that, one
 * row holding back/forward/reload and the address, in that order, because they
 * all act on the same thing.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "embk.h"
#include "oscfg.h"   /* the user's dark/light choice answers prefers-color-scheme */
#include "ui.h"
#include "em.h"
#include "theme.h"
#include "html.h"
#include "style.h"
#include "render.h"
#include "css.h"
#include "imgcache.h"
#include "favicon.h"
#include "jsdom.h"
#include "form.h"

/* who owns a fetch on the shared worker */
#define DOC_TAG 1
#include "url.h"
#include "net.h"
#include "fetchjob.h"
#include "select.h"
#include "cssref.h"
#include "cookie.h"
#include "find.h"
#include "history.h"
#include "tabs.h"
#include "store.h"

/* One document at a time, in fixed arenas. A browser that can be handed a
 * hostile page needs a bounded appetite -- see docs/BROWSER.md §7. */
#define SRC_MAX   (512 * 1024)
/* Sized from what real pages need, counted rather than rounded: Wikipedia's
 * "Operating system" article parses to about 9400 nodes and MDN's flex page to
 * about 8100, so 8192 missed both -- and missing means the DOCUMENT STOPS.
 * Wikipedia's header alone filled the old arena and the article was never
 * parsed, which is not a truncated page, it is no page. */
#define NODE_MAX  16384
#define STR_MAX   (1024 * 1024)

/* The bytes of the page being DISPLAYED. They live in the current TAB rather
 * than in one buffer here, because a background tab has to keep its own -- see
 * tabs.h for why the tab keeps its source and not its parsed document. */
#define g_src      (tab_src(tab_current()))
#define G_SRC_MAX  TAB_SRC_MAX
/* ...and a second buffer for the one being FETCHED. They cannot be the same
 * buffer: html_parse stores pointers INTO the source, so every text node of
 * the page on screen points into g_src. A worker writing the next response
 * there dissolves the current page under the renderer -- the window went blank
 * mid-fetch, which looked like a repaint bug and was not. The bytes only move
 * across once the fetch is finished and nothing is reading the old ones. */
static char             g_incoming[SRC_MAX];
static struct html_node g_nodes[NODE_MAX];
static char             g_strs[STR_MAX];
static struct html_doc  g_doc;
static struct css_sheet g_sheet;   /* the page's own <style>, cascaded */


/* The cascade is built from EVERY sheet the page has: the external ones that
 * have arrived so far, then the document's own <style>. Rebuilt each time a
 * <link> lands, because css_sheet_parse replaces a sheet rather than extending
 * it -- and because a page must be readable before the last stylesheet does. */
/* Every sheet the page has, concatenated. Sized from the same measurement
 * as CSS_MAX_RULES: python.org ships 513KB of CSS across four files, and a
 * buffer that overflows silently drops whichever sheet did not fit. */
static char g_allcss[1024 * 1024];
/* The width a media query is evaluated against: the document's content box,
 * which is the window minus the ScrollView's padding -- what the page actually
 * gets to lay out in. */
static float sheet_viewport_w(void) { return em_viewport_width() - 44.0f; }

/* Every caller of this is a reason the computed styles are stale. */
static void rebuild_sheet(void) {
    struct oscfg cfg; oscfg_load(&cfg);
    css_media_set(sheet_viewport_w(), em_viewport_height() - 110.0f, cfg.dark != 0);
    size_t n = 0, extn = 0;
    const char *ext = cssref_text(&extn);
    if (ext && extn) {
        if (extn > sizeof g_allcss - 2) extn = sizeof g_allcss - 2;
        memcpy(g_allcss, ext, extn);
        n = extn;
        g_allcss[n++] = '\n';
    }
    if (g_doc.css && g_doc.css_len) {
        size_t k = g_doc.css_len;
        if (n + k > sizeof g_allcss - 1) k = sizeof g_allcss - 1 - n;
        memcpy(g_allcss + n, g_doc.css, k);
        n += k;
    }
    g_allcss[n] = 0;
    css_sheet_parse(&g_sheet, n ? g_allcss : 0, n);
    /* The cascade changed, so every computed style is stale. This is the one
     * that matters mid-load: a <link> landing must restyle the page, and the
     * memo now survives frames precisely so that nothing else does. */
    vstyle_cache_invalidate();
}
static int              g_root = -1;

static char  g_url[512]   = "";
static char  g_bar[512]   = "";      /* what the URL field is showing        */
static char  g_status[256] = "";
static char  g_console[256] = "";     /* the page's last console.log */
static char  g_status_done[256] = ""; /* what the status said before loading */
static int   g_status_busy;

/* What the status line says about the load that produced this page. Kept as
 * FIELDS rather than composed once, because the counts it reports keep
 * changing after the document lands: an external stylesheet arriving adds
 * rules. The line said "1 css rule" on a page with four while three of them
 * were already applied on screen -- a status line that under-reports is the
 * same lie as one that over-reports. */
static struct {
    int    status;
    char   via[64];
    size_t bytes;
    int    res_trunc;
    int    res_partial;      /* fewer bytes than Content-Length promised */
} g_st;

static void update_status(void) {
    char css[80]; css[0] = 0;
    if (g_sheet.n) snprintf(css, sizeof css, "  %d css rule%s%s", g_sheet.n,
                            g_sheet.n == 1 ? "" : "s", g_sheet.truncated ? "+" : "");
    if (cssref_pending()) {
        size_t k = strlen(css);
        snprintf(css + k, sizeof css - k, " (+css)");
    }
    if (g_doc.n_js) {
        size_t k = strlen(css);
        snprintf(css + k, sizeof css - k, "  %d script%s",
                 g_doc.n_js, g_doc.n_js == 1 ? "" : "s");
    }
    snprintf(g_status, sizeof g_status, "%d  %s  %zu bytes  %d nodes%s%s%s%s",
             g_st.status, g_st.via, g_st.bytes, g_doc.n, css,
             g_st.res_trunc  ? "  (response truncated)" : "",
             g_st.res_partial ? "  (INCOMPLETE -- the connection ended early)" : "",
             g_doc.truncated ? "  (document truncated)" : "");
    snprintf(g_status_done, sizeof g_status_done, "%s", g_status);
}
static float g_scroll = 0;

/* Back/forward, the same shape Files uses -- "back" alone is half a history. */
/* Back and forward belong to the TAB -- see tabs.c. A shared stack would make
 * "back" in one tab go to a page you were reading in another. */

/* A navigation requested by a click, acted on AFTER the frame: the href lives
 * in the arena the load is about to overwrite. */
static char g_goto[512] = "";
/* Bumped per document; the view's root container is keyed by it. See
 * install_document. */
static unsigned g_doc_gen;
/* Tab clicks, deferred to the top of the next frame -- see where they are
 * acted on. */
static int  g_switch_to = -1, g_close_tab = -1, g_new_tab = 0;
static void on_link(const char *href) { (void)href; }

/* A script's output has to go SOMEWHERE a person can see, or console.log is a
 * call that does nothing observable -- the exact thing this browser refuses to
 * ship elsewhere. The status line is where the browser already tells the truth
 * about a page, so it is where a page's own words go too. */
/* A click reached an element a script is listening to. The handler may rewrite
 * the document, so the dirty flag is checked right after -- that is what makes
 * a button on a page actually change the page. */
/* A POST is a navigation that carries something. It shares the history and
 * load path with an ordinary one -- a form submission IS a page visit -- and
 * differs only in what goes on the wire. Declared up here because `load` is
 * what consults them and it comes first. */
static char g_post[1024];
static int  g_have_post;
static void navigate(const char *url);
static void navigate_post(const char *url, const char *body);

/* Submitting is just navigating, which is the whole reason a form is not a
 * special case in this browser: build the URL (and body, for POST) and hand it
 * to the same load path a link uses. */
static void on_submit(int node) {
    /* The page hears about it FIRST. A script that validates a form, or that
     * handles the submission itself, has to run before the navigation -- and
     * if it calls preventDefault the navigation must not happen at all.
     * preventDefault is not implemented, so this is a notification and not yet
     * a veto; docs/TODO.md says so rather than the script finding out. */
    if (jsdom_dispatch_submit(node)) {
        /* A handler called preventDefault: the page is handling this itself,
         * so the browser must NOT navigate. Half the forms on the modern web
         * are submitted with fetch() from exactly here. */
        em_request_frame();
        return;
    }
    char url[512], body[1024];
    int how = form_submit(&g_doc, node, g_url, url, sizeof url, body, sizeof body);
    if (how == 1)      navigate(url);
    else if (how == 2) navigate_post(url, body);
}

static void on_dom_click(int node) {
    if (jsdom_dispatch_click(node)) em_request_frame();
}

static void on_console(const char *line) {
    snprintf(g_console, sizeof g_console, "%s", line);
}

/* --- loading ------------------------------------------------------------ */

/* B2: this is now one call. Where the bytes come from -- EMBKFS, a socket, a
 * TLS session -- is net.c's business, and the seam the design put here is the
 * reason the browser above it did not have to change. */

/* The error page. A browser that shows a blank window on failure is a browser
 * you cannot debug -- so failures are DOCUMENTS, and go through exactly the
 * same parse/style/render path as any other page. */
static void load_error(const char *url, const char *why) {
    snprintf(g_src, G_SRC_MAX,
             "<h1>Cannot open this page</h1>"
             "<p>%s</p><p><b>%s</b></p>"
             "<p>Vellum reads documents from the filesystem in this build. "
             "Try <a href=\"/system/web/index.html\">the start page</a>.</p>",
             why, url);
    g_root = html_parse(&g_doc, g_src, strlen(g_src), g_nodes, NODE_MAX, g_strs, STR_MAX);
    cssref_reset(); rebuild_sheet();
    imgcache_reset();
    vsel_reset();
    snprintf(g_status, sizeof g_status, "%s", why);
}

/* Make the current tab's retained bytes the LIVE document: parse them, build
 * the cascade, and give the page a fresh JS world.
 *
 * Two callers, and the fact that they are the same code is the point. One is a
 * fetch landing. The other is a TAB SWITCH -- a tab keeps its source rather
 * than its parsed document (tabs.h says why), so returning to one runs exactly
 * the pipeline the page ran when it first arrived, with no second path that
 * could disagree about what the page means.
 *
 * Returns 0, or -1 having already shown the error page. */
static int install_document(size_t n) {
    g_root = html_parse(&g_doc, g_src, n, g_nodes, NODE_MAX, g_strs, STR_MAX);
    if (g_root < 0) { load_error(g_url, "The document could not be parsed."); return -1; }
    /* the author's stylesheet, borrowed from the document arena (which is why
     * it is parsed here, once, and not per frame) */
    cssref_start(&g_doc, g_url);
    rebuild_sheet();
    vstyle_cache_invalidate();     /* a different document entirely */
    imgcache_reset();          /* one page's pictures never leak into the next */
    vsel_reset();              /* ...nor does a selection: it indexed the OLD words */
    form_reset();              /* ...nor one page's typing into the next */
    vellum_reset_details();    /* ...nor which disclosures it had open */
    /* A NEW DOCUMENT means the view is REBUILT rather than reconciled against
     * the page that was there before -- see the key on the document's
     * container below. */
    g_doc_gen++;

    /* A NEW WORLD per page: the engine is torn down and rebuilt, so a script
     * cannot outlive the document that wrote it, and one page's globals can
     * never be read by the next. Then run what the page brought. */
    g_console[0] = 0;
    jsdom_set_console(on_console);
    jsdom_set_url(g_url);
    if (jsdom_open(&g_doc, &g_sheet) == 0 && g_doc.n_js > 0) {
        int failed = jsdom_run_scripts();
        jsdom_take_dirty();     /* the first render happens anyway */
        if (failed && !g_console[0])
            snprintf(g_console, sizeof g_console, "%d script(s) threw", failed);
    }
    return 0;
}

/* Starting a load no longer BLOCKS. A TLS handshake to a real host is several
 * round trips and each one is a frame the window does not draw -- from the
 * outside that is an application that has died. The fetch runs on a worker
 * (fetchjob.c) and the view keeps drawing, showing what it is waiting for. */
static int about_page(const char *url);

static void load(const char *url) {
    snprintf(g_url, sizeof g_url, "%s", url);
    snprintf(g_bar, sizeof g_bar, "%s", url);
    g_scroll = 0;
    /* A generated page needs no worker, no socket and no wait. */
    if (about_page(url)) return;
    int started = g_have_post
        ? fetchjob_start_post(url, g_post, g_incoming, sizeof g_incoming, DOC_TAG)
        : fetchjob_start(url, g_incoming, sizeof g_incoming, DOC_TAG);
    g_have_post = 0;           /* one submission: a later link must not re-post */
    if (started != 0) {
        /* One at a time. Refusing is honest: the page you asked for first is
         * still coming, and silently dropping it would be worse. */
        snprintf(g_status, sizeof g_status, "Still loading %s", fetchjob_url());
        return;
    }
    snprintf(g_status, sizeof g_status, "Loading...  0.0s");
    /* ARM THE REPAINT HERE, where the load begins.
     *
     * The clock that ticks "Loading..." is armed at the top of app(), by a
     * check that asks whether a fetch is running. But every navigation starts
     * INSIDE the view build -- a link click, Return in the address bar, a form
     * submit -- so on the frame that starts one, that check has already run and
     * said no. The frame ended with a fetch in flight and nothing scheduled to
     * look at it again.
     *
     * With a mouse that never stopped moving this was invisible: pointer events
     * kept producing frames, and one of them noticed the page had arrived.
     * Return from the keyboard leaves the machine perfectly still, and the page
     * then sat fully fetched and unpainted until the reader moved the mouse --
     * 559 bytes of example.com, arrived, parsed, and invisible. */
    em_app_set_refresh(200);
    em_request_frame();
    (void)url;
}

/* The other half of load(), run when the bytes actually arrive. */
static void finish_load(const struct vnet_result *res) {
    if (res->err[0] && res->len == 0) {
        load_error(g_url, res->err);
        return;
    }
    /* A redirect changes where you ARE, and the address bar has to say so. */
    if (res->redirects) {
        snprintf(g_url, sizeof g_url, "%s", res->final_url);
        snprintf(g_bar, sizeof g_bar, "%s", res->final_url);
    }

    /* Now, and only now, is it safe: the fetch is done, so nothing is writing
     * g_incoming and nothing is reading the old g_src any more. */
    size_t n = res->len < G_SRC_MAX - 1 ? res->len : G_SRC_MAX - 1;
    memcpy(g_src, g_incoming, n);
    g_src[n] = 0;
    tab_set_src_len(tab_current(), n);
    if (install_document(n) != 0) return;

    /* A response may have set a cookie, so the jar is written after a load
     * rather than on a timer: the moment it can have changed is the moment
     * worth spending a write on. */
    cookie_save();
    /* Record the visit, with the page's own title -- a history listed by URL
     * is a history you have to decode. Generated pages are excluded inside
     * hist_add: about:history inside the history is a mirror facing a mirror. */
    {
        const char *t = "";
        for (int i = 0; i < g_doc.n; i++)
            if (g_doc.nodes[i].kind == HTML_ELEM && !strcmp(g_doc.nodes[i].tag, "title")) {
                int c = g_doc.nodes[i].first_child;
                if (c >= 0 && g_doc.nodes[c].text) t = g_doc.nodes[c].text;
                break;
            }
        hist_add(g_url, t, embk_now_unix());
        hist_save();
        /* ...and name the TAB with the same title, so the strip says what the
         * page is rather than where it came from. */
        tab_set_title(tab_current(), t);
        tab_set_url(tab_current(), g_url);
    }
    g_st.status = res->status;
    snprintf(g_st.via, sizeof g_st.via, "%s", res->via);
    g_st.bytes = n;
    g_st.res_trunc   = res->truncated;
    g_st.res_partial = res->incomplete;
    update_status();
}

static void navigate_post(const char *url, const char *body) {
    snprintf(g_post, sizeof g_post, "%s", body ? body : "");
    g_have_post = 1;
    navigate(url);
}

/* about: -- a page the BROWSER writes and the browser then parses with its own
 * engine. There is no history widget anywhere in this app because a list of
 * links is a page, and this program already knows how to draw one. It also
 * means the history is styled by the same cascade, selectable by the same
 * selection, and searchable by the same find. */
static int about_page(const char *url) {
    if (!url || strncmp(url, "about:", 6)) return 0;
    static char page[48 * 1024];
    size_t n = 0;
    if (!strcmp(url + 6, "history")) n = hist_as_html(page, sizeof page);
    else n = (size_t)snprintf(page, sizeof page,
             "<html><head><title>%s</title></head><body><h1>Not a page</h1>"
             "<p>This browser has no <code>%s</code>. It has "
             "<a href=\"about:history\">about:history</a>.</p></body></html>",
             url, url);
    struct vnet_result r;
    memset(&r, 0, sizeof r);
    r.status = 200; r.len = n;
    snprintf(r.via, sizeof r.via, "generated");
    snprintf(r.final_url, sizeof r.final_url, "%s", url);
    if (n < sizeof g_incoming) memcpy(g_incoming, page, n + 1);
    finish_load(&r);
    return 1;
}

static void navigate(const char *url) {
    if (!url || !url[0]) return;
    if (g_url[0]) tab_hist_push(tab_current(), g_url);
    load(url);
}

/* What the ADDRESS BAR does with what you typed, as opposed to what a link
 * hands over already resolved.
 *
 * People type "example.com". This browser passed that straight to the fetcher,
 * which has no scheme to open it with, so nothing happened at all -- no page,
 * no error, just an address bar that had apparently ignored you. A leading '/'
 * still means a path on this machine, which is the other thing that gets typed
 * here and must keep working. */
/* Where a search goes. One line to change it, and the query is the only thing
 * appended -- no client id, no session, nothing this browser has no business
 * sending.
 *
 * Which engine was MEASURED, not preferred. The question is not which index is
 * best, it is which one puts its RESULTS IN THE HTML, because this browser
 * cannot run the script that fetches them otherwise. Asking each for
 * "operating system" with an honest User-Agent:
 *
 *   engine      http   bytes  scripts  result links  what came back
 *   Google       200   91585        5             0  a JS shell -- the words
 *                                                    "operating system" do not
 *                                                    appear anywhere in it
 *   Bing         200  121877       20             0  the same
 *   Startpage    200   10249        5             0  the same
 *   DuckDuckGo   200   28845        0             1  a CAPTCHA
 *   Marginalia   200   37395        4             3  thin
 *   Mojeek       200   20401        6             9  real, small index
 *   Brave        200  303202        2            28  REAL RESULTS
 *
 * So Brave: an index of actual scale, server-rendered. Its pages are big --
 * 367KB parses to 2044 nodes in 6ms here, which is fine -- and unlike Google it
 * is not withholding the answer behind a bundle we cannot execute. Google is
 * not a search problem; it is the JavaScript problem, and the day this browser
 * runs their bundle is the day that line can change. */
#define SEARCH_URL "https://search.brave.com/search?q="

/* Percent-encode a query. Unreserved characters (RFC 3986) pass through and a
 * space becomes '+', which is what a search form sends. */
static void query_escape(const char *in, char *out, size_t cap) {
    static const char HEX[] = "0123456789ABCDEF";
    size_t o = 0;
    for (; *in && o + 4 < cap; in++) {
        unsigned char c = (unsigned char)*in;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~')
            out[o++] = (char)c;
        else if (c == ' ')
            out[o++] = '+';
        else {
            out[o++] = '%'; out[o++] = HEX[c >> 4]; out[o++] = HEX[c & 15];
        }
    }
    out[o] = 0;
}

static void navigate_typed(const char *what) {
    while (*what == ' ') what++;
    if (!*what) return;
    /* A scheme is letters followed by "://" -- or one of the schemeless forms
     * the browser generates itself. Requiring the slashes is what keeps
     * "localhost:8080/x" a host and a port rather than a scheme named
     * "localhost". */
    int alpha = 0;
    while (((what[alpha] | 32) >= 'a' && (what[alpha] | 32) <= 'z') ||
           (alpha && what[alpha] >= '0' && what[alpha] <= '9')) alpha++;
    int has_scheme = alpha > 0 && !strncmp(what + alpha, "://", 3);
    if (!has_scheme && !strncmp(what, "about:", 6)) has_scheme = 1;
    if (has_scheme || what[0] == '/') { navigate(what); return; }

    /* URL OR SEARCH. The address bar is one field doing two jobs, and which
     * job it is doing is decided here rather than by making the reader say.
     *
     * A host has a dot in it and no spaces. That is the whole test, and it is
     * the test every browser uses, because "what people type that is not a URL"
     * has no shape worth pattern-matching -- it is a sentence. Anything failing
     * it is a query, which is why "how do i mount a disk" searches and
     * "kernel.org" does not.
     *
     * localhost is the exception a dot-based rule always has: it is a real host
     * with no dot, and it is the one people type most on a machine like this. */
    int host_like = 0;
    if (!strncmp(what, "localhost", 9) &&
        (what[9] == 0 || what[9] == '/' || what[9] == ':')) {
        host_like = 1;
    } else {
        for (const char *p = what; *p && *p != '/'; p++) {
            if (*p == ' ') { host_like = 0; break; }
            if (*p == '.' && p[1] && p[1] != '/') host_like = 1;
        }
    }

    char u[1024];
    if (host_like) {
        snprintf(u, sizeof u, "https://%s", what);
    } else {
        char q[768];
        query_escape(what, q, sizeof q);
        snprintf(u, sizeof u, "%s%s", SEARCH_URL, q);
    }
    navigate(u);
}

static void go_back(void) {
    char to[512];
    if (tab_hist_back(tab_current(), g_url, to, sizeof to) == 0) load(to);
}
static void go_fwd(void) {
    char to[512];
    if (tab_hist_fwd(tab_current(), g_url, to, sizeof to) == 0) load(to);
}

/* --- tabs ---------------------------------------------------------------- *
 *
 * Switching is: put down what this tab was holding, pick up what that one was,
 * and rebuild its document from the bytes it kept. Everything a tab remembers
 * is written back BEFORE the switch, because after it the globals belong to
 * somebody else.
 */
static void tab_stash(void) {
    int c = tab_current();
    tab_set_url(c, g_url);
    tab_set_scroll(c, g_scroll);
    tab_set_zoom(c, vellum_zoom());
    /* The page's own title, so the tab is named by what it IS. */
    for (int i = 0; i < g_doc.n; i++)
        if (g_doc.nodes[i].kind == HTML_ELEM && !strcmp(g_doc.nodes[i].tag, "title")) {
            int k = g_doc.nodes[i].first_child;
            if (k >= 0 && g_doc.nodes[k].text) tab_set_title(c, g_doc.nodes[k].text);
            break;
        }
}

/* Pick up tab `i`'s state as the live one. Assumes it is ALREADY current --
 * separate from show_tab so that closing the tab you are looking at can land
 * on its neighbour without pretending to switch away from a tab that is gone.
 */
/* Where this tab was, and how many frames to keep insisting on it.
 *
 * ScrollView clamps the offset to the content height it measured LAST frame --
 * a one-frame lag that is invisible while a page stays put and exactly wrong
 * when the document is swapped underneath it. Returning from a short page to a
 * long one, the saved offset was clamped against the SHORT page's extents and
 * came back as zero, so every tab you went back to had forgotten where you
 * were. Re-asserting it for a few frames outlives the lag. */
static float g_want_scroll; static int g_want_frames;

static void adopt_tab(int i) {
    snprintf(g_url, sizeof g_url, "%s", tab_url(i));
    snprintf(g_bar, sizeof g_bar, "%s", tab_url(i));
    vellum_set_zoom(tab_zoom(i));
    find_close();              /* a find belongs to the page it was run against */

    size_t n = tab_src_len(i);
    if (n) {
        install_document(n);
        /* AFTER the parse: install_document resets the view, and the scroll
         * offset is the one thing about this tab that must survive it. */
        g_scroll = g_want_scroll = tab_scroll(i);
        g_want_frames = 3;
        /* The status line describes THIS page. Leaving the previous tab's
         * numbers there is a line that lies about the document on screen --
         * and "new tab, 167 bytes" under a 2384-byte page is exactly the kind
         * of small lie that costs an hour later. What is honestly known here
         * is the size and where the bytes came from. */
        memset(&g_st, 0, sizeof g_st);
        g_st.status = 200; g_st.bytes = n;
        snprintf(g_st.via, sizeof g_st.via, "this tab");
    } else if (g_url[0]) {
        load(g_url);           /* a tab opened but never visited */
    } else {
        /* An empty new tab still has to be a document, because everything
         * downstream of here renders one. */
        snprintf(g_src, G_SRC_MAX,
                 "<html><head><title>New tab</title></head><body>"
                 "<h1>New tab</h1><p>Type an address above, or open "
                 "<a href=\"/system/web/index.html\">the start page</a>.</p>"
                 "</body></html>");
        size_t sn = strlen(g_src);
        tab_set_src_len(i, sn);
        install_document(sn);
        g_scroll = 0;
        /* ...and it is not the previous page's response, so the status line
         * must stop reporting that page's size and origin. */
        memset(&g_st, 0, sizeof g_st);
        g_st.status = 200; g_st.bytes = sn;
        snprintf(g_st.via, sizeof g_st.via, "new tab");
    }
    update_status();
    em_request_frame();
}

static void show_tab(int i) {
    if (i == tab_current() || i < 0 || i >= TAB_MAX) return;
    /* A fetch in flight writes into g_incoming and finishes into whatever tab
     * is current when it lands -- which would be the WRONG tab. Refusing is
     * honest and momentary; the alternative is a page appearing in a tab you
     * were not looking at. */
    if (fetchjob_busy()) {
        snprintf(g_status, sizeof g_status, "Still loading -- one page at a time");
        return;
    }
    tab_stash();
    if (tab_select(i) != 0) return;
    adopt_tab(i);
}

static void new_tab(const char *url) {
    int i = tab_open(url);
    if (i < 0) { snprintf(g_status, sizeof g_status, "No room for another tab"); return; }
    show_tab(i);
}

static void close_tab(int i) {
    if (tab_count() <= 1) return;      /* the last tab stays; see tabs.c */
    int was = tab_current();
    int landed = tab_close(i);
    /* Closing a BACKGROUND tab changes nothing on screen, and must not
     * re-parse -- otherwise closing tab 3 would visibly reload tab 1. */
    if (i != was) return;
    adopt_tab(landed);
}

/* Keyboard paging: Space a page down, 'b' a page up -- every browser's oldest
 * shortcut, and it scrolls without touching the wheel at all. (It also makes
 * scrolling drivable from the test harness, where wheel events cannot be
 * synthesized -- a feature and an instrument in one.) Consumed only while no
 * text field has focus, or typing a space in the URL bar would jump the page. */
/* Selection: a drag across the document, and a way to take it away. The drag
 * is tracked here rather than in select.c because the press/release EDGE is an
 * app-loop fact -- select.c is told what happened, not asked to guess. */
static bool g_ptr_was_down;

/* The two file operations store.c needs, and the only two this browser
 * performs. They are injected rather than called from store.c so that module
 * takes no syscall dependency -- the same shape as the cookie jar's clock. */
static long web_state_read(const char *path, char *buf, size_t cap) {
    int fd = (int)embk_open(path, EMBK_O_RDONLY, 0);
    if (fd < 0) return -1;
    size_t n = 0;
    for (;;) {
        int64_t got = embk_read(fd, buf + n, cap - n);
        if (got <= 0) break;
        n += (size_t)got;
        if (n >= cap) break;
    }
    embk_close(fd);
    return (long)n;
}

static long web_state_write(const char *path, const char *buf, size_t len) {
    /* Make the directory if it is not there. It cannot be shipped on the image
     * -- mkfs stages FILES, and an empty directory has none -- and the browser
     * is the only thing that will ever put anything in it. embk_mkdir on an
     * existing directory is harmless, so this needs no probe first.
     *
     * Both levels, because /data/apps/vellum exists (the app lives there) but
     * its `state` child does not. */
    /* Make the directory if it is not there. It cannot be shipped on the image
     * -- mkfs stages FILES, and an empty directory has none -- and the browser
     * is the only thing that will ever put anything in it. embk_mkdir on an
     * existing directory is harmless, so this needs no probe first. */
    char dir[192];
    snprintf(dir, sizeof dir, "%s", path);
    for (int i = (int)strlen(dir) - 1; i > 0; i--)
        if (dir[i] == '/') { dir[i] = 0; break; }
    embk_mkdir(dir);
    int fd = (int)embk_open(path, EMBK_O_WRONLY | EMBK_O_CREAT | EMBK_O_TRUNC, 0);
    if (fd < 0) return -1;
    long w = (long)embk_write(fd, buf, len);
    embk_close(fd);
    return w;
}

static const struct store_io g_state_io = { web_state_read, web_state_write };

static void selection_tick(void) {
    float px, py;
    ui_pointer_pos(&px, &py);
    bool down = ui_pointer_down();
    int changed = 0;
    if (down && !g_ptr_was_down)      changed = vsel_pointer(px, py, 1, 1);
    else if (down)                    changed = vsel_pointer(px, py, 0, 1);
    else if (g_ptr_was_down)          changed = vsel_pointer(px, py, 0, 0);
    g_ptr_was_down = down;
    if (changed) em_request_frame();
}

static char g_find_buf[96];

static int vellum_key(int ch) {
    /* ZOOM: + and - and 0, as BARE keys while nothing has focus -- the same
     * rule Space and b already follow for paging.
     *
     * Not Ctrl+= as every desktop browser uses, because the keyboard driver
     * turns Ctrl+letter into a control byte and passes Ctrl+SYMBOL through
     * unchanged (keyboard.c is explicit about that, so Ctrl+digit does not
     * become a stray control code). Ctrl+= and plain = are therefore the same
     * byte here, and binding "Ctrl+=" would be a lie about what was pressed.
     * Teaching the driver to report modifiers with symbols is the real fix and
     * belongs in the driver; see docs/TODO.md.
     *
     * The page scales; the address bar does not -- see vellum_set_zoom. */
    if (!ui_any_focus()) {
        if (ch == '=' || ch == '+') { vellum_set_zoom(vellum_zoom() * 1.2f); em_request_frame(); return 1; }
        if (ch == '-' || ch == '_') { vellum_set_zoom(vellum_zoom() / 1.2f); em_request_frame(); return 1; }
        if (ch == '0')              { vellum_set_zoom(1.0f);                 em_request_frame(); return 1; }
    }
    /* Tabs. Ctrl+T / Ctrl+W are letters, so the driver DOES give them as
     * control bytes -- unlike the zoom keys above, which is why these two get
     * the shortcut everyone already knows and those do not. */
    if (ch == 0x14) { new_tab("");   return 1; }   /* Ctrl+T */
    if (ch == 0x17) { close_tab(tab_current()); return 1; }   /* Ctrl+W */
    if (ch == '\t' && !ui_any_focus()) {          /* cycle, wrapping */
        int n = tab_current();
        for (int k = 1; k <= TAB_MAX; k++) {
            int c = (n + k) % TAB_MAX;
            if (c != n && tab_is_open(c)) { show_tab(c); break; }
        }
        return 1;
    }
    if (ch == 0x06) {                     /* Ctrl+F */
        find_open();
        em_request_frame();
        return 1;
    }
    if (find_is_open()) {
        if (ch == 27) { find_close(); em_request_frame(); return 1; }
        if (ch == '\n' || ch == '\r') {
            /* Enter is NEXT, which is what every browser does and what makes
             * the bar usable without reaching for the mouse. */
            find_step(1);
            float y;
            if (find_current_y(&y)) g_scroll += y - 200.0f;
            if (g_scroll < 0) g_scroll = 0;
            em_request_frame();
            return 1;
        }
    }
    if (ch == 0x03) {                     /* Ctrl+C */
        static char sel[16384];
        size_t n = vsel_copy_text(sel, sizeof sel);
        if (n) embk_clip_set(sel, n);
        if (n) snprintf(g_status, sizeof g_status, "Copied %u bytes", (unsigned)n);
        else   snprintf(g_status, sizeof g_status, "Nothing selected");
        snprintf(g_status_done, sizeof g_status_done, "%s", g_status);
        return 1;
    }
    if (ch == 0x01) { return vsel_all(); }   /* Ctrl+A: the whole document */
    if (ch == 27 && vsel_clear()) return 1;  /* Esc drops a selection first */
    /* Enter in a form field submits it -- a search box you cannot submit from
     * the keyboard feels broken, and every browser has behaved this way since
     * forms existed. */
    if (ch == '\n' || ch == '\r') {
        int f = vellum_focused_field();
        if (f >= 0) { on_submit(f); return 1; }
    }
    if (ui_any_focus()) return 0;
    float page = (em_viewport_height() - 110.0f) * 0.85f;
    if (ch == ' ')      { g_scroll += page; }
    else if (ch == 'b') { g_scroll -= page; }
    else return 0;
    if (g_scroll < 0) g_scroll = 0;
    return 1;
}

/* --- the window --------------------------------------------------------- */

/* Chrome palette. The app runs Dark (see EM_APPLICATION), so these are stated
 * rather than derived: the tab you are on is the same surface as the toolbar
 * beneath it, which is what makes it read as continuous with the page instead
 * of as a button floating above one. */
/* Unlike the renderer's argb(), a zero alpha stays ZERO here. That is the
 * whole representation of "no background": a button whose fill has no alpha
 * takes the ghost path and paints nothing, which is what an unselected tab is. */
static Color chrome_rgb(unsigned v) {
    Color c;
    c.r = (float)((v >> 16) & 0xFF) / 255.0f;
    c.g = (float)((v >>  8) & 0xFF) / 255.0f;
    c.b = (float)( v        & 0xFF) / 255.0f;
    c.a = (float)((v >> 24) & 0xFF) / 255.0f;
    return c;
}

#define TAB_ON        chrome_rgb(0xFF3A3A3CU)
#define TAB_OFF       chrome_rgb(0x00000000U)
#define TAB_TEXT_ON   chrome_rgb(0xFFF2F2F7U)
#define TAB_TEXT_OFF  chrome_rgb(0xFF98989EU)

/* The security dot beside the address. Green ONLY for a chain that verified --
 * anything weaker would be a lie told in the one place a reader trusts. */
static Color scheme_color(void) {
    if (strstr(g_st.via, "authenticated")) return chrome_rgb(0xFF30D158U);  /* verified TLS */
    if (!strncmp(g_url, "http://", 7))     return chrome_rgb(0xFFFF9F0AU);  /* in the clear */
    return chrome_rgb(0xFF636366U);                                         /* local file   */
}

static void app(void) {
    static bool first = true;
    if (first) {
        first = false;
        em_set_key_hook(vellum_key);
        tab_init();                  /* one tab, before anything can load into it */
        /* The jar's clock. cookie.c takes no syscall dependency of its own, so
         * the app is what tells it what time it is. */
        cookie_set_clock(embk_now_unix);
        /* State that outlives the process: the jar, so a login survives a
         * restart, and localStorage, which is the only place a page can keep
         * anything of its own. Both live under /data/apps/vellum/state, which
         * is the ONLY path this browser may write -- see vellum.ns. */
        store_set_io(&g_state_io);
        /* $HOME/.vellum -- the same path vellum.ns declares, and the launcher
         * expanded the same $HOME to grant it. If there is no HOME there is no
         * persistence, which is correct rather than a fallback to somewhere
         * this process was never given. */
        { const char *h = getenv("HOME");
          if (h && h[0]) { char d[192]; snprintf(d, sizeof d, "%s/.vellum", h);
                           store_set_dir(d); } }
        store_load();
        cookie_load();
        hist_load();
        em_set_post_layout_hook(vsel_sync_geometry);
        vsel_set_mark_hook(find_mark);
        vellum_set_link_handler(on_link);
        vellum_set_event_hooks(jsdom_has_listener, on_dom_click);
        vellum_set_submit_handler(on_submit);
        const char *start = getenv("VELLUM_URL");
        navigate(start && start[0] ? start : "/system/web/index.html");
    }
    /* Has the worker landed? Polled once per frame, which is the whole cost of
     * not freezing. */
    struct vnet_result res;
    if (fetchjob_poll(DOC_TAG, &res) == 1) finish_load(&res);

    /* Stylesheets BEFORE pictures. Both share the one worker, and a page that
     * paints its images before it knows what colour anything is shows the
     * reader a wrong-looking page and then rearranges it. Style first is also
     * the order a browser's own preload scanner uses, for the same reason. */
    if (cssref_pump()) { rebuild_sheet(); update_status(); em_request_frame(); }
    /* A RESIZE changes what the media queries answer, and the sheet was parsed
     * against the old width. Re-parse when the viewport actually moves --
     * otherwise a window dragged past a breakpoint keeps the other layout. */
    { static float last_w; float w = sheet_viewport_w();
      if (w != last_w) { last_w = w; rebuild_sheet(); em_request_frame(); } }

    /* ...and the page's pictures, one at a time on the same worker. Each one
     * that lands changes the page, so ask for a frame. */
    if (!cssref_pending() && imgcache_pump()) em_request_frame();
    /* The tab's icon: last in the queue behind the document, its sheets and its
     * pictures, because it is decoration and they are the page. */
    if (!cssref_pending() && !imgcache_pending() && favicon_pump()) em_request_frame();
    /* One pump for everything the engine owes the page: due timers, a landed
     * fetch, and the microtask queue promises resolve onto. Before the dirty
     * check, because a handler is the most likely thing to have changed the
     * document. */
    /* A field the person edited fires 'input' and 'change'. The toolkit writes
     * into the value buffer in place, so this is a POLL of what changed since
     * the last frame rather than a callback -- and a loop, because more than
     * one field can change between two frames. */
    for (int changed; (changed = form_take_changed()) >= 0; )
        if (jsdom_dispatch_input(changed)) em_request_frame();

    if (jsdom_pump(embk_uptime_ms())) em_request_frame();
    /* A script changed the DOM: classes, attributes, whole subtrees. Every
     * computed style is suspect. */
    if (jsdom_take_dirty()) { vstyle_cache_invalidate(); em_request_frame(); }
    /* Keep frames coming while the page has WORK OUTSTANDING -- a timer to
     * fire or a fetch to land -- and stop the moment it does not, so an idle
     * page costs nothing. */
    if (jsdom_next_timer() || jsdom_busy()) em_app_set_refresh(60);
    else if (!fetchjob_busy() && !imgcache_pending() && !cssref_pending())
        em_app_set_refresh(-1);
    if (imgcache_pending() || cssref_pending() || favicon_pending()) em_app_set_refresh(200);

    /* While a fetch is in flight the view has to keep being built, or the
     * runtime -- which draws on input by design -- would never poll again and
     * the page would land invisibly. A periodic tick, not a per-frame request:
     * five a second is plenty to notice a fetch landing, and it leaves the CPU
     * to the thing the user is actually waiting for. */
    static bool ticking = false;
    bool busy_now = fetchjob_busy() != 0;
    if (busy_now != ticking) { ticking = busy_now; em_app_set_refresh(busy_now ? 200 : -1); }

    /* Deliberately NOT forcing a full repaint here. The app renders straight
     * into the shared window buffer, so a full repaint begins by CLEARING the
     * pixels the compositor is showing -- and while a crypto-heavy worker has
     * the core, the redraw that follows takes long enough that an empty window
     * is what the user actually sees. Leaving the old pixels alone means the
     * page stays readable and only what changed is overwritten. */

    /* The progress report goes in the status line that is ALREADY THERE, and
     * that is a deliberate design choice rather than a compromise. A row that
     * appears and disappears moves every row below it, and the runtime's
     * incremental repaint paints the new layout over the old pixels -- a
     * dedicated "loading" strip made the window look corrupted for the whole
     * fetch. Changing a STRING changes no geometry, so there is nothing to get
     * out of step. The ticking tenths are a better liveness signal than a
     * spinner anyway: they prove the UI thread is running, which is the exact
     * thing the user could not tell before. */
    if (fetchjob_busy()) {
        /* Elapsed FIRST. The URL is already in the address bar two rows up, and
         * putting it here as well pushed the one piece of information that is
         * actually changing off the end of the line. */
        unsigned ms = fetchjob_elapsed_ms();
        snprintf(g_status, sizeof g_status, "Loading...  %u.%us", ms / 1000, (ms % 1000) / 100);
        g_status_busy = 1;
    } else if (g_status_busy) {
        /* ...and PUT IT BACK when the loading stops. Leaving "Loading..." on
         * screen after everything has arrived is a status line that lies --
         * and it lied for a whole minute while an image that could never load
         * was quietly failing, which is exactly when a person is reading it. */
        g_status_busy = 0;
        snprintf(g_status, sizeof g_status, "%s", g_status_done);
    }

    selection_tick();

    if (g_want_frames > 0) { g_want_frames--; g_scroll = g_want_scroll; em_request_frame(); }

    /* Tab actions from LAST frame's clicks, before this one is built. Same
     * reason as g_goto below, only sharper: switching tabs replaces the whole
     * document, and doing that while the view is walking it frees the nodes
     * out from under the walk. */
    if (g_switch_to >= 0) { int t = g_switch_to; g_switch_to = -1; show_tab(t); }
    if (g_close_tab >= 0) { int t = g_close_tab; g_close_tab = -1; close_tab(t); }
    if (g_new_tab)        { g_new_tab = 0; new_tab(""); }
    /* The strip APPEARS at two tabs and disappears at one, and either way every
     * row below it moves. The runtime's incremental repaint would paint the new
     * layout over the old pixels -- which is what a second tab looked like the
     * first time: the address bar drawn on top of the title bar. A structural
     * frame clears first. The find bar has the same shape and gets the same
     * treatment, for the same reason. */
    { static int last_rows = -1;
      int rows = find_is_open() ? 1 : 0;
      if (rows != last_rows) { last_rows = rows; em_structure_changed(); } }

    /* act on last frame's click before building this one */
    if (g_goto[0]) {
        /* Resolve against where we ARE. A relative href in a page fetched over
         * the network means a network location, and the same href in a local
         * document means the file beside it -- one rule, two worlds. */
        char u[512];
        if (url_resolve(g_url, g_goto, u, sizeof u) != 0)
            snprintf(u, sizeof u, "%s", g_goto);
        g_goto[0] = 0;
        navigate(u);
    }

    Window("Vellum") {
        /* ROW 1 -- THE TAB STRIP, and the window's own title bar at the same
         * time. The lights sit here rather than on a bar of their own, and
         * that is what removes a whole row: the old chrome spent one row on
         * the word "Vellum", which never changes and therefore tells you
         * nothing, and another on the address. The top row of a browser should
         * say which page you are looking at, and a tab already does.
         *
         * ALWAYS shown, even at one tab. That single tab is where the page
         * TITLE lives -- before this the title was parsed, stored, and never
         * put on screen anywhere. */
        HStack(.spacing = 3, .align = Center, .px = 8, .py = 4, .key = "tabstrip") {
            CloseButton(); MinimizeButton();
            /* .id() is a reconciliation KEY and takes a string. Stable per
             * SLOT rather than per label, so a tab whose title arrives later
             * is still the same widget and does not inherit the geometry of
             * whatever last held that position. */
            static const char *KEY[TAB_MAX]  = { "t0","t1","t2","t3","t4","t5" };
            static const char *KEYL[TAB_MAX] = { "l0","l1","l2","l3","l4","l5" };
            static const char *KEYX[TAB_MAX] = { "x0","x1","x2","x3","x4","x5" };
            for (int i = 0; i < TAB_MAX; i++) {
                if (!tab_is_open(i)) continue;
                int cur = (i == tab_current());
                char lbl[26];
                snprintf(lbl, sizeof lbl, "%.22s", tab_label(i));
                /* The current tab is RAISED -- a lighter surface, the colour
                 * the toolbar under it already is. It used to be the accent
                 * blue, which made the tab you are already on the loudest
                 * thing in the window. Selection is not an alert.
                 *
                 * The unselected ones must say .ghost() rather than just hand
                 * over a transparent background: a button's DEFAULT style is
                 * the filled accent, so "no background" fell through to it and
                 * every tab you were NOT on came out bright blue. */
                /* The site's icon. Asked for every frame because an origin
                 * already known is a string compare; only the CURRENT tab can
                 * offer the page's declared <link rel=icon>, which is fine --
                 * a background tab's origin was looked up while it was in
                 * front, and the cache is keyed by origin precisely so that
                 * survives. */
                struct favicon *fv = favicon_want(tab_url(i),
                                                  cur ? g_doc.iconref : 0);
                /* The tab is the CONTAINER, and the icon, the label and the ✕
                 * are three things inside it. Made of separate chips instead,
                 * a tab and its close button read as two things that happen to
                 * be adjacent -- which is what they were. */
                HStack(.spacing = 0, .align = Center, .px = 3, .corner = 8,
                       .background = cur ? TAB_ON : TAB_OFF, .key = KEY[i]) {
                    if (fv && fv->state == FAV_READY && fv->px) {
                        /* Keyed by the pixel pointer, which is stable per slot
                         * -- so the icon appearing does not shift the label and
                         * the ✕ onto each other's retained instances. */
                        em_flush();
                        ui_image_sized((uint64_t)(uintptr_t)fv->px, fv->px,
                                       fv->w, fv->h, 14, 14);
                    }
                    EmV tb = Button(lbl).ghost().font(Caption).py(3).px(7).id(KEYL[i]);
                    tb.color(cur ? TAB_TEXT_ON : TAB_TEXT_OFF);
                    if (tb.clicked() && !cur) g_switch_to = i;
                    /* The ✕ only on the tab you are on, and only when closing
                     * one is possible at all. A row of them is a row of things
                     * you did not mean to click. */
                    if (cur && tab_count() > 1 &&
                        Button("\xc3\x97").ghost().font(Caption).py(3).px(5)
                            .color(TAB_TEXT_OFF).id(KEYX[i]).clicked())
                        g_close_tab = i;
                }
            }
            if (Button("+").ghost().font(Caption).py(3).px(7).id("tnew").clicked())
                g_new_tab = 1;
            /* The rest of the strip is the window's drag zone -- the same
             * place every desktop browser lets you pick the window up. It also
             * replaces the Spacer that used to be here: a Spacer has no key,
             * and sitting after a VARIABLE number of tabs meant opening a
             * third tab shifted it a position and the grow landed on a button,
             * which read as a ragged gap in the middle of the strip. */
            DragHandle(.key = "tabdrag") { }
        }
        Divider("tabsep");

        /* ROW 2 -- navigation and the address, on ONE row.
         *
         * Back, forward and reload belong beside the address field because
         * they change what it says. They used to be at the far right of a
         * different row -- the width of the window away from the thing they
         * act on, and past the title, so going back meant crossing the whole
         * chrome to reach a control every browser puts at the left. */
        HStack(.spacing = 5, .align = Center, .px = 8, .py = 5, .key = "urlrow") {
            if (IconButton(IconChevronL).clicked()) go_back();
            if (IconButton(IconChevronR).clicked()) go_fwd();
            /* A circling arrow, not the plain "→" that used to be here: an
             * arrow pointing right is where a browser puts FORWARD, and this
             * button sat immediately beside the forward button meaning
             * something else entirely. */
            if (IconButton(IconReload).clicked())   load(g_url);

            /* How the bytes got here, as a dot: green only when a certificate
             * chain actually verified, amber for plain HTTP, quiet for a local
             * file. The browser has known this since TLS landed and had
             * nowhere to say it except a line of telemetry at the bottom. */
            Icon(IconDot).color(scheme_color()).font(Caption);

            /* Enter navigates. There used to be a blue "Open" button here --
             * the single loudest element in the chrome, for an action nobody
             * clicks, because a text field that could not submit left no other
             * way out. See EmV.submitted. */
            /* Make the HOST the legible part. "https://" and everything after
             * the host recede, because the host is what says who you are
             * actually talking to -- and it is what a hostile URL buries under
             * a long, plausible-looking path. The field draws it plain again
             * the moment you start editing: what you are editing is the whole
             * string, and dimming two thirds of it would misrepresent that. */
            { unsigned hs = 0, hn = 0;
              const char *p = strstr(g_bar, "://");
              if (p) {
                  hs = (unsigned)(p - g_bar) + 3;
                  const char *e = g_bar + hs;
                  while (*e && *e != '/' && *e != '?' && *e != '#') e++;
                  hn = (unsigned)(e - (g_bar + hs));
              }
              em_field_emphasis(hs, hn); }
            if (TextField(g_bar, sizeof g_bar, "Search or enter address").submitted())
                navigate_typed(g_bar);

            /* NOT a clock, however much history wants one: the clock glyph and
             * the reload glyph are both circling arrows, and side by side in
             * one toolbar they were the same button drawn twice. */
            if (IconButton(IconList).clicked()) navigate("about:history");
        }
        /* The find bar, only when it is open -- a browser that shows one
         * always has given up a line of the page for something you use once. */
        if (find_is_open()) {
            HStack(.spacing = 8, .align = Center, .px = 12, .py = 4, .key = "findrow") {
                Text("Find").caption().tertiary();
                if (TextField(g_find_buf, sizeof g_find_buf, "text on this page").focused()) { }
                char n[48];
                if (find_needle()[0])
                    snprintf(n, sizeof n, "%d of %d", find_current(), find_count());
                else n[0] = 0;
                Text(n).caption().tertiary();
                if (Button("Prev").ghost().font(Caption).py(2).clicked()) find_step(-1);
                if (Button("Next").ghost().font(Caption).py(2).clicked()) find_step(1);
                if (Button("Done").ghost().font(Caption).py(2).clicked()) find_close();
            }
            Divider("findsep");
            /* The field is the truth; the module is told what it says. Polling
             * beats a change callback here because the toolkit edits the buffer
             * in place -- the same reason 'input' events are a poll. */
            /* ...and ask for another frame. The count is computed by the
             * post-layout hook, AFTER this view has already drawn it, so the
             * number on screen is always one frame behind the query. With no
             * further frame it freezes showing the count for a shorter prefix
             * -- typing "browser" left "1 of 24", which is the answer for "b".
             * One more frame is the whole fix. */
            if (strcmp(find_needle(), g_find_buf)) {
                find_set_needle(g_find_buf);
                em_request_frame();
            }
        }

        Divider("chromesep");

        /* What the chrome costs, counted rather than guessed: the base rows,
         * plus each optional one. Guessing it (a single 132/168 constant) is
         * what made the tab strip push the document off the bottom the moment
         * a second tab existed. */
        float chrome = 110.0f;
        if (find_is_open())  chrome += 36.0f;
        /* The page area is PAPER-coloured, not window-coloured. With the dark
         * app surface showing through, the 22px margin around the document
         * read as a dark frame drawn around a white card -- a browser looking
         * at a page rather than a browser showing one. Same padding, no
         * frame: the margin is now part of the page. */
        ScrollView(&g_scroll, em_viewport_height() - chrome, .key = "page",
                   .background = chrome_rgb(PAGE_CANVAS)) {
            /* Fill, not Leading: this is the block every other block inherits
             * its width from. Left it Leading and the whole document sizes to
             * its longest line instead of to the window, so nothing wraps. */
            char dockey[24];
            snprintf(dockey, sizeof dockey, "doc%u", g_doc_gen);
            VStack(.spacing = 0, .align = Fill, .padding = 22, .grow = 1, .key = dockey) {
                const char *clicked = vellum_render_sized(&g_doc, g_root, &g_sheet, g_url,
                                                         em_viewport_width() - 44.0f);
                /* A DRAG that happens to end on a link is a selection, not a
                 * click. Without this, selecting a paragraph that contains a
                 * link navigates away the moment you let go -- and the text you
                 * just selected is gone with the page. */
                if (clicked && !vsel_active())
                    snprintf(g_goto, sizeof g_goto, "%s", clicked);
            }
        }

        Divider("statussep");
        HStack(.spacing = 10, .align = Center, .px = 12, .py = 4, .key = "statusrow") {
            /* what the PAGE said outranks what the browser has to say: a
             * script's output is the thing the reader is waiting for. */
            /* The page's status on the left, a script's complaint on the
             * RIGHT -- not one replacing the other. A console message used to
             * win outright, so the moment any page ran a script that threw,
             * the line that says how many bytes and nodes arrived was gone for
             * the rest of that page's life. That is exactly when it is worth
             * reading: a blank page with "ReferenceError" under it tells you a
             * script failed and nothing about whether the document did. */
            const char *h = vellum_hovered_link();
            Text(h ? h : g_status).caption().tertiary();
            /* The URL used to be echoed on the right, which put the address on
             * screen twice -- and the copy down here was the one nobody could
             * edit. The row keeps its height so that hovering a link does not
             * reflow the page underneath it. */
            Spacer();
            if (!h && g_console[0]) {
                char c[72];
                snprintf(c, sizeof c, "%.68s", g_console);
                Text(c).caption().tertiary();
            }
        }
    }
}

EM_APPLICATION {
    .title  = "Vellum",
    .size   = { 940, 620 },
    .theme  = Dark,
    .chrome = Chromeless,
    .resize = Resizable,
    .view   = app,
};

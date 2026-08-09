/* user/web/html.h -- an HTML parser and document model.
 *
 * Scope, stated honestly because a browser is a thing people have opinions
 * about: this parses the STRUCTURE of a document -- elements, attributes,
 * text, entities -- into a tree. It does not run scripts, does not implement
 * CSS, and does not attempt the HTML5 spec's full error-recovery algorithm.
 * What it does implement is the part that makes real documents readable:
 * implicit end tags, void elements, attribute quoting, character references,
 * and skipping the contents of <script> and <style> wholesale.
 *
 * That subset is not a toy. It is what a documentation site, a README, a man
 * page, an RSS-era blog and everything our own httpd serves actually are.
 *
 * The tree is allocated from a caller-supplied arena. No malloc: a parser
 * that cannot run out of memory in a bounded way is a parser that can be run
 * on untrusted input from the network, which is exactly what this is for.
 */
#ifndef _EMBLINK_WEB_HTML_H_
#define _EMBLINK_WEB_HTML_H_

#include <stddef.h>

#define HTML_TAG_MAX   16
#define HTML_HREF_MAX 512

enum html_kind { HTML_ELEM = 0, HTML_TEXT };

struct html_node {
    unsigned char kind;
    char  tag[HTML_TAG_MAX];        /* lowercased; HTML_ELEM only          */
    char *text;                     /* into the arena; HTML_TEXT only      */
    char *href;                     /* <a href> / <img src>, or NULL       */
    /* The three attributes CSS needs, and the ONLY parser change CSS
     * required (docs/BROWSER.md §3 predicted exactly this). Still not a
     * general attribute map: everything else remains arena spent on data
     * nobody downstream can act on. */
    char *klass;                    /* class="..." (space-separated), NULL */
    char *id;                       /* id="...", or NULL                   */
    char *style;                    /* style="..." declarations, or NULL   */
    char *alt;                      /* <img alt="...">, or NULL            */
    /* An inline <svg>'s OWN SOURCE, opening tag included. SVG is not HTML: its
     * elements mean nothing to this parser, and walking them produced a tree of
     * unknown inline boxes that drew as nothing -- so an icon-only button was
     * an empty rectangle. Kept whole and handed to svg.c, exactly as <style>
     * keeps its text for the cascade. NULL for everything else. */
    char *svg;
    /* Form controls. These are their OWN fields and do NOT share a slot with
     * class/id: those are what CSS selectors match on, so borrowing them would
     * make a styled input unstylable -- a bug that would look like the cascade
     * being broken. `type` doubles as a <form>'s METHOD, which is safe because
     * a form has no type attribute and an input has no method: genuinely
     * exclusive, unlike class. */
    char *name;                     /* <input name="q">                    */
    char *value;                    /* <input value="...">, the INITIAL one */
    char *type;                     /* <input type>, or <form method>      */

    /* <img width/height>, 0 = unstated. img_w DOUBLES as a table cell's
     * colspan: a cell is never an image and an image is never a cell, so the
     * two facts cannot collide, and a separate pair of shorts on every node in
     * the document would be arena spent to keep them apart. */
    short img_w, img_h;
    /* <table border=N>. Its own field, not another slot shared with something
     * "mutually exclusive": that trick is how img_w came to mean colspan, and
     * one such coincidence per struct is already the limit. 0 = unstated,
     * which for a table means NO rules -- the web's actual default, and what
     * `border="0"` says explicitly. */
    unsigned char tborder;
    /* <details open>. Its own byte rather than another shared slot -- see the
     * note on tborder. A <details> without it shows only its <summary>, which
     * is the difference between MDN's page being 15000 pixels tall and being
     * 300000: it collapses 112 sections, and rendering them all expanded is
     * not a slightly-too-long page, it is an unusable one. */
    unsigned char open;
    int   first_child, next_sibling, parent;   /* indices, -1 = none       */
};

struct html_doc {
    struct html_node *nodes;
    int  n, cap;
    char *strs;                     /* string arena                        */
    size_t strn, strcap;
    int  root;
    int  truncated;                 /* ran out of arena: tree is partial   */
    /* WHICH arena ran out, because they mean different things and the answer
     * used to be a single flag. A full NODE arena stops the document dead --
     * Wikipedia's header alone consumed all 8192 and the article was never
     * parsed at all, which read as a layout bug for far longer than it should
     * have. A full STRING arena keeps the structure and loses text. */
    int  trunc_nodes, trunc_strings;
    /* <style> content, concatenated in document order. The parser still does
     * not INTERPRET it -- it just stops throwing it away, so the stylist can
     * ask for it. NULL when the document has no <style>. */
    char *css;
    size_t css_len;
    /* <script> content, same bargain: kept, never run by the parser. Whether
     * it runs at all is the APP's decision -- a document viewer with no engine
     * simply ignores this field, which is how the browser behaved before there
     * was an engine to give it to. */
    char  *js[8];
    size_t js_len[8];
    /* The NODE each script came from. `document.currentScript` is how a modern
     * page finds the element it was written next to -- SvelteKit's bootstrap
     * is `document.currentScript.parentElement` -- and without a node for the
     * <script> there was nothing for it to be. -1 if the node could not be
     * made. */
    int    js_node[8];
    int    n_js;
    /* <link rel=stylesheet href=...> targets, in document order. The parser
     * records the URLs and nothing else: fetching them is a NETWORK act, and
     * the parser is the one part of this browser that never touches the
     * network. The app fetches each one and appends it to `css` before the
     * cascade runs -- which is also why they are in document order, since a
     * later sheet outranks an earlier one at equal specificity. */
    char  *cssref[8];
    /* The `media` each sheet was linked WITH. A <link media="print"> is for a
     * printer, and applying it on screen hides everything the page marked
     * unprintable -- on gnu.org that is the navigation, the header and the
     * breadcrumbs, which simply vanished. NULL means the link said nothing,
     * which means all media. */
    char  *cssmedia[8];
    int    n_cssref;
    /* <link rel="icon"> -- the page's own name for its favicon, which is not
     * always /favicon.ico and on some sites is the only one that exists. The
     * token is matched EXACTLY: "shortcut icon" contains it, "apple-touch-icon"
     * is a different token and a different (much larger) picture. */
    char  *iconref;
};

/* Replace an element's text content. Allocates from the document's own string
 * arena -- the DOM a script mutates is the same tree the renderer walks, so
 * there is exactly one document and no synchronisation question. Returns 0, or
 * -1 if the arena is full (the tree is left untouched, and truncated is set). */
int html_set_text(struct html_doc *doc, int node, const char *text);

/* Copy `n` bytes into the document's string arena and return the copy, or NULL
 * when it is full. Exposed because a script's mutations must live exactly as
 * long as the document does -- borrowing the same arena is what guarantees it,
 * and is why there is no second lifetime to reason about. */
char *html_intern(struct html_doc *doc, const char *s, size_t n);

/* ---- mutation: the document a script BUILDS ------------------------------
 *
 * A page that only reads its own markup is a document; one that creates nodes
 * is an application, and every framework written in the last fifteen years
 * does it. These allocate from the SAME arenas the parse used, so a script's
 * nodes live exactly as long as the document does and there is no second
 * lifetime to reason about -- and when the arena is full they FAIL and set
 * `truncated`, rather than growing into a page's hands.
 *
 * Created nodes start detached: appending is a separate act, so a script can
 * build a subtree before it is visible, which is what every one of them does.
 */
int  html_create_element(struct html_doc *doc, const char *tag);
int  html_create_text(struct html_doc *doc, const char *text);
/* Append `child` to `parent`, detaching it from wherever it was. Returns 0, or
 * -1 if either index is bad or the move would make a cycle. */
int  html_append_child(struct html_doc *doc, int parent, int child);
/* Detach `child` from its parent. It stays allocated (a script may re-append
 * it), it is simply no longer in the tree. */
int  html_remove_child(struct html_doc *doc, int child);
/* class / id / href / style, the attributes the cascade and the renderer read.
 * An unknown name is ignored rather than stored: there is no general attribute
 * map, and pretending otherwise would mean getAttribute lying back. */
int  html_set_attr(struct html_doc *doc, int node, const char *name, const char *val);

/* Parse `src` (len bytes) into `doc`. The caller owns both arenas; the parser
 * never allocates. Returns the root node index, or -1 if the arenas were too
 * small to hold even the root. Always leaves `doc` walkable. */
int html_parse(struct html_doc *doc, const char *src, size_t len,
               struct html_node *node_arena, int node_cap,
               char *str_arena, size_t str_cap);

/* Resolve `href` (which may be absolute, root-relative or relative) against
 * `base`, writing an absolute URL into out. Returns 0 on success. Link
 * following is most of what a browser IS, and it lives or dies on this. */
int html_resolve_url(const char *base, const char *href, char *out, size_t cap);

#endif /* _EMBLINK_WEB_HTML_H_ */

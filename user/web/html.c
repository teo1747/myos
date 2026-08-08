/* user/web/html.c -- the parser. See html.h for scope. */
#include <string.h>
#include <stdio.h>
#include "html.h"

/* ---- arena ------------------------------------------------------------- */

static int node_new(struct html_doc *d, int kind, int parent) {
    if (d->n >= d->cap) { d->truncated = 1; d->trunc_nodes = 1; return -1; }
    struct html_node *n = &d->nodes[d->n];
    memset(n, 0, sizeof *n);
    n->kind = (unsigned char)kind;
    n->first_child = n->next_sibling = -1;
    n->parent = parent;
    int idx = d->n++;
    if (parent >= 0) {                       /* append, keeping source order */
        struct html_node *p = &d->nodes[parent];
        if (p->first_child < 0) p->first_child = idx;
        else {
            int s = p->first_child;
            while (d->nodes[s].next_sibling >= 0) s = d->nodes[s].next_sibling;
            d->nodes[s].next_sibling = idx;
        }
    }
    return idx;
}

static char *str_put(struct html_doc *d, const char *s, size_t len) {
    if (d->strn + len + 1 > d->strcap) { d->truncated = 1; d->trunc_strings = 1; return 0; }
    char *out = d->strs + d->strn;
    memcpy(out, s, len);
    out[len] = 0;
    d->strn += len + 1;
    return out;
}

/* ---- character references ---------------------------------------------- */

/* Only the handful that actually occur in prose, plus numeric. An entity table
 * with two thousand names would be more spec-complete and no more useful; the
 * ones missing render as themselves, which is the failure a reader can see
 * through. */
static int entity(const char *s, size_t len, size_t *used, char *out);

/* Copy an attribute value, DECODING entities.
 *
 * They were decoded in text and not here, which is invisible until an
 * attribute is a URL: Wikipedia links its stylesheet as
 *
 *     href="/w/load.php?lang=en&amp;modules=...&amp;only=styles"
 *
 * and requesting that literally gives a server one parameter called `lang`
 * whose value happens to contain the word "modules". MediaWiki answered, quite
 * correctly, with an empty stylesheet reading "no modules were requested" --
 * so Wikipedia rendered as an unstyled column of blocks, and it looked for all
 * the world like the hardest layout bug in the corpus. Every `&` in a query
 * string is written `&amp;` in HTML; this is not an edge case.
 */
static void attr_copy(char *dst, size_t cap, const char *src, size_t len) {
    size_t o = 0;
    for (size_t i = 0; i < len && o + 1 < cap; ) {
        if (src[i] == '&') {
            char buf[8]; size_t used = 0;
            int n = entity(src + i, len - i, &used, buf);
            if (n) {
                for (int k = 0; k < n && o + 1 < cap; k++) dst[o++] = buf[k];
                i += used;
                continue;
            }
        }
        dst[o++] = src[i++];
    }
    dst[o] = 0;
}

static int entity(const char *s, size_t len, size_t *used, char *out) {
    static const struct { const char *name; const char *utf8; } tbl[] = {
        { "amp",  "&" }, { "lt",   "<" }, { "gt",   ">" }, { "quot", "\"" },
        { "apos", "'" }, { "nbsp", " " }, { "mdash", "\xE2\x80\x94" },
        { "ndash","\xE2\x80\x93" }, { "hellip", "\xE2\x80\xA6" },
        { "copy", "\xC2\xA9" }, { "reg", "\xC2\xAE" },
        { "ldquo","\xE2\x80\x9C" }, { "rdquo", "\xE2\x80\x9D" },
        { "lsquo","\xE2\x80\x98" }, { "rsquo", "\xE2\x80\x99" },
        /* the ones a technical document reaches for: dimensions, arrows,
         * fractions, degrees. Each earned its slot by appearing in real prose
         * rather than by being in the spec. */
        { "times","\xC3\x97" }, { "divide", "\xC3\xB7" }, { "minus", "\xE2\x88\x92" },
        { "deg",  "\xC2\xB0" }, { "plusmn", "\xC2\xB1" }, { "micro", "\xC2\xB5" },
        { "rarr", "\xE2\x86\x92" }, { "larr", "\xE2\x86\x90" },
        { "harr", "\xE2\x86\x94" }, { "darr", "\xE2\x86\x93" }, { "uarr", "\xE2\x86\x91" },
        { "bull", "\xE2\x80\xA2" }, { "middot", "\xC2\xB7" }, { "dagger", "\xE2\x80\xA0" },
        { "frac12","\xC2\xBD" }, { "frac14", "\xC2\xBC" }, { "sup2", "\xC2\xB2" },
        { "trade","\xE2\x84\xA2" }, { "euro", "\xE2\x82\xAC" }, { "pound", "\xC2\xA3" },
        { "laquo","\xC2\xAB" }, { "raquo", "\xC2\xBB" }, { "sect", "\xC2\xA7" },
        { "para", "\xC2\xB6" }, { "check", "\xE2\x9C\x93" }, { "cross", "\xE2\x9C\x95" },
    };
    if (len < 2 || s[0] != '&') return 0;
    size_t i = 1;
    if (s[i] == '#') {                                     /* numeric */
        i++;
        int hex = (i < len && (s[i] == 'x' || s[i] == 'X'));
        if (hex) i++;
        unsigned cp = 0; size_t d0 = i;
        while (i < len && s[i] != ';') {
            int c = s[i], v;
            if (c >= '0' && c <= '9') v = c - '0';
            else if (hex && c >= 'a' && c <= 'f') v = c - 'a' + 10;
            else if (hex && c >= 'A' && c <= 'F') v = c - 'A' + 10;
            else return 0;
            cp = cp * (hex ? 16 : 10) + (unsigned)v;
            i++;
        }
        if (i >= len || i == d0) return 0;
        i++;                                               /* the ';' */
        /* encode UTF-8 */
        int o = 0;
        if (cp < 0x80) out[o++] = (char)cp;
        else if (cp < 0x800) { out[o++] = (char)(0xC0 | (cp >> 6)); out[o++] = (char)(0x80 | (cp & 63)); }
        else { out[o++] = (char)(0xE0 | (cp >> 12)); out[o++] = (char)(0x80 | ((cp >> 6) & 63));
               out[o++] = (char)(0x80 | (cp & 63)); }
        out[o] = 0; *used = i;
        return o;
    }
    size_t ns = i;
    while (i < len && s[i] != ';' && i - ns < 12) i++;
    if (i >= len || s[i] != ';') return 0;
    size_t nl = i - ns;
    for (size_t k = 0; k < sizeof tbl / sizeof tbl[0]; k++) {
        if (strlen(tbl[k].name) == nl && memcmp(tbl[k].name, s + ns, nl) == 0) {
            int o = (int)strlen(tbl[k].utf8);
            memcpy(out, tbl[k].utf8, (size_t)o + 1);
            *used = i + 1;
            return o;
        }
    }
    return 0;
}

/* ---- tag classification ------------------------------------------------ */

static int ieq(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y) return 0;
    }
    return !*a && !*b;
}

static int is_void(const char *t) {
    static const char *v[] = { "br","img","hr","meta","link","input","area",
                               "base","col","embed","source","track","wbr", 0 };
    for (int i = 0; v[i]; i++) if (ieq(t, v[i])) return 1;
    return 0;
}

/* Elements that close an open one of the same kind. <p>a<p>b is two
 * paragraphs, not a nest -- get this wrong and a real page becomes one
 * infinitely-indented paragraph. */
static int closes_self(const char *t) {
    return ieq(t,"p") || ieq(t,"li") || ieq(t,"dt") || ieq(t,"dd") ||
           ieq(t,"tr") || ieq(t,"td") || ieq(t,"th") || ieq(t,"option");
}

/* Block elements that implicitly close an open <p>. */
static int closes_p(const char *t) {
    static const char *b[] = { "div","p","ul","ol","li","h1","h2","h3","h4","h5",
                               "h6","pre","table","blockquote","hr","section",
                               "article","header","footer","nav","form", 0 };
    for (int i = 0; b[i]; i++) if (ieq(t, b[i])) return 1;
    return 0;
}

/* ---- the parser -------------------------------------------------------- */

struct stack { int idx[64]; int n; };

static void push(struct stack *s, int i) { if (s->n < 64) s->idx[s->n++] = i; }
static int  top(struct stack *s) { return s->n ? s->idx[s->n - 1] : -1; }

/* Trim trailing space from an element's last text child, called when the
 * element closes. Leading/trailing whitespace is only insignificant at a
 * BLOCK's edges -- between inline elements it is a real space, which is why
 * trimming every text run turned "Hello <b>world</b>" into "Helloworld". */
static void trim_tail(struct html_doc *d, int elem) {
    if (elem < 0) return;
    int last = -1;
    for (int c = d->nodes[elem].first_child; c >= 0; c = d->nodes[c].next_sibling) last = c;
    if (last < 0 || d->nodes[last].kind != HTML_TEXT || !d->nodes[last].text) return;
    char *t = d->nodes[last].text;
    size_t l = strlen(t);
    while (l && t[l - 1] == ' ') t[--l] = 0;
}

/* Close the nearest open element named `tag`; if none is open, do nothing --
 * a stray </div> is noise, not a reason to unwind the document. */
static void close_tag(struct html_doc *d, struct stack *s, const char *tag) {
    for (int i = s->n - 1; i >= 0; i--) {
        if (ieq(d->nodes[s->idx[i]].tag, tag)) {
            for (int k = s->n - 1; k >= i; k--) trim_tail(d, s->idx[k]);
            s->n = i;
            return;
        }
    }
}

int html_parse(struct html_doc *d, const char *src, size_t len,
               struct html_node *nodes, int node_cap,
               char *strs, size_t str_cap)
{
    memset(d, 0, sizeof *d);
    d->nodes = nodes; d->cap = node_cap;
    d->strs = strs; d->strcap = str_cap;
    d->root = node_new(d, HTML_ELEM, -1);
    if (d->root < 0) return -1;
    snprintf(d->nodes[d->root].tag, HTML_TAG_MAX, "%s", "document");

    struct stack st = { {0}, 0 };
    push(&st, d->root);

    static char text[16384];        /* the run of text being accumulated */
    size_t tn = 0;

    /* flush accumulated text as a TEXT node, collapsing whitespace runs the
     * way HTML does -- otherwise every newline in the source becomes a gap */
    #define FLUSH() do {                                                     \
        if (tn) {                                                            \
            size_t a = 0, b = tn;                                            \
            /* only the FIRST run in a parent loses its leading space; the    \
             * trailing one is trimmed when the parent closes (trim_tail) */  \
            int pre_ = 0;                                                    \
            for (int k_ = st.n - 1; k_ >= 0; k_--)                            \
                if (ieq(d->nodes[st.idx[k_]].tag, "pre")) { pre_ = 1; break; }\
            if (!pre_ && (top(&st) < 0 || d->nodes[top(&st)].first_child < 0)) \
                while (a < b && text[a]==' ') a++;                            \
            if (b > a) {                                                     \
                char *p = str_put(d, text + a, b - a);                       \
                if (p) {                                                     \
                    int ti = node_new(d, HTML_TEXT, top(&st));               \
                    if (ti >= 0) d->nodes[ti].text = p;                      \
                }                                                            \
            }                                                                \
            tn = 0;                                                          \
        }                                                                    \
    } while (0)

    size_t i = 0;
    while (i < len) {
        if (src[i] != '<') {
            /* text, with entities decoded and whitespace runs collapsed */
            if (src[i] == '&') {
                char buf[8]; size_t used = 0;
                int o = entity(src + i, len - i, &used, buf);
                if (o) {
                    for (int k = 0; k < o && tn + 1 < sizeof text; k++) text[tn++] = buf[k];
                    i += used;
                    continue;
                }
            }
            /* Inside <pre> whitespace IS content: no collapsing, no newline
             * folding. Everywhere else a run of space is one space, which is
             * what makes source formatting invisible in the output. */
            int in_pre = 0;
            for (int k = st.n - 1; k >= 0; k--)
                if (ieq(d->nodes[st.idx[k]].tag, "pre")) { in_pre = 1; break; }
            char c = src[i++];
            if (!in_pre) {
                if (c == '\n' || c == '\t' || c == '\r') c = ' ';
                if (c == ' ' && tn && text[tn - 1] == ' ') continue;
            }
            if (tn + 1 < sizeof text) text[tn++] = c;
            continue;
        }

        /* --- a tag --- */
        if (len - i >= 4 && memcmp(src + i, "<!--", 4) == 0) {     /* comment */
            const char *e = 0;
            for (size_t k = i + 4; k + 2 < len; k++)
                if (src[k]=='-' && src[k+1]=='-' && src[k+2]=='>') { e = src + k; break; }
            i = e ? (size_t)(e - src) + 3 : len;
            continue;
        }
        if (len - i >= 2 && src[i+1] == '!') {                     /* doctype */
            while (i < len && src[i] != '>') i++;
            if (i < len) i++;
            continue;
        }

        int closing = (len - i >= 2 && src[i+1] == '/');
        size_t p = i + (closing ? 2 : 1);
        size_t ts = p;
        while (p < len && src[p] != '>' && src[p] != ' ' && src[p] != '\t'
               && src[p] != '\n' && src[p] != '/' ) p++;
        char tag[HTML_TAG_MAX];
        size_t tl = p - ts; if (tl >= HTML_TAG_MAX) tl = HTML_TAG_MAX - 1;
        for (size_t k = 0; k < tl; k++) {
            char c = src[ts + k];
            tag[k] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
        }
        tag[tl] = 0;

        /* attributes: only href/src are kept -- they are the only ones this
         * renderer can act on, and storing the rest would be arena spent on
         * data nobody reads */
        char href[HTML_HREF_MAX]; href[0] = 0;
        char klass[128]; klass[0] = 0;
        char eid[64];    eid[0] = 0;
        char sty[256];   sty[0] = 0;
        int  aopen = 0;
        char alt[256];   alt[0] = 0;
        int  aw = 0, ah = 0;
        char fname[64];  fname[0] = 0;
        char fval[256];  fval[0] = 0;
        char ftype[24];  ftype[0] = 0;
        char frel[32];   frel[0] = 0;
        int  tbord = 0;
        while (p < len && src[p] != '>') {
            while (p < len && (src[p]==' '||src[p]=='\t'||src[p]=='\n'||src[p]=='\r')) p++;
            if (p >= len || src[p] == '>' || src[p] == '/') break;
            size_t as = p;
            while (p < len && src[p]!='=' && src[p]!='>' && src[p]!=' ' && src[p]!='\t') p++;
            size_t al = p - as;
            char aname[24]; size_t an = al < sizeof aname - 1 ? al : sizeof aname - 1;
            for (size_t k = 0; k < an; k++) {
                char c = src[as + k];
                aname[k] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
            }
            aname[an] = 0;
            /* BOOLEAN attributes carry no value: `<details open>` is the whole
             * of it. Read from the name, before the branch that only runs when
             * there is an `=` -- which is where this was, so it never fired. */
            if (ieq(aname, "open")) aopen = 1;
            if (p < len && src[p] == '=') {
                p++;
                char q = 0;
                if (p < len && (src[p]=='"' || src[p]=='\'')) { q = src[p]; p++; }
                size_t vs = p;
                if (q) { while (p < len && src[p] != q) p++; }
                else   { while (p < len && src[p]!='>' && src[p]!=' ' && src[p]!='\t') p++; }
                size_t vl = p - vs;
                /* a form's ACTION is its target, which is what href already
                 * means -- same slot, same resolution against the base, and
                 * the same code path in url_resolve */
                if ((ieq(aname,"href") || ieq(aname,"src") ||
                     ieq(aname,"action")) && !href[0]) {
                    attr_copy(href, sizeof href, src + vs, vl);
                } else if (ieq(aname,"class") && !klass[0]) {
                    attr_copy(klass, sizeof klass, src + vs, vl);
                } else if (ieq(aname,"id") && !eid[0]) {
                    attr_copy(eid, sizeof eid, src + vs, vl);
                } else if (ieq(aname,"style") && !sty[0]) {
                    attr_copy(sty, sizeof sty, src + vs, vl);
                } else if (ieq(aname,"bgcolor") || ieq(aname,"align") ||
                           ieq(aname,"color")) {
                    /* PRESENTATIONAL ATTRIBUTES, mapped into the node's style.
                     *
                     * These are not a separate mechanism -- the HTML standard
                     * defines them as presentational hints that mean exactly
                     * these CSS declarations -- so folding them into `style`
                     * here costs no field on the node and no branch anywhere
                     * downstream: the inline-style path already handles them.
                     *
                     * They matter because the old web is built out of them.
                     * Hacker News paints its header with bgcolor="#ff6600" and
                     * puts its rank column in align="right"; without these the
                     * header was unreadable grey-on-dark and the ranks sat in
                     * the wrong place. Neither is expressible in that page's
                     * stylesheet, because that page does not have one for it.
                     *
                     * One deliberate inaccuracy: a real hint loses to any
                     * author rule, and these ride the inline style, which
                     * wins. A page whose CSS contradicts its own bgcolor is
                     * rarer than a page that only has the bgcolor. */
                    const char *prop = ieq(aname,"bgcolor") ? "background-color"
                                     : ieq(aname,"color")   ? "color" : "text-align";
                    size_t used = strlen(sty);
                    if (vl && used + vl + 20 < sizeof sty) {
                        int w = snprintf(sty + used, sizeof sty - used, "%s%s:%.*s",
                                         used ? ";" : "", prop, (int)vl, src + vs);
                        if (w < 0) sty[used] = 0;
                    }
                } else if (ieq(aname,"width") || ieq(aname,"height")) {
                    /* Kept because a stated size lets the layout RESERVE the
                     * space before the picture arrives -- without it every
                     * image that lands shoves the text the reader is in the
                     * middle of. Plain integers only; "50%" needs a
                     * containing block this box model does not expose. */
                    int v = 0, ok = (vl > 0);
                    for (size_t k2 = 0; k2 < vl; k2++) {
                        char c2 = src[vs + k2];
                        if (c2 < '0' || c2 > '9') { ok = 0; break; }
                        v = v * 10 + (c2 - '0');
                        if (v > 10000) { ok = 0; break; }
                    }
                    if (ok) { if (ieq(aname,"width")) aw = v; else ah = v; }
                } else if (ieq(aname,"colspan") || ieq(aname,"rowspan")) {
                    /* colspan shares img_w's slot: a cell is never an image,
                     * an image is never a cell, and a second pair of shorts on
                     * every node in the document would be arena spent to keep
                     * two mutually exclusive facts apart. Named honestly at
                     * both ends rather than left as a coincidence. */
                    int v = 0, ok = (vl > 0 && vl < 4);
                    for (size_t k2 = 0; k2 < vl && ok; k2++) {
                        char c2 = src[vs + k2];
                        if (c2 < '0' || c2 > '9') ok = 0; else v = v * 10 + (c2 - '0');
                    }
                    if (ok && v > 0 && v <= 32 && ieq(aname,"colspan")) aw = v;
                } else if (ieq(aname,"name") && !fname[0]) {
                    attr_copy(fname, sizeof fname, src + vs, vl);
                } else if (ieq(aname,"value") && !fval[0]) {
                    attr_copy(fval, sizeof fval, src + vs, vl);
                } else if ((ieq(aname,"type") || ieq(aname,"method")) && !ftype[0]) {
                    attr_copy(ftype, sizeof ftype, src + vs, vl);
                } else if (ieq(aname,"border")) {
                    /* Presentational, ancient, and still the only thing that
                     * decides whether an old page's table has rules. */
                    int v2 = 0, ok2 = (vl > 0 && vl < 4);
                    for (size_t k2 = 0; k2 < vl && ok2; k2++) {
                        char c2 = src[vs + k2];
                        if (c2 < '0' || c2 > '9') ok2 = 0; else v2 = v2 * 10 + (c2 - '0');
                    }
                    if (ok2) tbord = v2 > 255 ? 255 : v2;
                } else if (ieq(aname,"rel") && !frel[0]) {
                    attr_copy(frel, sizeof frel, src + vs, vl);
                } else if (ieq(aname,"alt") && !alt[0]) {
                    /* alt is not decoration: it is what the page SAYS when the
                     * picture cannot be shown, which for us is often. */
                    size_t c = vl < sizeof alt - 1 ? vl : sizeof alt - 1;
                    memcpy(alt, src + vs, c); alt[c] = 0;
                }
                if (q && p < len) p++;
            }
        }
        int self_closing = (p > i && src[p-1] == '/');
        if (p < len) p++;                              /* past '>' */

        if (!tag[0]) { i = p; continue; }

        /* <link rel=stylesheet>: remember WHERE the sheet is. rel is matched by
         * substring because it is a space-separated token list and real pages
         * write "alternate stylesheet" and "stylesheet noopener" alike. */
        if (ieq(tag, "link") && href[0] && d->n_cssref < 8) {
            int is_sheet = 0;
            for (size_t k2 = 0; frel[k2]; k2++) {
                int m = 1;
                const char *w = "stylesheet";
                for (int k3 = 0; k3 < 10; k3++) {
                    char c3 = frel[k2 + k3];
                    if (c3 >= 'A' && c3 <= 'Z') c3 = (char)(c3 - 'A' + 'a');
                    if (c3 != w[k3]) { m = 0; break; }
                }
                if (m) { is_sheet = 1; break; }
            }
            if (is_sheet) {
                char *held = str_put(d, href, strlen(href));
                if (held) d->cssref[d->n_cssref++] = held;
            }
        }

        if (closing) {
            FLUSH();
            close_tag(d, &st, tag);
            i = p;
            continue;
        }

        /* <script>/<style>: skip to the matching close without parsing. Their
         * contents are not markup, and treating them as such is how a page
         * ends up displaying its own code. */
        if (ieq(tag, "script") || ieq(tag, "style")) {
            char end[HTML_TAG_MAX + 4];
            snprintf(end, sizeof end, "</%s", tag);
            size_t el = strlen(end);
            size_t k = p;
            while (k + el <= len) {
                int m = 1;
                for (size_t j = 0; j < el; j++) {
                    char a = src[k+j], b = end[j];
                    if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
                    if (a != b) { m = 0; break; }
                }
                if (m) break;
                k++;
            }
            /* A <style> body is not markup -- but it is not garbage either.
             * KEEP it (concatenated across blocks, in document order, which is
             * the order the cascade needs); <script> is still discarded, since
             * nothing downstream can run it. */
            if (ieq(tag, "script") && k > p && d->n_js < 8) {
                char *held = str_put(d, src + p, k - p);
                if (held) { d->js[d->n_js] = held; d->js_len[d->n_js] = k - p; d->n_js++; }
            }
            if (ieq(tag, "style") && k > p) {
                char *held = str_put(d, src + p, k - p);
                if (held) {
                    if (!d->css) { d->css = held; d->css_len = k - p; }
                    else d->css_len = (size_t)(held - d->css) + (k - p);
                }
            }
            while (k < len && src[k] != '>') k++;
            i = k < len ? k + 1 : len;
            continue;
        }

        FLUSH();
        if (closes_self(tag) && ieq(d->nodes[top(&st)].tag, tag)) close_tag(d, &st, tag);
        else if (closes_p(tag) && ieq(d->nodes[top(&st)].tag, "p"))  close_tag(d, &st, "p");

        int ni = node_new(d, HTML_ELEM, top(&st));
        if (ni < 0) { i = p; continue; }
        snprintf(d->nodes[ni].tag, HTML_TAG_MAX, "%s", tag);
        if (href[0])  d->nodes[ni].href  = str_put(d, href,  strlen(href));
        if (klass[0]) d->nodes[ni].klass = str_put(d, klass, strlen(klass));
        if (eid[0])   d->nodes[ni].id    = str_put(d, eid,   strlen(eid));
        if (sty[0])   d->nodes[ni].style = str_put(d, sty,   strlen(sty));
        if (alt[0])   d->nodes[ni].alt   = str_put(d, alt,   strlen(alt));
        if (fname[0]) d->nodes[ni].name  = str_put(d, fname, strlen(fname));
        if (fval[0])  d->nodes[ni].value = str_put(d, fval,  strlen(fval));
        if (ftype[0]) d->nodes[ni].type  = str_put(d, ftype, strlen(ftype));
        d->nodes[ni].img_w = (short)aw; d->nodes[ni].img_h = (short)ah;
        d->nodes[ni].tborder = (unsigned char)tbord;
        d->nodes[ni].open = (unsigned char)aopen;
        if (!is_void(tag) && !self_closing) push(&st, ni);
        i = p;
    }
    FLUSH();
    for (int k = st.n - 1; k >= 0; k--) trim_tail(d, st.idx[k]);
    #undef FLUSH
    return d->root;
}

/* ---- URL resolution ---------------------------------------------------- */

int html_resolve_url(const char *base, const char *href, char *out, size_t cap) {
    if (!href || !href[0]) return -1;

    /* absolute */
    if (!strncmp(href, "http://", 7) || !strncmp(href, "https://", 8)) {
        snprintf(out, cap, "%s", href);
        return 0;
    }
    /* find the base's scheme://host boundary */
    const char *p = strstr(base, "://");
    if (!p) return -1;
    const char *hs = p + 3;
    const char *slash = strchr(hs, '/');
    size_t rootlen = slash ? (size_t)(slash - base) : strlen(base);

    if (href[0] == '/') {                       /* root-relative */
        snprintf(out, cap, "%.*s%s", (int)rootlen, base, href);
        return 0;
    }
    /* relative to the base's DIRECTORY -- everything up to the last slash. A
     * base of ".../a/b" makes "c" into ".../a/c", not ".../a/b/c". */
    size_t dirlen = strlen(base);
    while (dirlen > rootlen && base[dirlen - 1] != '/') dirlen--;
    if (dirlen < rootlen) dirlen = rootlen;
    if (dirlen == rootlen) snprintf(out, cap, "%.*s/%s", (int)rootlen, base, href);
    else                   snprintf(out, cap, "%.*s%s", (int)dirlen, base, href);
    return 0;
}

/* --- mutation ------------------------------------------------------------ */

int html_create_element(struct html_doc *d, const char *tag) {
    if (!d || !tag) return -1;
    int idx = node_new(d, HTML_ELEM, -1);      /* detached: no parent yet */
    if (idx < 0) return -1;
    snprintf(d->nodes[idx].tag, HTML_TAG_MAX, "%s", tag);
    for (char *p = d->nodes[idx].tag; *p; p++)
        if (*p >= 'A' && *p <= 'Z') *p = (char)(*p - 'A' + 'a');
    return idx;
}

int html_create_text(struct html_doc *d, const char *text) {
    if (!d || !text) return -1;
    int idx = node_new(d, HTML_TEXT, -1);
    if (idx < 0) return -1;
    char *held = str_put(d, text, strlen(text));
    if (!held) { d->truncated = 1; return -1; }
    d->nodes[idx].text = held;
    return idx;
}

int html_remove_child(struct html_doc *d, int child) {
    if (!d || child < 0 || child >= d->n) return -1;
    int p = d->nodes[child].parent;
    if (p < 0) return 0;                        /* already detached */
    if (d->nodes[p].first_child == child) {
        d->nodes[p].first_child = d->nodes[child].next_sibling;
    } else {
        int it = d->nodes[p].first_child;
        while (it >= 0 && d->nodes[it].next_sibling != child)
            it = d->nodes[it].next_sibling;
        if (it >= 0) d->nodes[it].next_sibling = d->nodes[child].next_sibling;
    }
    d->nodes[child].parent = -1;
    d->nodes[child].next_sibling = -1;
    return 0;
}

int html_append_child(struct html_doc *d, int parent, int child) {
    if (!d || parent < 0 || parent >= d->n || child < 0 || child >= d->n) return -1;
    if (parent == child) return -1;
    /* A node cannot be appended into its own subtree: the tree would gain a
     * cycle and every walker in this browser recurses without a visited set. */
    for (int a = parent; a >= 0; a = d->nodes[a].parent)
        if (a == child) return -1;
    html_remove_child(d, child);
    d->nodes[child].parent = parent;
    d->nodes[child].next_sibling = -1;
    if (d->nodes[parent].first_child < 0) {
        d->nodes[parent].first_child = child;
    } else {
        int s = d->nodes[parent].first_child;
        while (d->nodes[s].next_sibling >= 0) s = d->nodes[s].next_sibling;
        d->nodes[s].next_sibling = child;
    }
    return 0;
}

/* Attribute names are ASCII and case-insensitive; the freestanding libc this
 * builds against has no strcasecmp, and one comparison does not justify a
 * dependency. */
static int aeq(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        char x = (*a >= 'A' && *a <= 'Z') ? (char)(*a - 'A' + 'a') : *a;
        char y = (*b >= 'A' && *b <= 'Z') ? (char)(*b - 'A' + 'a') : *b;
        if (x != y) return 0;
    }
    return !*a && !*b;
}

int html_set_attr(struct html_doc *d, int node, const char *name, const char *val) {
    if (!d || node < 0 || node >= d->n || !name || !val) return -1;
    char *held = str_put(d, val, strlen(val));
    if (!held) { d->truncated = 1; return -1; }
    struct html_node *e = &d->nodes[node];
    if      (aeq(name, "class")) e->klass = held;
    else if (aeq(name, "id"))    e->id    = held;
    else if (aeq(name, "style")) e->style = held;
    else if (aeq(name, "href") || aeq(name, "src") ||
             aeq(name, "action")) e->href = held;
    else if (aeq(name, "alt"))   e->alt   = held;
    else if (aeq(name, "name"))  e->name  = held;
    else if (aeq(name, "type"))  e->type  = held;
    else return -1;                       /* not stored: see the header */
    return 0;
}

int html_set_text(struct html_doc *d, int node, const char *text) {
    if (!d || node < 0 || node >= d->n || !text) return -1;
    size_t n = strlen(text);
    char *held = str_put(d, text, n);
    if (!held) { d->truncated = 1; return -1; }

    struct html_node *e = &d->nodes[node];
    if (e->kind == HTML_TEXT) { e->text = held; return 0; }

    /* An ELEMENT's textContent replaces all its children with one text node.
     * Reusing the FIRST child when it is already text keeps the common case
     * (a <span> whose message a script updates) from consuming a node per
     * assignment -- an animation loop would otherwise exhaust the arena. */
    int c = e->first_child;
    if (c >= 0 && d->nodes[c].kind == HTML_TEXT) {
        d->nodes[c].text = held;
        d->nodes[c].next_sibling = -1;      /* drop any siblings after it */
        return 0;
    }
    int ni = node_new(d, HTML_TEXT, node);
    if (ni < 0) return -1;
    d->nodes[ni].text = held;
    e->first_child = ni;
    d->nodes[ni].next_sibling = -1;
    return 0;
}

char *html_intern(struct html_doc *d, const char *s, size_t n) { return str_put(d, s, n); }

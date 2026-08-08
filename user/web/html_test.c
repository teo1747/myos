/* user/web/html_test.c -- the parser's host test.
 *
 * Runs on the DEVELOPMENT machine (`make html-test`), not the OS. A parser is
 * the one part of a browser that can be fully exercised without a network, a
 * window or a font -- so it should be, and in seconds, rather than through a
 * two-minute image build and a boot.
 */
#include <stdio.h>
#include <string.h>
#include "html.h"
#include "url.h"
#include "style.h"
#include "css.h"
#include "cookie.h"
#include "tabs.h"
#include "png.h"
#include "png_fixtures.h"
#include "form.h"
#include "jpeg.h"
#include "jpeg_fixtures.h"

static int failures;
#define CHECK(c, what) do {                                            \
        if (c) printf("  ok:   %s\n", what);                           \
        else { printf("  FAIL: %s\n", what); failures++; }             \
    } while (0)

static struct html_node NODES[4096];
static char             STRS[65536];
static struct html_doc  D;

static int parse(const char *src) {
    return html_parse(&D, src, strlen(src), NODES, 4096, STRS, sizeof STRS);
}

/* first element with this tag, depth-first */
static int find(int at, const char *tag) {
    if (at < 0) return -1;
    if (D.nodes[at].kind == HTML_ELEM && !strcmp(D.nodes[at].tag, tag)) return at;
    for (int c = D.nodes[at].first_child; c >= 0; c = D.nodes[c].next_sibling) {
        int r = find(c, tag);
        if (r >= 0) return r;
    }
    return -1;
}
static int count(int at, const char *tag) {
    if (at < 0) return 0;
    int n = (D.nodes[at].kind == HTML_ELEM && !strcmp(D.nodes[at].tag, tag)) ? 1 : 0;
    for (int c = D.nodes[at].first_child; c >= 0; c = D.nodes[c].next_sibling)
        n += count(c, tag);
    return n;
}
/* concatenated text under a node */
static void gather(int at, char *out, size_t cap) {
    if (at < 0) return;
    if (D.nodes[at].kind == HTML_TEXT && D.nodes[at].text) {
        size_t l = strlen(out), a = strlen(D.nodes[at].text);
        if (l + a + 1 < cap) memcpy(out + l, D.nodes[at].text, a + 1);
        return;
    }
    for (int c = D.nodes[at].first_child; c >= 0; c = D.nodes[c].next_sibling)
        gather(c, out, cap);
}

static void t1_structure(void) {
    printf("T1 elements, nesting, text:\n");
    int r = parse("<html><body><h1>Title</h1><p>Hello <b>world</b>.</p></body></html>");
    CHECK(r >= 0, "parsed");
    CHECK(find(r, "h1") >= 0, "h1 found");
    CHECK(find(r, "b")  >= 0, "nested b found");
    char buf[256] = {0};
    gather(find(r, "p"), buf, sizeof buf);
    CHECK(!strcmp(buf, "Hello world."), "paragraph text reassembles");
}

static void t2_implicit_close(void) {
    printf("T2 implicit end tags (the one that ruins real pages):\n");
    int r = parse("<body><p>one<p>two<p>three</body>");
    CHECK(count(r, "p") == 3, "three sibling paragraphs, not nested");
    int p1 = find(r, "p");
    CHECK(D.nodes[p1].first_child >= 0 &&
          D.nodes[D.nodes[p1].first_child].kind == HTML_TEXT, "first p holds text");
    CHECK(find(D.nodes[p1].first_child, "p") < 0, "first p does NOT contain a p");

    r = parse("<ul><li>a<li>b<li>c</ul>");
    CHECK(count(r, "li") == 3, "three sibling list items");
}

static void t3_void_and_attrs(void) {
    printf("T3 void elements and attributes:\n");
    int r = parse("<p>a<br>b</p><img src=\"/pic.png\"><a href='/x'>link</a>");
    CHECK(count(r, "br") == 1, "br parsed");
    int img = find(r, "img");
    CHECK(img >= 0 && D.nodes[img].href && !strcmp(D.nodes[img].href, "/pic.png"),
          "img src captured (double-quoted)");
    int a = find(r, "a");
    CHECK(a >= 0 && D.nodes[a].href && !strcmp(D.nodes[a].href, "/x"),
          "a href captured (single-quoted)");
    char buf[64] = {0};
    gather(a, buf, sizeof buf);
    CHECK(!strcmp(buf, "link"), "anchor text");
    /* a void element must not swallow what follows it */
    CHECK(find(D.nodes[find(r,"br")].first_child, "a") < 0, "br has no children");
}

static void t4_entities(void) {
    printf("T4 character references:\n");
    int r = parse("<p>a &amp; b &lt;tag&gt; &#65; &#x42; &quot;q&quot;</p>");
    char buf[256] = {0};
    gather(find(r, "p"), buf, sizeof buf);
    CHECK(!strcmp(buf, "a & b <tag> A B \"q\""), "named, decimal and hex decode");
    r = parse("<p>bare &notanentity; stays</p>");
    buf[0] = 0; gather(find(r, "p"), buf, sizeof buf);
    CHECK(strstr(buf, "&notanentity;") != 0, "an unknown entity is left literal");
}

static void t5_script_style(void) {
    printf("T5 script/style contents are not markup:\n");
    int r = parse("<body><script>var x = '<p>fake</p>';</script><p>real</p></body>");
    CHECK(count(r, "p") == 1, "the <p> inside the script did not become an element");
    char buf[256] = {0};
    gather(r, buf, sizeof buf);
    CHECK(!strstr(buf, "var x"), "script source is not shown as text");
    CHECK(strstr(buf, "real") != 0, "content after the script still parses");

    r = parse("<style>p{color:red}</style><p>hi</p>");
    buf[0] = 0; gather(r, buf, sizeof buf);
    CHECK(!strstr(buf, "color:red"), "stylesheet is not shown as text");
}

static void t6_whitespace(void) {
    printf("T6 whitespace collapsing:\n");
    int r = parse("<p>one\n   two\t\tthree</p>");
    char buf[128] = {0};
    gather(find(r, "p"), buf, sizeof buf);
    CHECK(!strcmp(buf, "one two three"), "runs collapse to one space");
    r = parse("<p>   leading and trailing   </p>");
    buf[0] = 0; gather(find(r, "p"), buf, sizeof buf);
    CHECK(!strcmp(buf, "leading and trailing"), "edges trimmed");
}

static void t7_malformed(void) {
    printf("T7 malformed input does not derail the parse:\n");
    int r = parse("<p>text</div></span><p>after");
    CHECK(count(r, "p") == 2, "stray close tags ignored, both paragraphs kept");
    r = parse("<p>unclosed <b>bold");
    CHECK(r >= 0 && find(r, "b") >= 0, "unclosed tags at EOF still produce a tree");
    r = parse("<!-- <p>commented</p> --><p>live</p>");
    CHECK(count(r, "p") == 1, "commented markup is not parsed");
    r = parse("<!doctype html><p>x</p>");
    CHECK(count(r, "p") == 1, "doctype skipped");
}

static void t8_urls(void) {
    printf("T8 URL resolution (link following lives or dies here):\n");
    char out[512];
    html_resolve_url("http://a.example/dir/page.html", "http://b.example/x", out, sizeof out);
    CHECK(!strcmp(out, "http://b.example/x"), "absolute passes through");
    html_resolve_url("http://a.example/dir/page.html", "/root.html", out, sizeof out);
    CHECK(!strcmp(out, "http://a.example/root.html"), "root-relative");
    html_resolve_url("http://a.example/dir/page.html", "next.html", out, sizeof out);
    CHECK(!strcmp(out, "http://a.example/dir/next.html"), "relative to the DIRECTORY");
    html_resolve_url("http://a.example", "x.html", out, sizeof out);
    CHECK(!strcmp(out, "http://a.example/x.html"), "bare host gets a slash");
    html_resolve_url("http://a.example/dir/", "sub/y.html", out, sizeof out);
    CHECK(!strcmp(out, "http://a.example/dir/sub/y.html"), "relative with a subpath");
}

static void t9_bounded(void) {
    printf("T9 bounded arenas (this parses input from the network):\n");
    static struct html_node few[8];
    static char small[64];
    struct html_doc d;
    const char *src = "<body><p>a</p><p>b</p><p>c</p><p>d</p><p>e</p><p>f</p></body>";
    int r = html_parse(&d, src, strlen(src), few, 8, small, sizeof small);
    CHECK(r >= 0, "returns a usable root even when the arena is too small");
    CHECK(d.truncated == 1, "truncation is REPORTED, not silent");
    CHECK(d.n <= 8, "never exceeds the node arena");
}

/* T10-T11 cover url.c: what a location IS, before anything fetches it. Pure
 * string work, so it belongs in the fast host loop rather than in a boot. */
static void t10_url_parse(void) {
    printf("T10 url_parse (a path and a URL both go in the address bar):\n");
    struct url u;

    CHECK(url_parse("http://example.com/a/b?q=1", &u) == 0, "http parses");
    CHECK(u.kind == URL_HTTP && u.port == 80, "default port 80");
    CHECK(!strcmp(u.host, "example.com"), "host split off");
    CHECK(!strcmp(u.path, "/a/b?q=1"), "the query stays part of the request target");

    CHECK(url_parse("https://a.test", &u) == 0, "https with no path parses");
    CHECK(u.kind == URL_HTTPS && u.port == 443, "default port 443");
    CHECK(!strcmp(u.path, "/"), "an empty path becomes /");

    CHECK(url_parse("http://10.0.2.2:8000/hello.html", &u) == 0, "explicit port parses");
    CHECK(u.port == 8000 && !strcmp(u.host, "10.0.2.2"), "port split from a literal host");

    CHECK(url_parse("/system/web/index.html", &u) == 0, "a bare path is a LOCAL location");
    CHECK(u.kind == URL_LOCAL && !strcmp(u.path, "/system/web/index.html"), "kept verbatim");

    CHECK(url_parse("file:///data/x.html", &u) == 0, "file:// parses");
    CHECK(u.kind == URL_LOCAL && !strcmp(u.path, "/data/x.html"), "file:// yields the path");

    CHECK(url_parse("ftp://x/y", &u) != 0, "an unsupported scheme is refused");
    CHECK(url_parse("nonsense", &u) != 0, "a bare word is refused, not guessed at");
    CHECK(url_parse("http://", &u) != 0, "a URL with no host is refused");
}

static void t11_url_resolve(void) {
    printf("T11 url_resolve (one rule, two worlds):\n");
    char out[512];

    CHECK(url_resolve("http://h/a/b.html", "c.html", out, sizeof out) == 0 &&
          !strcmp(out, "http://h/a/c.html"), "network base + relative -> sibling");
    CHECK(url_resolve("http://h/a/b.html", "/z.html", out, sizeof out) == 0 &&
          !strcmp(out, "http://h/z.html"), "network base + root-relative -> host root");
    CHECK(url_resolve("http://h/a/b.html", "https://o/x", out, sizeof out) == 0 &&
          !strcmp(out, "https://o/x"), "an absolute href ignores the base");

    /* the local half, which the network resolver cannot do: it has no "://" to
     * anchor on, and a leading '/' IS the root rather than being relative to one */
    CHECK(url_resolve("/system/web/index.html", "about.html", out, sizeof out) == 0 &&
          !strcmp(out, "/system/web/about.html"), "local base + relative -> sibling file");
    CHECK(url_resolve("/system/web/index.html", "/data/x.html", out, sizeof out) == 0 &&
          !strcmp(out, "/data/x.html"), "local base + absolute path -> that path");
    CHECK(url_resolve("/system/web/index.html", "http://h/x", out, sizeof out) == 0 &&
          !strcmp(out, "http://h/x"), "a local page can link OUT to the network");

    CHECK(url_resolve("http://h/a", "", out, sizeof out) != 0, "an empty href resolves to nothing");
}

/* ======================= CSS (B5) ======================================= */

static struct html_node CN[512];
static char CS[16384];
static struct html_doc CD;

static int cparse(const char *src) {
    return html_parse(&CD, src, strlen(src), CN, 512, CS, sizeof CS);
}
/* find the first element with this tag */
static int find_tag(const char *tag) {
    for (int i = 0; i < CD.n; i++)
        if (CD.nodes[i].kind == HTML_ELEM && !strcmp(CD.nodes[i].tag, tag)) return i;
    return -1;
}

static void t11b_entities(void) {
    printf("T11b the entities a technical document actually uses:\n");
    cparse("<p>300&times;60 &deg; 90&rarr;100 &frac12; &bull; &euro;5 &check;</p>");
    const char *t = CD.nodes[CD.nodes[find_tag("p")].first_child].text;
    CHECK(t && strstr(t, "\xC3\x97") != 0, "&times; -> U+00D7");
    CHECK(t && strstr(t, "\xC2\xB0") != 0, "&deg; -> U+00B0");
    CHECK(t && strstr(t, "\xE2\x86\x92") != 0, "&rarr; -> U+2192");
    CHECK(t && strstr(t, "\xC2\xBD") != 0, "&frac12; -> U+00BD");
    CHECK(t && !strstr(t, "&times;"), "...and none of them survive as raw text");
    cparse("<p>a &notreal; b</p>");
    t = CD.nodes[CD.nodes[find_tag("p")].first_child].text;
    CHECK(t && strstr(t, "&notreal;") != 0,
          "an unknown entity renders as itself -- a failure the reader sees through");
}

static void t12_attrs_and_style_block(void) {
    printf("T12 the parser keeps what CSS needs:\n");
    cparse("<style>p{color:red}</style>"
           "<p class='lead big' id='intro' style='font-weight:bold'>hi</p>");
    int p = find_tag("p");
    CHECK(p >= 0, "the element parsed");
    CHECK(CD.nodes[p].klass && !strcmp(CD.nodes[p].klass, "lead big"), "class is kept verbatim");
    CHECK(CD.nodes[p].id && !strcmp(CD.nodes[p].id, "intro"), "id is kept");
    CHECK(CD.nodes[p].style && !strcmp(CD.nodes[p].style, "font-weight:bold"), "inline style is kept");
    CHECK(CD.css && CD.css_len > 0, "<style> content is KEPT, not discarded");
    CHECK(!strncmp(CD.css, "p{color:red}", 12), "...and it is the stylesheet text");

    cparse("<script>var x = '<p>not markup</p>';</script><p>real</p>");
    CHECK(CD.css == 0, "<script> is still discarded -- nothing can run it");
    CHECK(find_tag("p") >= 0 && CD.nodes[find_tag("p")].first_child >= 0,
          "and script content never becomes elements");
}

static void t13_declarations(void) {
    printf("T13 declarations mean what they say:\n");
    struct vstyle v;
    memset(&v, 0, sizeof v);
    css_apply_decls("color:#c00; font-weight:bold; font-style:italic", strlen("color:#c00; font-weight:bold; font-style:italic"), &v);
    CHECK(v.color == 0xFFCC0000u, "#c00 expands to #cc0000");
    CHECK(v.bold == 1 && v.italic == 1, "weight and style applied");

    memset(&v, 0, sizeof v);
    css_apply_decls("color:red", strlen("color:red"), &v);
    CHECK(v.color == 0xFFFF0000u, "named colours work");

    memset(&v, 0, sizeof v);
    css_apply_decls("display:none", strlen("display:none"), &v);
    CHECK(v.display == VD_NONE, "display:none hides");

    memset(&v, 0, sizeof v);
    css_apply_decls("margin: 4px 8px 12px", strlen("margin: 4px 8px 12px"), &v);
    CHECK(v.margin_top == 4 && v.margin_bottom == 12, "the margin shorthand's 3-value form");

    memset(&v, 0, sizeof v);
    css_apply_decls("font-family: Menlo, monospace", strlen("font-family: Menlo, monospace"), &v);
    CHECK(v.mono == 1, "a mono family is recognised");

    memset(&v, 0, sizeof v);
    v.bold = 1;
    /* The example used to be `float:left`, which this browser now honours --
     * so the case had to move to properties that are still genuinely beyond
     * it. The claim under test is unchanged: an unknown property, a vendor
     * hack and a declaration with no value are all SKIPPED rather than
     * half-applied. */
    const char *junk = "text-shadow:1px 1px red; -webkit-hack:1; color:";
    int n = css_apply_decls(junk, strlen(junk), &v);
    CHECK(n == 0, "properties we cannot honour are skipped, not faked");
    CHECK(v.bold == 1, "...and skipping one does not disturb the rest");

    memset(&v, 0, sizeof v);
    css_apply_decls("float: left", strlen("float: left"), &v);
    CHECK(v.floatp == VF_LEFT, "float IS honoured now");
    memset(&v, 0, sizeof v);
    css_apply_decls("clear: both", strlen("clear: both"), &v);
    CHECK(v.clearp == 3, "...and so is clear");

    memset(&v, 0, sizeof v);
    css_apply_decls("color:red;;; ; font-weight:bold", strlen("color:red;;; ; font-weight:bold"), &v);
    CHECK(v.color && v.bold, "malformed separators do not derail the block");
}

static void t14_selectors(void) {
    printf("T14 selectors match and carry specificity:\n");
    cparse("<nav><ul><li class='item'><a id='home' href='#'>x</a></li></ul></nav>");
    int a = find_tag("a"), li = find_tag("li");
    struct css_sel s;

    CHECK(css_sel_parse("a", strlen("a"), &s) == 0 && css_sel_match(&s, &CD, a), "type selector");
    CHECK(s.spec == 1, "...specificity 1");
    CHECK(css_sel_parse(".item", strlen(".item"), &s) == 0 && css_sel_match(&s, &CD, li), "class selector");
    CHECK(s.spec == 10, "...specificity 10");
    CHECK(css_sel_parse("#home", strlen("#home"), &s) == 0 && css_sel_match(&s, &CD, a), "id selector");
    CHECK(s.spec == 100, "...specificity 100");
    CHECK(css_sel_parse("*", strlen("*"), &s) == 0 && css_sel_match(&s, &CD, a), "universal matches anything");

    CHECK(css_sel_parse("nav a", strlen("nav a"), &s) == 0 && css_sel_match(&s, &CD, a),
          "descendant selector crosses generations");
    CHECK(css_sel_parse("nav ul li a", strlen("nav ul li a"), &s) == 0 && css_sel_match(&s, &CD, a),
          "a four-part descendant chain");
    CHECK(css_sel_parse("li.item", strlen("li.item"), &s) == 0 && css_sel_match(&s, &CD, li),
          "a compound (type + class)");
    CHECK(css_sel_parse("p a", strlen("p a"), &s) == 0 && !css_sel_match(&s, &CD, a),
          "a wrong ancestor does NOT match");
    CHECK(css_sel_parse("li.missing", strlen("li.missing"), &s) == 0 && !css_sel_match(&s, &CD, li),
          "a wrong class does NOT match");
    CHECK(css_sel_parse("a:hover", strlen("a:hover"), &s) == 0 && css_sel_match(&s, &CD, a),
          "an unevaluable pseudo-class keeps the rest of the compound");
}

static void t15_cascade(void) {
    printf("T15 the cascade resolves by specificity, then order:\n");
    /* STATIC: a sheet holds CSS_MAX_RULES rules and is megabytes now, which
     * is a segfault the moment it goes on the stack. */
    static struct css_sheet sh;
    struct vstyle v;

    /* specificity beats document order */
    static const char *css1 = "p { color: red } .lead { color: blue }";
    cparse("<p class='lead'>x</p>");
    css_sheet_parse(&sh, css1, strlen(css1));
    CHECK(sh.n == 2, "two rules parsed");
    memset(&v, 0, sizeof v);
    css_sheet_apply(&sh, &CD, find_tag("p"), &v);
    CHECK(v.color == 0xFF0000FFu, "the class (10) beats the type (1) whatever the order");

    /* ...and when specificity ties, the LATER rule wins */
    static const char *css2 = "p { color: red } p { color: blue }";
    css_sheet_parse(&sh, css2, strlen(css2));
    memset(&v, 0, sizeof v);
    css_sheet_apply(&sh, &CD, find_tag("p"), &v);
    CHECK(v.color == 0xFF0000FFu, "equal specificity -> document order decides");

    /* a weaker rule still contributes properties the stronger one omits */
    static const char *css3 = "p { color: red; font-style: italic } .lead { color: blue }";
    css_sheet_parse(&sh, css3, strlen(css3));
    memset(&v, 0, sizeof v);
    css_sheet_apply(&sh, &CD, find_tag("p"), &v);
    CHECK(v.color == 0xFF0000FFu && v.italic == 1,
          "the loser still supplies what the winner did not set");

    /* selector lists split into rules with their OWN specificity */
    static const char *css4 = "h1, .lead, #x { font-weight: bold }";
    css_sheet_parse(&sh, css4, strlen(css4));
    CHECK(sh.n == 3, "a comma list becomes three rules");
    memset(&v, 0, sizeof v);
    css_sheet_apply(&sh, &CD, find_tag("p"), &v);
    CHECK(v.bold == 1, "...and the one that matches applies");

    /* comments and @media */
    static const char *css5 = "/* c */ p { color: red } @media print { p { color: lime } }";
    css_sheet_parse(&sh, css5, strlen(css5));
    memset(&v, 0, sizeof v);
    css_sheet_apply(&sh, &CD, find_tag("p"), &v);
    CHECK(v.color == 0xFFFF0000u, "comments skipped; @media dropped rather than misapplied");
}

static void t16_origin_order(void) {
    printf("T16 origin order: user-agent < author < inline:\n");
    /* STATIC: a sheet holds CSS_MAX_RULES rules and is megabytes now, which
     * is a segfault the moment it goes on the stack. */
    static struct css_sheet sh;
    struct vstyle root, v;
    vstyle_root(&root);

    cparse("<h1 style='color:lime'>t</h1>");
    static const char *css = "h1 { color: red }";
    css_sheet_parse(&sh, css, strlen(css));

    int h = find_tag("h1");
    vstyle_for_node(&CD, h, &root, &sh, &v);
    CHECK(v.color == 0xFF00FF00u, "inline style beats the author stylesheet");
    CHECK(v.size == 3 && v.bold == 1, "...and the UA stylesheet still supplies the rest");

    /* author beats UA */
    cparse("<h1>t</h1>");
    static const char *css6 = "h1 { font-weight: normal }";
    css_sheet_parse(&sh, css6, strlen(css6));
    vstyle_for_node(&CD, find_tag("h1"), &root, &sh, &v);
    CHECK(v.bold == 0, "the author can un-bold what the UA stylesheet bolded");

    /* no sheet at all == the pre-CSS behaviour, exactly */
    vstyle_for_node(&CD, find_tag("h1"), &root, 0, &v);
    CHECK(v.bold == 1 && v.size == 3, "no stylesheet -> the UA result, unchanged");
}

static void t17_bounded(void) {
    printf("T17 a stylesheet from a stranger is bounded:\n");
    /* Sized FROM the cap, not from a number that happened to exceed it when
     * this was written. The literal 400 silently stopped testing anything the
     * day CSS_MAX_RULES went past it -- the check still passed, against a
     * sheet that no longer truncated. */
    static char big[(CSS_MAX_RULES + 64) * 24];
    size_t k = 0;
    for (int i = 0; i < CSS_MAX_RULES + 32 && k < sizeof big - 40; i++)
        k += (size_t)snprintf(big + k, sizeof big - k, ".c%d { color: red } ", i);
    /* STATIC: a sheet holds CSS_MAX_RULES rules and is megabytes now, which
     * is a segfault the moment it goes on the stack. */
    static struct css_sheet sh;
    css_sheet_parse(&sh, big, k);
    CHECK(sh.n <= CSS_MAX_RULES, "never exceeds the rule table");
    CHECK(sh.truncated == 1, "and truncation is REPORTED, not silent");

    /* A selector longer than the parser can hold keeps its SUBJECT -- the
     * element the rule is about -- and drops the outermost ancestors. Getting
     * this backwards applied the rule to an ancestor, which for display:none
     * removes a whole subtree rather than one element. */
    struct css_sel sel;
    const char *deep = "div.a div.b div.c div.d div.e span.leaf";
    CHECK(css_sel_parse(deep, strlen(deep), &sel) == 0, "an over-long selector still parses");
    CHECK(sel.n == CSS_SEL_PARTS, "...filling the parts it has");
    CHECK(!strcmp(sel.part[sel.n - 1].tag, "span") &&
          !strcmp(sel.part[sel.n - 1].klass, "leaf"),
          "...and the SUBJECT is the last compound, not the first");
    CHECK(sel.part[0].comb == CSS_COMB_DESC,
          "the outermost kept compound is a descendant: its real ancestor is gone");
    /* six classes + one on the subject = 7 classes (10 each), and six tag
     * names (1 each) -- counted across the WHOLE selector, dropped compounds
     * included, which is what a real engine reports. */
    /* Six compounds, each one tag name (1) plus one class (10). Counted
     * across the WHOLE selector, dropped compounds included, which is what a
     * real engine reports. */
    CHECK(sel.spec == 6 * (10 + 1),
          "specificity counts every compound, including dropped ones");

    /* The cascade must be able to INDEX every rule it is allowed to hold.
     * The match list was an unsigned char array -- exactly wide enough at 256
     * rules, and silent corruption at anything more: rule 300 was recorded as
     * 44, so a page's <body> was styled by a rule written for something else.
     * A sheet whose LAST rule is the one that matters is the shape that
     * catches it. */
    {
        static char many[(CSS_MAX_RULES + 8) * 32];
        size_t k = 0;
        for (int i = 0; i < CSS_MAX_RULES - 1 && k < sizeof many - 64; i++)
            k += (size_t)snprintf(many + k, sizeof many - k, ".none%d { color: #010101 } ", i);
        k += (size_t)snprintf(many + k, sizeof many - k, "p { color: #20c040 } ");
        static struct css_sheet big2;
        css_sheet_parse(&big2, many, k);
        CHECK(big2.n == CSS_MAX_RULES && !big2.truncated, "a full sheet, not truncated");

        static struct html_node nd[64];
        static char st[4096];
        struct html_doc dd;
        const char *src = "<body><p>x</p></body>";
        html_parse(&dd, src, strlen(src), nd, 64, st, sizeof st);
        int p = -1;
        for (int i = 0; i < dd.n; i++)
            if (dd.nodes[i].kind == HTML_ELEM && !strcmp(dd.nodes[i].tag, "p")) { p = i; break; }
        struct vstyle rv, pv;
        vstyle_root(&rv);
        vstyle_for_node(&dd, p, &rv, &big2, &pv);
        CHECK(p >= 0 && pv.color == 0xFF20C040u,
              "the LAST rule in a full sheet is the one that applies");
    }
}

/* ======================= PNG (B6) ======================================= */
/* Fixtures are generated PNGs (see png_fixtures.h). Each is small enough to
 * reason about by hand, which is the point: a decoder verified against big
 * photos tells you it did not crash, not that it is correct. */

static uint32_t PIX[64 * 64];
static uint8_t  SCR[64 * 64 * 8];

#define A(p) (((p) >> 24) & 0xFF)
#define R(p) (((p) >> 16) & 0xFF)
#define G(p) (((p) >>  8) & 0xFF)
#define B(p) ( (p)        & 0xFF)

static void t17b_image_sizing(void) {
    printf("T17b <img> sizing: the space is reserved BEFORE the picture:\n");
    cparse("<img src='a.png' width='320' height='180' alt='x'>"
           "<img src='b.png' alt='y'>"
           "<img src='c.png' width='9999999' height='4'>");
    int i0 = -1, i1 = -1, i2 = -1, seen = 0;
    for (int i = 0; i < CD.n; i++)
        if (CD.nodes[i].kind == HTML_ELEM && !strcmp(CD.nodes[i].tag, "img")) {
            if (seen == 0) i0 = i; else if (seen == 1) i1 = i; else i2 = i;
            seen++;
        }
    CHECK(seen == 3, "three images parsed");
    CHECK(CD.nodes[i0].img_w == 320 && CD.nodes[i0].img_h == 180,
          "width/height attributes are kept");
    CHECK(CD.nodes[i1].img_w == 0 && CD.nodes[i1].img_h == 0,
          "an image with no stated size says so with 0");
    CHECK(CD.nodes[i2].img_w == 0,
          "an absurd width is refused rather than stored");

    /* CSS outranks the attributes, which is the cascade doing its job */
    /* STATIC: a sheet holds CSS_MAX_RULES rules and is megabytes now, which
     * is a segfault the moment it goes on the stack. */
    static struct css_sheet sh; struct vstyle v, root;
    vstyle_root(&root);
    static const char *css = "img { width: 100px; height: 50px }";
    css_sheet_parse(&sh, css, strlen(css));
    vstyle_for_node(&CD, i0, &root, &sh, &v);
    CHECK(v.width == 100 && v.height == 50, "CSS width/height beat the attributes");

    memset(&v, 0, sizeof v);
    css_apply_decls("max-width: 200px", strlen("max-width: 200px"), &v);
    CHECK(v.width == 200, "max-width caps the box");
    v.width = 120;
    css_apply_decls("max-width: 400px", strlen("max-width: 400px"), &v);
    CHECK(v.width == 120, "...but never widens one that is already narrower");

    memset(&v, 0, sizeof v);
    css_apply_decls("width: 50%", strlen("width: 50%"), &v);
    CHECK(v.width == 0, "a percentage is refused -- no containing block to resolve it");
}

static void t17c_forms(void) {
    printf("T17c forms: what submitting one means:\n");
    char url[512], body[512];

    form_reset();
    cparse("<form action='/search' method='get'>"
           "<input name='q' value='hello world'>"
           "<input name='lang' value='en'>"
           "<input value='no name -- skipped'>"
           "<input type='submit' value='Go'></form>");
    int sub = -1;
    for (int i = 0; i < CD.n; i++)
        if (CD.nodes[i].kind == HTML_ELEM && CD.nodes[i].type &&
            !strcmp(CD.nodes[i].type, "submit")) sub = i;
    CHECK(sub >= 0, "the submit button parsed");

    /* touch each field so its value is seeded from the markup */
    for (int i = 0; i < CD.n; i++)
        if (CD.nodes[i].kind == HTML_ELEM && !strcmp(CD.nodes[i].tag, "input"))
            form_value(&CD, i);

    int how = form_submit(&CD, sub, "/page.html", url, sizeof url, body, sizeof body);
    CHECK(how == 1, "method=get submits as GET");
    CHECK(!strcmp(url, "/search?q=hello+world&lang=en"),
          "fields are urlencoded, joined with &, and space becomes +");
    CHECK(!strstr(url, "skipped"), "a control with no NAME is not submitted");

    /* POST puts the same encoding in the body, not the URL */
    form_reset();
    cparse("<form action='/post' method='POST'><input name='a' value='x y'>"
           "<input type='submit'></form>");
    sub = -1;
    for (int i = 0; i < CD.n; i++)
        if (CD.nodes[i].kind == HTML_ELEM && CD.nodes[i].type &&
            !strcmp(CD.nodes[i].type, "submit")) sub = i;
    for (int i = 0; i < CD.n; i++)
        if (CD.nodes[i].kind == HTML_ELEM && !strcmp(CD.nodes[i].tag, "input"))
            form_value(&CD, i);
    how = form_submit(&CD, sub, "/page.html", url, sizeof url, body, sizeof body);
    CHECK(how == 2, "method=POST (any case) submits as POST");
    CHECK(!strcmp(url, "/post") && !strcmp(body, "a=x+y"),
          "the URL stays clean and the fields go in the body");

    /* no action -> this page; and the query REPLACES the action's own */
    form_reset();
    cparse("<form action='/s?old=1'><input name='n' value='2'>"
           "<input type='submit'></form>");
    sub = -1;
    for (int i = 0; i < CD.n; i++)
        if (CD.nodes[i].kind == HTML_ELEM && CD.nodes[i].type) sub = i;
    for (int i = 0; i < CD.n; i++)
        if (CD.nodes[i].kind == HTML_ELEM && !strcmp(CD.nodes[i].tag, "input"))
            form_value(&CD, i);
    form_submit(&CD, sub, "/page.html", url, sizeof url, body, sizeof body);
    CHECK(!strcmp(url, "/s?n=2"),
          "a form REPLACES the action's query rather than appending to it");

    /* percent-encoding of the characters that would break the encoding */
    form_reset();
    cparse("<form action='/e'><input name='k' value='a&b=c/d?e'>"
           "<input type='submit'></form>");
    sub = -1;
    for (int i = 0; i < CD.n; i++)
        if (CD.nodes[i].kind == HTML_ELEM && CD.nodes[i].type) sub = i;
    for (int i = 0; i < CD.n; i++)
        if (CD.nodes[i].kind == HTML_ELEM && !strcmp(CD.nodes[i].tag, "input"))
            form_value(&CD, i);
    form_submit(&CD, sub, "/page.html", url, sizeof url, body, sizeof body);
    CHECK(!strcmp(url, "/e?k=a%26b%3Dc%2Fd%3Fe"),
          "& = / ? are escaped, so a value cannot forge a second field");

    /* a script's value beats the markup's, and the user's beats both */
    form_reset();
    cparse("<form action='/v'><input name='n' value='markup'>"
           "<input type='submit'></form>");
    int fld = -1;
    for (int i = 0; i < CD.n; i++)
        if (CD.nodes[i].kind == HTML_ELEM && CD.nodes[i].name) fld = i;
    CHECK(!strcmp(form_value(&CD, fld), "markup"), "value= seeds the field");
    form_set(&CD, fld, "typed");
    CHECK(!strcmp(form_peek(fld), "typed"), "a later set wins");
    CHECK(!strcmp(form_value(&CD, fld), "typed"),
          "...and re-reading does NOT re-seed from the markup");
}

static void t18_png_basics(void) {
    printf("T18 PNG: colour types decode to premultiplied BGRA:\n");
    uint32_t w = 0, h = 0;

    CHECK(png_probe(png_rgb, sizeof png_rgb, &w, &h) == PNG_OK && w == 4 && h == 2,
          "probe reads the header without decoding");

    int rc = png_decode(png_rgb, sizeof png_rgb, PIX, sizeof PIX, SCR, sizeof SCR, &w, &h);
    CHECK(rc == PNG_OK && w == 4 && h == 2, "8-bit RGB decodes");
    CHECK(R(PIX[0]) == 255 && G(PIX[0]) == 0 && B(PIX[0]) == 0, "pixel 0 is red");
    CHECK(G(PIX[1]) == 255 && R(PIX[1]) == 0, "pixel 1 is green");
    CHECK(B(PIX[2]) == 255, "pixel 2 is blue");
    CHECK(R(PIX[4]) == 255 && G(PIX[4]) == 0, "row 1 starts a new scanline");

    rc = png_decode(png_rgba, sizeof png_rgba, PIX, sizeof PIX, SCR, sizeof SCR, &w, &h);
    CHECK(rc == PNG_OK && w == 2 && h == 2, "8-bit RGBA decodes");
    CHECK(A(PIX[0]) == 255 && R(PIX[0]) == 255, "opaque pixel keeps its colour");
    CHECK(A(PIX[1]) == 128 && G(PIX[1]) == 128,
          "half-alpha green is PREMULTIPLIED (128, not 255)");
    CHECK(A(PIX[2]) == 0 && R(PIX[2]) == 0 && G(PIX[2]) == 0 && B(PIX[2]) == 0,
          "fully transparent premultiplies to all-zero");
}

static void t19_png_palette_and_filters(void) {
    printf("T19 PNG: palettes, sub-byte depths, and all five filters:\n");
    uint32_t w = 0, h = 0;

    int rc = png_decode(png_pal8, sizeof png_pal8, PIX, sizeof PIX, SCR, sizeof SCR, &w, &h);
    CHECK(rc == PNG_OK && w == 4 && h == 4, "8-bit palette decodes");
    CHECK(R(PIX[0]) == 255 && G(PIX[0]) == 0, "palette index 0 -> red");
    CHECK(B(PIX[2]) == 255, "palette index 2 -> blue");
    CHECK(A(PIX[3]) == 0, "tRNS makes index 3 transparent");

    rc = png_decode(png_pal4, sizeof png_pal4, PIX, sizeof PIX, SCR, sizeof SCR, &w, &h);
    CHECK(rc == PNG_OK && w == 4 && h == 1, "4-bit palette decodes (2 px per byte)");
    CHECK(R(PIX[0]) == 0 && R(PIX[1]) == 255 && G(PIX[1]) == 255,
          "the high nibble is the FIRST pixel, not the second");
    CHECK(R(PIX[2]) == 255 && G(PIX[2]) == 0, "index 2 -> red");
    CHECK(B(PIX[3]) == 255, "index 3 -> blue");

    /* The filters are the part PNG owns. Row 0 is unfiltered 0,10,20..;
     * row 1 is Sub(+10) so it accumulates left-to-right; row 2 is Up on row 1;
     * row 3 Average; row 4 Paeth. Checking row 1 pixel 2 proves Sub actually
     * accumulated rather than being copied. */
    rc = png_decode(png_grayfilters, sizeof png_grayfilters, PIX, sizeof PIX, SCR, sizeof SCR, &w, &h);
    CHECK(rc == PNG_OK && w == 8 && h == 5, "the five-filter image decodes");
    CHECK(R(PIX[0]) == 0 && R(PIX[1]) == 10 && R(PIX[7]) == 70, "row 0 (None) is verbatim");
    CHECK(R(PIX[8]) == 10 && R(PIX[9]) == 20 && R(PIX[10]) == 30,
          "row 1 (Sub) accumulates from the left");
    CHECK(R(PIX[16]) == 15 && R(PIX[17]) == 25, "row 2 (Up) adds the row above");
    CHECK(R(PIX[24]) == 8, "row 3 (Average) adds (left+up)/2");
    CHECK(R(PIX[32]) == 10, "row 4 (Paeth) picks its predictor");
}

static void t20_png_hostile(void) {
    printf("T20 PNG: bytes from a stranger:\n");
    uint32_t w = 0, h = 0;

    int rc = png_decode(png_split_idat, sizeof png_split_idat, PIX, sizeof PIX, SCR, sizeof SCR, &w, &h);
    CHECK(rc == PNG_OK && w == 3 && h == 3,
          "IDAT split across three chunks is CONCATENATED, not inflated piecewise");

    CHECK(png_decode(png_interlaced, sizeof png_interlaced, PIX, sizeof PIX, SCR, sizeof SCR, &w, &h)
          == PNG_EUNSUP, "interlaced is REFUSED, not half-decoded");

    static const uint8_t notpng[] = { 'h','e','l','l','o',0,1,2,3,4,5,6,7,8 };
    CHECK(png_decode(notpng, sizeof notpng, PIX, sizeof PIX, SCR, sizeof SCR, &w, &h)
          == PNG_ENOTPNG, "a non-PNG is rejected on its signature");

    /* Truncation at EVERY length. Cutting into the compressed data must be
     * refused; losing only the trailing IEND must NOT be, because the image
     * is all there and a browser that discards a complete picture over a
     * missing end-marker is worse than one that shows it. The real invariant
     * is that no length reads out of bounds -- run under a sanitizer this
     * loop is the whole point. */
    size_t iend = sizeof png_rgb - 12;        /* IEND chunk is the last 12 bytes */
    int refused = 0, accepted = 0;
    for (size_t n = 1; n < sizeof png_rgb; n++) {
        uint32_t tw = 0, th = 0;
        int r2 = png_decode(png_rgb, n, PIX, sizeof PIX, SCR, sizeof SCR, &tw, &th);
        if (r2 != PNG_OK) { refused++; continue; }
        accepted++;
        CHECK(tw == 4 && th == 2, "an accepted truncation still has the right size");
        break;                                 /* one report is enough */
    }
    CHECK(refused >= (int)iend - 1, "every cut into the image data is refused");

    /* a picture larger than the caller's buffer must be REFUSED, not clipped */
    static uint32_t tiny[4];
    CHECK(png_decode(png_rgb, sizeof png_rgb, tiny, sizeof tiny, SCR, sizeof SCR, &w, &h)
          == PNG_ETOOBIG, "an image that does not fit is refused, never truncated");

    /* a corrupt compressed stream must fail rather than emit garbage */
    static uint8_t bad[sizeof png_rgb];
    memcpy(bad, png_rgb, sizeof bad);
    for (size_t i = 40; i < sizeof bad - 8; i++) bad[i] ^= 0x5A;
    rc = png_decode(bad, sizeof bad, PIX, sizeof PIX, SCR, sizeof SCR, &w, &h);
    CHECK(rc != PNG_OK, "corrupt DEFLATE data is detected");
}

/* ======================= cookies ======================================== */

static unsigned long long fake_now(void) { return 1786500000ull; }  /* 2026-08-08 */

static void t23_cookies(void) {
    printf("T23 the cookie jar: scope, and who may read it:\n");
    char out[512];
    /* A FIXED clock, so an expiry test asserts a rule and not the date the
     * suite happened to run on. 2026-08-08. */
    cookie_set_clock(fake_now);

    cookie_reset();
    cookie_set("example.com", "sid=abc123; Path=/");
    cookie_header("example.com", "/", 0, out, sizeof out);
    CHECK(!strcmp(out, "sid=abc123"), "a cookie comes back to the host that set it");

    /* THE domain check. A suffix match alone lets evil-example.com claim
     * example.com's cookies, so the match must fall on a dot. */
    cookie_header("evil-example.com", "/", 0, out, sizeof out);
    CHECK(out[0] == 0, "a look-alike host does NOT get the cookie");
    cookie_header("www.example.com", "/", 0, out, sizeof out);
    CHECK(out[0] == 0, "...nor a subdomain, when no Domain was stated");

    cookie_reset();
    cookie_set("example.com", "wide=1; Domain=example.com");
    cookie_header("api.example.com", "/", 0, out, sizeof out);
    CHECK(!strcmp(out, "wide=1"), "a stated Domain DOES reach a subdomain");

    /* a server may not scope a cookie to somebody else's domain */
    cookie_reset();
    cookie_set("evil.com", "bad=1; Domain=example.com");
    cookie_header("example.com", "/", 0, out, sizeof out);
    CHECK(out[0] == 0, "a host cannot set a cookie for another domain");

    /* Path: /app must not match /applesauce */
    cookie_reset();
    cookie_set("example.com", "p=1; Path=/app");
    cookie_header("example.com", "/app/x", 0, out, sizeof out);
    CHECK(!strcmp(out, "p=1"), "a path cookie reaches a deeper path");
    cookie_header("example.com", "/applesauce", 0, out, sizeof out);
    CHECK(out[0] == 0, "...but not a path that merely starts the same");

    /* HttpOnly is invisible to scripts -- the whole point of the flag */
    cookie_reset();
    cookie_set("example.com", "sess=zz; HttpOnly");
    cookie_set("example.com", "theme=dark");
    cookie_header("example.com", "/", 0, out, sizeof out);
    CHECK(strstr(out, "sess=zz") && strstr(out, "theme=dark"),
          "the REQUEST carries both cookies");
    cookie_for_script("example.com", "/", out, sizeof out);
    CHECK(!strstr(out, "sess=zz"), "document.cookie cannot see an HttpOnly one");
    CHECK(strstr(out, "theme=dark") != 0, "...and can see the ordinary one");

    /* Secure stays off a plain connection */
    cookie_reset();
    cookie_set("example.com", "s=1; Secure");
    cookie_header("example.com", "/", 0, out, sizeof out);
    CHECK(out[0] == 0, "a Secure cookie is not sent over plain http");
    cookie_header("example.com", "/", 1, out, sizeof out);
    CHECK(!strcmp(out, "s=1"), "...and is sent over https");

    /* replacing, and deleting the way a logout does */
    cookie_reset();
    cookie_set("example.com", "k=one");
    cookie_set("example.com", "k=two");
    cookie_header("example.com", "/", 0, out, sizeof out);
    CHECK(!strcmp(out, "k=two"), "setting the same name REPLACES it");
    cookie_set("example.com", "k=; Max-Age=0");
    cookie_header("example.com", "/", 0, out, sizeof out);
    CHECK(out[0] == 0, "Max-Age=0 deletes it, which is how logout works");

    /* DATES. Checked against values computed independently, because a date
     * routine that is wrong by a day, or by a leap year, fails silently: the
     * cookie simply goes missing one day earlier than the server meant. */
    CHECK(cookie_parse_date("Thu, 01 Jan 1970 00:00:00 GMT", 29) == 0ull,
          "the epoch is a real date, and reads as zero");
    CHECK(cookie_parse_date("Fri, 02 Jan 1970 00:00:00 GMT", 29) == 86400ull,
          "one day after the epoch");
    CHECK(cookie_parse_date("Tue, 01 Jan 2019 00:00:00 GMT", 29) == 1546300800ull,
          "2019-01-01 (past two leap years and a century rule)");
    CHECK(cookie_parse_date("Sat, 01 Mar 2020 00:00:00 GMT", 29) == 1583020800ull,
          "just after a LEAP day");
    CHECK(cookie_parse_date("Wed, 09 Jun 2027 10:18:14 GMT", 29) == 1812536294ull,
          "a real Expires value, to the second");
    CHECK(cookie_parse_date("not a date at all", 17) == COOKIE_DATE_BAD,
          "garbage is BAD, which is distinct from the epoch -- a deletion IS the epoch");

    /* an Expires in the FUTURE keeps the cookie; one in the past deletes it */
    cookie_reset();
    cookie_set("example.com", "keep=1; Expires=Wed, 09 Jun 2099 10:18:14 GMT");
    cookie_header("example.com", "/", 0, out, sizeof out);
    CHECK(!strcmp(out, "keep=1"), "a future Expires keeps the cookie");
    cookie_set("example.com", "keep=1; Expires=Thu, 01 Jan 1970 00:00:00 GMT");
    cookie_header("example.com", "/", 0, out, sizeof out);
    CHECK(out[0] == 0, "a past Expires deletes it -- which is what logout sends");

    /* a whole response block, with the comma inside Expires that is exactly
     * why Set-Cookie must not be comma-joined with its siblings */
    cookie_reset();
    const char *resp =
        "HTTP/1.1 200 OK\r\n"
        "Set-Cookie: a=1; Path=/\r\n"
        "Content-Type: text/html\r\n"
        "Set-Cookie: b=2; Expires=Wed, 09 Jun 2027 10:18:14 GMT\r\n"
        "\r\n";
    int got = cookie_take_headers("example.com", resp);
    CHECK(got == 2, "two Set-Cookie headers are taken separately");
    cookie_header("example.com", "/", 0, out, sizeof out);
    CHECK(strstr(out, "a=1") && strstr(out, "b=2"), "...and both are sent back");
    cookie_reset();
}

/* ================= external stylesheets ================================ */

static void t22_linkcss(void) {
    printf("T22 <link rel=stylesheet>: the parser records WHERE, never fetches:\n");
    static struct html_node nodes[256];
    static char strs[8192];
    struct html_doc d;

    const char *doc =
        "<html><head>"
        "<link rel=stylesheet href=\"/a.css\">"
        "<link rel='alternate stylesheet' href='b.css'>"
        "<link rel=icon href=/favicon.ico>"
        "<link href=/no-rel.css>"
        "<style>p{color:red}</style>"
        "</head><body><p>hi</p></body></html>";
    html_parse(&d, doc, strlen(doc), nodes, 256, strs, sizeof strs);

    CHECK(d.n_cssref == 2, "two stylesheets found, and only the stylesheets");
    CHECK(d.n_cssref > 0 && !strcmp(d.cssref[0], "/a.css"), "...the first, in document order");
    CHECK(d.n_cssref > 1 && !strcmp(d.cssref[1], "b.css"), "...and the second");
    /* rel is a TOKEN LIST: "alternate stylesheet" counts, "icon" does not, and
     * a <link> with no rel at all is not a stylesheet however it is named. */
    CHECK(d.css && !strncmp(d.css, "p{color:red}", 12), "an inline <style> is still captured");

    /* A page may reference more sheets than we will hold. The cap must be a
     * bounded truncation, not an overrun. */
    static char many[4096];
    size_t mn = 0;
    mn += (size_t)snprintf(many + mn, sizeof many - mn, "<html><head>");
    for (int i = 0; i < 20; i++)
        mn += (size_t)snprintf(many + mn, sizeof many - mn,
                               "<link rel=stylesheet href=/s%d.css>", i);
    snprintf(many + mn, sizeof many - mn, "</head><body>x</body></html>");
    html_parse(&d, many, strlen(many), nodes, 256, strs, sizeof strs);
    CHECK(d.n_cssref == 8, "twenty sheets truncate to the cap, without overrunning");
}

/* ======================= JPEG ========================================== */

static void t21_jpeg(void) {
    printf("T21 JPEG: Huffman, IDCT, chroma, colour:\n");
    uint32_t w = 0, h = 0;

    CHECK(jpeg_probe(jpg_flat, sizeof jpg_flat, &w, &h) == JPG_OK && w == 16 && h == 16,
          "probe reads the frame header");

    int rc = jpeg_decode(jpg_flat, sizeof jpg_flat, PIX, sizeof PIX, SCR, sizeof SCR, &w, &h);
    CHECK(rc == JPG_OK && w == 16 && h == 16, "a 4:4:4 baseline image decodes");
    /* JPEG is lossy, so the test is NEARNESS, not equality -- an exact match
     * would only prove the fixture was re-encoded by the same code. */
    int r = R(PIX[8 * 16 + 8]), g = G(PIX[8 * 16 + 8]), b = B(PIX[8 * 16 + 8]);
    CHECK(r > 200 && r < 240 && g < 60 && b < 70,
          "flat red comes back red (within JPEG's own error)");
    CHECK(A(PIX[0]) == 255, "opaque: JPEG has no alpha");

    rc = jpeg_decode(jpg_sub420, sizeof jpg_sub420, PIX, sizeof PIX, SCR, sizeof SCR, &w, &h);
    CHECK(rc == JPG_OK && w == 32 && h == 16, "4:2:0 subsampled image decodes");
    /* the gradient must still ascend after chroma upsampling */
    CHECK(R(PIX[2]) < R(PIX[16]) && R(PIX[16]) < R(PIX[29]),
          "the red ramp still ascends across the row");
    CHECK(B(PIX[2]) > B(PIX[29]), "...and the blue ramp still descends");

    rc = jpeg_decode(jpg_odd, sizeof jpg_odd, PIX, sizeof PIX, SCR, sizeof SCR, &w, &h);
    CHECK(rc == JPG_OK && w == 17 && h == 9,
          "a size that is NOT a multiple of 8 decodes (blocks overrun the edge)");
    CHECK(G(PIX[8 * 17 + 8]) > 150, "...and the interior is still the right colour");

    rc = jpeg_decode(jpg_gray, sizeof jpg_gray, PIX, sizeof PIX, SCR, sizeof SCR, &w, &h);
    CHECK(rc == JPG_OK && w == 16 && h == 16, "single-component greyscale decodes");
    CHECK(R(PIX[8 * 16 + 2]) == G(PIX[8 * 16 + 2]) &&
          G(PIX[8 * 16 + 2]) == B(PIX[8 * 16 + 2]),
          "...and grey means r == g == b");

    CHECK(jpeg_decode(jpg_progressive, sizeof jpg_progressive, PIX, sizeof PIX,
                      SCR, sizeof SCR, &w, &h) == JPG_EUNSUP,
          "progressive is REFUSED, not half-decoded");

    static const uint8_t notjpg[] = { 'n','o','t',' ','a',' ','j','p','g',0,1,2 };
    CHECK(jpeg_decode(notjpg, sizeof notjpg, PIX, sizeof PIX, SCR, sizeof SCR, &w, &h)
          == JPG_ENOTJPG, "a non-JPEG is rejected on its signature");

    static uint32_t tiny[4];
    CHECK(jpeg_decode(jpg_flat, sizeof jpg_flat, tiny, sizeof tiny, SCR, sizeof SCR, &w, &h)
          == JPG_ETOOBIG, "an image that does not fit is refused, never truncated");

    /* Every truncation must terminate and never read out of bounds. A JPEG cut
     * mid-scan should show what arrived, so success is allowed -- what is not
     * allowed is a hang or a wrong size. */
    int ok = 1;
    for (size_t n = 2; n < sizeof jpg_flat; n += 7) {
        uint32_t tw = 0, th = 0;
        int r2 = jpeg_decode(jpg_flat, n, PIX, sizeof PIX, SCR, sizeof SCR, &tw, &th);
        if (r2 == JPG_OK && (tw != 16 || th != 16)) ok = 0;
    }
    CHECK(ok, "every truncation either refuses or decodes at the right size");
}

/* --- t24: tabs ----------------------------------------------------------- *
 *
 * The tab model is bookkeeping with no syscalls in it, which is exactly the
 * kind of thing that is worth testing here: every rule below ("closing the
 * last tab is refused", "a new place clears forward") is a rule someone will
 * otherwise discover by losing a page.
 */
static void t24_tabs(void) {
    printf("\n-- t24: tabs --\n");
    tab_init();
    CHECK(tab_count() == 1 && tab_current() == 0, "a browser starts with one tab");

    tab_set_url(0, "http://a.example/one");
    int b = tab_open("http://b.example/two");
    CHECK(b == 1 && tab_count() == 2, "opening gives a second tab");
    CHECK(tab_current() == 0, "...and does NOT steal focus: opening in the "
                              "background is the common case");

    /* Each tab keeps its own place. This is the whole feature. */
    tab_set_scroll(0, 640.0f);
    tab_set_scroll(1, 20.0f);
    tab_set_zoom(1, 1.44f);
    CHECK(tab_select(1) == 0, "switching reports that it changed");
    CHECK(tab_select(1) == -1, "...and reports that it did not, when it did not");
    CHECK(tab_scroll(0) == 640.0f && tab_scroll(1) == 20.0f,
          "each tab keeps its own scroll position");
    CHECK(tab_zoom(1) > 1.4f && tab_zoom(0) == 1.0f, "...and its own zoom");

    /* The retained source: what makes a background tab re-showable without
     * going back to the network. */
    tab_set_src(0, "<h1>ONE</h1>", 12);
    tab_set_src(1, "<h1>TWO</h1>", 12);
    CHECK(tab_src_len(0) == 12 && !memcmp(tab_src(0), "<h1>ONE</h1>", 12),
          "a background tab keeps the bytes it was built from");
    CHECK(tab_src(0) != tab_src(1), "...in its own buffer, not a shared one");

    /* Per-tab history. Tab 1's back stack must not see tab 0's pages. */
    tab_hist_push(0, "http://a.example/one");
    tab_set_url(0, "http://a.example/deeper");
    CHECK(tab_can_back(0) && !tab_can_back(1), "history is per tab, not global");
    char out[TAB_URL_MAX];
    CHECK(tab_hist_back(0, tab_url(0), out, sizeof out) == 0 &&
          !strcmp(out, "http://a.example/one"), "back goes where it went");
    CHECK(tab_can_fwd(0), "...and leaves a forward");
    tab_hist_push(0, "http://a.example/one");
    CHECK(!tab_can_fwd(0), "a new destination CLEARS forward -- you branched");

    /* Labels: a tab with no title is still clickable. */
    CHECK(!strcmp(tab_label(1), "two"), "a titleless tab is named from its URL");
    tab_set_title(1, "Second page");
    CHECK(!strcmp(tab_label(1), "Second page"), "...and by its title once it has one");
    int t2 = tab_open("http://c.example/");
    CHECK(!strcmp(tab_label(t2), "c.example"),
          "a trailing slash names the tab after the host, not nothing");

    /* Closing. */
    CHECK(tab_select(t2) == 0, "");
    int now = tab_close(t2);
    CHECK(now == 1, "closing the current tab lands on its left neighbour");
    CHECK(tab_count() == 2, "...and the count drops");
    CHECK(tab_scroll(1) == 20.0f, "the tab you land on still knows where it was");

    tab_close(1);
    CHECK(tab_count() == 1 && tab_current() == 0, "closing down to one works");
    int before = tab_current();
    CHECK(tab_close(0) == before && tab_count() == 1,
          "closing the LAST tab is refused -- a browser with no tabs shows nothing");

    /* Bounded, like everything else a page can drive. */
    while (tab_open("http://x/") >= 0) { }
    CHECK(tab_count() == TAB_MAX, "tabs are capped, and opening past the cap refuses");
}

int main(void) {
    printf("=== html-test ===\n");
    t1_structure(); t2_implicit_close(); t3_void_and_attrs(); t4_entities();
    t5_script_style(); t6_whitespace(); t7_malformed(); t8_urls(); t9_bounded();
    t10_url_parse(); t11_url_resolve();
    t11b_entities(); t12_attrs_and_style_block(); t13_declarations(); t14_selectors();
    t15_cascade(); t16_origin_order(); t17_bounded();
    t17b_image_sizing(); t17c_forms(); t18_png_basics(); t19_png_palette_and_filters(); t20_png_hostile();
    t22_linkcss();
    t23_cookies();
    t24_tabs();
    t21_jpeg();
    printf("=== html-test: %s (%d failures) ===\n", failures ? "FAIL" : "OK", failures);
    return failures ? 1 : 0;
}
